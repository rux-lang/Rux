// The entry point of a test build: a generated `Main` that runs every `#Test()` function of the package.
// The tests are found in the parsed sources, before anything is resolved, so the runner is ordinary Rux source that
// the rest of the pipeline compiles like any other file: a table of `Test::TestCase` values handed to
// `Test::RunTests`. A test that returns nothing gets an adapter that returns `! TestFailure`, so every entry of the
// table has the same function type.

#include "Diagnostics/Diagnostics.h"
#include "Driver/CompileContext.h"
#include "Lexer/Lexer.h"
#include "Syntax/Ast/Ast.h"
#include "Syntax/Parser/Parser.h"

#include <format>
#include <string>
#include <utility>
#include <vector>

namespace Rux::Driver {

namespace {

struct DiscoveredTest {
    std::string name;      ///< the name the runner reports, with its module path
    std::string reference; ///< how the generated source names the function
    bool returnsNothing = false;
};

/// Whether `type` is `! TestFailure` or `! Test::TestFailure`.
[[nodiscard]] bool IsTestFailureResult(const TypeExpr &type) {
    const auto *fallible = dynamic_cast<const FallibleTypeExpr *>(&type);
    if (fallible == nullptr || fallible->success != nullptr || fallible->error == nullptr) {
        return false;
    }
    if (const auto *named = dynamic_cast<const NamedTypeExpr *>(fallible->error.get())) {
        return named->name == "TestFailure" && named->typeArgs.empty();
    }
    if (const auto *path = dynamic_cast<const PathTypeExpr *>(fallible->error.get())) {
        return path->segments.size() == 2 && path->segments[0] == "Test" && path->segments[1] == "TestFailure";
    }
    return false;
}

struct TestCollector {
    std::string sourceName;
    std::vector<DiscoveredTest> &tests;
    std::vector<Diagnostic> &problems;

    void Problem(const SourceLocation &location, std::string message, std::string help = {}) const {
        Diagnostic diagnostic;
        diagnostic.severity = Diagnostic::Severity::Error;
        diagnostic.message = std::move(message);
        diagnostic.sourceName = sourceName;
        diagnostic.location = location;
        if (!help.empty()) {
            diagnostic.help = std::move(help);
        }
        problems.push_back(std::move(diagnostic));
    }

    void Function(const FuncDecl &function, const std::vector<std::string> &modulePath) const {
        if (!function.isTest) {
            return;
        }
        if (function.returnType && !IsTestFailureResult(**function.returnType)) {
            Problem(function.location,
                    std::format("'#Test' function '{}' must return nothing or '! TestFailure'", function.name));
            return;
        }
        DiscoveredTest test;
        for (const auto &segment : modulePath) {
            test.name += segment + "::";
        }
        test.name += function.name;
        test.reference = test.name;
        test.returnsNothing = !function.returnType.has_value();
        tests.push_back(std::move(test));
    }

    void Items(const std::vector<DeclPtr> &items, std::vector<std::string> &modulePath) const {
        for (const auto &item : items) {
            if (const auto *function = dynamic_cast<const FuncDecl *>(item.get())) {
                Function(*function, modulePath);
            }
            else if (const auto *module = dynamic_cast<const ModuleDecl *>(item.get())) {
                modulePath.push_back(module->name);
                Items(module->items, modulePath);
                modulePath.pop_back();
            }
            else if (const auto *extension = dynamic_cast<const ImplDecl *>(item.get())) {
                for (const auto &method : extension->methods) {
                    if (method->isTest) {
                        Problem(method->location, "a '#Test' function must be declared outside 'extend' blocks");
                    }
                }
            }
            else if (const auto *conditional = dynamic_cast<const WhenDecl *>(item.get())) {
                for (const auto &branch : conditional->branches) {
                    for (const auto &nested : branch.items) {
                        const auto *branchFunction = dynamic_cast<const FuncDecl *>(nested.get());
                        if (branchFunction != nullptr && branchFunction->isTest) {
                            Problem(branchFunction->location, "a '#Test' function cannot be declared inside 'when'",
                                    "use '#build' or '#target' checks inside the test body instead");
                        }
                    }
                }
            }
        }
    }
};

/// Escapes `text` for a Rux string literal.
[[nodiscard]] std::string Quoted(const std::string &text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    out += '"';
    return out;
}

[[nodiscard]] bool DeclaresMain(const std::vector<ParseResult> &modules) {
    for (const auto &result : modules) {
        for (const auto &item : result.module.items) {
            const auto *function = dynamic_cast<const FuncDecl *>(item.get());
            if (function != nullptr && function->name == "Main") {
                return true;
            }
        }
    }
    return false;
}

} // namespace

bool CompilerDriver::Impl::SynthesizeTestEntry() {
    std::vector<DiscoveredTest> tests;
    std::vector<Diagnostic> problems;
    for (const auto &result : parseResults) {
        std::vector<std::string> modulePath;
        const TestCollector collector{result.module.name, tests, problems};
        collector.Items(result.module.items, modulePath);
    }
    if (EmitAll(problems)) {
        return false;
    }
    const auto packageName = opts.manifest.package.name.Text();
    if (tests.empty()) {
        if (opts.manifest.package.type != ManifestPackageType::Executable) {
            Emit(ErrorDiagnostic(std::format("package '{}' has no '#Test()' functions to run", packageName), {},
                                 "mark a function '#Test()', or build the package that depends on this one"));
            return false;
        }
        return true;
    }
    if (DeclaresMain(parseResults)) {
        Emit(ErrorDiagnostic(
            std::format("package '{}' declares 'Main' and '#Test()' functions; a test build generates its own 'Main'",
                        packageName),
            {}, "move the '#Test()' functions into a package without 'Main', or remove 'Main'"));
        return false;
    }
    bool hasTestPackage = packageName == "Test";
    for (const auto &dependency : opts.manifest.dependencies) {
        hasTestPackage |= dependency.importName.Text() == "Test";
    }
    if (!hasTestPackage) {
        Emit(ErrorDiagnostic(
            std::format("package '{}' has '#Test()' functions but does not depend on 'Test'", packageName),
            {"the generated runner uses 'Test::RunTests'"},
            "add 'Test = { Namespace = \"Rux\", Version = \"0.1.0\" }' to [Dependencies]"));
        return false;
    }

    std::string source = "// Generated by a test build: runs every '#Test()' function of the package.\n"
                         "import Test::{ RunTests, TestCase, TestFailure };\n\n";
    std::vector<std::string> runners;
    for (std::size_t index = 0; index < tests.size(); ++index) {
        const auto &test = tests[index];
        if (!test.returnsNothing) {
            runners.push_back(test.reference);
            continue;
        }
        const auto adapter = std::format("RuxTestAdapter{}", index);
        source += std::format("func {}() -> ! TestFailure {{\n    {}();\n}}\n\n", adapter, test.reference);
        runners.push_back(adapter);
    }
    source += "func Main() -> int {\n    let tests = [\n";
    for (std::size_t index = 0; index < tests.size(); ++index) {
        source += std::format("        TestCase {{ name: {}, run: {} }}{}\n", Quoted(tests[index].name), runners[index],
                              index + 1 < tests.size() ? "," : "");
    }
    source += "    ];\n    return RunTests(tests[..]);\n}\n";

    std::vector<SourceFile> generated;
    generated.push_back(SourceFile{root / "Src" / "RuxTestMain.rux", std::move(source)});
    const auto views = RememberSources(generated);
    const auto &view = views.front();
    auto lexResult = Lexer::TokenizeSource(view.source.Text(), view.path.string());
    if (EmitAll(lexResult.diagnostics)) {
        return false;
    }
    Parser parser(std::move(lexResult.tokens), view.path.string(), compileTimeContext.target.arch);
    auto parseResult = parser.Parse();
    if (EmitAll(parseResult.diagnostics) || parseResult.HasErrors()) {
        return false;
    }
    parseResults.push_back(std::move(parseResult));
    return true;
}

} // namespace Rux::Driver
