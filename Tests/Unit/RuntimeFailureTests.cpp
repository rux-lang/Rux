#include "CodeGen/AArch64/RcuEmitter.h"
#include "CodeGen/RuntimeFailure.h"
#include "CodeGen/X86_64/AssemblyPrinter.h"
#include "CodeGen/X86_64/Encoder.h"
#include "CodeGen/X86_64/RcuEmitter.h"
#include "Driver/BuildTarget.h"
#include "Ir/Lir/Lir.h"
#include "Lexer/Lexer.h"
#include "Linker/Linker.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"
#include "System/Process.h"
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
constexpr std::string_view kRuntimePrelude = ""
                                             "intrinsic func Assert(condition: bool, message: char8[..]);\n"
                                             "intrinsic func Panic(message: char8[..]);\n";

LirPackage CompileRuntimeFailures(const Target::OS os, const Target::Arch arch, const std::string_view source = {}) {
    const auto triple = Target::TargetTriple::From(os, arch);
    REQUIRE(triple.has_value());

    CompileTimeContext context;
    context.target = Driver::TargetContextForTriple(*triple);
    context.targetTriple = triple->CanonicalName();
    context.sourceRoot = "Project";

    const std::string_view defaultSource = R"(
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
)";
    Lexer lexer(std::string(kRuntimePrelude) + std::string(source.empty() ? defaultSource : source),
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

// A conforming callee may reuse its incoming stack arguments. Forward to the real WriteFile, then overwrite the
// fifth argument's slot. A subsequent call must supply null again, independently of the previous call's stack.
RcuFile StackArgumentClobberingWriter() {
    RcuFile object;
    object.arch = RcuArch::X86_64;
    object.sourcePath = "StackArgumentClobberingWriter";
    RcuSection section;
    section.name = ".text";
    section.type = RcuSecType::Text;
    section.flags = RcuSecFlag::Alloc | RcuSecFlag::Exec | RcuSecFlag::Read;
    section.alignment = 16;
    X64Enc encoder(section.data);
    // cmp qword [rsp + 40], 0: the fifth argument follows the return address and four home slots.
    for (const std::uint8_t byte : {0x48, 0x83, 0x7c, 0x24, 0x28, 0x00})
        encoder.Byte(byte);
    std::uint32_t invalidArgument;
    encoder.Jnz(invalidArgument);
    encoder.SubRspImm32(56);
    encoder.MovQwordRspImm32(32, 0);
    std::uint32_t writeCall;
    encoder.Call(writeCall);
    section.relocs.push_back({writeCall, 1, RcuRelType::Rel32, 0});
    encoder.AddRspImm32(56);
    encoder.MovQwordRspImm32(40, 1);
    encoder.Ret();
    encoder.Patch32(invalidArgument, static_cast<std::int32_t>(encoder.Size() - invalidArgument - 4));
    encoder.MovEaxImm32(92);
    encoder.MovArgWin64Rax(0);
    encoder.SubRspImm32(40);
    std::uint32_t exitCall;
    encoder.Call(exitCall);
    section.relocs.push_back({exitCall, 2, RcuRelType::Rel32, 0});
    encoder.Ud2();
    object.symbols = {
        {"StackArgumentClobberingWriter", "", 0, encoder.Size(), RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global},
        {"WriteFile", "KERNEL32.DLL", 0, 0, RCU_SEC_EXTERNAL, RcuSymKind::ExternFunc, RcuSymVis::Global},
        {"ExitProcess", "KERNEL32.DLL", 0, 0, RCU_SEC_EXTERNAL, RcuSymKind::ExternFunc, RcuSymVis::Global}};
    object.sections.push_back(std::move(section));
    return object;
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
    CHECK_EQ(assertion->sourceLine, 8);
    CHECK_EQ(assertion->sourceColumn, 15);
    CHECK_EQ(panic->sourceLine, 12);
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
            CHECK(ContainsText(bytes, "\n  at Main (Src/Runtime.rux:17:11)\n"));
            CHECK(ContainsText(bytes, "\n  at Main (Src/Runtime.rux:18:10)\n"));
        }
    }
}

TEST_CASE("Windows runtime failures renew stack arguments between output writes") {
    if constexpr (Target::HostOS != Target::OS::Windows || Target::HostArch != Target::Arch::X86_64)
        return;
    for (const std::string_view statement : {"Panic(\"first line\\nsecond line\");", "Assert(false, \"\");"}) {
        CAPTURE(statement);
        const auto package = CompileRuntimeFailures(Target::OS::Windows, Target::Arch::X86_64,
                                                    "func Main() -> int { " + std::string(statement) + " return 0; }");
        RcuEmitter emitter(package, "RuntimeFailureTest", Target::OS::Windows);
        auto objects = emitter.Generate();
        REQUIRE(emitter.Diagnostics().empty());
        std::size_t replacements = 0;
        for (auto &object : objects) {
            for (auto &symbol : object.symbols) {
                if (symbol.name == "WriteFile" && symbol.kind == RcuSymKind::ExternFunc) {
                    symbol.name = "StackArgumentClobberingWriter";
                    symbol.typeName.clear();
                    symbol.kind = RcuSymKind::Func;
                    ++replacements;
                }
            }
        }
        REQUIRE(replacements > 0);
        objects.push_back(StackArgumentClobberingWriter());
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto executable =
            std::filesystem::temp_directory_path() / ("rux-runtime-stack-arguments-" + std::to_string(nonce) + ".exe");
        Linker linker(std::move(objects), "RuntimeFailureTest", {}, ArtifactKind::Executable, Target::OS::Windows,
                      Target::Arch::X86_64);
        REQUIRE(linker.Link(executable));
        const auto result = System::RunCaptured(executable);
        std::error_code error;
        std::filesystem::remove(executable, error);
        CHECK(!error);
        REQUIRE(result.has_value());
        CAPTURE(result->output);
        CHECK(result->exitCode == static_cast<int>(0xc000001dU));
        const bool panic = statement.starts_with("Panic");
        const auto layout = BuildRuntimeFailureLayout(panic ? RuntimeFailureKind::Panic : RuntimeFailureKind::Assertion,
                                                      "Main", "Src/Runtime.rux", 3, panic ? 27 : 28);
        CHECK(result->output == layout.Join(panic ? "first line\nsecond line" : ""));
    }
}

TEST_CASE("Windows assembly supplies the overlapped argument for every runtime failure write") {
    for (const std::string_view statement : {"Panic(\"message\");", "Assert(false, \"\");"}) {
        const auto package = CompileRuntimeFailures(Target::OS::Windows, Target::Arch::X86_64,
                                                    "func Main() -> int { " + std::string(statement) + " return 0; }");
        const auto assembly = AssemblyPrinter(package, Target::OS::Windows).Generate();
        std::size_t begin = 0;
        std::size_t writes = 0;
        for (auto call = assembly.find("call    WriteFile"); call != std::string::npos;
             call = assembly.find("call    WriteFile", begin)) {
            const std::string_view arguments(assembly.data() + begin, call - begin);
            CHECK(arguments.contains("mov     qword [rsp + 32], 0"));
            CHECK(arguments.contains("lea     r9, [rsp + 40]"));
            begin = call + 1;
            ++writes;
        }
        CHECK(writes == 3);
    }
}
