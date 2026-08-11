// AArch64 assembler for `asm func` bodies: the encoding each written
// instruction selects, the branches resolved inside the body, and the
// references left for the linker to resolve.
//
// Every expected word below was assembled by `clang --target=aarch64-linux-gnu`
// and read back with llvm-objdump, so a disagreement here is a disagreement
// with a second implementation rather than with someone's reading of the
// manual. The encoders themselves are verified against their own vector table
// in AArch64EncoderTests.cpp; what these cases pin down is the choice between
// them — which of the three `ADD`s an operand list means, and which of the five
// `LDR`s.

#include "CodeGen/AArch64/Assembler.h"
#include "Lexer/Lexer.h"
#include "Object/Rcu/Rcu.h"
#include "Syntax/Parser/Parser.h"

#include <cstddef>
#include <cstdint>
#include <doctest.h>
#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using namespace Rux;

namespace {
// The machine code one body assembled to, read back a word at a time.
struct Assembled {
    // Every AArch64 instruction is this many bytes wide, with no exceptions.
    static constexpr std::size_t InstrSize = 4;

    std::vector<std::uint8_t> code;
    AsmAssembly result;

    [[nodiscard]] std::size_t Words() const {
        return code.size() / InstrSize;
    }

    [[nodiscard]] std::uint32_t Word(const std::size_t index) const {
        std::uint32_t word = 0;
        for (std::size_t i = 0; i < InstrSize; ++i) {
            word |= static_cast<std::uint32_t>(code[index * InstrSize + i]) << (i * 8U);
        }
        return word;
    }
};

// An instruction word as a table spells it: doctest reports an integer in
// decimal, which says nothing at a glance against a column of hexadecimal.
[[nodiscard]] std::string HexWord(const std::uint32_t word) {
    return std::format("0x{:08X}", word);
}

// Assemble `body` as the whole of an `asm func` compiled for AArch64. The body
// goes through the real lexer and parser rather than through hand-built
// operands, since which shape an operand arrives in is half of what the
// assembler is being asked here.
[[nodiscard]] Assembled Assemble(const std::string_view body) {
    const std::string source = std::format("asm func Probe() {{\n{}\n}}\n", body);
    Lexer lexer(source, "asm.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());

    Parser parser(std::move(lexed.tokens), "asm.rux", Target::Arch::AArch64);
    auto parsed = parser.Parse();
    for (const auto &diag : parsed.diagnostics) {
        INFO("unexpected parse diagnostic: ", diag.message);
        CHECK(diag.severity != Diagnostic::Severity::Error);
    }

    Assembled assembled;
    for (const auto &item : parsed.module.items) {
        if (const auto *func = dynamic_cast<const FuncDecl *>(item.get()); func != nullptr && func->isAsm) {
            assembled.result = AssembleAArch64AsmFunc(func->asmBody, "asm.rux", assembled.code);
            return assembled;
        }
    }
    FAIL("no asm func in the parsed module");
    return assembled;
}

// The first error a body reported, or an empty string when it reported none.
[[nodiscard]] std::string FirstError(const Assembled &assembled) {
    for (const auto &diag : assembled.result.diagnostics) {
        if (diag.severity == Diagnostic::Severity::Error) {
            return diag.message;
        }
    }
    return {};
}
} // namespace

TEST_CASE("AArch64 assembler encodes the forms its encoders provide") {
    static constexpr struct {
        std::string_view source;
        std::uint32_t word;
    } forms[] = {
        {"add x0, x1, #8", 0x91002020},
        {"add x0, x1, #1, lsl #12", 0x91400420},
        {"adds x2, x3, x4", 0xAB040062},
        {"sub sp, sp, #16", 0xD10043FF},
        {"subs x0, x1, x2, lsl #3", 0xEB020C20},
        {"add x0, x1, w2, uxtw #2", 0x8B224820},
        {"cmp x0, #42", 0xF100A81F},
        {"cmp x0, x1", 0xEB01001F},
        {"cmn x0, x1", 0xAB01001F},
        {"and x0, x1, #0xFF", 0x92401C20},
        {"ands x0, x1, x2", 0xEA020020},
        {"orr x0, x1, x2, lsl #4", 0xAA021020},
        {"eor x0, x1, #1", 0xD2400020},
        {"bic x0, x1, x2", 0x8A220020},
        {"orn x0, x1, x2", 0xAA220020},
        {"eon x0, x1, x2", 0xCA220020},
        {"bics x0, x1, x2", 0xEA220020},
        {"tst x0, #7", 0xF240081F},
        {"tst x0, x1", 0xEA01001F},
        {"mov x0, x1", 0xAA0103E0},
        {"mov x0, sp", 0x910003E0},
        {"movz x0, #0x1234, lsl #16", 0xD2A24680},
        {"movn x0, #5", 0x928000A0},
        {"movk x0, #0xABCD, lsl #32", 0xF2D579A0},
        {"neg x0, x1", 0xCB0103E0},
        {"negs x0, x1", 0xEB0103E0},
        {"mvn x0, x1", 0xAA2103E0},
        {"lsl x0, x1, #4", 0xD37CEC20},
        {"lsr w0, w1, #3", 0x53037C20},
        {"asr x0, x1, #63", 0x937FFC20},
        {"ror x0, x1, #8", 0x93C12020},
        {"lsl x0, x1, x2", 0x9AC22020},
        {"lslv x0, x1, x2", 0x9AC22020},
        {"sbfm x0, x1, #3, #7", 0x93431C20},
        {"ubfx x0, x1, #4, #8", 0xD3442C20},
        {"sbfx x0, x1, #4, #8", 0x93442C20},
        {"bfi x0, x1, #4, #8", 0xB37C1C20},
        {"bfxil x0, x1, #4, #8", 0xB3442C20},
        {"extr x0, x1, x2, #5", 0x93C21420},
        {"sxtb x0, w1", 0x93401C20},
        {"sxtw x0, w1", 0x93407C20},
        {"uxth w0, w1", 0x53003C20},
        {"clz x0, x1", 0xDAC01020},
        {"rbit x0, x1", 0xDAC00020},
        {"rev x0, x1", 0xDAC00C20},
        {"rev16 w0, w1", 0x5AC00420},
        {"rev32 x0, x1", 0xDAC00820},
        {"mul x0, x1, x2", 0x9B027C20},
        {"mneg x0, x1, x2", 0x9B02FC20},
        {"madd x0, x1, x2, x3", 0x9B020C20},
        {"msub x0, x1, x2, x3", 0x9B028C20},
        {"smull x0, w1, w2", 0x9B227C20},
        {"umaddl x0, w1, w2, x3", 0x9BA20C20},
        {"smulh x0, x1, x2", 0x9B427C20},
        {"udiv x0, x1, x2", 0x9AC20820},
        {"sdiv x0, x1, x2", 0x9AC20C20},
        {"csel x0, x1, x2, eq", 0x9A820020},
        {"csinc x0, x1, x2, ne", 0x9A821420},
        {"cset x0, lt", 0x9A9FA7E0},
        {"csetm w0, ge", 0x5A9FB3E0},
        {"cinc x0, x1, hi", 0x9A819420},
        {"cneg x0, x1, mi", 0xDA815420},
        {"ldr x0, [x1]", 0xF9400020},
        {"ldr x0, [x1, #16]", 0xF9400820},
        {"ldr x0, [x1, #-8]", 0xF85F8020},
        {"ldr w0, [x1, #4]!", 0xB8404C20},
        {"ldr x0, [x1], #8", 0xF8408420},
        {"ldr x0, [x1, x2]", 0xF8626820},
        {"ldr x0, [x1, x2, lsl #3]", 0xF8627820},
        {"ldr x0, [x1, w2, uxtw #3]", 0xF8625820},
        {"ldrb w0, [x1, #3]", 0x39400C20},
        {"ldrsw x0, [x1, #8]", 0xB9800820},
        {"ldur x0, [x1, #-4]", 0xF85FC020},
        {"str x0, [sp, #24]", 0xF9000FE0},
        {"strh w0, [x1, x2]", 0x78226820},
        {"stp x29, x30, [sp, #-16]!", 0xA9BF7BFD},
        {"ldp x29, x30, [sp], #16", 0xA8C17BFD},
        {"ldr d0, [x1, #8]", 0xFD400420},
        {"str q0, [x1]", 0x3D800020},
        {"br x0", 0xD61F0000},
        {"blr x1", 0xD63F0020},
        {"ret", 0xD65F03C0},
        {"ret x1", 0xD65F0020},
        {"svc #0", 0xD4000001},
        {"brk #1", 0xD4200020},
        {"nop", 0xD503201F},
        {"hint #5", 0xD50320BF},
        {"dmb ish", 0xD5033BBF},
        {"dsb sy", 0xD5033F9F},
        {"isb", 0xD5033FDF},
        {"mrs x0, nzcv", 0xD53B4200},
        {"msr nzcv, x0", 0xD51B4200},
        {"fmov d0, d1", 0x1E604020},
        {"fmov x0, d0", 0x9E660000},
        {"fadd d0, d1, d2", 0x1E622820},
        {"fsub s0, s1, s2", 0x1E223820},
        {"fmul d0, d1, d2", 0x1E620820},
        {"fdiv d0, d1, d2", 0x1E621820},
        {"fmax d0, d1, d2", 0x1E624820},
        {"fminnm s0, s1, s2", 0x1E227820},
        {"fneg d0, d1", 0x1E614020},
        {"fabs d0, d1", 0x1E60C020},
        {"fsqrt d0, d1", 0x1E61C020},
        {"fcvt s0, d1", 0x1E624020},
        {"fcvtzs w0, d0", 0x1E780000},
        {"scvtf d0, x1", 0x9E620020},
        {"ucvtf s0, w1", 0x1E230020},
        {"frintz d0, d1", 0x1E65C020},
        {"fmadd d0, d1, d2, d3", 0x1F420C20},
        {"fcmp d0, d1", 0x1E612000},
        {"fccmp d0, d1, #3, eq", 0x1E610403},
        {"fcsel d0, d1, d2, gt", 0x1E62CC20},
    };

    for (const auto &[source, word] : forms) {
        const Assembled assembled = Assemble(source);
        INFO(source);
        CHECK(FirstError(assembled) == "");
        REQUIRE(assembled.Words() == 1);
        CHECK(HexWord(assembled.Word(0)) == HexWord(word));
    }
}

TEST_CASE("AArch64 assembler resolves branches to a label the body defines") {
    const Assembled assembled = Assemble(R"(
    b fwd
    bl fwd
    b.eq fwd
    cbz x0, fwd
    cbnz w1, back
    tbz x0, #3, fwd
    tbnz x0, #63, back
back:
    adr x0, fwd
    ldr x1, fwd
    nop
fwd:
    ret
)");

    CHECK(FirstError(assembled) == "");
    // A name the body defines is resolved here, so nothing is left over for
    // the linker.
    CHECK(assembled.result.fixups.empty());

    static constexpr std::uint32_t expected[] = {
        0x1400000A, // b fwd            — forward, ten instructions
        0x94000009, // bl fwd
        0x54000100, // b.eq fwd
        0xB40000E0, // cbz x0, fwd
        0x35000061, // cbnz w1, back    — backward, three instructions
        0x361800A0, // tbz x0, #3, fwd
        0xB7F80020, // tbnz x0, #63, back
        0x10000060, // adr x0, fwd      — a byte offset split across two fields
        0x58000041, // ldr x1, fwd
        0xD503201F, // nop
        0xD65F03C0, // ret
    };
    REQUIRE(assembled.Words() == std::size(expected));
    for (std::size_t i = 0; i < std::size(expected); ++i) {
        INFO("word ", i);
        CHECK(HexWord(assembled.Word(i)) == HexWord(expected[i]));
    }
}

TEST_CASE("AArch64 assembler reports a name the body does not define as a fixup") {
    const Assembled assembled = Assemble(R"(
    bl External
    adrp x0, Sym
    add x0, x0, Sym
    ldr x1, [x0, Sym]
    b Elsewhere
    ret
)");

    CHECK(FirstError(assembled) == "");
    REQUIRE(assembled.Words() == 6);

    // Each instruction is emitted with an immediate of zero: the relocation
    // carries the address, and an AArch64 relocation patches fields of a whole
    // instruction word rather than a displacement inside it.
    CHECK(HexWord(assembled.Word(0)) == HexWord(0x94000000));
    CHECK(HexWord(assembled.Word(1)) == HexWord(0x90000000));
    CHECK(HexWord(assembled.Word(2)) == HexWord(0x91000000));
    CHECK(HexWord(assembled.Word(3)) == HexWord(0xF9400001));
    CHECK(HexWord(assembled.Word(4)) == HexWord(0x14000000));

    static constexpr struct {
        std::uint32_t offset;
        std::string_view symbol;
        std::uint16_t relType;
    } expected[] = {
        {0, "External", RcuRelType::AArch64Call26},   {4, "Sym", RcuRelType::AArch64AdrPrelPgHi21},
        {8, "Sym", RcuRelType::AArch64AddAbsLo12Nc},  {12, "Sym", RcuRelType::AArch64LdstAbsLo12Nc},
        {16, "Elsewhere", RcuRelType::AArch64Jump26},
    };

    REQUIRE(assembled.result.fixups.size() == std::size(expected));
    for (std::size_t i = 0; i < std::size(expected); ++i) {
        INFO("fixup ", i);
        CHECK(assembled.result.fixups[i].offset == expected[i].offset);
        CHECK(assembled.result.fixups[i].symbol == expected[i].symbol);
        CHECK(assembled.result.fixups[i].relType == expected[i].relType);
    }
}

TEST_CASE("AArch64 assembler refuses a branch past the reach of its field") {
    // TBZ keeps its target in fourteen instructions of signed offset, so a
    // label 32 KiB away is one instruction too far.
    std::string body = "    tbz x0, #0, done\n";
    for (int i = 0; i < 8192; ++i) {
        body += "    nop\n";
    }
    body += "done:\n    ret\n";

    const Assembled assembled = Assemble(body);
    CHECK_FALSE(assembled.result.ok);
    const std::string error = FirstError(assembled);
    INFO(error);
    CHECK(error.find("'tbz' is 32772 bytes from 'done'") != std::string::npos);
    CHECK(error.find("+/-32 KiB") != std::string::npos);
}

TEST_CASE("AArch64 assembler materializes a constant no single MOV reaches") {
    // MOV of a constant is whatever reaches it, exactly as `LDR Xd, =value`
    // would be: one instruction where the value has an encoding, and the
    // MOVZ / MOVK chain where it does not.
    const Assembled assembled = Assemble(R"(
    mov x0, #0x123456789
    mov w0, #-1
    mov x1, #42
)");

    CHECK(FirstError(assembled) == "");
    static constexpr std::uint32_t expected[] = {
        0xD28CF120, // movz x0, #0x6789
        0xF2A468A0, // movk x0, #0x2345, lsl #16
        0xF2C00020, // movk x0, #1, lsl #32
        0x12800000, // mov w0, #-1 — one MOVN, since inverting empties the rest
        0xD2800541, // movz x1, #42
    };
    REQUIRE(assembled.Words() == std::size(expected));
    for (std::size_t i = 0; i < std::size(expected); ++i) {
        INFO("word ", i);
        CHECK(HexWord(assembled.Word(i)) == HexWord(expected[i]));
    }
}

TEST_CASE("AArch64 assembler reports what it cannot encode") {
    SUBCASE("an instruction the architecture has and the encoders do not") {
        const Assembled assembled = Assemble("    adc x0, x1, x2\n");
        CHECK(FirstError(assembled) == "unsupported instruction 'adc'");
    }

    SUBCASE("a misspelling") {
        const Assembled assembled = Assemble("    mvo x0, x1\n");
        CHECK(FirstError(assembled) == "unsupported instruction 'mvo'");
    }

    SUBCASE("the wrong number of operands") {
        const Assembled assembled = Assemble("    mul x0, x1\n");
        CHECK(FirstError(assembled) == "'mul' expects 3 operands, found 2");
    }

    SUBCASE("an immediate the field cannot hold") {
        const Assembled assembled = Assemble("    add x0, x1, #4097\n");
        CHECK(FirstError(assembled) == "cannot encode 'add': invalid immediate");
    }

    SUBCASE("operands whose widths disagree") {
        const Assembled assembled = Assemble("    add x0, x1, w2\n");
        CHECK(FirstError(assembled) == "cannot encode 'add': invalid register");
    }

    SUBCASE("an unknown condition") {
        const Assembled assembled = Assemble("    csel x0, x1, x2, zz\n");
        CHECK(FirstError(assembled) == "unknown condition 'zz'");
    }

    SUBCASE("a form with no relocation to carry a symbol") {
        const Assembled assembled = Assemble("    adr x0, Sym\n");
        CHECK(FirstError(assembled).starts_with("'adr' cannot reference 'Sym'"));
    }

    SUBCASE("a refused instruction still occupies its place") {
        // Otherwise one mistake would move every label after it and invent a
        // second diagnostic about a branch that is perfectly in range.
        const Assembled assembled = Assemble(R"(
    b done
    add x0, x1, #4097
done:
    ret
)");
        REQUIRE(assembled.Words() == 3);
        CHECK(assembled.result.diagnostics.size() == 1);
        CHECK(HexWord(assembled.Word(0)) == HexWord(0x14000002));
    }
}
