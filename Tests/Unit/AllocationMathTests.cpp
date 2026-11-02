// Compile-time layout queries and the checked arithmetic an allocation size is computed with.

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
std::vector<SemanticDiagnostic> AnalyzeSource(const std::string &source) {
    Lexer lexer(source, "allocation.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "allocation.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    return analyzer.Analyze().diagnostics;
}

HirPackage LowerSource(const std::string &source) {
    Lexer lexer(source, "allocation.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "allocation.rux");
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

/// The initializer of the nth `let` in a function body.
const HirExpr &RequireInitializer(const HirFunc &function, const std::size_t statement) {
    REQUIRE(function.body.has_value());
    REQUIRE_GT(function.body->stmts.size(), statement);
    const auto *binding = dynamic_cast<const HirLetStmt *>(function.body->stmts[statement].get());
    REQUIRE(binding != nullptr);
    REQUIRE(binding->init != nullptr);
    return *binding->init;
}

} // namespace

TEST_CASE("both layout queries fold to unsigned constants") {
    const HirPackage package = LowerSource(R"(
        struct Header { tag: uint32; payload: uint64; }
        func Query() -> uint64 {
            let size = sizeof(Header);
            let alignment = alignof(Header);
            let elementSize = sizeof(uint32);
            return size + alignment + elementSize;
        }
    )");

    const HirFunc &query = RequireFunction(package, "Query");
    const auto folded = [&](const std::size_t statement) {
        const auto *literal = dynamic_cast<const HirLiteralExpr *>(&RequireInitializer(query, statement));
        REQUIRE(literal != nullptr);
        CHECK_EQ(literal->type, TypeRef::MakeUInt64());
        return literal->value;
    };
    CHECK_EQ(folded(0), "16");
    CHECK_EQ(folded(1), "8");
    CHECK_EQ(folded(2), "4");
}

TEST_CASE("a layout query on a type parameter answers the question it was asked") {
    // Where the type is concrete the answer is folded during analysis; where it is a type parameter it is answered
    // at each instantiation instead, and that path was returning the size for both queries. Header is sixteen bytes
    // aligned to eight, which is what separates the two answers.
    const HirPackage package = LowerSource(R"(
        struct Header { tag: uint32; payload: uint64; }
        func SizeOf<T>() -> uint64 { return sizeof(T); }
        func AlignOf<T>() -> uint64 { return alignof(T); }
        func Query() -> uint64 { return SizeOf<Header>() + AlignOf<Header>(); }
    )");

    const auto returned = [&](const std::string &name) {
        const HirFunc &function = RequireFunction(package, name);
        REQUIRE(function.body.has_value());
        REQUIRE_EQ(function.body->stmts.size(), 1);
        const auto *statement = dynamic_cast<const HirReturnStmt *>(function.body->stmts.front().get());
        REQUIRE(statement != nullptr);
        REQUIRE(statement->value.has_value());
        const auto *literal = dynamic_cast<const HirLiteralExpr *>(statement->value->get());
        REQUIRE(literal != nullptr);
        return literal->value;
    };
    CHECK_EQ(returned("SizeOf_Header"), "16");
    CHECK_EQ(returned("AlignOf_Header"), "8");
}

TEST_CASE("a layout query over a type with no layout is reported once") {
    const auto diagnostics = AnalyzeSource(R"(
        struct Node { next: Node; value: int32; }
        func Query() -> uint64 { return alignof(Node); }
    )");

    REQUIRE_FALSE(diagnostics.empty());
    const auto reported = std::ranges::find_if(diagnostics, [](const SemanticDiagnostic &diagnostic) {
        return diagnostic.message.starts_with("cannot determine the alignment");
    });
    REQUIRE(reported != diagnostics.end());
    CHECK_EQ(reported->message, "cannot determine the alignment of type 'Node'");
    REQUIRE_EQ(reported->notes.size(), 1);
    CHECK_EQ(reported->notes[0], "'alignof' needs a type whose layout is known at compile time");
}
