// `when` conditional compilation: the taken branch is spliced into its parent
// and the others are dropped before anything type-checks them.

#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Ast/Ast.h"
#include "Syntax/Parser/Parser.h"

#include <algorithm>
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

const ExternBlockDecl *FindExternBlock(const Module &module) {
    for (const auto &item : module.items) {
        if (const auto *block = dynamic_cast<const ExternBlockDecl *>(item.get())) {
            return block;
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

TEST_CASE("when evaluates logical right shifts at the signed operand width") {
    auto parsed = ParseSource(R"(
const Negative: int8 = -8;

func Do() -> int {
    when Negative >>> 2 == 62 && (-8i16 >>> 2) == 16382 {
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
import Rux::{ #target, OperatingSystem };

struct Animal {}

extend Animal {
    when #target.os == OperatingSystem::Windows {
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

TEST_CASE("when tests the target OS through #target.os") {
    const std::string source = R"(
import Rux::{ #target, OperatingSystem };

func Do() -> int {
    when #target.os == OperatingSystem::Windows {
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

TEST_CASE("#target.os compares against the OperatingSystem enum") {
    auto parsed = ParseSource(R"(
import Rux::{ #target, OperatingSystem };

func Do() -> int {
    when #target.os != OperatingSystem::Linux {
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

TEST_CASE("an enum shorthand takes its enum from the other side of a when condition") {
    const std::string source = R"(
import Rux::{ #target };

func Do() -> int {
    when #target.os == .Windows {
        return 1;
    } else {
        return 0;
    }
}
)";

    auto windows = ParseSource(source);
    const auto windowsModel = Analyze(windows.module, "Windows");
    CHECK_FALSE(windowsModel.HasErrors());
    CHECK(ReturnedLiteral(*FindFunc(windows.module, "Do")) == "1");

    // On another target the shorthand still resolves; the branch is just dropped.
    auto linux = ParseSource(source);
    const auto linuxModel = Analyze(linux.module, "Linux");
    CHECK_FALSE(linuxModel.HasErrors());
    CHECK(ReturnedLiteral(*FindFunc(linux.module, "Do")) == "0");
}

TEST_CASE("an enum shorthand in a when condition still validates the variant") {
    auto parsed = ParseSource(R"(
import Rux::{ #target };

func Do() -> int {
    when #target.os == .Wndows {
        return 1;
    }
    return 0;
}
)");
    const auto model = Analyze(parsed.module, "Windows");
    REQUIRE(model.HasErrors());
    CHECK(model.diagnostics[0].message ==
          "'.Wndows' is not a variant of 'OperatingSystem'; the variants are: .AIX, .Android, .DragonFlyBSD, .FreeBSD, "
          ".Fuchsia, .Haiku, .Illumos, .IOS, .Linux, .MacOS, .NetBSD, .OpenBSD, .QNX, .Redox, .Solaris, .Windows");
}

TEST_CASE("#Error in a taken branch emits its message as a compile-time error") {
    auto parsed = ParseSource(R"(
import Rux::{ #target, #Error };

func Do() -> int {
    when #target.os == .Windows {
        #Error("Windows is not supported");
    }
    return 0;
}
)");
    const auto model = Analyze(parsed.module, "Windows");
    REQUIRE(model.HasErrors());
    CHECK(model.diagnostics[0].message == "Windows is not supported");
}

TEST_CASE("#Error in a branch that is not taken never fires") {
    auto parsed = ParseSource(R"(
import Rux::{ #target, #Error };

func Do() -> int {
    when #target.os == .Linux {
        #Error("Linux is not supported");
    }
    return 0;
}
)");
    const auto model = Analyze(parsed.module, "Windows");
    CHECK_FALSE(model.HasErrors());
}

TEST_CASE("#Warn emits a warning, not an error") {
    auto parsed = ParseSource(R"(
import Rux::{ #Warn };

func Do() -> int {
    #Warn("deprecated path");
    return 0;
}
)");
    const auto model = Analyze(parsed.module);
    CHECK_FALSE(model.HasErrors());
    REQUIRE(model.diagnostics.size() == 1);
    CHECK(model.diagnostics[0].severity == Diagnostic::Severity::Warning);
    CHECK(model.diagnostics[0].message == "deprecated path");
}

TEST_CASE("a #Warn call leaves no runtime code behind") {
    auto parsed = ParseSource(R"(
struct Slice<T> { data: *T; length: uint; }
intrinsic func #Warn(message: Slice<char8>);

func Do() {
    #Warn("compile-time only");
}
)");
    const auto model = AnalyzeNoDeps(parsed.module, CompileTimeContext{});
    REQUIRE_FALSE(model.HasErrors());

    AstToHirLowering lowering(model);
    const HirPackage package = lowering.Generate();
    REQUIRE(package.modules.size() == 1);
    REQUIRE(package.modules[0].funcs.size() == 1);
    REQUIRE(package.modules[0].funcs[0].body.has_value());
    // The directive is dropped: the body has no statements to run.
    CHECK(package.modules[0].funcs[0].body->stmts.empty());
}

TEST_CASE("#Error requires a string-literal message") {
    auto parsed = ParseSource(R"(
import Rux::{ #Error };

const Message = "hi";

func Do() -> int {
    #Error(Message);
    return 0;
}
)");
    const auto model = Analyze(parsed.module);
    REQUIRE(model.HasErrors());
    CHECK(model.diagnostics[0].message == "'#Error' message must be a string literal");
}

TEST_CASE("a compile-time match statement selects the arm for the target") {
    const std::string source = R"(
import Rux::{ #target };

func Do() -> int {
    when #target.os {
        .Windows => { return 1; }
        .Linux => { return 2; }
        else => { return 3; }
    }
}
)";
    auto windows = ParseSource(source);
    CHECK_FALSE(Analyze(windows.module, "Windows").HasErrors());
    CHECK(ReturnedLiteral(*FindFunc(windows.module, "Do")) == "1");

    auto linux = ParseSource(source);
    CHECK_FALSE(Analyze(linux.module, "Linux").HasErrors());
    CHECK(ReturnedLiteral(*FindFunc(linux.module, "Do")) == "2");

    // No arm names macOS, so the `else` arm is taken.
    auto mac = ParseSource(source);
    CHECK_FALSE(Analyze(mac.module, "MacOS").HasErrors());
    CHECK(ReturnedLiteral(*FindFunc(mac.module, "Do")) == "3");
}

TEST_CASE("a compile-time match arm may list several patterns") {
    const std::string source = R"(
import Rux::{ #target };

func Do() -> int {
    when #target.os {
        .Windows, .Linux => { return 1; }
        .MacOS => { return 2; }
        else => { return 3; }
    }
}
)";
    // Either pattern of the grouped arm selects it.
    auto windows = ParseSource(source);
    CHECK_FALSE(Analyze(windows.module, "Windows").HasErrors());
    CHECK(ReturnedLiteral(*FindFunc(windows.module, "Do")) == "1");

    auto linux = ParseSource(source);
    CHECK_FALSE(Analyze(linux.module, "Linux").HasErrors());
    CHECK(ReturnedLiteral(*FindFunc(linux.module, "Do")) == "1");

    auto mac = ParseSource(source);
    CHECK_FALSE(Analyze(mac.module, "MacOS").HasErrors());
    CHECK(ReturnedLiteral(*FindFunc(mac.module, "Do")) == "2");

    auto freebsd = ParseSource(source);
    CHECK_FALSE(Analyze(freebsd.module, "FreeBSD").HasErrors());
    CHECK(ReturnedLiteral(*FindFunc(freebsd.module, "Do")) == "3");
}

TEST_CASE("a compile-time match accepts a bare-expression arm and the full enum form") {
    auto parsed = ParseSource(R"(
import Rux::{ #target, OperatingSystem };

func Do() -> int {
    when #target.os {
        OperatingSystem::Windows => 1,
        else => 2
    }
    return 0;
}
)");
    const auto model = Analyze(parsed.module, "Windows");
    CHECK_FALSE(model.HasErrors());
    // The taken arm's expression became a statement spliced before `return 0;`.
    const auto *func = FindFunc(parsed.module, "Do");
    REQUIRE(func != nullptr);
    REQUIRE(func->body->stmts.size() == 2);
    CHECK(dynamic_cast<const ExprStmt *>(func->body->stmts[0].get()) != nullptr);
}

TEST_CASE("a compile-time match with no matching arm and no else is an error") {
    auto parsed = ParseSource(R"(
import Rux::{ #target };

func Do() -> int {
    when #target.os {
        .Linux => { return 1; }
        .MacOS => { return 2; }
    }
    return 0;
}
)");
    const auto model = Analyze(parsed.module, "Windows");
    REQUIRE(model.HasErrors());
    CHECK(model.diagnostics[0].message == "no arm of this 'when' matches .Windows");
}

TEST_CASE("a compile-time match arm fires #Error only when it is the taken arm") {
    const std::string source = R"(
import Rux::{ #target, #Error };

func Do() -> int {
    when #target.os {
        .Windows => { return 1; }
        else => #Error("unsupported")
    }
    return 0;
}
)";
    // Windows takes the first arm; the else's #Error never fires.
    auto windows = ParseSource(source);
    CHECK_FALSE(Analyze(windows.module, "Windows").HasErrors());

    // Another target falls to the else arm and its #Error fires.
    auto linux = ParseSource(source);
    const auto model = Analyze(linux.module, "Linux");
    REQUIRE(model.HasErrors());
    CHECK(model.diagnostics[0].message == "unsupported");
}

TEST_CASE("a declaration-level match groups patterns and takes a semicolon-less import body") {
    const std::string source = R"(
import Rux::{ #target, #Error };

when #target.os {
    .Windows, .Linux => import Rux::{ OperatingSystem }
    else => #Error("unsupported")
}
)";
    // Either grouped pattern keeps the imported item, without a trailing ';'.
    auto windows = ParseSource(source);
    CHECK_FALSE(Analyze(windows.module, "Windows").HasErrors());

    auto linux = ParseSource(source);
    CHECK_FALSE(Analyze(linux.module, "Linux").HasErrors());

    // A target named by no pattern falls to the else directive.
    auto mac = ParseSource(source);
    const auto macModel = Analyze(mac.module, "MacOS");
    REQUIRE(macModel.HasErrors());
    CHECK(macModel.diagnostics[0].message == "unsupported");
}

TEST_CASE("a declaration-level compile-time match splices the taken arm") {
    const std::string source = R"(
import Rux::{ #target, #Error };

when #target.os {
    .Windows => { func Tag() -> int { return 1; } }
    else => #Error("unsupported")
}
)";
    auto windows = ParseSource(source);
    const auto windowsModel = Analyze(windows.module, "Windows");
    CHECK_FALSE(windowsModel.HasErrors());
    CHECK(FindFunc(windows.module, "Tag") != nullptr);

    // The `else` directive fires on a target the arms do not name.
    auto linux = ParseSource(source);
    const auto linuxModel = Analyze(linux.module, "Linux");
    REQUIRE(linuxModel.HasErrors());
    CHECK(linuxModel.diagnostics[0].message == "unsupported");
}

TEST_CASE("the dropped OS alias no longer names the OperatingSystem enum") {
    auto parsed = ParseSource(R"(
import Rux::{ #target };

func Do() -> int {
    when #target.os == OS::Windows {
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
import Rux::{ #target };

func Do() -> int {
    when #target.os == OperatingSystem::Windows {
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
    when #target.os == OperatingSystem::Windows {
        return 1;
    }
    return 0;
}
)");
    const auto model = Analyze(parsed.module, "Windows");
    REQUIRE(model.HasErrors());
    CHECK(model.diagnostics[0].message == "unknown identifier '#target'");
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

TEST_CASE("#target.os tells the BSDs apart") {
    const std::string source = R"(
import Rux::{ #target, OperatingSystem };

func Do() -> int {
    when #target.os == OperatingSystem::FreeBSD {
        return 1;
    } else when #target.os == OperatingSystem::OpenBSD {
        return 2;
    } else when #target.os == OperatingSystem::DragonFlyBSD {
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
import Rux::{ #target, OperatingSystem };

func Do() -> int {
    when #target.os == OperatingSystem::Haiku {
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
import Rux::{ #target, OperatingSystem };

func Do() -> int {
    when #target.os == OperatingSystem::Wndows {
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
          ".Haiku, .Illumos, .IOS, .Linux, .MacOS, .NetBSD, .OpenBSD, .QNX, .Redox, .Solaris, .Windows");
}

TEST_CASE("compiler-initialized constants are ordinary expressions outside a when condition") {
    auto parsed = ParseSource(R"(
struct Target {
    pointerBits: uint;
}

intrinsic #target: Target;

func Do() -> int {
    let bits = #target.pointerBits;
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
import Rux::{ #target, #build, #compiler, OperatingSystem, Architecture, ApplicationBinaryInterface, Endianness,
             DataModel, ObjectFormat, BuildMode, OptimizationMode, OutputKind };

func Selected() -> int {
    when #target.os == OperatingSystem::Windows &&
        #target.arch == Architecture::X86_64 &&
        #target.abi == ApplicationBinaryInterface::WindowsX64 &&
        #target.endian == Endianness::Little &&
        #target.pointerBits == 64 &&
        #target.dataModel == DataModel::LLP64 &&
        #target.objectFormat == ObjectFormat::COFF &&
        #target.triple == "windows-x86_64" &&
        #target.HasFeature(.AVX2) &&
        #build.profile == "Production" &&
        #build.mode == BuildMode::Release &&
        #build.optimization == OptimizationMode::Speed &&
        !#build.debugAssertions &&
        #build.debugInfo &&
        #build.isTest &&
        #build.outputKind == OutputKind::SharedLibrary {
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
    context.targetTriple = "windows-x86_64";
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
import Rux::{ #config, #compiler, #source, SemanticVersion };

const FutureCompiler = SemanticVersion::New(1, 2, 4);

func Selected() -> int {
    when #config.Has("sqlite") &&
        #config.Get("allocator") == "mimalloc" &&
        #compiler.HasFeature("conditional-compilation") &&
        !#compiler.HasFeature("imaginary-feature") &&
        #compiler.version.major == 1 &&
        #compiler.version.minor == 2 &&
        #compiler.version.patch == 3 &&
        #compiler.version == SemanticVersion::New(1, 2, 3) &&
        #compiler.version != FutureCompiler &&
        #compiler.version < FutureCompiler &&
        #compiler.version <= SemanticVersion::New(1, 2, 3) &&
        #compiler.version > SemanticVersion::New(1, 2, 2) &&
        #compiler.version >= SemanticVersion::New(1, 2, 3) &&
        #source.function == "Selected" &&
        #source.module == "test" {
        return 1;
    } else {
        return 0;
    }
}
)");
    CompileTimeContext context;
    context.config["sqlite"] = "true";
    context.config["allocator"] = "mimalloc";
    context.compilerVersion = "1.2.3-rc.1+build.7";

    const auto model = Analyze(parsed.module, std::move(context));
    CHECK_FALSE(model.HasErrors());
    CHECK(ReturnedLiteral(*FindFunc(parsed.module, "Selected")) == "1");
}

TEST_CASE("an undeclared intrinsic name is rejected") {
    // `#line` parses as a `#`-prefixed intrinsic value, but no such intrinsic
    // exists (source facts live under `#source`), so it is caught in analysis.
    auto parsed = ParseSource(R"(
func Selected() -> int {
    return #line;
}
)");
    const auto model = Analyze(parsed.module);
    REQUIRE(model.HasErrors());
    CHECK(std::ranges::any_of(model.diagnostics,
                              [](const auto &diagnostic) { return diagnostic.message == "undefined name '#line'"; }));
}

TEST_CASE("intrinsic declares a compiler-initialized ordinary constant") {
    auto parsed = ParseSource(R"(
struct Target { pointerBits: uint; }

intrinsic #target: Target;
)");
    REQUIRE(parsed.module.items.size() == 2);
    const auto *decl = dynamic_cast<const ConstDecl *>(parsed.module.items[1].get());
    REQUIRE(decl != nullptr);
    CHECK(decl->name == "#target");
    // The type names the intrinsic, so the constant can be renamed freely.
    CHECK(decl->intrinsicName == "Target");
    CHECK(decl->value == nullptr);
}

TEST_CASE("ordinary intrinsic expressions lower to context literals") {
    auto parsed = ParseSource(R"(
struct Slice<T> { data: *T; length: uint; }

enum TargetFeature { SSE2, SSE3, SSSE3, SSE41, SSE42, AVX, AVX2, AVX512, NEON, SVE, RVV }

struct Target {
    pointerBits: uint;
    triple: Slice<char8>;
}

extend Target {
    intrinsic func HasFeature(self, feature: TargetFeature) -> bool;
}

struct Build {
    profile: Slice<char8>;
    debugAssertions: bool;
    debugInfo: bool;
    isTest: bool;
    timestamp: uint64;
    date: Slice<char8>;
    time: Slice<char8>;
}

struct SemanticVersion {
    major: uint;
    minor: uint;
    patch: uint;
}
struct Compiler { version: SemanticVersion; }
extend Compiler {
    intrinsic func HasFeature(self, feature: Slice<char8>) -> bool;
}

struct Source {
    line: uint;
    column: uint;
    fileName: Slice<char8>;
    filePath: Slice<char8>;
    function: Slice<char8>;
    module: Slice<char8>;
}

struct Config {}
extend Config {
    intrinsic func Get(self, name: Slice<char8>) -> Slice<char8>;
    intrinsic func Has(self, name: Slice<char8>) -> bool;
}

intrinsic #target: Target;
intrinsic #build: Build;
intrinsic #compiler: Compiler;
intrinsic #source: Source;
intrinsic #config: Config;

module Demo {
    func Values() {
        let line = #source.line;
        let column = #source.column;
        let fileName = #source.fileName;
        let filePath = #source.filePath;
        let function = #source.function;
        let moduleName = #source.module;
        let date = #build.date;
        let time = #build.time;
        let timestamp = #build.timestamp;
        let pointerBits = #target.pointerBits;
        let targetTriple = #target.triple;
        let feature = #target.HasFeature(TargetFeature::AVX2);
        let profile = #build.profile;
        let debugAssertions = #build.debugAssertions;
        let debugInfo = #build.debugInfo;
        let isTest = #build.isTest;
        let configValue = #config.Get("allocator");
        let hasConfig = #config.Has("allocator");
        let version = #compiler.version;
        let compilerFeature = #compiler.HasFeature("namespaced-intrinsics");
        let currentTarget = #target;
    }
}
)");
    CompileTimeContext context;
    context.target.pointer_size = 8;
    context.target.cpu_features = Target::CpuFeature::AVX2;
    context.targetTriple = "windows-x86_64";
    context.profileName = "Testing";
    context.debugAssertions = false;
    context.debugInfo = true;
    context.isTest = true;
    context.buildTimestamp = 0;
    context.config["allocator"] = "mimalloc";
    context.compilerVersion = "1.2.3-rc.1+build.7";

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
            if (let->name == "version") {
                CHECK(object->typeName == "SemanticVersion");
                REQUIRE(object->fields.size() == 3);
                CHECK(dynamic_cast<const HirLiteralExpr *>(object->fields[0].value.get())->value == "1");
                CHECK(dynamic_cast<const HirLiteralExpr *>(object->fields[1].value.get())->value == "2");
                CHECK(dynamic_cast<const HirLiteralExpr *>(object->fields[2].value.get())->value == "3");
            }
            else {
                CHECK(let->name == "currentTarget");
                CHECK(object->typeName == "Target");
                CHECK(object->fields.size() == 2);
            }
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
    CHECK(values["targetTriple"] == "windows-x86_64");
    CHECK(values["feature"] == "true");
    CHECK(values["profile"] == "Testing");
    CHECK(values["debugAssertions"] == "false");
    CHECK(values["debugInfo"] == "true");
    CHECK(values["isTest"] == "true");
    CHECK(values["configValue"] == "mimalloc");
    CHECK(values["hasConfig"] == "true");
    CHECK(values["compilerFeature"] == "true");
}

TEST_CASE("when includes true declarations and removes false declarations and imports") {
    auto parsed = ParseSource(R"(
import Rux::{ #compiler };

const Enabled = true;

when Enabled && #compiler.HasFeature("conditional-compilation") {
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

TEST_CASE("Link resolves a target-selected compile-time string constant") {
    auto parsed = ParseSource(R"(
import Rux::{ OperatingSystem, #target };

when #target.os == OperatingSystem::Windows {
    const LibName = "ucrtbase.dll";
} else {
    const LibName = "libm.so.6";
}

#Link(LibName)
extern {
    func cos(value: float64) -> float64;
}
)");

    const auto model = Analyze(parsed.module, "Linux");
    CHECK_FALSE(model.HasErrors());

    const auto *block = FindExternBlock(parsed.module);
    REQUIRE(block != nullptr);
    CHECK_EQ(block->dll, "libm.so.6");
    REQUIRE_EQ(block->items.size(), 1);
    const auto *function = dynamic_cast<const ExternFuncDecl *>(block->items.front().get());
    REQUIRE(function != nullptr);
    CHECK_EQ(function->dll, "libm.so.6");
}

TEST_CASE("Link rejects a compile-time constant that is not a string") {
    auto parsed = ParseSource(R"(
const LibName = 42;
#Link(LibName)
extern func Run();
)");

    const auto model = Analyze(parsed.module);
    REQUIRE(model.HasErrors());
    CHECK(std::ranges::any_of(model.diagnostics, [](const auto &diagnostic) {
        return diagnostic.message == "'#Link' library name 'LibName' must be a string";
    }));
}
