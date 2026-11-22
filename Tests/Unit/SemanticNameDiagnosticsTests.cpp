#include "Lexer/Lexer.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <doctest.h>
#include <string>
#include <utility>
#include <vector>

using namespace Rux;

namespace {
std::vector<SemanticDiagnostic> AnalyzeSource(const std::string &source) {
    Lexer lexer(source, "test.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "test.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    return analyzer.Analyze().diagnostics;
}

std::vector<SemanticDiagnostic>
AnalyzeWithDeps(const std::string &userSource,
                const std::vector<std::pair<std::string, std::string>> &dependencySources) {
    std::vector<ParseResult> parsedDependencies;
    parsedDependencies.reserve(dependencySources.size());
    std::vector<DepPackage> dependencies;
    dependencies.reserve(dependencySources.size());
    for (const auto &[name, source] : dependencySources) {
        Lexer dependencyLexer(source, name + ".rux");
        auto dependencyTokens = dependencyLexer.Tokenize();
        REQUIRE_FALSE(dependencyTokens.HasErrors());
        Parser dependencyParser(std::move(dependencyTokens.tokens), name + ".rux");
        parsedDependencies.push_back(dependencyParser.Parse());
        REQUIRE_FALSE(parsedDependencies.back().HasErrors());
        DepPackage dependency;
        dependency.name = name;
        dependency.modules.push_back({name, &parsedDependencies.back().module});
        dependencies.push_back(std::move(dependency));
    }

    Lexer lexer(userSource, "test.rux");
    auto tokens = lexer.Tokenize();
    REQUIRE_FALSE(tokens.HasErrors());
    Parser parser(std::move(tokens.tokens), "test.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, std::move(dependencies), "App", "Windows");
    return analyzer.Analyze().diagnostics;
}
} // namespace

TEST_CASE("semantic name diagnostics distinguish missing names and deterministic near matches") {
    const auto diagnostics = AnalyzeSource(R"(
        func Main() {
            let count = 1;
            let cat = 2;
            let cut = 3;
            coutn;
            cot;
            absent;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 3);
    CHECK_EQ(diagnostics[0].message, "name 'coutn' is not defined in this scope");
    REQUIRE(diagnostics[0].help.has_value());
    CHECK_EQ(*diagnostics[0].help, "did you mean 'count'?");
    CHECK_EQ(diagnostics[1].message, "name 'cot' is not defined in this scope");
    REQUIRE(diagnostics[1].help.has_value());
    CHECK_EQ(*diagnostics[1].help, "did you mean 'cat'?");
    CHECK_EQ(diagnostics[2].message, "name 'absent' is not defined in this scope");
    CHECK_FALSE(diagnostics[2].help.has_value());
}

TEST_CASE("a short name is offered only a one-edit near match") {
    // 'note' is two edits from 'byte', which is no likely typo of it; a name of four characters is offered one edit.
    const auto diagnostics = AnalyzeSource(R"(
        func Main() {
            let byte = 1;
            let item = 2;
            note;
            itme;
            iten;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 3);
    CHECK_EQ(diagnostics[0].message, "name 'note' is not defined in this scope");
    CHECK_FALSE(diagnostics[0].help.has_value());
    // Two neighbouring characters swapped are one edit.
    CHECK_EQ(diagnostics[1].help, "did you mean 'item'?");
    CHECK_EQ(diagnostics[2].help, "did you mean 'item'?");
}

TEST_CASE("semantic declaration diagnostics identify duplicate and conflicting kinds") {
    const auto diagnostics = AnalyzeSource(R"(
        const Answer = 1;
        struct Answer {}

        func Main(value: int, value: int) {
            let local = 1;
            let local = 2;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 3);
    CHECK_EQ(diagnostics[0].message,
             "name 'Answer' cannot be declared as a type because it is already a constant in this scope");
    REQUIRE_EQ(diagnostics[0].notes.size(), 1);
    CHECK(diagnostics[0].notes[0].contains("'Answer' was declared as a constant at 'test.rux':2:"));
    CHECK_EQ(diagnostics[1].message, "variable 'value' is already declared in this scope");
    REQUIRE_EQ(diagnostics[1].notes.size(), 1);
    CHECK(diagnostics[1].notes[0].contains("'value' was declared as a variable at 'test.rux':5:"));
    CHECK_EQ(diagnostics[2].message, "variable 'local' is already declared in this scope");
    REQUIRE_EQ(diagnostics[2].notes.size(), 1);
    CHECK(diagnostics[2].notes[0].contains("'local' was declared as a variable at 'test.rux':6:"));
}

TEST_CASE("nested declarations may shadow names from an outer scope") {
    const auto diagnostics = AnalyzeSource(R"(
        const value = 1;
        func Main(value: int) {
            if true {
                let value = 2;
                value;
            }
        }
    )");

    CHECK(diagnostics.empty());
}

TEST_CASE("semantic type names distinguish missing names from wrong symbol kinds") {
    const auto diagnostics = AnalyzeSource(R"(
        struct Counter {}
        func Factory() {}
        struct Holder {
            near: Counetr;
            wrong: Factory;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 2);
    CHECK_EQ(diagnostics[0].message, "type 'Counetr' is not defined in this scope");
    REQUIRE(diagnostics[0].help.has_value());
    CHECK_EQ(*diagnostics[0].help, "did you mean 'Counter'?");
    CHECK_EQ(diagnostics[1].message, "name 'Factory' is a function, not a type");
    REQUIRE_EQ(diagnostics[1].notes.size(), 1);
    CHECK(diagnostics[1].notes[0].contains("'Factory' was declared as a function"));
}

TEST_CASE("semantic module paths report wrong-kind owners with declaration context") {
    const auto diagnostics = AnalyzeSource(R"(
        const Config = 1;
        func Main() { Config::Value; }
    )");

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics[0].message, "name 'Config' is a constant, not a module");
    REQUIRE_EQ(diagnostics[0].notes.size(), 1);
    CHECK(diagnostics[0].notes[0].contains("'Config' was declared as a constant"));
}

TEST_CASE("interface implementations distinguish missing interfaces, wrong kinds, and requirements") {
    const auto diagnostics = AnalyzeSource(R"(
        struct Widget {}
        const Contract = 1;
        interface Drawable {
            func Draw(*self);
        }

        extend Widget : Missing {}
        extend Widget : Contract {}
        extend Widget : Drawable {}
        extend Widgte {}
    )");

    REQUIRE_EQ(diagnostics.size(), 4);
    CHECK_EQ(diagnostics[0].message, "interface 'Missing' is not defined");
    CHECK_EQ(diagnostics[1].message, "name 'Contract' is a constant, not an interface");
    REQUIRE_EQ(diagnostics[1].notes.size(), 1);
    CHECK(diagnostics[1].notes[0].contains("'Contract' was declared as a constant"));
    CHECK_EQ(diagnostics[2].message,
             "implementation of interface 'Drawable' for type 'Widget' is missing method 'Draw'");
    REQUIRE_EQ(diagnostics[2].notes.size(), 1);
    CHECK_EQ(diagnostics[2].notes[0], "interface 'Drawable' requires method 'Draw'");
    CHECK_EQ(diagnostics[3].message, "cannot extend type 'Widgte' because it is not defined");
    REQUIRE(diagnostics[3].help.has_value());
    CHECK_EQ(*diagnostics[3].help, "did you mean 'Widget'?");
}

TEST_CASE("ambiguous module imports list candidate packages deterministically") {
    const auto diagnostics = AnalyzeWithDeps("import Shared::Value;", {{"Zulu", "module Shared { func Value() {} }"},
                                                                       {"Alpha", "module Shared { func Value() {} }"}});

    // Reported once, although the import is bound both before and after signatures are resolved.
    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK_EQ(diagnostics[0].message, "module 'Shared' is ambiguous");
    REQUIRE_EQ(diagnostics[0].notes.size(), 2);
    CHECK_EQ(diagnostics[0].notes[0], "module 'Shared' is available from package 'Alpha'");
    CHECK_EQ(diagnostics[0].notes[1], "module 'Shared' is available from package 'Zulu'");
    REQUIRE(diagnostics[0].help.has_value());
    CHECK_EQ(*diagnostics[0].help, "qualify the import with one of the listed package names");
}

namespace {
// Analyzes `source` as the root package of an executable build, which holds `Main` to an entry point's forms.
std::vector<SemanticDiagnostic> AnalyzeEntryPoint(const std::string &source) {
    Lexer lexer(source, "main.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "main.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    CompileTimeContext context;
    context.checksEntryPoint = true;
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", context);
    return analyzer.Analyze().diagnostics;
}
} // namespace

TEST_CASE("an executable's Main is func Main() -> int") {
    CHECK(AnalyzeEntryPoint("func Main() -> int { return 0; }").empty());
    for (const std::string refused :
         {"func Main(count: int) -> int { return count; }", "func Main() -> bool { return true; }",
          "func Main() -> float64 { return 0.0; }", "func Main<T>() -> int { return 0; }", "func Main() {}",
          "func Main() -> int32 { return 0; }", "variant E { Bad }\nfunc Main() -> !E {}",
          "variant E { Bad }\nfunc Main() -> int ! E { return 0; }"}) {
        CAPTURE(refused);
        const auto diagnostics = AnalyzeEntryPoint(refused);
        REQUIRE_EQ(diagnostics.size(), 1);
        CHECK(diagnostics.front().message.starts_with("'Main' must be 'func Main() -> int', but it"));
    }
    // A Main inside a module is an ordinary function.
    CHECK(AnalyzeEntryPoint(
              "module Tools {\n func Main(x: int) -> bool { return true; }\n}\nfunc Main() -> int { return 0; }")
              .empty());
}

TEST_CASE("an error that would leave Main is handled there, never declared on it") {
    // `Main` cannot fail, so the help never offers a fallible signature for it.
    const auto passed = AnalyzeEntryPoint(R"(
        enum InstallError: int32 { Failed = 1 }
        func Install() -> !InstallError {}
        func Main() -> int {
            Install()!;
            return 0;
        }
    )");
    REQUIRE_EQ(passed.size(), 1);
    CHECK_EQ(passed[0].message, "call to 'Install' can fail with 'InstallError', but 'Main' cannot fail");
    CHECK_EQ(passed[0].help, "handle the error here, as in '!! return 1', or move the steps that can fail into a "
                             "function and handle its error in 'Main', as in 'return Run() !! 1;'");

    const auto thrown = AnalyzeEntryPoint(R"(
        enum LookupError: int32 { Missing = 1 }
        func Find() -> int? { return none; }
        func Main() -> int {
            let found = Find() ?? throw LookupError::Missing;
            return found;
        }
    )");
    REQUIRE_EQ(thrown.size(), 1);
    CHECK_EQ(thrown[0].message, "'throw' needs a function that can fail, but 'Main' returns 'int'");
    CHECK_EQ(thrown[0].help, "leave 'Main' with an exit status, as in 'return 1;', or move the steps that can fail "
                             "into a function and handle its error in 'Main', as in 'return Run() !! 1;'");

    // Any other function is offered the fallible signature.
    const auto ordinary = AnalyzeEntryPoint(R"(
        enum InstallError: int32 { Failed = 1 }
        func Install() -> !InstallError {}
        func Setup() -> int {
            Install()!;
            return 0;
        }
        func Main() -> int { return Setup(); }
    )");
    REQUIRE_EQ(ordinary.size(), 1);
    CHECK_EQ(ordinary[0].help, "handle the error with '!!' or 'catch { ... }', or declare '-> int ! InstallError' on "
                               "'Setup'");
}
