// What a `for` loop becomes: the direct loop for an array, a slice or a range, and the calls of the iterator convention
// for anything else.

#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <doctest.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace Rux;

namespace {
HirPackage LowerSource(const std::string &source) {
    Lexer lexer(source, "iteration.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "iteration.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());
    return AstToHirLowering(model).Generate();
}

const HirFunc &RequireFunction(const HirPackage &package, const std::string &name) {
    REQUIRE_EQ(package.modules.size(), 1);
    for (const HirFunc &function : package.modules.front().funcs) {
        if (function.name == name) {
            return function;
        }
    }
    FAIL("missing lowered function " << name);
    throw std::runtime_error("missing lowered function");
}

/// The scope a convention-driven loop lowers to, taken from the nth statement of a function body.
const HirScopeStmt &RequireIterationScope(const HirFunc &function, const std::size_t statement) {
    REQUIRE(function.body.has_value());
    REQUIRE_GT(function.body->stmts.size(), statement);
    const auto *scope = dynamic_cast<const HirScopeStmt *>(function.body->stmts[statement].get());
    REQUIRE(scope != nullptr);
    return *scope;
}

const HirMatchStmt &RequireAdvanceStep(const HirScopeStmt &scope) {
    REQUIRE_EQ(scope.block.stmts.size(), 2);
    const auto *loop = dynamic_cast<const HirLoopStmt *>(scope.block.stmts[1].get());
    REQUIRE(loop != nullptr);
    REQUIRE_EQ(loop->body.stmts.size(), 1);
    const auto *step = dynamic_cast<const HirMatchStmt *>(loop->body.stmts.front().get());
    REQUIRE(step != nullptr);
    return *step;
}

std::string CalleeName(const HirExpr &expr) {
    const auto *call = dynamic_cast<const HirCallExpr *>(&expr);
    REQUIRE(call != nullptr);
    const auto *callee = dynamic_cast<const HirVarExpr *>(call->callee.get());
    REQUIRE(callee != nullptr);
    return callee->name;
}

const std::string kIterationPrelude = R"(
    enum Option<T> { Some(T), None }
    struct Counter { value: int32; limit: int32; }
    extend Counter {
        func Next(self: &var Counter) -> Option<int32> {
            if self.value >= self.limit { return Option::None<int32>(); }
            let current = self.value;
            self.value = self.value + 1i32;
            return Option::Some<int32>(current);
        }
    }
    struct Span { limit: int32; }
    extend Span {
        func Iterate(self: &Span) -> Counter { return Counter { value: 0i32, limit: self.limit }; }
    }
)";
} // namespace

TEST_CASE("a loop over an iterator advances it and matches what it reports") {
    const HirPackage package = LowerSource(kIterationPrelude + R"(
        func Walk() -> int32 {
            var counter = Counter { value: 0i32, limit: 3i32 };
            var total = 0i32;
            for item in counter { total = total + item; }
            return total;
        }
    )");

    const HirScopeStmt &scope = RequireIterationScope(RequireFunction(package, "Walk"), 2);
    // The iterator is a local of the loop's own scope, so nothing is allocated and nothing outlives the loop.
    const auto *iterator = dynamic_cast<const HirLetStmt *>(scope.block.stmts.front().get());
    REQUIRE(iterator != nullptr);
    CHECK(iterator->isMut);
    CHECK_EQ(iterator->type, TypeRef::MakeNamed("Counter"));
    // The subject is already an iterator, so it is moved into that local rather than being asked for one.
    CHECK(dynamic_cast<const HirCallExpr *>(iterator->init.get()) == nullptr);

    const HirMatchStmt &step = RequireAdvanceStep(scope);
    CHECK_EQ(CalleeName(*step.subject), "Counter::Next");
    REQUIRE_EQ(step.arms.size(), 2);
    const auto *item = dynamic_cast<const HirEnumPattern *>(step.arms[0].pattern.get());
    REQUIRE(item != nullptr);
    CHECK_EQ(item->path, std::vector<std::string>{"Option", "Some"});
    CHECK(item->hasPayload);
    const auto *end = dynamic_cast<const HirEnumPattern *>(step.arms[1].pattern.get());
    REQUIRE(end != nullptr);
    CHECK_EQ(end->path, std::vector<std::string>{"Option", "None"});
    CHECK_FALSE(end->hasPayload);
}

TEST_CASE("the item arm binds the loop variable and the end arm leaves the loop") {
    const HirPackage package = LowerSource(kIterationPrelude + R"(
        func Walk() -> int32 {
            var counter = Counter { value: 0i32, limit: 3i32 };
            var total = 0i32;
            for item in counter { total = total + item; }
            return total;
        }
    )");

    const HirMatchStmt &step = RequireAdvanceStep(RequireIterationScope(RequireFunction(package, "Walk"), 2));
    const auto *itemBody = dynamic_cast<const HirBlockExpr *>(step.arms[0].body.get());
    REQUIRE(itemBody != nullptr);
    REQUIRE_FALSE(itemBody->block.stmts.empty());
    const auto *variable = dynamic_cast<const HirLetStmt *>(itemBody->block.stmts.front().get());
    REQUIRE(variable != nullptr);
    CHECK_EQ(variable->name, "item");
    CHECK_EQ(variable->type, TypeRef::MakeInt32());

    const auto *endBody = dynamic_cast<const HirBlockExpr *>(step.arms[1].body.get());
    REQUIRE(endBody != nullptr);
    REQUIRE_EQ(endBody->block.stmts.size(), 1);
    CHECK(dynamic_cast<const HirBreakStmt *>(endBody->block.stmts.front().get()) != nullptr);
}

TEST_CASE("a container is asked for its iterator once, before the loop") {
    const HirPackage package = LowerSource(kIterationPrelude + R"(
        func Walk() -> int32 {
            let span = Span { limit: 3i32 };
            var total = 0i32;
            for item in span { total = total + item; }
            return total;
        }
    )");

    const HirScopeStmt &scope = RequireIterationScope(RequireFunction(package, "Walk"), 2);
    const auto *iterator = dynamic_cast<const HirLetStmt *>(scope.block.stmts.front().get());
    REQUIRE(iterator != nullptr);
    CHECK_EQ(CalleeName(*iterator->init), "Span::Iterate");
    CHECK_EQ(iterator->type, TypeRef::MakeNamed("Counter"));
    CHECK_EQ(CalleeName(*RequireAdvanceStep(scope).subject), "Counter::Next");
}

TEST_CASE("nested convention-driven loops advance separate iterators") {
    const HirPackage package = LowerSource(kIterationPrelude + R"(
        func Walk() -> int32 {
            var count = 0i32;
            var outer = Counter { value: 0i32, limit: 3i32 };
            for first in outer {
                var inner = Counter { value: 0i32, limit: 3i32 };
                for second in inner { count = count + 1i32; }
            }
            return count;
        }
    )");

    const HirScopeStmt &outerScope = RequireIterationScope(RequireFunction(package, "Walk"), 2);
    const auto *outerIterator = dynamic_cast<const HirLetStmt *>(outerScope.block.stmts.front().get());
    REQUIRE(outerIterator != nullptr);

    const auto *itemBody = dynamic_cast<const HirBlockExpr *>(RequireAdvanceStep(outerScope).arms[0].body.get());
    REQUIRE(itemBody != nullptr);
    const HirScopeStmt *innerScope = nullptr;
    for (const auto &statement : itemBody->block.stmts) {
        if (const auto *candidate = dynamic_cast<const HirScopeStmt *>(statement.get())) {
            innerScope = candidate;
        }
    }
    REQUIRE(innerScope != nullptr);
    const auto *innerIterator = dynamic_cast<const HirLetStmt *>(innerScope->block.stmts.front().get());
    REQUIRE(innerIterator != nullptr);
    CHECK_NE(outerIterator->name, innerIterator->name);
}

TEST_CASE("arrays, slices and ranges keep the direct loop they already lowered to") {
    const HirPackage package = LowerSource(R"(
        func Walk(values: int32[4]) -> int32 {
            var total = 0i32;
            for value in values { total = total + value; }
            for index in 0..4 { total = total + index as int32; }
            return total;
        }
    )");

    const HirFunc &walk = RequireFunction(package, "Walk");
    REQUIRE(walk.body.has_value());
    REQUIRE_GT(walk.body->stmts.size(), 2);
    CHECK(dynamic_cast<const HirForStmt *>(walk.body->stmts[1].get()) != nullptr);
    CHECK(dynamic_cast<const HirForStmt *>(walk.body->stmts[2].get()) != nullptr);
}
