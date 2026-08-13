#include "Lexer/Lexer.h"
#include "Semantic/SemanticProgramIndex.h"
#include "Syntax/Parser/Parser.h"

#include <doctest.h>

using namespace Rux;

namespace {
ParseResult ParseModule(const std::string &source, const std::string &sourceName) {
    Lexer lexer(source, sourceName);
    auto tokens = lexer.Tokenize();
    REQUIRE_FALSE(tokens.HasErrors());
    Parser parser(std::move(tokens.tokens), sourceName);
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    return parsed;
}
} // namespace

TEST_CASE("semantic program index isolates packages and records declaration ownership") {
    auto first = ParseModule(R"(
        module Api {
            func Make(value: int) -> Entry {}
            func Read(value: int) -> int { return value; }
        }
    )",
                             "first.rux");
    auto second = ParseModule(R"(
        module Api {
            struct Entry { value: int; }
            func Read(value: bool) -> int { return 0; }
        }
    )",
                              "second.rux");

    std::vector<SemanticDiagnostic> diagnostics;
    std::vector<SemanticSymbol> symbols;
    SemanticDetail::SemanticProgramIndex index(diagnostics, symbols);
    auto resolveType = [](const TypeExpr &) { return TypeRef::MakeUnknown(); };
    SemanticDetail::Scope &packageRoot = index.CreatePackageRoot("acme-core");
    const std::string packageName = "acme-core";
    for (const auto &declaration : first.module.items) {
        index.CollectDeclaration(*declaration, packageRoot, first.module.name, resolveType, &packageName);
    }
    for (const auto &declaration : second.module.items) {
        index.CollectDeclaration(*declaration, packageRoot, second.module.name, resolveType, &packageName);
    }

    REQUIRE(diagnostics.empty());
    CHECK(index.GlobalScope().Lookup("Api") == nullptr);
    const auto &packageScopes = index.Packages().at("acme-core");
    REQUIRE(packageScopes.contains("Api"));
    const SemanticDetail::Symbol *read = packageScopes.at("Api")->LookupLocal("Read");
    REQUIRE(read != nullptr);
    REQUIRE_EQ(read->funcOverloads.size(), 2);
    CHECK(index.FunctionModulePaths().at(read->funcOverloads[0]) == "Api");
    CHECK(index.FunctionSources().at(read->funcOverloads[0]) == "first.rux");
    CHECK(index.FunctionSources().at(read->funcOverloads[1]) == "second.rux");
    CHECK(packageScopes.at("Api")->LookupLocal("Entry") != nullptr);
}
