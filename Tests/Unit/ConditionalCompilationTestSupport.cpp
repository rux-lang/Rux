#include "ConditionalCompilationTestSupport.h"

#include "Lexer/Lexer.h"
#include "Semantic/SemanticAnalyzer.h"

#include <doctest.h>
#include <utility>
#include <vector>

namespace Rux::Testing {
namespace {
// A stand-in intrinsics package so `import Core::{...}` resolves in these tests. The
// fold uses its own built-in variant tables, so the enum bodies here only need
// to exist, not to be complete.
constexpr std::string_view kCorePackageSource = R"(
struct Slice<T> { data: *T; length: uint; }
struct Target {}
struct Build {}
struct Compiler {}
struct SemanticVersion {
    major: uint;
    minor: uint;
    patch: uint;
}
extend SemanticVersion {
    func New(major: uint, minor: uint, patch: uint) -> SemanticVersion {
        return SemanticVersion { major: major, minor: minor, patch: patch };
    }
}
struct Source {}
struct Config {}
intrinsic #target: Target;
intrinsic #build: Build;
intrinsic #compiler: Compiler;
intrinsic #source: Source;
intrinsic #config: Config;
intrinsic func #Error(message: Slice<char8>);
intrinsic func #Warn(message: Slice<char8>);
enum OperatingSystem { Windows }
enum Architecture { X86_64 }
enum ApplicationBinaryInterface { WindowsX64 }
enum Endianness { Little }
enum DataModel { LLP64 }
enum ObjectFormat { COFF }
enum BuildMode { Debug }
enum OptimizationMode { Speed }
enum OutputKind { SharedLibrary }
)";

} // namespace

DepPackage ConditionalCoreDependency(ParseResult &storage) {
    storage = ParseConditionalSource(std::string(kCorePackageSource));
    DepPackage dependency;
    dependency.name = "Core";
    dependency.modules.push_back({"Core", &storage.module});
    return dependency;
}

ParseResult ParseConditionalSource(const std::string &source) {
    Lexer lexer(source, "test.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());

    Parser parser(std::move(lexed.tokens), "test.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    return parsed;
}

SemanticModel AnalyzeConditionalModule(Module &module, const std::string &targetSystem) {
    ParseResult core;
    std::vector<Module *> modules = {&module};
    SemanticAnalyzer analyzer(modules, {ConditionalCoreDependency(core)}, "test", targetSystem);
    return analyzer.Analyze();
}

SemanticModel AnalyzeConditionalModule(Module &module, CompileTimeContext context) {
    ParseResult core;
    std::vector<Module *> modules = {&module};
    SemanticAnalyzer analyzer(modules, {ConditionalCoreDependency(core)}, "test", std::move(context));
    return analyzer.Analyze();
}

SemanticModel AnalyzeConditionalModuleWithoutDependencies(Module &module, CompileTimeContext context) {
    std::vector<Module *> modules = {&module};
    SemanticAnalyzer analyzer(modules, {}, "test", std::move(context));
    return analyzer.Analyze();
}

const FuncDecl *FindConditionalFunc(const Module &module, const std::string_view name) {
    for (const auto &item : module.items) {
        const auto *func = dynamic_cast<const FuncDecl *>(item.get());
        if (func && func->name == name) {
            return func;
        }
    }
    return nullptr;
}

const ExternBlockDecl *FindConditionalExternBlock(const Module &module) {
    for (const auto &item : module.items) {
        if (const auto *block = dynamic_cast<const ExternBlockDecl *>(item.get())) {
            return block;
        }
    }
    return nullptr;
}

std::string ConditionalReturnedLiteral(const FuncDecl &func) {
    REQUIRE(func.body != nullptr);
    REQUIRE(func.body->stmts.size() == 1);
    const auto *ret = dynamic_cast<const ReturnStmt *>(func.body->stmts[0].get());
    REQUIRE(ret != nullptr);
    REQUIRE(ret->value.has_value());
    const auto *literal = dynamic_cast<const LiteralExpr *>(ret->value->get());
    REQUIRE(literal != nullptr);
    return literal->token.text;
}
} // namespace Rux::Testing
