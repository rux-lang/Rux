#pragma once

#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"
#include "Types/PrimitiveCatalog.h"

#include <algorithm>
#include <array>
#include <doctest.h>
#include <string>
#include <string_view>
#include <vector>

namespace Rux::Testing::SemanticTestSupport {
using namespace Rux;

inline std::vector<SemanticDiagnostic> AnalyzeSource(const std::string &source, const SemanticOptions &options = {}) {
    Lexer lexer(source, "test.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());

    Parser parser(std::move(lexed.tokens), "test.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    analyzer.SetOptions(options);
    return analyzer.Analyze().diagnostics;
}

// Analyze `userSource` with a single dependency package `depName` whose source
// is `depSource`. The parsed modules stay alive for the whole Analyze() call.
inline std::vector<SemanticDiagnostic> AnalyzeWithDep(const std::string &userSource, const std::string &depName,
                                                      const std::string &depSource,
                                                      const SemanticOptions &options = {}) {
    Lexer depLexer(depSource, "dep.rux");
    auto depLexed = depLexer.Tokenize();
    REQUIRE_FALSE(depLexed.HasErrors());
    Parser depParser(std::move(depLexed.tokens), "dep.rux");
    auto depParsed = depParser.Parse();
    REQUIRE_FALSE(depParsed.HasErrors());

    Lexer lexer(userSource, "test.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "test.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    DepPackage dep;
    dep.name = depName;
    dep.modules.push_back({depName, &depParsed.module});

    SemanticAnalyzer analyzer({&parsed.module}, {std::move(dep)}, "App", "Windows");
    analyzer.SetOptions(options);
    return analyzer.Analyze().diagnostics;
}

template <typename Node>
const TypeRef &ResolvedType(const SemanticModel &model, const Node &node) {
    const TypeRef *type = model.TryGetType(node);
    REQUIRE(type != nullptr);
    return *type;
}

} // namespace Rux::Testing::SemanticTestSupport
