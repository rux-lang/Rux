// `when` folding splices the selected branch into its parent before semantic analysis.

#include "ConditionalCompilationTestSupport.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Semantic/ConditionalCompilation.h"
#include "Semantic/SemanticAnalyzer.h"

#include <algorithm>
#include <doctest.h>
#include <string>
#include <vector>

using namespace Rux;
using namespace Rux::Testing;

namespace {
constexpr auto ParseSource = ParseConditionalSource;
constexpr auto Analyze = [](Module &module, const std::string &targetSystem = "Windows") {
    return AnalyzeConditionalModule(module, targetSystem);
};
constexpr auto AnalyzeWithContext =
    static_cast<SemanticModel (*)(Module &, CompileTimeContext)>(AnalyzeConditionalModule);
constexpr auto AnalyzeNoDeps = AnalyzeConditionalModuleWithoutDependencies;
constexpr auto FindFunc = FindConditionalFunc;
constexpr auto FindExternBlock = FindConditionalExternBlock;
constexpr auto ReturnedLiteral = ConditionalReturnedLiteral;
} // namespace

TEST_CASE("the conditional folder directly splices selected declarations and statements") {
    auto parsed = ParseSource(R"(
const Enabled = true;

when Enabled {
    func Selected() -> int { return 1; }
}

func Run() -> int {
    when Enabled {
        return 2;
    } else {
        return 3;
    }
}
)");

    std::vector<Module *> modules = {&parsed.module};
    std::vector<Diagnostic> diagnostics;
    ResolveConditionalCompilation(modules, CompileTimeContext{}, diagnostics);

    CHECK(diagnostics.empty());
    const auto *selected = FindFunc(parsed.module, "Selected");
    const auto *run = FindFunc(parsed.module, "Run");
    REQUIRE(selected != nullptr);
    REQUIRE(run != nullptr);
    CHECK(ReturnedLiteral(*selected) == "1");
    CHECK(ReturnedLiteral(*run) == "2");
    CHECK(std::ranges::none_of(parsed.module.items,
                               [](const auto &item) { return dynamic_cast<const WhenDecl *>(item.get()) != nullptr; }));
}

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

TEST_CASE("conditional folding preserves taken item order and source locations") {
    auto parsed = ParseSource(R"(
const Enabled = true;

when Enabled {
    func First() {}
    func Second() {}
}

func Run() {
    let before = 0;
    when Enabled {
        let first = 1;
        let second = 2;
    }
    let after = 3;
}
)");

    auto *declarationWhen = dynamic_cast<WhenDecl *>(parsed.module.items[1].get());
    REQUIRE(declarationWhen != nullptr);
    REQUIRE(declarationWhen->branches.size() == 1);
    REQUIRE(declarationWhen->branches[0].items.size() == 2);
    Decl *firstDeclaration = declarationWhen->branches[0].items[0].get();
    Decl *secondDeclaration = declarationWhen->branches[0].items[1].get();
    const SourceLocation firstDeclarationLocation = firstDeclaration->location;
    const SourceLocation secondDeclarationLocation = secondDeclaration->location;

    auto *run = dynamic_cast<FuncDecl *>(parsed.module.items[2].get());
    REQUIRE(run != nullptr);
    REQUIRE(run->body != nullptr);
    REQUIRE(run->body->stmts.size() == 3);
    auto *statementWhen = dynamic_cast<IfStmt *>(run->body->stmts[1].get());
    REQUIRE(statementWhen != nullptr);
    REQUIRE(statementWhen->thenBlock != nullptr);
    REQUIRE(statementWhen->thenBlock->stmts.size() == 2);
    Stmt *firstStatement = statementWhen->thenBlock->stmts[0].get();
    Stmt *secondStatement = statementWhen->thenBlock->stmts[1].get();
    const SourceLocation firstStatementLocation = firstStatement->location;
    const SourceLocation secondStatementLocation = secondStatement->location;
    const auto sameLocation = [](const SourceLocation left, const SourceLocation right) {
        return left.line == right.line && left.column == right.column && left.offset == right.offset;
    };

    const auto model = Analyze(parsed.module);
    REQUIRE_FALSE(model.HasErrors());
    REQUIRE(parsed.module.items.size() == 4);
    CHECK(parsed.module.items[1].get() == firstDeclaration);
    CHECK(parsed.module.items[2].get() == secondDeclaration);
    CHECK(sameLocation(parsed.module.items[1]->location, firstDeclarationLocation));
    CHECK(sameLocation(parsed.module.items[2]->location, secondDeclarationLocation));

    run = dynamic_cast<FuncDecl *>(parsed.module.items[3].get());
    REQUIRE(run != nullptr);
    REQUIRE(run->body != nullptr);
    REQUIRE(run->body->stmts.size() == 4);
    CHECK(run->body->stmts[1].get() == firstStatement);
    CHECK(run->body->stmts[2].get() == secondStatement);
    CHECK(sameLocation(run->body->stmts[1]->location, firstStatementLocation));
    CHECK(sameLocation(run->body->stmts[2]->location, secondStatementLocation));
}

TEST_CASE("when selects methods inside an extend block") {
    auto parsed = ParseSource(R"(
import Core::{ #target, OperatingSystem };

struct Animal {}

extend Animal {
    when #target.os == OperatingSystem::Windows {
        func Sound(self: &Animal) -> int { return 1; }
    } else {
        func Sound(self: &Animal) -> int { return 2; }
    }

    func Legs(self: &Animal) -> int { return 4; }
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
import Core::{ #target, OperatingSystem };

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
import Core::{ #target, OperatingSystem };

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
import Core::{ #target };

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
import Core::{ #target };

func Do() -> int {
    when #target.os == .Wndows {
        return 1;
    }
    return 0;
}
)");
    const auto model = Analyze(parsed.module, "Windows");
    REQUIRE(model.HasErrors());
    CHECK(model.diagnostics[0].message == "'.Wndows' is not a variant of 'OperatingSystem'; the variants are: "
                                          ".FreeBSD, .Linux, .macOS, .Windows");
}

TEST_CASE("#Error in a taken branch emits its message as a compile-time error") {
    auto parsed = ParseSource(R"(
import Core::{ #target, #Error };

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
import Core::{ #target, #Error };

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
import Core::{ #Warn };

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
import Core::{ #Error };

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
import Core::{ #target };

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
    CHECK_FALSE(Analyze(mac.module, "macOS").HasErrors());
    CHECK(ReturnedLiteral(*FindFunc(mac.module, "Do")) == "3");
}

TEST_CASE("a compile-time match arm may list several patterns") {
    const std::string source = R"(
import Core::{ #target };

func Do() -> int {
    when #target.os {
        .Windows, .Linux => { return 1; }
        .macOS => { return 2; }
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
    CHECK_FALSE(Analyze(mac.module, "macOS").HasErrors());
    CHECK(ReturnedLiteral(*FindFunc(mac.module, "Do")) == "2");

    auto freebsd = ParseSource(source);
    CHECK_FALSE(Analyze(freebsd.module, "FreeBSD").HasErrors());
    CHECK(ReturnedLiteral(*FindFunc(freebsd.module, "Do")) == "3");
}

TEST_CASE("a compile-time match accepts a bare-expression arm and the full enum form") {
    auto parsed = ParseSource(R"(
import Core::{ #target, OperatingSystem };

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
import Core::{ #target };

func Do() -> int {
    when #target.os {
        .Linux => { return 1; }
        .macOS => { return 2; }
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
import Core::{ #target, #Error };

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
import Core::{ #target, #Error };

when #target.os {
    .Windows, .Linux => import Core::{ OperatingSystem }
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
    const auto macModel = Analyze(mac.module, "macOS");
    REQUIRE(macModel.HasErrors());
    CHECK(macModel.diagnostics[0].message == "unsupported");
}

TEST_CASE("a declaration-level compile-time match splices the taken arm") {
    const std::string source = R"(
import Core::{ #target, #Error };

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
import Core::{ #target };

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
import Core::{ #target };

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

TEST_CASE("#target.os tells the supported systems apart") {
    const std::string source = R"(
import Core::{ #target, OperatingSystem };

func Do() -> int {
    when #target.os == OperatingSystem::FreeBSD {
        return 1;
    } else when #target.os == OperatingSystem::Linux {
        return 2;
    } else when #target.os == OperatingSystem::macOS {
        return 3;
    } else {
        return 4;
    }
}
)";

    auto freeBsd = ParseSource(source);
    CHECK_FALSE(Analyze(freeBsd.module, "FreeBSD").HasErrors());
    CHECK(ReturnedLiteral(*FindFunc(freeBsd.module, "Do")) == "1");

    auto linux = ParseSource(source);
    CHECK_FALSE(Analyze(linux.module, "Linux").HasErrors());
    CHECK(ReturnedLiteral(*FindFunc(linux.module, "Do")) == "2");

    auto macos = ParseSource(source);
    CHECK_FALSE(Analyze(macos.module, "macOS").HasErrors());
    CHECK(ReturnedLiteral(*FindFunc(macos.module, "Do")) == "3");

    auto windows = ParseSource(source);
    CHECK_FALSE(Analyze(windows.module, "Windows").HasErrors());
    CHECK(ReturnedLiteral(*FindFunc(windows.module, "Do")) == "4");
}

TEST_CASE("a misspelled OS variant is an error, not a silently false branch") {
    auto parsed = ParseSource(R"(
import Core::{ #target, OperatingSystem };

func Do() -> int {
    when #target.os == OperatingSystem::Wndows {
        return 1;
    }
    return 0;
}
)");
    const auto model = Analyze(parsed.module);
    REQUIRE(model.HasErrors());
    CHECK(model.diagnostics[0].message == "'.Wndows' is not a variant of 'OperatingSystem'; the variants are: "
                                          ".FreeBSD, .Linux, .macOS, .Windows");
}

TEST_CASE("the former MacOS spelling is not an OperatingSystem variant") {
    auto parsed = ParseSource(R"(
import Core::{ #target, OperatingSystem };

func Do() -> int {
    when #target.os == OperatingSystem::MacOS {
        return 1;
    }
    return 0;
}
)");
    const auto model = Analyze(parsed.module, "macOS");
    REQUIRE(model.HasErrors());
    CHECK(model.diagnostics[0].message == "'.MacOS' is not a variant of 'OperatingSystem'; the variants are: "
                                          ".FreeBSD, .Linux, .macOS, .Windows");
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
    CHECK(model.diagnostics[0].message == "'runtime' is not a compile-time constant");
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
    CHECK(model.diagnostics[0].message == "'when' condition must have type 'bool', but found 'signed integer'");
}

TEST_CASE("compile-time evaluation distinguishes fields operand types values overflow and duplicate branches") {
    const std::string prefix = "import Core::{ #target };\nfunc Do() -> int {\n";
    const std::string suffix = "return 0;\n}\n";

    auto field = ParseSource(prefix + "when #target.cpu { return 1; }\n" + suffix);
    CHECK(Analyze(field.module).diagnostics[0].message == "unknown compile-time field '#target.cpu'");

    auto operand = ParseSource(prefix + "when 1 && true { return 1; }\n" + suffix);
    CHECK(Analyze(operand.module).diagnostics[0].message ==
          "compile-time operator '&&' requires 'bool' operands, but the left operand has type 'signed integer'");

    auto zero = ParseSource(prefix + "when 4 / 0 == 0 { return 1; }\n" + suffix);
    CHECK(Analyze(zero.module).diagnostics[0].message == "compile-time operator '/' cannot divide by zero");

    auto overflow = ParseSource(prefix + "when 127i8 + 1i8 == 0 { return 1; }\n" + suffix);
    CHECK(Analyze(overflow.module).diagnostics[0].message ==
          "compile-time evaluation of '+' overflows a signed 8-bit integer");

    auto typedOverflow =
        ParseSource("const TooLarge: uint8 = 256;\n" + prefix + "when TooLarge == 0u8 { return 1; }\n" + suffix);
    CHECK(Analyze(typedOverflow.module).diagnostics[0].message ==
          "compile-time constant 'TooLarge' overflows its unsigned 8-bit type");

    auto duplicate =
        ParseSource(prefix + "when 1 { 1 => { return 1; } 1 => { return 2; } else => { return 3; } }\n" + suffix);
    CHECK(std::ranges::any_of(Analyze(duplicate.module).diagnostics, [](const auto &diagnostic) {
        return diagnostic.message == "duplicate compile-time 'when' pattern 1";
    }));
}

TEST_CASE("compile-time warning and error directives preserve the authored message text") {
    auto parsed = ParseSource(R"(
        import Core::{ #Error, #Warn };
        func Do() {
            #Warn("user text: keep 'quotes' and punctuation!");
            #Error("line one\nline two");
        }
    )");
    const auto model = Analyze(parsed.module);
    const auto &diagnostics = model.diagnostics;
    REQUIRE_EQ(diagnostics.size(), 2);
    CHECK_EQ(diagnostics[0].message, "user text: keep 'quotes' and punctuation!");
    CHECK_EQ(diagnostics[1].message, "line one\nline two");
}

TEST_CASE("target and build intrinsics expose the full compile-time context") {
    auto parsed = ParseSource(R"(
import Core::{ #target, #build, #compiler, OperatingSystem, Architecture, ApplicationBinaryInterface, Endianness,
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
        #build.profile == "Release" &&
        #build.mode == BuildMode::Release &&
        #build.optimization == OptimizationMode::Speed &&
        !#build.debugAssertions &&
        !#build.debugInfo &&
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
    context.profile = BuildProfile::Release;
    context.isTest = true;
    context.outputKind = OutputKind::SharedLibrary;

    const auto model = AnalyzeWithContext(parsed.module, std::move(context));
    CHECK_FALSE(model.HasErrors());
    CHECK(ReturnedLiteral(*FindFunc(parsed.module, "Selected")) == "1");
}

TEST_CASE("configuration and compiler feature intrinsics are queryable") {
    auto parsed = ParseSource(R"(
import Core::{ #config, #compiler, #source, SemanticVersion };

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
    context.buildInfo = BuildInfo("1.2.3-rc.1+build.7", 0);

    const auto model = AnalyzeWithContext(parsed.module, std::move(context));
    CHECK_FALSE(model.HasErrors());
    CHECK(ReturnedLiteral(*FindFunc(parsed.module, "Selected")) == "1");
}

TEST_CASE("when includes true declarations and removes false declarations and imports") {
    auto parsed = ParseSource(R"(
import Core::{ #compiler };

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
import Core::{ OperatingSystem, #target };

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

// The intrinsics package is recognized by the import name its owning manifest
// bound it to, which `Package` lets a manifest choose. Keying the fold on a
// fixed spelling silently cost conditional compilation to anyone who renamed
// the dependency, so both directions are pinned here.

TEST_CASE("a condition accepts intrinsics imported under the manifest's own alias") {
    auto parsed = ParseSource(R"(
import Lang::{ #target, OperatingSystem };

func Which() -> int {
    when #target.os == OperatingSystem::Windows { return 1; }
    else { return 2; }
}
)");
    CompileTimeContext context;
    context.target.os = Target::OS::Windows;
    context.intrinsicsAliases = {"Lang"};

    ParseResult core;
    std::vector<Module *> modules = {&parsed.module};
    DepPackage dep = ConditionalCoreDependency(core);
    dep.name = "Lang";
    dep.modules[0].moduleName = "Lang";
    SemanticAnalyzer analyzer(modules, {dep}, "test", std::move(context));
    const auto model = analyzer.Analyze();
    CHECK_FALSE(model.HasErrors());
}

TEST_CASE("a condition still requires the intrinsic to be imported from that package") {
    auto parsed = ParseSource(R"(
import Other::{ #target, OperatingSystem };

func Which() -> int {
    when #target.os == OperatingSystem::Windows { return 1; }
    else { return 2; }
}
)");
    CompileTimeContext context;
    context.target.os = Target::OS::Windows;
    context.intrinsicsAliases = {"Core"};

    ParseResult core;
    std::vector<Module *> modules = {&parsed.module};
    DepPackage dep = ConditionalCoreDependency(core);
    dep.name = "Other";
    dep.modules[0].moduleName = "Other";
    SemanticAnalyzer analyzer(modules, {dep}, "test", std::move(context));
    const auto model = analyzer.Analyze();
    CHECK(model.HasErrors());
}
