// `#if` conditional compilation: the taken branch is spliced into its parent
// and the others are dropped before anything type-checks them.

#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Ast/Ast.h"
#include "Syntax/Parser/Parser.h"

#include <doctest.h>
#include <string>
#include <unordered_map>
#include <vector>

using namespace Rux;

namespace {

ParseResult ParseSource(const std::string &source) {
    Lexer lexer(source, "test.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());

    Parser parser(std::move(lexed.tokens), "test.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    return parsed;
}

// Runs the front end the way the driver does, which folds `#if` in `module`.
SemanticModel Analyze(Module &module, const std::string &targetSystem = "Windows") {
    std::vector<Module *> modules = {&module};
    SemanticAnalyzer analyzer(modules, {}, "test", targetSystem);
    return analyzer.Analyze();
}

SemanticModel Analyze(Module &module, CompileTimeContext context) {
    std::vector<Module *> modules = {&module};
    SemanticAnalyzer analyzer(modules, {}, "test", std::move(context));
    return analyzer.Analyze();
}

const FuncDecl *FindFunc(const Module &module, const std::string_view name) {
    for (const auto &item : module.items) {
        const auto *func = dynamic_cast<const FuncDecl *>(item.get());
        if (func && func->name == name) {
            return func;
        }
    }
    return nullptr;
}

// The integer literal a single-statement `return <int>;` body returns.
std::string ReturnedLiteral(const FuncDecl &func) {
    REQUIRE(func.body != nullptr);
    REQUIRE(func.body->stmts.size() == 1);
    const auto *ret = dynamic_cast<const ReturnStmt *>(func.body->stmts[0].get());
    REQUIRE(ret != nullptr);
    REQUIRE(ret->value.has_value());
    const auto *literal = dynamic_cast<const LiteralExpr *>(ret->value->get());
    REQUIRE(literal != nullptr);
    return literal->token.text;
}

} // namespace

TEST_CASE("a #if statement keeps only the taken branch") {
    auto parsed = ParseSource(R"(
const Debug = true;

func Do() -> int {
    #if Debug {
        return 1;
    } else {
        return 0;
    }
}
)");
    const auto model = Analyze(parsed.module);
    CHECK_FALSE(model.HasErrors());

    const auto *func = FindFunc(parsed.module, "Do");
    REQUIRE(func != nullptr);
    CHECK(ReturnedLiteral(*func) == "1");
}

TEST_CASE("a #if statement that is false keeps the else branch") {
    auto parsed = ParseSource(R"(
const Debug = false;

func Do() -> int {
    #if Debug {
        return 1;
    } else {
        return 0;
    }
}
)");
    const auto model = Analyze(parsed.module);
    CHECK_FALSE(model.HasErrors());

    const auto *func = FindFunc(parsed.module, "Do");
    REQUIRE(func != nullptr);
    CHECK(ReturnedLiteral(*func) == "0");
}

TEST_CASE("a branch that is not taken is never type-checked") {
    auto parsed = ParseSource(R"(
const Debug = false;

func Do() -> int {
    #if Debug {
        return NoSuchFunction(NoSuchVariable);
    } else {
        return 7;
    }
}
)");
    const auto model = Analyze(parsed.module);
    CHECK_FALSE(model.HasErrors());

    const auto *func = FindFunc(parsed.module, "Do");
    REQUIRE(func != nullptr);
    CHECK(ReturnedLiteral(*func) == "7");
}

TEST_CASE("a #if statement introduces no scope of its own") {
    auto parsed = ParseSource(R"(
const Debug = true;

func Do() -> int {
    #if Debug {
        let value = 5;
    }
    return value;
}
)");
    const auto model = Analyze(parsed.module);
    CHECK_FALSE(model.HasErrors());

    const auto *func = FindFunc(parsed.module, "Do");
    REQUIRE(func != nullptr);
    REQUIRE(func->body->stmts.size() == 2); // the `let` was spliced in, not nested
    CHECK(dynamic_cast<const LetStmt *>(func->body->stmts[0].get()) != nullptr);
}

TEST_CASE("an else-if chain picks the first true branch") {
    auto parsed = ParseSource(R"(
const Level = 3;

func Do() -> int {
    #if Level > 3 {
        return 1;
    } else if Level == 3 {
        return 2;
    } else {
        return 3;
    }
}
)");
    const auto model = Analyze(parsed.module);
    CHECK_FALSE(model.HasErrors());

    const auto *func = FindFunc(parsed.module, "Do");
    REQUIRE(func != nullptr);
    CHECK(ReturnedLiteral(*func) == "2");
}

TEST_CASE("#if selects declarations, not just statements") {
    auto parsed = ParseSource(R"(
const Debug = false;

#if Debug {
    func Tag() -> int { return 1; }
} else {
    func Tag() -> int { return 2; }
}
)");
    const auto model = Analyze(parsed.module);
    CHECK_FALSE(model.HasErrors());

    const auto *func = FindFunc(parsed.module, "Tag");
    REQUIRE(func != nullptr);
    CHECK(ReturnedLiteral(*func) == "2");

    // The conditional itself is gone: only the surviving declarations remain.
    for (const auto &item : parsed.module.items) {
        CHECK(dynamic_cast<const CompileTimeIfDecl *>(item.get()) == nullptr);
    }
}

TEST_CASE("#if selects methods inside an extend block") {
    auto parsed = ParseSource(R"(
struct Animal {}

extend Animal {
    #if target.os == .Windows {
        func Sound(self) -> int { return 1; }
    } else {
        func Sound(self) -> int { return 2; }
    }

    func Legs(self) -> int { return 4; }
}
)");
    const auto model = Analyze(parsed.module, "Linux");
    CHECK_FALSE(model.HasErrors());

    const ImplDecl *impl = nullptr;
    for (const auto &item : parsed.module.items) {
        if (const auto *candidate = dynamic_cast<const ImplDecl *>(item.get())) {
            impl = candidate;
        }
    }
    REQUIRE(impl != nullptr);
    // The taken branch's method joined the unconditional one, and the `#if` is
    // gone: `extend` holds nothing but methods by the time analysis runs.
    CHECK(impl->conditionals.empty());
    REQUIRE(impl->methods.size() == 2);

    const FuncDecl *sound = nullptr;
    for (const auto &method : impl->methods) {
        if (method->name == "Sound") {
            sound = method.get();
        }
    }
    REQUIRE(sound != nullptr);
    CHECK(ReturnedLiteral(*sound) == "2");
}

TEST_CASE("#if tests the target OS through target.os") {
    const std::string source = R"(
func Do() -> int {
    #if target.os == .Windows {
        return 1;
    } else {
        return 2;
    }
}
)";

    auto windows = ParseSource(source);
    CHECK_FALSE(Analyze(windows.module, "Windows").HasErrors());
    const auto *windowsFunc = FindFunc(windows.module, "Do");
    REQUIRE(windowsFunc != nullptr);
    CHECK(ReturnedLiteral(*windowsFunc) == "1");

    auto linux = ParseSource(source);
    CHECK_FALSE(Analyze(linux.module, "Linux").HasErrors());
    const auto *linuxFunc = FindFunc(linux.module, "Do");
    REQUIRE(linuxFunc != nullptr);
    CHECK(ReturnedLiteral(*linuxFunc) == "2");
}

TEST_CASE("target.os compares against the long form of OperatingSystem too") {
    auto parsed = ParseSource(R"(
func Do() -> int {
    #if target.os != OperatingSystem::Linux {
        return 1;
    } else {
        return 2;
    }
}
)");
    CHECK_FALSE(Analyze(parsed.module, "Windows").HasErrors());

    const auto *func = FindFunc(parsed.module, "Do");
    REQUIRE(func != nullptr);
    CHECK(ReturnedLiteral(*func) == "1");
}

TEST_CASE("the shorthand works for the program's own enums") {
    auto parsed = ParseSource(R"(
enum Mode { Fast, Small }

const Build = Mode::Small;

func Do() -> int {
    #if Build == .Fast {
        return 1;
    } else {
        return 2;
    }
}
)");
    CHECK_FALSE(Analyze(parsed.module).HasErrors());

    const auto *func = FindFunc(parsed.module, "Do");
    REQUIRE(func != nullptr);
    CHECK(ReturnedLiteral(*func) == "2");
}

TEST_CASE("target.os tells the BSDs apart") {
    const std::string source = R"(
func Do() -> int {
    #if target.os == .FreeBSD {
        return 1;
    } else if target.os == .OpenBSD {
        return 2;
    } else if target.os == .DragonFlyBSD {
        return 3;
    } else {
        return 4;
    }
}
)";

    auto freeBsd = ParseSource(source);
    CHECK_FALSE(Analyze(freeBsd.module, "FreeBSD").HasErrors());
    CHECK(ReturnedLiteral(*FindFunc(freeBsd.module, "Do")) == "1");

    auto openBsd = ParseSource(source);
    CHECK_FALSE(Analyze(openBsd.module, "OpenBSD").HasErrors());
    CHECK(ReturnedLiteral(*FindFunc(openBsd.module, "Do")) == "2");

    // The host spelling of DragonFly BSD is "Dragonfly"; the variant is not.
    auto dragonFly = ParseSource(source);
    CHECK_FALSE(Analyze(dragonFly.module, "Dragonfly").HasErrors());
    CHECK(ReturnedLiteral(*FindFunc(dragonFly.module, "Do")) == "3");

    auto netBsd = ParseSource(source);
    CHECK_FALSE(Analyze(netBsd.module, "NetBSD").HasErrors());
    CHECK(ReturnedLiteral(*FindFunc(netBsd.module, "Do")) == "4");
}

TEST_CASE("an OS no build target produces warns instead of quietly never running") {
    auto parsed = ParseSource(R"(
func Do() -> int {
    #if target.os == .Haiku {
        return 1;
    }
    return 0;
}
)");
    const auto model = Analyze(parsed.module);
    CHECK_FALSE(model.HasErrors());
    REQUIRE(model.diagnostics.size() == 1);
    CHECK(model.diagnostics[0].message == "no build target produces '.Haiku', so this branch is never taken");

    const auto *func = FindFunc(parsed.module, "Do");
    REQUIRE(func != nullptr);
    CHECK(ReturnedLiteral(*func) == "0");
}

TEST_CASE("a misspelled OS variant is an error, not a silently false branch") {
    auto parsed = ParseSource(R"(
func Do() -> int {
    #if target.os == .Wndows {
        return 1;
    }
    return 0;
}
)");
    const auto model = Analyze(parsed.module);
    REQUIRE(model.HasErrors());
    CHECK(model.diagnostics[0].message ==
          "'.Wndows' is not a variant of 'OperatingSystem'; the variants are: .AIX, .Android, .DragonFlyBSD, .FreeBSD, "
          ".Fuchsia, "
          ".Haiku, .Illumos, .iOS, .Linux, .MacOS, .NetBSD, .OpenBSD, .QNX, .Redox, .Solaris, .Windows");
}

TEST_CASE("compiler-initialized constants are ordinary expressions outside #if") {
    auto parsed = ParseSource(R"(
struct Target {
    pointerBits: uint;
}

#Intrinsic("Target")
const $target: Target;

func Do() -> int {
    let bits = target.pointerBits;
    return 0;
}
)");
    CompileTimeContext context;
    context.target.pointer_size = 8;
    const auto model = Analyze(parsed.module, std::move(context));
    REQUIRE_FALSE(model.HasErrors());

    AstToHirLowering lowering(model);
    const HirPackage package = lowering.Generate();
    REQUIRE(package.modules.size() == 1);
    REQUIRE(package.modules[0].funcs.size() == 1);
    const auto *let = dynamic_cast<const HirLetStmt *>(package.modules[0].funcs[0].body->stmts[0].get());
    REQUIRE(let != nullptr);
    const auto *literal = dynamic_cast<const HirLiteralExpr *>(let->init.get());
    REQUIRE(literal != nullptr);
    CHECK(literal->value == "64");
}

TEST_CASE("an enum shorthand outside a #if condition is an error") {
    auto parsed = ParseSource(R"(
enum Mode { Fast, Small }

func Do() -> Mode {
    return .Fast;
}
)");
    const auto model = Analyze(parsed.module);
    REQUIRE(model.HasErrors());
    CHECK(model.diagnostics[0].message ==
          "'.Fast' can only be used in a '#if' condition; write the enum out in full, as in 'Enum::Fast'");
}

TEST_CASE("a #if condition that is not a compile-time constant is an error") {
    auto parsed = ParseSource(R"(
func Do() -> int {
    var runtime = 1;
    #if runtime > 0 {
        return 1;
    }
    return 0;
}
)");
    const auto model = Analyze(parsed.module);
    REQUIRE(model.HasErrors());
    CHECK(model.diagnostics[0].message == "'#if' condition must be a compile-time constant expression");
}

TEST_CASE("a #if condition that is not a bool is an error") {
    auto parsed = ParseSource(R"(
const Level = 3;

func Do() -> int {
    #if Level {
        return 1;
    }
    return 0;
}
)");
    const auto model = Analyze(parsed.module);
    REQUIRE(model.HasErrors());
    CHECK(model.diagnostics[0].message == "'#if' condition must be of type 'bool'");
}

TEST_CASE("target and build intrinsics expose the full compile-time context") {
    auto parsed = ParseSource(R"(
func Selected() -> int {
    #if target.os == .Windows &&
        target.arch == .X86_64 &&
        target.abi == .WindowsX64 &&
        target.endian == .Little &&
        target.pointerBits == 64 &&
        target.dataModel == .LLP64 &&
        target.objectFormat == .COFF &&
        target.triple == "windows-x64" &&
        target.hasFeature(.AVX2) &&
        build.profile == "Production" &&
        build.mode == .Release &&
        build.optimization == .Speed &&
        !build.debugAssertions &&
        build.debugInfo &&
        build.isTest &&
        build.outputKind == .SharedLibrary {
        return 1;
    } else {
        return 0;
    }
}
)");

    CompileTimeContext context;
    context.target.os = Target::OS::Windows;
    context.target.arch = Target::Arch::X86_64;
    context.target.abi = Target::ABI::WindowsX64;
    context.target.endianness = Target::Endian::Little;
    context.target.pointer_size = 8;
    context.target.data_model = Target::DataModel::LLP64;
    context.target.object_format = Target::ObjectFormat::COFF;
    context.target.cpu_features = Target::CpuFeature::AVX2;
    context.targetTriple = "windows-x64";
    context.profileName = "Production";
    context.buildMode = Target::BuildMode::Release;
    context.optimization = OptimizationMode::Speed;
    context.debugAssertions = false;
    context.debugInfo = true;
    context.isTest = true;
    context.outputKind = OutputKind::SharedLibrary;

    const auto model = Analyze(parsed.module, std::move(context));
    CHECK_FALSE(model.HasErrors());
    CHECK(ReturnedLiteral(*FindFunc(parsed.module, "Selected")) == "1");
}

TEST_CASE("configuration and compiler feature intrinsics are queryable") {
    auto parsed = ParseSource(R"(
func Selected() -> int {
    #if config.has("sqlite") &&
        config.get("allocator") == "mimalloc" &&
        compiler.hasFeature("conditional-compilation") &&
        !compiler.hasFeature("imaginary-feature") &&
        compiler.version == "1.2.3" &&
        source.function == "Selected" &&
        source.module == "test" {
        return 1;
    } else {
        return 0;
    }
}
)");
    CompileTimeContext context;
    context.config["sqlite"] = "true";
    context.config["allocator"] = "mimalloc";
    context.compilerVersion = "1.2.3";

    const auto model = Analyze(parsed.module, std::move(context));
    CHECK_FALSE(model.HasErrors());
    CHECK(ReturnedLiteral(*FindFunc(parsed.module, "Selected")) == "1");
}

TEST_CASE("flat intrinsic aliases are rejected") {
    Lexer lexer(R"(
func Selected() -> int {
    return #line;
}
)",
                "test.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());

    Parser parser(std::move(lexed.tokens), "test.rux");
    const auto parsed = parser.Parse();
    CHECK(parsed.HasErrors());
}

TEST_CASE("#Intrinsic declares a compiler-initialized ordinary constant") {
    auto parsed = ParseSource(R"(
struct Target { pointerBits: uint; }

#Intrinsic("Target")
const $target: Target;
)");
    REQUIRE(parsed.module.items.size() == 2);
    const auto *decl = dynamic_cast<const ConstDecl *>(parsed.module.items[1].get());
    REQUIRE(decl != nullptr);
    CHECK(decl->name == "target");
    CHECK(decl->intrinsicName == "Target");
    CHECK(decl->isCompilerInitialized);
    CHECK(decl->value == nullptr);
}

TEST_CASE("ordinary intrinsic expressions lower to context literals") {
    auto parsed = ParseSource(R"(
enum TargetFeature { SSE2, SSE3, SSSE3, SSE41, SSE42, AVX, AVX2, AVX512, NEON, SVE, RVV }

struct Target {
    pointerBits: uint;
    triple: char8[];
}

extend Target {
    #Intrinsic("Target.HasFeature")
    func hasFeature(self, feature: TargetFeature) -> bool;
}

struct Build {
    profile: char8[];
    debugAssertions: bool;
    debugInfo: bool;
    isTest: bool;
    timestamp: uint64;
    date: char8[];
    time: char8[];
}

struct Compiler { version: char8[]; }
extend Compiler {
    #Intrinsic("Compiler.HasFeature")
    func hasFeature(self, feature: char8[]) -> bool;
}

struct Source {
    line: uint;
    column: uint;
    fileName: char8[];
    filePath: char8[];
    function: char8[];
    module: char8[];
}

struct Config {}
extend Config {
    #Intrinsic("Config.Get")
    func get(self, name: char8[]) -> char8[];
    #Intrinsic("Config.Has")
    func has(self, name: char8[]) -> bool;
}

#Intrinsic("Target")
const $target: Target;
#Intrinsic("Build")
const $build: Build;
#Intrinsic("Compiler")
const $compiler: Compiler;
#Intrinsic("Source")
const $source: Source;
#Intrinsic("Config")
const $config: Config;

module Demo {
    func Values() {
        let line = source.line;
        let column = source.column;
        let fileName = source.fileName;
        let filePath = source.filePath;
        let function = source.function;
        let moduleName = source.module;
        let date = build.date;
        let time = build.time;
        let timestamp = build.timestamp;
        let pointerBits = target.pointerBits;
        let targetTriple = target.triple;
        let feature = target.hasFeature(TargetFeature::AVX2);
        let profile = build.profile;
        let debugAssertions = build.debugAssertions;
        let debugInfo = build.debugInfo;
        let isTest = build.isTest;
        let configValue = config.get("allocator");
        let hasConfig = config.has("allocator");
        let version = compiler.version;
        let compilerFeature = compiler.hasFeature("namespaced-intrinsics");
        let currentTarget = target;
    }
}
)");
    CompileTimeContext context;
    context.target.pointer_size = 8;
    context.target.cpu_features = Target::CpuFeature::AVX2;
    context.targetTriple = "windows-x64";
    context.profileName = "Testing";
    context.debugAssertions = false;
    context.debugInfo = true;
    context.isTest = true;
    context.buildTimestamp = 0;
    context.config["allocator"] = "mimalloc";
    context.compilerVersion = "1.2.3";

    const SemanticModel model = Analyze(parsed.module, std::move(context));
    std::string diagnosticMessages;
    for (const Diagnostic &diagnostic : model.diagnostics) {
        diagnosticMessages += diagnostic.message + "\n";
    }
    INFO(diagnosticMessages);
    REQUIRE_FALSE(model.HasErrors());
    AstToHirLowering lowering(model);
    const HirPackage package = lowering.Generate();
    REQUIRE(package.modules.size() == 1);
    REQUIRE(package.modules[0].funcs.size() == 1);
    REQUIRE(package.modules[0].funcs[0].body.has_value());

    std::unordered_map<std::string, std::string> values;
    for (const auto &stmt : package.modules[0].funcs[0].body->stmts) {
        const auto *let = dynamic_cast<const HirLetStmt *>(stmt.get());
        REQUIRE(let != nullptr);
        if (const auto *literal = dynamic_cast<const HirLiteralExpr *>(let->init.get())) {
            values[let->name] = literal->value;
        }
        else {
            const auto *object = dynamic_cast<const HirStructInitExpr *>(let->init.get());
            REQUIRE(object != nullptr);
            CHECK(let->name == "currentTarget");
            CHECK(object->typeName == "Target");
            CHECK(object->fields.size() == 2);
        }
    }
    CHECK(values["line"] != "0");
    CHECK(values["column"] != "0");
    CHECK(values["fileName"] == "test.rux");
    CHECK(values["filePath"] == "test.rux");
    CHECK(values["function"] == "Demo::Values");
    CHECK(values["moduleName"] == "test::Demo");
    CHECK(values["date"] == "1970-01-01");
    CHECK(values["time"] == "00:00:00");
    CHECK(values["timestamp"] == "0");
    CHECK(values["pointerBits"] == "64");
    CHECK(values["targetTriple"] == "windows-x64");
    CHECK(values["feature"] == "true");
    CHECK(values["profile"] == "Testing");
    CHECK(values["debugAssertions"] == "false");
    CHECK(values["debugInfo"] == "true");
    CHECK(values["isTest"] == "true");
    CHECK(values["configValue"] == "mimalloc");
    CHECK(values["hasConfig"] == "true");
    CHECK(values["version"] == "1.2.3");
    CHECK(values["compilerFeature"] == "true");
}

TEST_CASE("#When includes true declarations and removes false declarations and imports") {
    auto parsed = ParseSource(R"(
const Enabled = true;

#When(Enabled && compiler.hasFeature("when-attribute"))
func Kept() -> int { return 1; }

#When(false)
func Removed(value: MissingType) {}

#When(false)
import Missing::Thing;

#When(false)
extern func MissingLibrary();

#When(true)
#Link("Kernel32.dll", "Beep")
extern func Tone(freq: uint32, duration: uint32) -> bool32;
)");

    const auto model = Analyze(parsed.module);
    CHECK_FALSE(model.HasErrors());
    CHECK(FindFunc(parsed.module, "Kept") != nullptr);
    CHECK(FindFunc(parsed.module, "Removed") == nullptr);

    const ExternFuncDecl *external = nullptr;
    for (const auto &item : parsed.module.items) {
        if (const auto *candidate = dynamic_cast<const ExternFuncDecl *>(item.get())) {
            external = candidate;
        }
    }
    REQUIRE(external != nullptr);
    CHECK_EQ(external->name, "Tone");
    CHECK_EQ(external->dll, "Kernel32.dll");
    CHECK_EQ(external->symbolName, "Beep");
}

TEST_CASE("#When conditionally includes methods inside an extend block") {
    auto parsed = ParseSource(R"(
struct Animal {}

extend Animal {
    #When(target.os == .Windows)
    func Sound(self) -> int { return 1; }

    #When(false)
    func Removed(self, value: MissingType) {}
}
)");

    const auto model = Analyze(parsed.module, "Windows");
    CHECK_FALSE(model.HasErrors());

    const auto *impl = dynamic_cast<const ImplDecl *>(parsed.module.items[1].get());
    REQUIRE(impl != nullptr);
    REQUIRE_EQ(impl->methods.size(), 1);
    CHECK_EQ(impl->methods[0]->name, "Sound");
}

TEST_CASE("#When rejects non-boolean and runtime conditions") {
    auto nonBoolean = ParseSource(R"(
#When(1)
func Bad() {}
)");
    const auto nonBooleanModel = Analyze(nonBoolean.module);
    REQUIRE(nonBooleanModel.HasErrors());
    CHECK_EQ(nonBooleanModel.diagnostics[0].message, "'#When' condition must be of type 'bool'");

    auto runtime = ParseSource(R"(
func Runtime() -> bool { return true; }

#When(Runtime())
func Bad() {}
)");
    const auto runtimeModel = Analyze(runtime.module);
    REQUIRE(runtimeModel.HasErrors());
    CHECK_EQ(runtimeModel.diagnostics[0].message, "'#When' condition must be a compile-time constant expression");
}

TEST_CASE("duplicate #When attributes are rejected") {
    Lexer lexer(R"(
#When(true)
#When(false)
func Duplicate() {}
)",
                "test.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());

    Parser parser(std::move(lexed.tokens), "test.rux");
    const auto parsed = parser.Parse();
    REQUIRE(parsed.HasErrors());
    REQUIRE_EQ(parsed.diagnostics.size(), 1);
    CHECK_EQ(parsed.diagnostics[0].message, "duplicate '#When' attribute");
}
