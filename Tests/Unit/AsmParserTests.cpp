// Inline-assembly front end: the per-architecture register and mnemonic
// tables, and the operand shapes the parser reads for each architecture.

#include "Lexer/Lexer.h"
#include "Syntax/Parser/Parser.h"
#include "Target/AsmInstr.h"
#include "Target/AsmRegisters.h"

#include <doctest.h>
#include <string>
#include <vector>

using namespace Rux;

namespace {

// Parse one source file for `arch` and hand back the body of its first
// `asm func`. Parse diagnostics fail the test where they appear, since every
// body below is meant to be read without complaint.
std::vector<AsmInstr> ParseAsmBody(const std::string &source, const Target::Arch arch) {
    Lexer lexer(source, "asm.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());

    Parser parser(std::move(lexed.tokens), "asm.rux", arch);
    auto parsed = parser.Parse();
    for (const auto &diag : parsed.diagnostics) {
        INFO("unexpected diagnostic: ", diag.message);
        CHECK(diag.severity != Diagnostic::Severity::Error);
    }
    for (const auto &item : parsed.module.items) {
        if (const auto *func = dynamic_cast<const FuncDecl *>(item.get()); func != nullptr && func->isAsm) {
            return func->asmBody;
        }
    }
    FAIL("no asm func in the parsed module");
    return {};
}

} // namespace

TEST_CASE("Asm register tables are selected by architecture") {
    CHECK(IsRegisterName(Target::Arch::X86_64, "rax"));
    CHECK_FALSE(IsRegisterName(Target::Arch::AArch64, "rax"));
    CHECK(IsRegisterName(Target::Arch::AArch64, "x0"));
    CHECK_FALSE(IsRegisterName(Target::Arch::X86_64, "x0"));
    // `sp` names a register on both, and a different one on each.
    CHECK(LookupRegister(Target::Arch::X86_64, "sp").size == 2);
    CHECK(LookupRegister(Target::Arch::AArch64, "sp").size == 8);

    SUBCASE("x86-64 keeps its encoding quirks") {
        CHECK(LookupRegister(Target::Arch::X86_64, "sil").rexRequired);
        CHECK(LookupRegister(Target::Arch::X86_64, "ah").high8);
        CHECK(LookupRegister(Target::Arch::X86_64, "ah").code == 4);
        CHECK(LookupRegister(Target::Arch::X86_64, "xmm9").IsVector());
        CHECK(LookupRegister(Target::Arch::X86_64, "xmm9").code == 9);
        CHECK(LookupRegister(Target::Arch::X86_64, "r13d").size == 4);
    }

    SUBCASE("AArch64 distinguishes the two readings of code 31") {
        CHECK(LookupRegister(Target::Arch::AArch64, "xzr").code == 31);
        CHECK_FALSE(LookupRegister(Target::Arch::AArch64, "xzr").stackPointer);
        CHECK(LookupRegister(Target::Arch::AArch64, "sp").code == 31);
        CHECK(LookupRegister(Target::Arch::AArch64, "sp").stackPointer);
        CHECK(LookupRegister(Target::Arch::AArch64, "wsp").stackPointer);
        CHECK(LookupRegister(Target::Arch::AArch64, "wsp").size == 4);
    }

    SUBCASE("AArch64 names the scalar views of the vector file") {
        CHECK(LookupRegister(Target::Arch::AArch64, "d7").IsVector());
        CHECK(LookupRegister(Target::Arch::AArch64, "d7").size == 8);
        CHECK(LookupRegister(Target::Arch::AArch64, "s7").size == 4);
        CHECK(LookupRegister(Target::Arch::AArch64, "q31").size == 16);
        CHECK(LookupRegister(Target::Arch::AArch64, "b0").size == 1);
        // The role names AAPCS64 gives two of the general-purpose registers.
        CHECK(LookupRegister(Target::Arch::AArch64, "lr").code == 30);
        CHECK(LookupRegister(Target::Arch::AArch64, "fp").code == 29);
        // Bare V names take an element qualifier the encoder has no use for.
        CHECK_FALSE(IsRegisterName(Target::Arch::AArch64, "v0"));
        CHECK_FALSE(IsRegisterName(Target::Arch::AArch64, "x31"));
    }
}

TEST_CASE("Asm mnemonics name the architecture they belong to") {
    CHECK(IsAsmMnemonic(Target::Arch::X86_64, "syscall"));
    CHECK(IsAsmMnemonic(Target::Arch::X86_64, "jz"));
    CHECK(IsAsmMnemonic(Target::Arch::X86_64, "setne"));
    CHECK_FALSE(IsAsmMnemonic(Target::Arch::X86_64, "jqq"));
    CHECK(IsAsmMnemonic(Target::Arch::AArch64, "ldrsw"));
    CHECK(IsAsmMnemonic(Target::Arch::AArch64, "b.eq"));
    CHECK_FALSE(IsAsmMnemonic(Target::Arch::AArch64, "b.qq"));
    CHECK_FALSE(IsAsmMnemonic(Target::Arch::AArch64, "mov.eq"));

    SUBCASE("a mnemonic both architectures have belongs to neither") {
        CHECK(AsmMnemonicArch("add") == Target::Arch::Unknown);
        CHECK(AsmMnemonicArch("ret") == Target::Arch::Unknown);
        CHECK(AsmMnemonicArch("mov") == Target::Arch::Unknown);
        CHECK(AsmMnemonicArch("nop") == Target::Arch::Unknown);
        // Neither has it either: a misspelling is nobody's instruction.
        CHECK(AsmMnemonicArch("mvo") == Target::Arch::Unknown);
    }

    SUBCASE("a mnemonic only one has names it") {
        CHECK(AsmMnemonicArch("push") == Target::Arch::X86_64);
        CHECK(AsmMnemonicArch("syscall") == Target::Arch::X86_64);
        CHECK(AsmMnemonicArch("imul") == Target::Arch::X86_64);
        CHECK(AsmMnemonicArch("ldr") == Target::Arch::AArch64);
        CHECK(AsmMnemonicArch("stp") == Target::Arch::AArch64);
        CHECK(AsmMnemonicArch("svc") == Target::Arch::AArch64);
        CHECK(AsmMnemonicArch("b.eq") == Target::Arch::AArch64);
    }

    SUBCASE("an instruction neither back end encodes is still recognized") {
        // Otherwise it would be reported as foreign rather than unsupported.
        CHECK(AsmMnemonicArch("fadd") == Target::Arch::Unknown); // x87 and AArch64 alike
        CHECK(AsmMnemonicArch("adc") == Target::Arch::Unknown);
        CHECK(AsmMnemonicArch("hlt") == Target::Arch::Unknown);
    }
}

TEST_CASE("Parser reads AArch64 asm operand syntax") {
    const auto body = ParseAsmBody(R"(
asm func Probe(p: *uint8) -> int64 {
    add x0, x1, x2
    add x0, x1, x2, lsl #3
    mov w3, #-5
    add x0, x0, #1, lsl #12
    ldr x1, [x0]
    ldr x2, [x0, #8]
    ldr x3, [x0, x1, lsl #3]
    ldr w4, [x0, w1, uxtw #2]
    ldr x5, [x0], #16
    str x5, [sp, #-16]!
    csel x0, x1, x2, ge
    b.ne done
    svc #0
    ret
done:
    ret
}
)",
                                   Target::Arch::AArch64);
    REQUIRE(body.size() == 16);
    for (const auto &instr : body) {
        CHECK(instr.arch == Target::Arch::AArch64);
    }

    SUBCASE("registers come from the AArch64 table") {
        REQUIRE(body[0].operands.size() == 3);
        CHECK(body[0].mnemonic == "add");
        CHECK(body[0].operands[0].kind == AsmOperand::Kind::Reg);
        CHECK(body[0].operands[2].name == "x2");
        CHECK(body[0].operands[2].shift == AsmShiftKind::None);
    }

    SUBCASE("a shifted register carries its shift") {
        const auto &op = body[1].operands[2];
        CHECK(op.kind == AsmOperand::Kind::Reg);
        CHECK(op.name == "x2");
        CHECK(op.shift == AsmShiftKind::Lsl);
        CHECK(op.shiftAmount == 3);
    }

    SUBCASE("immediates are written with '#'") {
        REQUIRE(body[2].operands.size() == 2);
        CHECK(body[2].operands[1].kind == AsmOperand::Kind::Imm);
        CHECK(body[2].operands[1].imm == -5);
        // `#1, LSL #12` is one operand, not two.
        REQUIRE(body[3].operands.size() == 3);
        CHECK(body[3].operands[2].imm == 1);
        CHECK(body[3].operands[2].shift == AsmShiftKind::Lsl);
        CHECK(body[3].operands[2].shiftAmount == 12);
    }

    SUBCASE("memory operands take every addressing form") {
        const auto &plain = body[4].operands[1];
        CHECK(plain.kind == AsmOperand::Kind::Mem);
        CHECK(plain.memBase == "x0");
        CHECK(plain.imm == 0);
        CHECK(plain.indexMode == AsmIndexMode::Offset);

        const auto &offset = body[5].operands[1];
        CHECK(offset.memBase == "x0");
        CHECK(offset.imm == 8);

        const auto &shifted = body[6].operands[1];
        CHECK(shifted.memBase == "x0");
        CHECK(shifted.memIndex == "x1");
        CHECK(shifted.shift == AsmShiftKind::Lsl);
        CHECK(shifted.shiftAmount == 3);

        const auto &extended = body[7].operands[1];
        CHECK(extended.memBase == "x0");
        CHECK(extended.memIndex == "w1");
        CHECK(extended.extend == AsmExtendKind::Uxtw);
        CHECK(extended.shiftAmount == 2);

        const auto &post = body[8].operands[1];
        CHECK(post.memBase == "x0");
        CHECK(post.imm == 16);
        CHECK(post.indexMode == AsmIndexMode::PostIndex);

        const auto &pre = body[9].operands[1];
        CHECK(pre.memBase == "sp");
        CHECK(pre.imm == -16);
        CHECK(pre.indexMode == AsmIndexMode::PreIndex);
    }

    SUBCASE("conditions are read as a suffix and as an operand") {
        CHECK(body[10].mnemonic == "csel");
        REQUIRE(body[10].operands.size() == 4);
        CHECK(body[10].operands[3].kind == AsmOperand::Kind::Sym);
        CHECK(body[10].operands[3].name == "ge");
        CHECK(body[11].mnemonic == "b.ne");
        REQUIRE(body[11].operands.size() == 1);
        CHECK(body[11].operands[0].name == "done");
    }

    SUBCASE("an instruction with no operands ends where the next one starts") {
        CHECK(body[12].mnemonic == "svc");
        REQUIRE(body[12].operands.size() == 1);
        CHECK(body[12].operands[0].imm == 0);
        CHECK(body[13].mnemonic == "ret");
        CHECK(body[13].operands.empty());
        CHECK(body[14].labelDef == "done");
        CHECK(body[15].mnemonic == "ret");
    }

    SUBCASE("RET keeps an explicit link register") {
        const auto explicitRet = ParseAsmBody(R"(
asm func Probe() -> int64 {
    ret x1
    nop
    ret lr
}
)",
                                              Target::Arch::AArch64);
        REQUIRE(explicitRet.size() == 3);
        REQUIRE(explicitRet[0].operands.size() == 1);
        CHECK(explicitRet[0].operands[0].name == "x1");
        CHECK(explicitRet[1].mnemonic == "nop");
        CHECK(explicitRet[1].operands.empty());
        REQUIRE(explicitRet[2].operands.size() == 1);
        CHECK(explicitRet[2].operands[0].name == "lr");
    }
}

TEST_CASE("Parser reads x86-64 asm operand syntax unchanged") {
    const auto body = ParseAsmBody(R"(
asm func Probe(a: int64) -> int64 {
    mov rax, rcx
    mov qword [rsp - 8], rcx
    lea rax, [rbp + rax*4 - 8]
    shl rax, 1
    jz done
    ret
done:
    ret
}
)",
                                   Target::Arch::X86_64);
    REQUIRE(body.size() == 8);
    for (const auto &instr : body) {
        CHECK(instr.arch == Target::Arch::X86_64);
    }

    CHECK(body[0].operands[0].kind == AsmOperand::Kind::Reg);
    CHECK(body[0].operands[1].name == "rcx");

    const auto &mem = body[1].operands[0];
    CHECK(mem.kind == AsmOperand::Kind::Mem);
    CHECK(mem.memBase == "rsp");
    CHECK(mem.imm == -8);
    CHECK(mem.memSize == 8);
    CHECK(mem.indexMode == AsmIndexMode::Offset);

    const auto &scaled = body[2].operands[1];
    CHECK(scaled.memBase == "rbp");
    CHECK(scaled.memIndex == "rax");
    CHECK(scaled.memScale == 4);
    CHECK(scaled.imm == -8);

    // A bare integer is an immediate on x86-64, where AArch64 writes '#'.
    CHECK(body[3].operands[1].kind == AsmOperand::Kind::Imm);
    CHECK(body[3].operands[1].imm == 1);
    CHECK(body[4].operands[0].kind == AsmOperand::Kind::Sym);
    CHECK(body[5].operands.empty());
    CHECK(body[6].labelDef == "done");
}
