#include "CodeGen/AArch64/Assembler.h"
#include "CodeGen/AArch64/RcuEmitter.h"
#include "CodeGen/X86_64/Assembler.h"
#include "CodeGen/X86_64/RcuEmitter.h"
#include "Ir/Lir/Lir.h"

#include <cstdint>
#include <doctest.h>
#include <string>
#include <vector>

using namespace Rux;

namespace {
[[nodiscard]] AsmInstr Instruction(std::string mnemonic, const Target::Arch arch) {
    AsmInstr instruction;
    instruction.location = {.line = 3, .column = 7};
    instruction.mnemonic = std::move(mnemonic);
    instruction.arch = arch;
    return instruction;
}

[[nodiscard]] const Diagnostic &OnlyDiagnostic(const AsmAssembly &assembled) {
    REQUIRE(assembled.diagnostics.size() == 1);
    return assembled.diagnostics.front();
}

[[nodiscard]] LirPackage PackageWithOpcode(const LirOpcode opcode) {
    LirInstr instruction;
    instruction.op = opcode;

    LirBlock block;
    block.label = "entry";
    block.instrs.push_back(std::move(instruction));
    block.term.emplace();
    block.term->kind = LirTermKind::Unreachable;

    LirFunc function;
    function.name = "Main";
    function.returnType = TypeRef::MakeInt32();
    function.blocks.push_back(std::move(block));

    LirModule module;
    module.name = "Backend.rux";
    module.funcs.push_back(std::move(function));

    LirPackage package;
    package.modules.push_back(std::move(module));
    return package;
}
} // namespace

TEST_CASE("assemblers distinguish foreign instructions and offer only known equivalents") {
    std::vector<std::uint8_t> code;
    const std::vector aarch64Instructions = {Instruction("imul", Target::Arch::AArch64)};
    const auto aarch64 = AssembleAArch64AsmFunc(aarch64Instructions, "Foreign.rux", code, Target::OS::Linux);
    const auto &aarch64Diagnostic = OnlyDiagnostic(aarch64);
    CHECK(aarch64Diagnostic.message == "instruction 'imul' is not available for target 'linux-aarch64'");
    REQUIRE(aarch64Diagnostic.notes.size() == 1);
    CHECK(aarch64Diagnostic.notes.front() == "'imul' is an x86-64 instruction");
    REQUIRE(aarch64Diagnostic.help.has_value());
    CHECK(*aarch64Diagnostic.help == "use 'mul' for integer multiplication on AArch64");
    CHECK(aarch64Diagnostic.sourceName == "Foreign.rux");
    CHECK(aarch64Diagnostic.location.line == 3);

    code.clear();
    const std::vector x86Instructions = {Instruction("svc", Target::Arch::X86_64)};
    const auto x86 = AssembleAsmFunc(x86Instructions, "Foreign.rux", code, Target::OS::Windows);
    const auto &x86Diagnostic = OnlyDiagnostic(x86);
    CHECK(x86Diagnostic.message == "instruction 'svc' is not available for target 'windows-x86_64'");
    REQUIRE(x86Diagnostic.help.has_value());
    CHECK(*x86Diagnostic.help == "use 'syscall' to request an operating-system service on x86-64");

    code.clear();
    const std::vector noEquivalent = {Instruction("paciasp", Target::Arch::X86_64)};
    const auto withoutHelp = AssembleAsmFunc(noEquivalent, "Foreign.rux", code, Target::OS::Linux);
    CHECK(OnlyDiagnostic(withoutHelp).message == "instruction 'paciasp' is not available for target 'linux-x86_64'");
    CHECK_FALSE(OnlyDiagnostic(withoutHelp).help.has_value());
}

TEST_CASE("assemblers distinguish known limitations from unknown and malformed assembly") {
    std::vector<std::uint8_t> code;
    const std::vector unsupported = {Instruction("bsf", Target::Arch::X86_64)};
    const auto known = AssembleAsmFunc(unsupported, "Known.rux", code, Target::OS::FreeBSD);
    const auto &knownDiagnostic = OnlyDiagnostic(known);
    CHECK(knownDiagnostic.message == "instruction 'bsf' is recognized for target 'freebsd-x86_64' but is not "
                                     "implemented by its assembler");
    REQUIRE(knownDiagnostic.notes.size() == 1);
    CHECK(knownDiagnostic.notes.front().contains("internal compiler limitation"));

    code.clear();
    const std::vector unknown = {Instruction("qwerty", Target::Arch::X86_64)};
    CHECK(OnlyDiagnostic(AssembleAsmFunc(unknown, "Unknown.rux", code)).message == "unknown instruction 'qwerty'");

    AsmInstr add = Instruction("add", Target::Arch::X86_64);
    AsmOperand destination;
    destination.kind = AsmOperand::Kind::Reg;
    destination.name = "rax";
    add.operands.push_back(std::move(destination));
    AsmOperand immediate;
    immediate.kind = AsmOperand::Kind::Imm;
    immediate.imm = 2'147'483'648;
    add.operands.push_back(std::move(immediate));
    code.clear();
    const auto malformed = AssembleAsmFunc(std::vector{add}, "Range.rux", code);
    const auto &range = OnlyDiagnostic(malformed);
    CHECK(range.message == "immediate 2147483648 is outside the signed 32-bit encoding range for 'add'");
    REQUIRE(range.notes.size() == 1);
    CHECK(range.notes.front() == "supported range: -2147483648 to 2147483647");
    CHECK(code.empty());
}

TEST_CASE("assemblers reject a body parsed for a different selected architecture") {
    std::vector<std::uint8_t> code;
    const std::vector instructions = {Instruction("add", Target::Arch::AArch64)};
    const auto assembled = AssembleAsmFunc(instructions, "Mismatch.rux", code, Target::OS::MacOS);
    const auto &diagnostic = OnlyDiagnostic(assembled);
    CHECK(diagnostic.message == "inline assembly parsed for AArch64 cannot be emitted for target 'macos-x86_64'");
    REQUIRE(diagnostic.help.has_value());
    CHECK(code.empty());
}

TEST_CASE("both RCU backends diagnose an unknown LIR opcode with the selected target") {
    constexpr auto unknown = static_cast<LirOpcode>(999);
    const LirPackage package = PackageWithOpcode(unknown);

    RcuEmitter x86(package, "test", Target::OS::Windows);
    (void)x86.Generate();
    REQUIRE(x86.Diagnostics().size() == 1);
    CHECK(x86.Diagnostics().front().message ==
          "cannot generate unknown LIR opcode 999 for target 'windows-x86_64' in function 'Main'");
    REQUIRE(x86.Diagnostics().front().help.has_value());

    AArch64RcuEmitter aarch64(package, "test", Target::OS::FreeBSD);
    (void)aarch64.Generate();
    REQUIRE(aarch64.Diagnostics().size() == 1);
    CHECK(aarch64.Diagnostics().front().message ==
          "cannot generate unknown LIR opcode 999 for target 'freebsd-aarch64' in function 'Main'");
    CHECK(aarch64.Diagnostics().front().notes == x86.Diagnostics().front().notes);
}
