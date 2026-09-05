#include "ConditionalCompilationTestSupport.h"

#include "Lexer/Lexer.h"
#include "Semantic/SemanticAnalyzer.h"

#include <doctest.h>
#include <utility>
#include <vector>

namespace Rux::Testing {
namespace {
// A stand-in intrinsics package so `import Core::{...}` resolves in these tests. The
// fold resolves these declarations and their enum variants just like any other provider.
constexpr std::string_view kCorePackageSource = R"(
pub intrinsic struct Slice<T> { pub data: *T; pub length: uint; }
pub intrinsic struct string8 { pub data: *char8; pub length: uint; }
pub type string = string8;
pub struct Target {}
pub struct Build {}
pub struct Compiler {}
pub struct SemanticVersion {
    pub major: uint;
    pub minor: uint;
    pub patch: uint;
}
extend SemanticVersion {
    pub func SemanticVersion(major: uint, minor: uint, patch: uint) -> SemanticVersion {
        return SemanticVersion { major: major, minor: minor, patch: patch };
    }
}
pub struct Source {}
pub struct Config {}
pub intrinsic #target: Target;
pub intrinsic #build: Build;
pub intrinsic #compiler: Compiler;
pub intrinsic #source: Source;
pub intrinsic #config: Config;
pub intrinsic func #Error(message: Slice<char8>);
pub intrinsic func #Warn(message: Slice<char8>);
pub enum OperatingSystem { FreeBSD, Linux, macOS, Windows }
pub enum Architecture { AArch64, X86_64 }
pub enum ApplicationBinaryInterface { AAPCS64, SystemV, WindowsX64 }
pub enum Endianness { Big, Little }
pub enum DataModel { LLP64, LP64 }
pub enum ObjectFormat { COFF, ELF, MachO }
pub enum BuildMode { Debug, Release }
pub enum OptimizationMode { None, Size, Speed }
pub enum OutputKind { Executable, SharedLibrary, StaticLibrary, SourceLibrary }
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
