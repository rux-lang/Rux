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
HirPackage LowerCoalescingSource(const std::string &source) {
    Lexer lexer(source, "coalescing-lowering.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "coalescing-lowering.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());
    return AstToHirLowering(model).Generate();
}

const HirFunc &RequireCoalescingFunction(const HirPackage &package, const std::string &name) {
    REQUIRE_EQ(package.modules.size(), 1);
    for (const HirFunc &function : package.modules.front().funcs) {
        if (function.name == name) {
            return function;
        }
    }
    FAIL("missing lowered function " << name);
    throw std::runtime_error("missing lowered function");
}

const HirMatchExpr &RequireCoalescingMatch(const HirFunc &function, const std::size_t statement = 0) {
    REQUIRE(function.body.has_value());
    REQUIRE_GT(function.body->stmts.size(), statement);
    const auto *binding = dynamic_cast<const HirLetStmt *>(function.body->stmts[statement].get());
    REQUIRE(binding != nullptr);
    const auto *match = dynamic_cast<const HirMatchExpr *>(binding->init.get());
    REQUIRE(match != nullptr);
    return *match;
}
} // namespace

TEST_CASE("coalescing parses right-associatively between logical-or and ternary") {
    Lexer lexer(R"(
        func Read() -> int32 { return 1i32; }
        func Main(a: int32, b: int32, c: int32, flag: bool) {
            let chain = a ?? b ?? c;
            let logical = flag || flag ?? c;
            let conditional = a ?? b ? c : a;
            let arms = flag ? a ?? b : c ?? a;
            let propagated = Read()? ?? c;
            a = b ?? c;
        }
    )",
                "coalescing-parser.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "coalescing-parser.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    const auto *main = dynamic_cast<const FuncDecl *>(parsed.module.items[1].get());
    REQUIRE(main != nullptr);
    REQUIRE(main->body != nullptr);

    const auto initializer = [&](const std::size_t index) -> const Expr * {
        const auto *binding = dynamic_cast<const LetStmt *>(main->body->stmts[index].get());
        REQUIRE(binding != nullptr);
        return binding->init.get();
    };

    const auto *chain = dynamic_cast<const BinaryExpr *>(initializer(0));
    REQUIRE(chain != nullptr);
    CHECK_EQ(chain->op, TokenKind::QuestionQuestion);
    const auto *chainRight = dynamic_cast<const BinaryExpr *>(chain->right.get());
    REQUIRE(chainRight != nullptr);
    CHECK_EQ(chainRight->op, TokenKind::QuestionQuestion);

    const auto *logical = dynamic_cast<const BinaryExpr *>(initializer(1));
    REQUIRE(logical != nullptr);
    CHECK_EQ(logical->op, TokenKind::QuestionQuestion);
    const auto *logicalLeft = dynamic_cast<const BinaryExpr *>(logical->left.get());
    REQUIRE(logicalLeft != nullptr);
    CHECK_EQ(logicalLeft->op, TokenKind::PipePipe);

    const auto *conditional = dynamic_cast<const TernaryExpr *>(initializer(2));
    REQUIRE(conditional != nullptr);
    const auto *condition = dynamic_cast<const BinaryExpr *>(conditional->condition.get());
    REQUIRE(condition != nullptr);
    CHECK_EQ(condition->op, TokenKind::QuestionQuestion);

    const auto *arms = dynamic_cast<const TernaryExpr *>(initializer(3));
    REQUIRE(arms != nullptr);
    CHECK(dynamic_cast<const BinaryExpr *>(arms->thenExpr.get()) != nullptr);
    CHECK(dynamic_cast<const BinaryExpr *>(arms->elseExpr.get()) != nullptr);

    const auto *propagated = dynamic_cast<const BinaryExpr *>(initializer(4));
    REQUIRE(propagated != nullptr);
    CHECK(dynamic_cast<const TryExpr *>(propagated->left.get()) != nullptr);

    const auto *assignmentStatement = dynamic_cast<const ExprStmt *>(main->body->stmts[5].get());
    REQUIRE(assignmentStatement != nullptr);
    const auto *assignment = dynamic_cast<const AssignExpr *>(assignmentStatement->expr.get());
    REQUIRE(assignment != nullptr);
    CHECK(dynamic_cast<const BinaryExpr *>(assignment->value.get()) != nullptr);
}

TEST_CASE("coalescing lowers to one Option match with a lazy fallback arm") {
    const HirPackage package = LowerCoalescingSource(R"(
        variant Option<T> { Some(T), None }
        func Left() -> Option<int32> { return Option::Some<int32>(7i32); }
        func Fallback() -> int32 { return 9i32; }
        func Pick() -> int32 {
            let value = Left() ?? Fallback();
            return value;
        }
    )");

    const HirMatchExpr &match = RequireCoalescingMatch(RequireCoalescingFunction(package, "Pick"));
    CHECK(dynamic_cast<const HirCallExpr *>(match.subject.get()) != nullptr);
    CHECK_EQ(match.type, TypeRef::MakeInt32());
    REQUIRE_EQ(match.arms.size(), 2);

    const auto *some = dynamic_cast<const HirEnumPattern *>(match.arms[0].pattern.get());
    REQUIRE(some != nullptr);
    CHECK_EQ(some->path, std::vector<std::string>{"Option", "Some"});
    CHECK(some->hasPayload);
    REQUIRE(some->discriminant.has_value());
    CHECK_EQ(*some->discriminant, "0");
    const auto *payload = dynamic_cast<const HirVarExpr *>(match.arms[0].body.get());
    REQUIRE(payload != nullptr);
    const auto *bound = dynamic_cast<const HirBindingPattern *>(some->args.front().get());
    REQUIRE(bound != nullptr);
    CHECK_EQ(payload->name, bound->name);
    CHECK_EQ(bound->bindingId, 0);

    const auto *none = dynamic_cast<const HirEnumPattern *>(match.arms[1].pattern.get());
    REQUIRE(none != nullptr);
    CHECK_EQ(none->path, std::vector<std::string>{"Option", "None"});
    CHECK_FALSE(none->hasPayload);
    REQUIRE(none->discriminant.has_value());
    CHECK_EQ(*none->discriminant, "1");
    CHECK(dynamic_cast<const HirCallExpr *>(match.arms[1].body.get()) != nullptr);
}

TEST_CASE("coalescing uses the payload custom move plan") {
    const HirPackage package = LowerCoalescingSource(R"(
        variant Option<T> { Some(T), None }
        struct Handle { value: int32; }
        extend Handle {
            func =(self: &var Handle, other: &Handle);
            func <-(self: &var Handle, other: Handle) { self.value = other.value; }
            func ~Handle(self: &var Handle) {}
        }
        func Fallback() -> Handle { return Handle { value: 9i32 }; }
        func Pick(option: Option<Handle>) -> int32 {
            let value = (<-option) ?? Fallback();
            return value.value;
        }
    )");

    const HirMatchExpr &match = RequireCoalescingMatch(RequireCoalescingFunction(package, "Pick"));
    const auto *moved = dynamic_cast<const HirMoveExpr *>(match.arms[0].body.get());
    REQUIRE(moved != nullptr);
    CHECK_EQ(moved->plan.kind, HirMovePlan::Kind::Custom);
}
