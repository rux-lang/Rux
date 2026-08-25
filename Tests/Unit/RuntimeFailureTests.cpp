#include "CodeGen/AArch64/RcuEmitter.h"
#include "CodeGen/RuntimeFailure.h"
#include "CodeGen/X86_64/RcuEmitter.h"
#include "Driver/BuildTarget.h"
#include "Ir/Lir/Lir.h"
#include "Lexer/Lexer.h"
#include "Linker/Linker.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"
#include "Target/TargetTriple.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using namespace Rux;

namespace {
constexpr std::string_view kRuntimePrelude = "struct Slice<T> { data: *T; length: uint; }\n"
                                             "intrinsic func Assert(condition: bool, message: Slice<char8>);\n"
                                             "intrinsic func Panic(message: Slice<char8>);\n";

LirPackage CompileRuntimeFailures(const Target::OS os, const Target::Arch arch) {
    const auto triple = Target::TargetTriple::From(os, arch);
    REQUIRE(triple.has_value());

    CompileTimeContext context;
    context.target = Driver::TargetContextForTriple(*triple);
    context.targetTriple = triple->CanonicalName();
    context.sourceRoot = "Project";

    Lexer lexer(std::string(kRuntimePrelude) + R"(
struct Reporter { value: int; }

extend Reporter {
    func AssertNow(self: &Reporter) {
        Assert(false, "");
    }

    func PanicNow(self: &Reporter) {
        Panic("Помилка 🚨");
    }
}

func Main() -> int {
    Assert(false, "");
    Panic("Помилка 🚨");
}
)",
                "Project/Src/Runtime.rux");
    auto lexed = lexer.Tokenize();
    CAPTURE(lexed.diagnostics.empty() ? std::string{} : lexed.diagnostics.front().message);
    REQUIRE_FALSE(lexed.HasErrors());

    Parser parser(std::move(lexed.tokens), "Project/Src/Runtime.rux");
    auto parsed = parser.Parse();
    CAPTURE(parsed.diagnostics.empty() ? std::string{} : parsed.diagnostics.front().message);
    REQUIRE_FALSE(parsed.HasErrors());

    SemanticAnalyzer analyzer({&parsed.module}, {}, "RuntimeFailureTest", context);
    auto model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());

    AstToHirLowering hirLowering(model);
    HirToLirLowering lirLowering(hirLowering.Generate(), context.target);
    return lirLowering.Generate();
}

const LirInstr *FindFailure(const LirPackage &package, const LirOpcode opcode, const std::string_view functionName) {
    for (const auto &module : package.modules) {
        for (const auto &function : module.funcs) {
            for (const auto &block : function.blocks) {
                const auto found = std::ranges::find(block.instrs, opcode, &LirInstr::op);
                if (found != block.instrs.end() && found->sourceFunction == functionName) {
                    return &*found;
                }
            }
        }
    }
    return nullptr;
}

bool ContainsText(const std::vector<std::uint8_t> &bytes, const std::string_view text) {
    const std::string_view image(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    return image.contains(text);
}

std::vector<std::uint8_t> LinkRuntimeFailures(const Target::OS os, const Target::Arch arch) {
    auto package = CompileRuntimeFailures(os, arch);
    std::vector<RcuFile> objects;
    if (arch == Target::Arch::X86_64) {
        RcuEmitter emitter(package, "RuntimeFailureTest", os);
        objects = emitter.Generate();
        CAPTURE(emitter.Diagnostics().empty() ? std::string{} : emitter.Diagnostics().front().message);
        REQUIRE(emitter.Diagnostics().empty());
    }
    else {
        AArch64RcuEmitter emitter(package, "RuntimeFailureTest", os);
        objects = emitter.Generate();
        CAPTURE(emitter.Diagnostics().empty() ? std::string{} : emitter.Diagnostics().front().message);
        REQUIRE(emitter.Diagnostics().empty());
    }

    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto output = std::filesystem::temp_directory_path() / ("rux-runtime-failure-" + std::to_string(nonce) +
                                                                  (os == Target::OS::Windows ? ".exe" : std::string{}));
    std::error_code fileError;
    std::filesystem::remove(output, fileError);
    Linker linker(std::move(objects), "RuntimeFailureTest", {}, ArtifactKind::Executable, os, arch);
    const bool linked = linker.Link(output);
    CAPTURE(linker.Errors().empty() ? std::string{} : linker.Errors().front().message);
    REQUIRE(linked);

    std::ifstream stream(output, std::ios::binary);
    REQUIRE(stream.is_open());
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    stream.close();
    std::filesystem::remove(output, fileError);
    return bytes;
}
} // namespace

TEST_CASE("runtime failure layout is exact for empty and Unicode messages") {
    const RuntimeFailureLayout assertion =
        BuildRuntimeFailureLayout(RuntimeFailureKind::Assertion, "Main", "Src/Main.rux", 12, 5);
    CHECK_EQ(assertion.Join(""), "Assertion failed: \n  at Main (Src/Main.rux:12:5)\n");

    const RuntimeFailureLayout panic =
        BuildRuntimeFailureLayout(RuntimeFailureKind::Panic, "Worker::Stop", "Src/помилка.rux", 27, 9);
    CHECK_EQ(panic.Join("Помилка 🚨"), "Panic: Помилка 🚨\n  at Worker::Stop (Src/помилка.rux:27:9)\n");
    CHECK_FALSE(panic.Join("Помилка 🚨").contains('\r'));

    const RuntimeFailureLayout unknown = BuildRuntimeFailureLayout(RuntimeFailureKind::Panic, "", "", 0, 0);
    CHECK_EQ(unknown.Join("lost context"), "Panic: lost context\n  at <unknown> (<unknown>:0:0)\n");
}

TEST_CASE("runtime failure lowering preserves qualified function, logical path, line, and column") {
    const LirPackage package = CompileRuntimeFailures(Target::OS::Linux, Target::Arch::X86_64);
    const LirInstr *assertion = FindFailure(package, LirOpcode::Assert, "Reporter::AssertNow");
    const LirInstr *panic = FindFailure(package, LirOpcode::Panic, "Reporter::PanicNow");
    REQUIRE(assertion != nullptr);
    REQUIRE(panic != nullptr);

    CHECK_EQ(assertion->sourceFunction, "Reporter::AssertNow");
    CHECK_EQ(panic->sourceFunction, "Reporter::PanicNow");
    CHECK_EQ(assertion->sourceFile, "Src/Runtime.rux");
    CHECK_EQ(panic->sourceFile, "Src/Runtime.rux");
    CHECK_EQ(assertion->sourceLine, 9);
    CHECK_EQ(assertion->sourceColumn, 15);
    CHECK_EQ(panic->sourceLine, 13);
    CHECK_EQ(panic->sourceColumn, 14);
}

TEST_CASE("ELF, PE, and Mach-O images preserve runtime failure text on both backends") {
    for (const Target::OS os : {Target::OS::FreeBSD, Target::OS::Linux, Target::OS::Windows, Target::OS::MacOS}) {
        for (const Target::Arch arch : {Target::Arch::X86_64, Target::Arch::AArch64}) {
            CAPTURE(Target::ToString(os));
            CAPTURE(Target::ToString(arch));
            const auto bytes = LinkRuntimeFailures(os, arch);
            CHECK(ContainsText(bytes, "Assertion failed: "));
            CHECK(ContainsText(bytes, "Panic: "));
            CHECK(ContainsText(bytes, "Помилка 🚨"));
            CHECK(ContainsText(bytes, "\n  at Main (Src/Runtime.rux:18:11)\n"));
            CHECK(ContainsText(bytes, "\n  at Main (Src/Runtime.rux:19:10)\n"));
        }
    }
}
