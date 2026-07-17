// `when` conditional compilation: the taken branch is spliced into its parent
// and the others are dropped before anything type-checks them.

#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Ast/Ast.h"
#include "Syntax/Parser/Parser.h"

#include <doctest.h>
#include <string>
#include <string_view>
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

// A stand-in `Rux` package so `import Rux::{...}` resolves in these tests. The
// fold uses its own built-in variant tables, so the enum bodies here only need
// to exist, not to be complete.
constexpr std::string_view kRuxPackageSource = R"(
const target = 0;
const build = 0;
const compiler = 0;
const source = 0;
const config = 0;
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

DepPackage RuxDep(ParseResult &storage) {
    storage = ParseSource(std::string(kRuxPackageSource));
    DepPackage dep;
    dep.name = "Rux";
    dep.modules.push_back({"Rux", &storage.module});
    return dep;
}

// Runs the front end the way the driver does, which folds `when` in `module`,
// with the stand-in `Rux` package available so `import Rux::{...}` resolves.
SemanticModel Analyze(Module &module, const std::string &targetSystem = "Windows") {
    ParseResult rux;
    std::vector<Module *> modules = {&module};
    SemanticAnalyzer analyzer(modules, {RuxDep(rux)}, "test", targetSystem);
    return analyzer.Analyze();
}

SemanticModel Analyze(Module &module, CompileTimeContext context) {
    ParseResult rux;
    std::vector<Module *> modules = {&module};
    SemanticAnalyzer analyzer(modules, {RuxDep(rux)}, "test", std::move(context));
    return analyzer.Analyze();
}

// For tests that lower the result: no dependency, so the lowered package holds
// only the module under test.
SemanticModel AnalyzeNoDeps(Module &module, CompileTimeContext context) {
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

TEST_CASE("a when statement keeps only the taken branch") {
    auto parsed = ParseSource(R"(
const Debug = true;

func Do() -> int {
    when Debug {
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

TEST_CASE("a when statement that is false keeps the else branch") {
    auto parsed = ParseSource(R"(
const Debug = false;

func Do() -> int {
    when Debug {
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
    when Debug {
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

TEST_CASE("a when statement introduces no scope of its own") {
    auto parsed = ParseSource(R"(
const Debug = true;

func Do() -> int {
    when Debug {
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
    when Level > 3 {
        return 1;
    } else when Level == 3 {
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

TEST_CASE("when selects declarations, not just statements") {
    auto parsed = ParseSource(R"(
const Debug = false;

when Debug {
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
        CHECK(dynamic_cast<const WhenDecl *>(item.get()) == nullptr);
    }
}

TEST_CASE("when selects methods inside an extend block") {
    auto parsed = ParseSource(R"(
import Rux::{ target, OperatingSystem };

struct Animal {}

extend Animal {
    when target.os == OperatingSystem::Windows {
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
    // The taken branch's method joined the unconditional one, and the `when` is
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

TEST_CASE("when tests the target OS through target.os") {
    const std::string source = R"(
import Rux::{ target, OperatingSystem };

func Do() -> int {
    when target.os == OperatingSystem::Windows {
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

TEST_CASE("target.os compares against the OperatingSystem enum") {
    auto parsed = ParseSource(R"(
import Rux::{ target, OperatingSystem };

func Do() -> int {
    when target.os != OperatingSystem::Linux {
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

TEST_CASE("an enum shorthand is rejected in a when condition") {
    auto parsed = ParseSource(R"(
import Rux::{ target };

func Do() -> int {
    when target.os == .Windows {
        return 1;
    }
    return 0;
}
)");
    const auto model = Analyze(parsed.module, "Windows");
    REQUIRE(model.HasErrors());
    CHECK(model.diagnostics[0].message ==
          "enum shorthand '.Windows' is not allowed in a 'when' condition; write it in full, as in "
          "'OperatingSystem::Windows'");
}

TEST_CASE("the dropped OS alias no longer names the OperatingSystem enum") {
    auto parsed = ParseSource(R"(
import Rux::{ target };

func Do() -> int {
    when target.os == OS::Windows {
        return 1;
    }
    return 0;
}
)");
    const auto model = Analyze(parsed.module, "Windows");
    REQUIRE(model.HasErrors());
    CHECK(model.diagnostics[0].message == "cannot compare 'OperatingSystem' with 'OS'");
}

TEST_CASE("a built-in enum named in a condition must be imported from Rux") {
    auto parsed = ParseSource(R"(
import Rux::{ target };

func Do() -> int {
    when target.os == OperatingSystem::Windows {
        return 1;
    }
    return 0;
}
)");
    const auto model = Analyze(parsed.module, "Windows");
    REQUIRE(model.HasErrors());
    CHECK(model.diagnostics[0].message == "unknown identifier 'OperatingSystem'");
}

TEST_CASE("a build intrinsic used in a condition must be imported from Rux") {
    auto parsed = ParseSource(R"(
func Do() -> int {
    when target.os == OperatingSystem::Windows {
        return 1;
    }
    return 0;
}
)");
    const auto model = Analyze(parsed.module, "Windows");
    REQUIRE(model.HasErrors());
    CHECK(model.diagnostics[0].message == "unknown identifier 'target'");
}

TEST_CASE("the program's own enums compare by their qualified name") {
    auto parsed = ParseSource(R"(
enum Mode { Fast, Small }

const Build = Mode::Small;

func Do() -> int {
    when Build == Mode::Fast {
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
import Rux::{ target, OperatingSystem };

func Do() -> int {
    when target.os == OperatingSystem::FreeBSD {
        return 1;
    } else when target.os == OperatingSystem::OpenBSD {
        return 2;
    } else when target.os == OperatingSystem::DragonFlyBSD {
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
import Rux::{ target, OperatingSystem };

func Do() -> int {
    when target.os == OperatingSystem::Haiku {
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
import Rux::{ target, OperatingSystem };

func Do() -> int {
    when target.os == OperatingSystem::Wndows {
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

TEST_CASE("compiler-initialized constants are ordinary expressions outside a when condition") {
    auto parsed = ParseSource(R"(
struct Target {
    pointerBits: uint;
}

intrinsic const target: Target;

func Do() -> int {
    let bits = target.pointerBits;
    return 0;
}
)");
    CompileTimeContext context;
    context.target.pointer_size = 8;
    const auto model = AnalyzeNoDeps(parsed.module, std::move(context));
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

TEST_CASE("an enum shorthand is an error; the variant must be written in full") {
    auto parsed = ParseSource(R"(
enum Mode { Fast, Small }

func Do() -> Mode {
    return .Fast;
}
)");
    const auto model = Analyze(parsed.module);
    REQUIRE(model.HasErrors());
    CHECK(model.diagnostics[0].message == "'.Fast' must be written in full, as in 'Enum::Fast'");
}

TEST_CASE("a when condition that is not a compile-time constant is an error") {
    auto parsed = ParseSource(R"(
func Do() -> int {
    var runtime = 1;
    when runtime > 0 {
        return 1;
    }
    return 0;
}
)");
    const auto model = Analyze(parsed.module);
    REQUIRE(model.HasErrors());
    CHECK(model.diagnostics[0].message == "'when' condition must be a compile-time constant expression");
}

TEST_CASE("a when condition that is not a bool is an error") {
    auto parsed = ParseSource(R"(
const Level = 3;

func Do() -> int {
    when Level {
        return 1;
    }
    return 0;
}
)");
    const auto model = Analyze(parsed.module);
    REQUIRE(model.HasErrors());
    CHECK(model.diagnostics[0].message == "'when' condition must be of type 'bool'");
}

TEST_CASE("target and build intrinsics expose the full compile-time context") {
    auto parsed = ParseSource(R"(
import Rux::{ target, build, compiler, OperatingSystem, Architecture, ApplicationBinaryInterface, Endianness,
             DataModel, ObjectFormat, BuildMode, OptimizationMode, OutputKind };

func Selected() -> int {
    when target.os == OperatingSystem::Windows &&
        target.arch == Architecture::X86_64 &&
        target.abi == ApplicationBinaryInterface::WindowsX64 &&
        target.endian == Endianness::Little &&
        target.pointerBits == 64 &&
        target.dataModel == DataModel::LLP64 &&
        target.objectFormat == ObjectFormat::COFF &&
        target.triple == "windows-x64" &&
        target.HasFeature(.AVX2) &&
        build.profile == "Production" &&
        build.mode == BuildMode::Release &&
        build.optimization == OptimizationMode::Speed &&
        !build.debugAssertions &&
        build.debugInfo &&
        build.isTest &&
        build.outputKind == OutputKind::SharedLibrary {
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
import Rux::{ config, compiler, source };

func Selected() -> int {
    when config.Has("sqlite") &&
        config.Get("allocator") == "mimalloc" &&
        compiler.HasFeature("conditional-compilation") &&
        !compiler.HasFeature("imaginary-feature") &&
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

TEST_CASE("intrinsic declares a compiler-initialized ordinary constant") {
    auto parsed = ParseSource(R"(
struct Target { pointerBits: uint; }

intrinsic const target: Target;
)");
    REQUIRE(parsed.module.items.size() == 2);
    const auto *decl = dynamic_cast<const ConstDecl *>(parsed.module.items[1].get());
    REQUIRE(decl != nullptr);
    CHECK(decl->name == "target");
    // The type names the intrinsic, so the constant can be renamed freely.
    CHECK(decl->intrinsicName == "Target");
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
    intrinsic func HasFeature(self, feature: TargetFeature) -> bool;
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
    intrinsic func HasFeature(self, feature: char8[]) -> bool;
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
    intrinsic func Get(self, name: char8[]) -> char8[];
    intrinsic func Has(self, name: char8[]) -> bool;
}

intrinsic const target: Target;
intrinsic const build: Build;
intrinsic const compiler: Compiler;
intrinsic const source: Source;
intrinsic const config: Config;

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
        let feature = target.HasFeature(TargetFeature::AVX2);
        let profile = build.profile;
        let debugAssertions = build.debugAssertions;
        let debugInfo = build.debugInfo;
        let isTest = build.isTest;
        let configValue = config.Get("allocator");
        let hasConfig = config.Has("allocator");
        let version = compiler.version;
        let compilerFeature = compiler.HasFeature("namespaced-intrinsics");
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

    const SemanticModel model = AnalyzeNoDeps(parsed.module, std::move(context));
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

TEST_CASE("when includes true declarations and removes false declarations and imports") {
    auto parsed = ParseSource(R"(
import Rux::{ compiler };

const Enabled = true;

when Enabled && compiler.HasFeature("conditional-compilation") {
    func Kept() -> int { return 1; }

    #Link("Kernel32.dll", "Beep")
    extern func Tone(freq: uint32, duration: uint32) -> bool32;
}

when false {
    func Removed(value: MissingType) {}

    import Missing::Thing;

    extern func MissingLibrary();
}
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
