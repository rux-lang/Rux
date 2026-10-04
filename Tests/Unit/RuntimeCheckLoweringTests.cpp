// Compiler-inserted run-time checks: each one stops the program with the same `Panic:` report a `Core::Panic` call
// gives, names the source location of the operation that failed, and leaves nothing behind once it is proven to pass.

#include "CodeGen/AArch64/RcuEmitter.h"
#include "CodeGen/RuntimeFailure.h"
#include "CodeGen/X86_64/RcuEmitter.h"
#include "Driver/BuildTarget.h"
#include "Ir/Lir/Lir.h"
#include "Lexer/Lexer.h"
#include "Linker/Linker.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Optimization/Pipeline.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"
#include "System/Process.h"
#include "Target/TargetTriple.h"

#include <chrono>
#include <cstddef>
#include <doctest.h>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Rux;

namespace {
constexpr std::string_view kSourcePath = "Project/Src/Checks.rux";
constexpr std::string_view kLogicalPath = "Src/Checks.rux";

LirPackage CompileChecks(const std::string_view source, const BuildProfile profile,
                         const Target::TargetTriple triple = Target::TargetTriple::Host()) {
    CompileTimeContext context;
    context.profile = profile;
    context.target = Driver::TargetContextForTriple(triple);
    context.targetTriple = triple.CanonicalName();
    context.sourceRoot = "Project";

    Lexer lexer{std::string(source), std::string(kSourcePath)};
    auto lexed = lexer.Tokenize();
    CAPTURE(lexed.diagnostics.empty() ? std::string{} : lexed.diagnostics.front().message);
    REQUIRE_FALSE(lexed.HasErrors());

    Parser parser(std::move(lexed.tokens), std::string(kSourcePath));
    auto parsed = parser.Parse();
    CAPTURE(parsed.diagnostics.empty() ? std::string{} : parsed.diagnostics.front().message);
    REQUIRE_FALSE(parsed.HasErrors());

    SemanticAnalyzer analyzer({&parsed.module}, {}, "RuntimeCheckTest", context);
    auto model = analyzer.Analyze();
    CAPTURE(model.diagnostics.empty() ? std::string{} : model.diagnostics.front().message);
    REQUIRE_FALSE(model.HasErrors());

    AstToHirLowering hirLowering(model);
    auto hir = hirLowering.Generate();
    auto pipeline = Optimization::OptimizationPipeline::ForProfile(profile);
    REQUIRE(pipeline.RunHir(hir).reachedFixedPoint);
    HirToLirLowering lirLowering(std::move(hir), context.target);
    auto lir = lirLowering.Generate();
    CAPTURE(lirLowering.Diagnostics().empty() ? std::string{} : lirLowering.Diagnostics().front().message);
    REQUIRE(lirLowering.Diagnostics().empty());
    REQUIRE(pipeline.RunLir(lir).reachedFixedPoint);
    return lir;
}

/// Every compiler-inserted trap in `function` that reports `message`.
std::vector<const LirInstr *> Traps(const LirPackage &package, const std::string_view function,
                                    const std::string_view message) {
    std::vector<const LirInstr *> found;
    for (const auto &module : package.modules) {
        for (const auto &func : module.funcs) {
            if (func.name != function) {
                continue;
            }
            for (const auto &block : func.blocks) {
                for (const auto &instruction : block.instrs) {
                    if (instruction.op == LirOpcode::Panic && instruction.strArg == message) {
                        found.push_back(&instruction);
                    }
                }
            }
        }
    }
    return found;
}

/// Builds `source` for the host, runs it, and returns what it printed and how it ended.
System::RunResult RunOnHost(const std::string_view source, const BuildProfile profile) {
    const auto triple = Target::TargetTriple::Host();
    auto package = CompileChecks(source, profile, triple);
    std::vector<RcuFile> objects;
    if (triple.Architecture() == Target::Arch::X86_64) {
        RcuEmitter emitter(package, "RuntimeCheckTest", triple.Os());
        objects = emitter.Generate();
        CAPTURE(emitter.Diagnostics().empty() ? std::string{} : emitter.Diagnostics().front().message);
        REQUIRE(emitter.Diagnostics().empty());
    }
    else {
        AArch64RcuEmitter emitter(package, "RuntimeCheckTest", triple.Os());
        objects = emitter.Generate();
        CAPTURE(emitter.Diagnostics().empty() ? std::string{} : emitter.Diagnostics().front().message);
        REQUIRE(emitter.Diagnostics().empty());
    }

    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto executable =
        std::filesystem::temp_directory_path() /
        ("rux-runtime-check-" + std::to_string(nonce) + (triple.Os() == Target::OS::Windows ? ".exe" : std::string{}));
    Linker linker(std::move(objects), "RuntimeCheckTest", {}, ArtifactKind::Executable, triple.Os(),
                  triple.Architecture());
    const bool linked = linker.Link(executable);
    CAPTURE(linker.Errors().empty() ? std::string{} : linker.Errors().front().message);
    REQUIRE(linked);
    const auto result = System::RunCaptured(executable);
    std::error_code error;
    std::filesystem::remove(executable, error);
    REQUIRE(result.has_value());
    return *result;
}

/// The exact report a trap with `message` in `function` at `line`:`column` prints.
std::string Report(const std::string_view message, const std::string_view function, const std::size_t line,
                   const std::size_t column) {
    return BuildRuntimeFailureLayout(RuntimeFailureKind::Panic, function, kLogicalPath, line, column).Join(message);
}

void CheckTrapsOnHost(const std::string_view source, const std::string_view expected) {
    for (const BuildProfile profile : {BuildProfile::Debug, BuildProfile::Release}) {
        const std::string profileName = profile == BuildProfile::Debug ? "debug" : "release";
        CAPTURE(profileName);
        const auto result = RunOnHost(source, profile);
        CHECK_EQ(result.output, expected);
        CHECK_NE(result.exitCode, 0);
    }
}

void CheckRunsOnHost(const std::string_view source, const int exitCode) {
    for (const BuildProfile profile : {BuildProfile::Debug, BuildProfile::Release}) {
        const std::string profileName = profile == BuildProfile::Debug ? "debug" : "release";
        CAPTURE(profileName);
        const auto result = RunOnHost(source, profile);
        CHECK_EQ(result.output, "");
        CHECK_EQ(result.exitCode, exitCode);
    }
}
} // namespace

TEST_CASE("integer division by zero stops the program with a panic") {
    CheckTrapsOnHost(R"(func Divide(a: int32, b: int32) -> int32 {
    return a / b;
}

func Main() -> int {
    return Divide(7, 0) as int;
}
)",
                     Report("division by zero", "Divide", 2, 14));

    CheckTrapsOnHost(R"(func Main() -> int {
    var count: uint8 = 9;
    var zero: uint8 = 0;
    count %= zero;
    return count as int;
}
)",
                     Report("division by zero", "Main", 4, 11));

    CheckTrapsOnHost(R"(func Remainder(a: int128, b: int128) -> int128 {
    return a % b;
}

func Main() -> int {
    return Remainder(5, 0) as int;
}
)",
                     Report("division by zero", "Remainder", 2, 14));
}

TEST_CASE("a signed minimum divided by minus one stops the program with a panic") {
    CheckTrapsOnHost(R"(func Divide(a: int64, b: int64) -> int64 {
    return a / b;
}

func Main() -> int {
    return Divide(-9223372036854775807 - 1, -1) as int;
}
)",
                     Report("division overflow", "Divide", 2, 14));

    CheckTrapsOnHost(R"(func Remainder(a: int8, b: int8) -> int8 {
    return a % b;
}

func Main() -> int {
    return Remainder(-128, -1) as int;
}
)",
                     Report("division overflow", "Remainder", 2, 14));
}

TEST_CASE("checked division computes ordinary quotients and remainders") {
    CheckRunsOnHost(R"(func Divide(a: int32, b: int32) -> int32 {
    return a / b;
}

func Remainder(a: int64, b: int64) -> int64 {
    return a % b;
}

func Main() -> int {
    var wide: int128 = -170141183460469231731687303715884105727i128 - 1;
    wide /= 2;
    if wide != -85070591730234615865843651857942052864i128 {
        return 1;
    }
    if Divide(-7, -1) != 7 || Divide(-2147483647 - 1, 1) != -2147483647 - 1 || Remainder(-7, 2) != -1 {
        return 2;
    }
    return 42;
}
)",
                    42);
}

TEST_CASE("division checks are omitted where the divisor's spelling settles them") {
    const LirPackage package = CompileChecks(R"(func Half(a: int32) -> int32 {
    return a / 2;
}

func Negate(a: int32) -> int32 {
    return a / -1;
}

func Spread(a: uint32, b: uint32) -> uint32 {
    return a / b;
}

func Split(a: int32, b: int32) -> int32 {
    return a % b;
}
)",
                                             BuildProfile::Debug);
    CHECK(Traps(package, "Half", "division by zero").empty());
    CHECK(Traps(package, "Half", "division overflow").empty());
    CHECK(Traps(package, "Negate", "division by zero").empty());
    CHECK_EQ(Traps(package, "Negate", "division overflow").size(), 1);
    CHECK_EQ(Traps(package, "Spread", "division by zero").size(), 1);
    CHECK(Traps(package, "Spread", "division overflow").empty());
    CHECK_EQ(Traps(package, "Split", "division by zero").size(), 1);
    CHECK_EQ(Traps(package, "Split", "division overflow").size(), 1);
}

TEST_CASE("release folds a division check whose divisor became constant") {
    const LirPackage package = CompileChecks(R"(func Quarter(a: int32) -> int32 {
    let k: int32 = 4;
    return a % k;
}
)",
                                             BuildProfile::Release);
    CHECK(Traps(package, "Quarter", "division by zero").empty());
    CHECK(Traps(package, "Quarter", "division overflow").empty());
}

TEST_CASE("AArch64 emits the division checks as runtime failures") {
    for (const Target::OS os : {Target::OS::Linux, Target::OS::MacOS, Target::OS::Windows}) {
        const auto triple = Target::TargetTriple::From(os, Target::Arch::AArch64);
        REQUIRE(triple.has_value());
        const LirPackage package = CompileChecks(R"(func Divide(a: int64, b: int64) -> int64 {
    return a / b;
}
)",
                                                 BuildProfile::Debug, *triple);
        const auto zero = Traps(package, "Divide", "division by zero");
        REQUIRE_EQ(zero.size(), 1);
        CHECK_EQ(zero.front()->sourceFile, kLogicalPath);
        CHECK_EQ(zero.front()->sourceFunction, "Divide");
        CHECK_EQ(zero.front()->sourceLine, 2);
        CHECK_EQ(Traps(package, "Divide", "division overflow").size(), 1);
        AArch64RcuEmitter emitter(package, "RuntimeCheckTest", os);
        const auto objects = emitter.Generate();
        CAPTURE(emitter.Diagnostics().empty() ? std::string{} : emitter.Diagnostics().front().message);
        CHECK(emitter.Diagnostics().empty());
        CHECK_FALSE(objects.empty());
    }
}
