// The AArch64 RCU back end: the object it produces for a whole function, and
// the reports it makes for everything it does not lower yet.
//
// The expected words below came from `llvm-mc -triple=aarch64 -show-encoding`
// on the instruction named beside each, so a disagreement here is a
// disagreement with a second implementation rather than with someone's reading
// of the ARM manual.

#include "CodeGen/AArch64/CallLayout.h"
#include "CodeGen/AArch64/Encoder.h"
#include "CodeGen/AArch64/RcuEmitter.h"
#include "Driver/BuildTarget.h"
#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Semantic/CompileTimeContext.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <doctest.h>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Rux;

namespace {
LirPackage CompileToAArch64Lir(const std::string &source, const std::string_view targetTriple = "linux-aarch64") {
    Lexer lexer(source, "test.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());

    const TargetContext target = Driver::TargetContextForTriple(targetTriple);
    Parser parser(std::move(lexed.tokens), "test.rux", target.arch);
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    // The whole front end is told which machine this is for, not just the
    // parser: an `asm func` body is checked against the target's own
    // instruction set, so a semantic stage left on the host would refuse every
    // AArch64 mnemonic that is not also an x86-64 one.
    CompileTimeContext context;
    context.target = target;
    context.targetTriple = targetTriple;

    std::vector<Module *> modules = {&parsed.module};
    SemanticAnalyzer analyzer(modules, {}, "test", context);
    auto semaModel = analyzer.Analyze();
    REQUIRE_FALSE(semaModel.HasErrors());

    AstToHirLowering hirLowering(semaModel);
    auto hirPackage = hirLowering.Generate();

    HirToLirLowering lirLowering(std::move(hirPackage), target);
    return lirLowering.Generate();
}

// An instruction word as a table spells it: doctest reports an integer in
// decimal, which says nothing at a glance against a column of hexadecimal.
[[nodiscard]] std::string HexWord(const std::uint32_t word) {
    return std::format("0x{:08X}", word);
}

// The words of one function's body, read back out of .text through its symbol.
[[nodiscard]] std::vector<std::uint32_t> FunctionWords(const RcuFile &object, const std::string_view name) {
    const auto symbol = std::ranges::find_if(object.symbols, [name](const RcuSymbol &s) { return s.name == name; });
    REQUIRE(symbol != object.symbols.end());
    REQUIRE(symbol->sectionIdx < object.sections.size());

    const auto &text = object.sections[symbol->sectionIdx].data;
    REQUIRE(symbol->value + symbol->size <= text.size());
    REQUIRE(symbol->size % 4 == 0);

    std::vector<std::uint32_t> words;
    for (std::uint32_t offset = symbol->value; offset < symbol->value + symbol->size; offset += 4) {
        words.push_back(static_cast<std::uint32_t>(text[offset]) | static_cast<std::uint32_t>(text[offset + 1]) << 8U |
                        static_cast<std::uint32_t>(text[offset + 2]) << 16U |
                        static_cast<std::uint32_t>(text[offset + 3]) << 24U);
    }
    return words;
}

// The byte count of `sub sp, sp, #imm` or `add sp, sp, #imm`, in either shift
// position, or nothing when the word is some other instruction. The frame of a
// function too large for a pre-indexed STP is opened by a run of these, and how
// many it takes is FrameAdjust's business rather than this test's.
[[nodiscard]] std::optional<std::int64_t> StackPointerAdjustment(const std::uint32_t word, const bool subtract) {
    const std::uint32_t opcode = subtract ? 0xD1000000U : 0x91000000U;
    if ((word & 0xFF800000U) != opcode || (word & 0x3FFU) != 0x3FFU) {
        return std::nullopt; // not SUB/ADD (immediate) with SP as both operands
    }
    const std::int64_t imm12 = (word >> 10U) & 0xFFFU;
    return (word & (1U << 22U)) != 0 ? imm12 << 12U : imm12;
}

// The page touch in a Windows stack-probing sequence. XZR supplies the value,
// so probing consumes no register that can hold an argument or result.
constexpr std::uint32_t kStackProbeTouch = 0xF90003FFU; // str xzr, [sp]

// A symbol by name, or nothing when the object carries none.
[[nodiscard]] const RcuSymbol *FindSymbol(const RcuFile &object, const std::string_view name) {
    const auto found = std::ranges::find_if(object.symbols, [name](const RcuSymbol &s) { return s.name == name; });
    return found == object.symbols.end() ? nullptr : &*found;
}

// The bytes a read-only symbol names, which is how a constant's contents are
// checked without depending on where in .rodata it landed.
[[nodiscard]] std::vector<std::uint8_t> RodataOf(const RcuFile &object, const std::string_view name) {
    const RcuSymbol *symbol = FindSymbol(object, name);
    REQUIRE_MESSAGE(symbol != nullptr, name);
    REQUIRE_EQ(symbol->sectionIdx, RCU_RODATA_IDX);
    const auto &rodata = object.sections[RCU_RODATA_IDX].data;
    REQUIRE(symbol->value + symbol->size <= rodata.size());
    return {rodata.begin() + symbol->value, rodata.begin() + symbol->value + symbol->size};
}

// The relocations of a section that name `symbol`, in the order they were
// recorded, so a two-instruction symbol reference is one pair to check.
[[nodiscard]] std::vector<RcuReloc> RelocsFor(const RcuFile &object, const std::uint16_t sectionIdx,
                                              const std::string_view symbol) {
    std::vector<RcuReloc> found;
    for (const auto &reloc : object.sections[sectionIdx].relocs) {
        if (reloc.symbolIndex < object.symbols.size() && object.symbols[reloc.symbolIndex].name == symbol) {
            found.push_back(reloc);
        }
    }
    return found;
}

// The word at a byte offset into .text, which is where a relocation says an
// instruction it patches sits.
[[nodiscard]] std::uint32_t TextWordAt(const RcuFile &object, const std::uint32_t offset) {
    const auto &text = object.sections[RCU_TEXT_IDX].data;
    REQUIRE(offset + 4 <= text.size());
    return static_cast<std::uint32_t>(text[offset]) | static_cast<std::uint32_t>(text[offset + 1]) << 8U |
           static_cast<std::uint32_t>(text[offset + 2]) << 16U | static_cast<std::uint32_t>(text[offset + 3]) << 24U;
}

// The immediate of `add xN, x29, #imm`, or nothing when the word is some other
// instruction. An alloca's address is the frame pointer plus a displacement,
// and which displacement is the frame layout's business rather than this test's.
[[nodiscard]] std::optional<std::uint32_t> FramePointerAddImm(const std::uint32_t word, const unsigned reg = 9) {
    if ((word & 0xFFC003FFU) != (0x910003A0U | reg)) {
        return std::nullopt;
    }
    return word >> 10U & 0xFFFU;
}

// Whether the function holds a floating-point instruction of this opcode,
// whichever registers it names: `sources` is how many of its three register
// fields hold an operand, which is what decides how much of the word is masked
// away. The precision is part of the opcode rather than of the registers, so it
// is checked rather than masked.
[[nodiscard]] bool HasFloatForm(const std::vector<std::uint32_t> &words, const std::uint32_t opcode,
                                const unsigned sources = 2) {
    const std::uint32_t mask = sources == 2 ? 0xFFE0FC00U : 0xFFFFFC00U;
    return std::ranges::any_of(words, [mask, opcode](const std::uint32_t w) { return (w & mask) == opcode; });
}

// Whether the function holds a CSET of this condition, whichever register
// receives the boolean it produces.
[[nodiscard]] bool HasCset(const std::vector<std::uint32_t> &words, const std::uint32_t cset) {
    return std::ranges::any_of(words, [cset](const std::uint32_t w) { return (w & 0xFFFFFFE0U) == cset; });
}

// Whether the function holds a three-register data-processing instruction of
// this opcode, whichever registers it names: which those are is the
// allocation's business, so they are masked out rather than written down.
[[nodiscard]] bool HasRegisterForm(const std::vector<std::uint32_t> &words, const std::uint32_t opcode) {
    return std::ranges::any_of(words, [opcode](const std::uint32_t w) { return (w & 0xFFE0FC00U) == opcode; });
}

// The immediate of `add xD, xN, #imm` where the base is neither the frame
// pointer nor the stack pointer, which is what a field of an aggregate is
// reached at. Which register holds the base is the allocation's business, so
// this names neither of the two.
[[nodiscard]] std::optional<std::uint32_t> FieldAddImm(const std::uint32_t word) {
    const std::uint32_t base = word >> 5U & 31U;
    if ((word & 0xFFC00000U) != 0x91000000U || base == 29 || base == 31) {
        return std::nullopt;
    }
    return word >> 10U & 0xFFFU;
}

// The same for `add xN, sp, #imm`, which is how the address of something in the
// outgoing argument area is reached: the area is opened relative to the stack
// pointer and the frame pointer knows nothing about it.
[[nodiscard]] std::optional<std::uint32_t> StackPointerAddImm(const std::uint32_t word, const unsigned reg) {
    if ((word & 0xFFC003FFU) != (0x910003E0U | reg)) {
        return std::nullopt;
    }
    return word >> 10U & 0xFFFU;
}

// Whether `word` is a load or a store of a register pair, at any of the three
// index modes. Everything in this family shares the top seven bits.
[[nodiscard]] bool IsPairAccess(const std::uint32_t word) {
    return (word & 0xFE000000U) == 0xA8000000U;
}

// The instruction displacement a branch carries, sign-extended out of the field
// its own form keeps it in: twenty-six bits at bit zero for B, and nineteen at
// bit five for every conditional form.
[[nodiscard]] std::int32_t BranchDisplacement(const std::uint32_t word) {
    if ((word & 0xFC000000U) == 0x14000000U) {
        return static_cast<std::int32_t>((word & 0x03FFFFFFU) << 6U) >> 6;
    }
    return static_cast<std::int32_t>((word >> 5U & 0x7FFFFU) << 13U) >> 13;
}

// The byte displacement of `ldr x9, [x29, #imm]` or of `str x9, [x29, #imm]`,
// which is how a copy from one place in the frame to another is read back. The
// immediate counts doublewords, so it is scaled by the width of the access.
[[nodiscard]] std::optional<std::uint32_t> SlotDisplacement(const std::uint32_t word, const bool store) {
    if ((word & 0xFFC003FFU) != (store ? 0xF90003A9U : 0xF94003A9U)) {
        return std::nullopt;
    }
    return (word >> 10U & 0xFFFU) * 8U;
}

// The index of the first `bl` in a body, which is where a direct call site is
// found without counting the instructions ahead of it.
[[nodiscard]] std::optional<std::size_t> BranchAndLinkIndex(const std::vector<std::uint32_t> &words) {
    for (std::size_t i = 0; i < words.size(); ++i) {
        if ((words[i] & 0xFC000000U) == 0x94000000U) {
            return i;
        }
    }
    return std::nullopt;
}

// The register a value arrives in, or nothing when the word puts a value
// nowhere. Two instructions do it: `ldr xN, [x29, #imm]` where the value lives
// in the frame, and `mov xN, xM` where the allocation gave it a register of its
// own. Which of the two a particular value takes is the allocation's business
// rather than this test's, and either way the register named is the one filled.
[[nodiscard]] std::optional<unsigned> ArgumentFilled(const std::uint32_t word) {
    if ((word & 0xFFC003E0U) == 0xF94003A0U) {
        return word & 0x1FU;
    }
    if ((word & 0xFFE0FFE0U) == 0xAA0003E0U) {
        return word & 0x1FU; // orr xN, xzr, xM
    }
    return std::nullopt;
}

// The register a value leaves, on the same terms: `str xN, [x29, #imm]` where
// it is spilled to the frame, and `mov xM, xN` where it is moved to a register.
// This is how a parameter spill and a kept result are read back.
[[nodiscard]] std::optional<unsigned> ArgumentDrained(const std::uint32_t word) {
    if ((word & 0xFFC003E0U) == 0xF90003A0U) {
        return word & 0x1FU;
    }
    if ((word & 0xFFE0FFE0U) == 0xAA0003E0U) {
        return word >> 16U & 0x1FU; // orr xM, xzr, xN
    }
    return std::nullopt;
}

// The byte displacement a doubleword access to the frame names, whichever
// register it moves — SlotDisplacement answers only for the X9 this generator
// computes in, and a composite is moved in the registers it is passed in.
[[nodiscard]] std::uint32_t SlotAccessDisplacement(const std::uint32_t word) {
    return (word >> 10U & 0xFFFU) * 8U;
}

// One access to a stack slot in the vector file: which register it names and
// which byte of the frame it reached. `bits` selects the S or the D form, which
// is also what says how wide one member of a homogeneous aggregate is.
struct VectorSlotAccess {
    unsigned reg = 0;
    std::uint32_t displacement = 0;
};

[[nodiscard]] std::optional<VectorSlotAccess> VectorSlotAccessOf(const std::uint32_t word, const unsigned bits,
                                                                 const bool store) {
    const std::uint32_t opcode = (bits == 64 ? 0xFD0003A0U : 0xBD0003A0U) | (store ? 0U : 0x00400000U);
    if ((word & 0xFFC003E0U) != opcode) {
        return std::nullopt;
    }
    return VectorSlotAccess{word & 0x1FU, (word >> 10U & 0xFFFU) * (bits / 8U)};
}

// The vector register a value arrives in, on the same terms as ArgumentFilled:
// `ldr dN, [x29, #imm]` where the value lives in the frame, and `fmov dN, dM`
// where the allocation gave it a register of its own.
[[nodiscard]] std::optional<unsigned> VectorArgumentFilled(const std::uint32_t word, const unsigned bits) {
    if (const auto access = VectorSlotAccessOf(word, bits, false)) {
        return access->reg;
    }
    if ((word & 0xFFFFFC00U) == (bits == 64 ? 0x1E604000U : 0x1E204000U)) {
        return word & 0x1FU;
    }
    return std::nullopt;
}

// A floating-point value transferred by bit pattern into the general-purpose
// argument file, which is the distinctive Windows C variadic operation.
[[nodiscard]] std::optional<unsigned> FloatBitsArgumentFilled(const std::uint32_t word, const unsigned bits) {
    const std::uint32_t opcode = bits == 64 ? 0x9E660000U : 0x1E260000U; // fmov xD, dN / fmov wD, sN
    return (word & 0xFFFFFC00U) == opcode ? std::optional<unsigned>(word & 0x1FU) : std::nullopt;
}

// The byte offset of `str[b|h] {w|x}9, [sp, #imm]`, used by outgoing stack
// arguments. The immediate is scaled by the selected access width.
[[nodiscard]] std::optional<std::uint32_t> StackArgumentStored(const std::uint32_t word, const unsigned width = 8) {
    const std::uint32_t opcode = width == 1 ? 0x390003E9U
                               : width == 2 ? 0x790003E9U
                               : width == 4 ? 0xB90003E9U
                                            : 0xF90003E9U;
    if ((word & 0xFFC003FFU) != opcode) {
        return std::nullopt;
    }
    return (word >> 10U & 0xFFFU) * width;
}

// The vector register a value leaves, on the same terms as ArgumentDrained.
[[nodiscard]] std::optional<unsigned> VectorArgumentDrained(const std::uint32_t word, const unsigned bits) {
    if (const auto access = VectorSlotAccessOf(word, bits, true)) {
        return access->reg;
    }
    if ((word & 0xFFFFFC00U) == (bits == 64 ? 0x1E604000U : 0x1E204000U)) {
        return word >> 5U & 0x1FU;
    }
    return std::nullopt;
}

// The same for the general-purpose file.
[[nodiscard]] std::size_t DrainStart(const std::vector<std::uint32_t> &words) {
    const auto found = std::ranges::find_if(
        words, [](const std::uint32_t w) { return ArgumentDrained(w) == std::optional<unsigned>(0); });
    return static_cast<std::size_t>(found - words.begin());
}

// Where a run of consecutive argument registers is taken out of, which is the
// first word that drains the first of them: what stands before it is the
// prologue's, whose length depends on how many registers the allocation took.
[[nodiscard]] std::size_t VectorDrainStart(const std::vector<std::uint32_t> &words, const unsigned bits) {
    const auto found = std::ranges::find_if(
        words, [bits](const std::uint32_t w) { return VectorArgumentDrained(w, bits) == std::optional<unsigned>(0); });
    return static_cast<std::size_t>(found - words.begin());
}

// The frame `stp x29, x30, [sp, #-N]!` opens, or nothing when the word is some
// other instruction. The immediate counts pairs of doublewords below the stack
// pointer, so the frame it names is that scaled back and made positive — which
// is also the displacement an incoming stack argument sits at.
[[nodiscard]] std::optional<std::int32_t> PreIndexedFrameSize(const std::uint32_t word) {
    constexpr std::uint32_t registers = 30U << 10U | 31U << 5U | 29U; // x30, sp, x29
    if ((word & 0xFFC00000U) != 0xA9800000U || (word & 0x7FFFU) != registers) {
        return std::nullopt;
    }
    const auto imm7 = static_cast<std::int32_t>((word >> 15U & 0x7FU) << 25U) >> 25;
    return -imm7 * 8;
}

// The byte displacement an incoming stack-argument load names. The access uses
// the parameter's width and extension, and its immediate counts units of that
// width.
[[nodiscard]] std::optional<std::int32_t> IncomingDisplacement(const std::uint32_t word, const unsigned width,
                                                               const bool sign = false) {
    const std::uint32_t opcode = width == 1 ? (sign ? 0x398003A9U : 0x394003A9U)
                               : width == 2 ? (sign ? 0x798003A9U : 0x794003A9U)
                               : width == 4 ? (sign ? 0xB98003A9U : 0xB94003A9U)
                                            : 0xF94003A9U;
    if ((word & 0xFFC003FFU) != opcode) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>((word >> 10U & 0xFFFU) * width);
}

// The registers a prologue writes into the frame before it does anything else,
// which for a function with no parameters is exactly the ones the allocation
// handed out: the run of frame stores that follows `mov x29, sp`. A single
// register goes in an STR and a pair in an STP, and the two files are kept
// apart because the pool each is allocated from is its own.
struct PreservedRegisters {
    std::vector<unsigned> general;
    std::vector<unsigned> vector;
};

[[nodiscard]] PreservedRegisters SavedRegisters(const std::vector<std::uint32_t> &words) {
    PreservedRegisters saved;
    for (std::size_t i = 2; i < words.size(); ++i) {
        const std::uint32_t word = words[i];
        if ((word & 0xFFC003E0U) == 0xF90003A0U) { // str xN, [x29, #imm]
            saved.general.push_back(word & 31U);
        }
        else if ((word & 0xFFC003E0U) == 0xA90003A0U) { // stp xN, xM, [x29, #imm]
            saved.general.push_back(word & 31U);
            saved.general.push_back(word >> 10U & 31U);
        }
        else if ((word & 0xFFC003E0U) == 0xFD0003A0U) { // str dN, [x29, #imm]
            saved.vector.push_back(word & 31U);
        }
        else if ((word & 0xFFC003E0U) == 0x6D0003A0U) { // stp dN, dM, [x29, #imm]
            saved.vector.push_back(word & 31U);
            saved.vector.push_back(word >> 10U & 31U);
        }
        else {
            break;
        }
    }
    return saved;
}

// Whether the function reads `reg` back out of the frame, which is what an
// epilogue does with everything the prologue above preserved.
[[nodiscard]] bool RestoresRegister(const std::vector<std::uint32_t> &words, const unsigned reg, const bool isVector) {
    const std::uint32_t single = isVector ? 0xFD4003A0U : 0xF94003A0U;
    const std::uint32_t pair = isVector ? 0x6D4003A0U : 0xA94003A0U;
    return std::ranges::any_of(words, [reg, single, pair](const std::uint32_t w) {
        if ((w & 0xFFC003E0U) == single) {
            return (w & 31U) == reg;
        }
        if ((w & 0xFFC003E0U) == pair) {
            return (w & 31U) == reg || (w >> 10U & 31U) == reg;
        }
        return false;
    });
}

// A one-function package, for the two constructs no source program reaches yet:
// the LIR's switch terminator, which the front end never emits, and a phi whose
// copies form a cycle, which takes two phis in one block naming each other.
[[nodiscard]] LirPackage PackageOf(LirFunc func) {
    LirModule module;
    module.name = "test.rux";
    module.funcs.push_back(std::move(func));
    LirPackage package;
    package.modules.push_back(std::move(module));
    return package;
}

[[nodiscard]] LirInstr ConstInstr(const LirReg dst, const std::string_view value, TypeRef type) {
    LirInstr instr;
    instr.op = LirOpcode::Const;
    instr.dst = dst;
    instr.type = std::move(type);
    instr.strArg = value;
    return instr;
}

[[nodiscard]] LirInstr PhiInstr(const LirReg dst, const std::vector<std::pair<LirReg, std::uint32_t>> &preds) {
    LirInstr instr;
    instr.op = LirOpcode::Phi;
    instr.dst = dst;
    instr.type = TypeRef::MakeInt64();
    instr.phiPreds = preds;
    return instr;
}

[[nodiscard]] LirTerminator ReturnTerm(const LirReg value) {
    LirTerminator term;
    term.kind = LirTermKind::Return;
    term.retVal = value;
    term.retType = TypeRef::MakeInt64();
    return term;
}

// Every word of one function's body, checked against `expected` in order. This
// is a whole-image comparison rather than a search: what the back end produces
// is asserted down to the register numbers, so a changed frame layout or a
// changed allocation order shows as a diff instead of passing unnoticed because
// the instruction a masked test looked for is still somewhere in the function.
void CheckFunctionImage(const RcuFile &object, const std::string_view name,
                        const std::vector<std::uint32_t> &expected) {
    const auto words = FunctionWords(object, name);
    CHECK_EQ(words.size(), expected.size());
    for (std::size_t i = 0; i < std::min(words.size(), expected.size()); ++i) {
        CHECK_EQ(HexWord(words[i]), HexWord(expected[i]));
    }
}

// The relocations `symbol` is named by inside one function, as byte offsets
// from that function's first instruction rather than from the section: a
// function preceded by another one starts somewhere in the middle of .text, and
// where is not what these tests are about.
[[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint16_t>>
FunctionRelocs(const RcuFile &object, const std::string_view function, const std::string_view symbol) {
    const RcuSymbol *owner = FindSymbol(object, function);
    REQUIRE_MESSAGE(owner != nullptr, function);
    std::vector<std::pair<std::uint32_t, std::uint16_t>> found;
    for (const auto &reloc : RelocsFor(object, RCU_TEXT_IDX, symbol)) {
        if (reloc.sectionOffset >= owner->value && reloc.sectionOffset < owner->value + owner->size) {
            found.emplace_back(reloc.sectionOffset - owner->value, reloc.type);
        }
    }
    return found;
}

// The one message an emitter is expected to have produced, joined so a failure
// prints what was actually reported rather than a count.
[[nodiscard]] std::string JoinMessages(const std::vector<Diagnostic> &diagnostics) {
    std::string joined;
    for (const auto &diagnostic : diagnostics) {
        if (!joined.empty()) {
            joined += " | ";
        }
        joined += diagnostic.message;
    }
    return joined;
}
} // namespace

TEST_CASE("AArch64 RCU emitter generates a complete function returning a constant") {
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            return 42;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    REQUIRE_EQ(objects.size(), 1);

    const auto &object = objects.front();
    CHECK_EQ(object.arch, RcuArch::AArch64);
    REQUIRE_EQ(object.sections.size(), 3);
    CHECK_EQ(object.sections[RCU_TEXT_IDX].name, ".text");
    CHECK_EQ(object.sections[RCU_RODATA_IDX].name, ".rodata");
    CHECK_EQ(object.sections[RCU_DATA_IDX].name, ".data");

    // The constant is the one value this function holds, so the allocation
    // gives it X19 and the constant is materialized there rather than in the
    // scratch register and stored. One doubleword preserves X19 for the caller
    // and one is the slot the value keeps anyway, which above the 16-byte frame
    // record rounds the frame to 32.
    CheckFunctionImage(object, "Main",
                       {
                           0xA9BE7BFD, // stp  x29, x30, [sp, #-32]!
                           0x910003FD, // mov  x29, sp
                           0xF9000BB3, // str  x19, [x29, #16]
                           0xD2800553, // mov  x19, #42
                           0xAA1303E0, // mov  x0, x19
                           0xF9400BB3, // ldr  x19, [x29, #16]
                           0xA8C27BFD, // ldp  x29, x30, [sp], #32
                           0xD65F03C0, // ret
                       });
}

TEST_CASE("AArch64 RCU emitter predeclares every function before emitting bodies") {
    const auto package = CompileToAArch64Lir(R"(
        pub func First() -> int {
            return 1;
        }

        func Second() -> int {
            return 2;
        }
    )");

    const auto objects = AArch64RcuEmitter(package, "test").Generate();
    REQUIRE_EQ(objects.size(), 1);
    const auto &object = objects.front();

    // Both symbols exist, both point into .text, and the second body starts
    // where the first one ended rather than at zero.
    const auto first = std::ranges::find_if(object.symbols, [](const RcuSymbol &s) { return s.name == "First"; });
    const auto second = std::ranges::find_if(object.symbols, [](const RcuSymbol &s) { return s.name == "Second"; });
    REQUIRE(first != object.symbols.end());
    REQUIRE(second != object.symbols.end());
    CHECK_EQ(first->sectionIdx, RCU_TEXT_IDX);
    CHECK_EQ(second->sectionIdx, RCU_TEXT_IDX);
    CHECK_EQ(first->visibility, RcuSymVis::Global);
    CHECK_EQ(second->visibility, RcuSymVis::Local);
    CHECK_EQ(first->value, 0);
    CHECK_EQ(second->value, first->size);
    CHECK_EQ(first->size + second->size, object.sections[RCU_TEXT_IDX].data.size());
}

TEST_CASE("AArch64 RCU emitter keeps the stack pointer 16-byte aligned across a large frame") {
    // Enough locals to put the frame past the reach of a pre-indexed STP, so
    // the prologue opens it with FrameAdjust instead.
    std::string body;
    for (int i = 0; i < 200; ++i) {
        body += std::format("    var v{}: int = {};\n", i, i);
    }
    const auto package = CompileToAArch64Lir(std::format(R"(
        func Main() -> int {{
{}            return 0;
        }}
    )",
                                                         body));

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    const auto words = FunctionWords(objects.front(), "Main");
    REQUIRE_GE(words.size(), 6);

    // The prologue opens the frame with as many SUBs as it takes, and only then
    // stores the frame record and takes the frame pointer.
    std::int64_t opened = 0;
    std::size_t index = 0;
    while (index < words.size()) {
        const auto step = StackPointerAdjustment(words[index], true);
        if (!step) {
            break;
        }
        // Each instruction of a multi-instruction adjustment has to leave SP a
        // multiple of 16, not just the last one.
        CHECK_EQ(*step % 16, 0);
        opened += *step;
        ++index;
    }
    CHECK_MESSAGE(opened > 512, "the frame is meant to be past the reach of a pre-indexed STP: ", opened);
    CHECK_EQ(opened % 16, 0);
    REQUIRE_GT(index + 1, 1);
    CHECK_EQ(HexWord(words[index]), HexWord(0xA9007BFD));     // stp x29, x30, [sp]
    CHECK_EQ(HexWord(words[index + 1]), HexWord(0x910003FD)); // mov x29, sp

    // The epilogue restores the record first and closes exactly what was opened.
    CHECK_EQ(HexWord(words.back()), HexWord(0xD65F03C0)); // ret
    std::int64_t closed = 0;
    std::size_t tail = words.size() - 1;
    while (tail > 0) {
        const auto step = StackPointerAdjustment(words[tail - 1], false);
        if (!step) {
            break;
        }
        CHECK_EQ(*step % 16, 0);
        closed += *step;
        --tail;
    }
    CHECK_EQ(closed, opened);
    REQUIRE_GT(tail, 0);
    CHECK_EQ(HexWord(words[tail - 1]), HexWord(0xA9407BFD)); // ldp x29, x30, [sp]

    // Linux keeps its existing frame emission byte-for-byte: page touches are
    // selected only for Windows.
    CHECK(std::ranges::find(words, kStackProbeTouch) == words.end());
}

TEST_CASE("Windows AArch64 selects stack probing at the 4 KiB frame boundary") {
    struct BoundaryCase {
        int arrayBytes;
        std::int64_t frameBytes;
        std::size_t probes;
    };

    // The frame record, alloca pointer slot, and return constant occupy the 48 bytes
    // above these arrays. The three resulting frames sit immediately below,
    // at, and immediately above one Windows stack page.
    constexpr std::array cases{
        BoundaryCase{4032, 4080, 0},
        BoundaryCase{4048, 4096, 1},
        BoundaryCase{4064, 4112, 1},
    };

    for (const BoundaryCase &boundary : cases) {
        CAPTURE(boundary.arrayBytes);
        CAPTURE(boundary.frameBytes);
        const auto package = CompileToAArch64Lir(std::format(R"(
            func Main() -> int {{
                var frame: uint8[{}];
                return 0;
            }}
        )",
                                                             boundary.arrayBytes),
                                                 "windows-aarch64");

        AArch64RcuEmitter emitter(package, "test", Target::OS::Windows);
        const auto objects = emitter.Generate();
        CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
        const auto words = FunctionWords(objects.front(), "Main");

        std::int64_t opened = 0;
        std::size_t probes = 0;
        std::size_t index = 0;
        while (index < words.size()) {
            const auto step = StackPointerAdjustment(words[index], true);
            if (!step) {
                break;
            }
            opened += *step;
            ++index;
            if (*step == 4096) {
                REQUIRE_LT(index, words.size());
                CHECK_EQ(HexWord(words[index]), HexWord(kStackProbeTouch));
                ++probes;
                ++index;
            }
        }
        CHECK_EQ(opened, boundary.frameBytes);
        CHECK_EQ(probes, boundary.probes);
        REQUIRE_LT(index, words.size());
        CHECK_EQ(HexWord(words[index]), HexWord(0xA9007BFD)); // stp x29, x30, [sp]
    }
}

TEST_CASE("Windows AArch64 probes every page before opening a multi-page function frame") {
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            var pages: uint8[12288];
            pages[0] = 1;
            pages[4096] = 2;
            pages[12287] = 3;
            return 0;
        }
    )",
                                             "windows-aarch64");

    AArch64RcuEmitter emitter(package, "test", Target::OS::Windows);
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    // Each full page is entered by one aligned SUB and touched immediately.
    // The frame contains three pages of local storage plus its record and
    // slots, so there are at least three pairs before the final aligned tail.
    std::int64_t opened = 0;
    std::size_t probes = 0;
    std::size_t index = 0;
    while (index < words.size()) {
        const auto step = StackPointerAdjustment(words[index], true);
        if (!step) {
            break;
        }
        CHECK_EQ(*step % 16, 0);
        opened += *step;
        ++index;
        if (*step == 4096) {
            REQUIRE_LT(index, words.size());
            CHECK_EQ(HexWord(words[index]), HexWord(kStackProbeTouch));
            ++probes;
            ++index;
        }
    }
    CHECK_GE(probes, 3);
    CHECK_GE(opened, 12288);
    CHECK_EQ(opened % 16, 0);

    // The ordinary frame record and frame pointer follow the completed probe,
    // preserving the x29/x30 chain used by every other AArch64 frame.
    REQUIRE_LT(index + 1, words.size());
    CHECK_EQ(HexWord(words[index]), HexWord(0xA9007BFD));     // stp x29, x30, [sp]
    CHECK_EQ(HexWord(words[index + 1]), HexWord(0x910003FD)); // mov x29, sp

    // Returning restores the full frame with ordinary ADDs. Growing SP cannot
    // encounter a guard page, so no page touch belongs in the epilogue.
    CHECK_EQ(HexWord(words.back()), HexWord(0xD65F03C0)); // ret
    std::int64_t closed = 0;
    std::size_t tail = words.size() - 1;
    while (tail > 0) {
        const auto step = StackPointerAdjustment(words[tail - 1], false);
        if (!step) {
            break;
        }
        closed += *step;
        --tail;
    }
    CHECK_EQ(closed, opened);
    REQUIRE_GT(tail, 0);
    CHECK_EQ(HexWord(words[tail - 1]), HexWord(0xA9407BFD)); // ldp x29, x30, [sp]
}

TEST_CASE("Windows AArch64 probes a large outgoing copy and restores it without touching result registers") {
    const auto package = CompileToAArch64Lir(R"(
        func Take(payload: uint8[8192]) -> int {
            return payload[0] as int;
        }

        func Main() -> int {
            var payload: uint8[8192];
            payload[0] = 37;
            return Take(payload);
        }
    )",
                                             "windows-aarch64");

    AArch64RcuEmitter emitter(package, "test", Target::OS::Windows);
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto caller = FunctionWords(objects.front(), "Main");
    const auto call = BranchAndLinkIndex(caller);
    REQUIRE(call.has_value());

    // Ignore the function-frame probes before MOV x29, sp. The by-reference
    // copy opens an exact two-page outgoing area later, and each page is
    // touched before the unrolled copy begins.
    const auto framePointer = std::ranges::find(caller, 0x910003FDU);
    REQUIRE(framePointer != caller.end());
    const auto bodyStart = static_cast<std::size_t>(framePointer - caller.begin() + 1);
    REQUIRE_LT(bodyStart, *call);
    const auto outgoingTouches =
        std::ranges::count(std::ranges::subrange(caller.begin() + static_cast<std::ptrdiff_t>(bodyStart),
                                                 caller.begin() + static_cast<std::ptrdiff_t>(*call)),
                           kStackProbeTouch);
    CHECK_EQ(outgoingTouches, 2);

    // The call result arrives in X0. Closing the area is a single ordinary ADD
    // that names only SP, followed by the store that keeps X0 in its slot.
    REQUIRE_LT(*call + 2, caller.size());
    CHECK_EQ(StackPointerAdjustment(caller[*call + 1], false), std::optional<std::int64_t>(8192));
    CHECK_EQ(ArgumentDrained(caller[*call + 2]), std::optional<unsigned>(0));
    CHECK(std::ranges::find(caller.begin() + static_cast<std::ptrdiff_t>(*call + 1), caller.end(), kStackProbeTouch) ==
          caller.end());
}

// A struct and two values of it, which is the one construct a source program
// reaches that this back end still refuses: comparing two values that are not a
// bit pattern in one register is a run of comparisons the x86-64 back end emits
// and this one does not.
constexpr std::string_view kAggregateCompare = R"(
        struct Point {
            x: int;
            y: int;
        }
)";

TEST_CASE("AArch64 RCU emitter reports an unimplemented opcode by name") {
    const auto package = CompileToAArch64Lir(std::format(R"(
        {}
        func Main() -> int {{
            let a = Point {{ x: 1, y: 2 }};
            let b = Point {{ x: 1, y: 2 }};
            if a == b {{
                return 1;
            }}
            return 0;
        }}
    )",
                                                         kAggregateCompare));

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_EQ(objects.size(), 1);

    const auto reports = JoinMessages(emitter.Diagnostics());
    CHECK_MESSAGE(reports.contains("'cmpeq' opcode on 'Point'"), reports);
    CHECK_MESSAGE(reports.contains("not implemented yet"), reports);
    CHECK_MESSAGE(reports.contains("'Main'"), reports);
    for (const auto &diagnostic : emitter.Diagnostics()) {
        CHECK(diagnostic.IsError());
    }
}

TEST_CASE("AArch64 RCU emitter names each unimplemented construct once") {
    // Three comparisons, which are three instructions of the one opcode this
    // back end does not lower yet: what a report names is the construct, so
    // reaching it again says nothing new.
    const auto package = CompileToAArch64Lir(std::format(R"(
        {}
        func Main() -> int {{
            let a = Point {{ x: 1, y: 2 }};
            let b = Point {{ x: 1, y: 2 }};
            let first = a == b;
            let second = a == b;
            let third = a == b;
            return 0;
        }}
    )",
                                                         kAggregateCompare));

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_EQ(objects.size(), 1);

    std::vector<std::string> messages;
    for (const auto &diagnostic : emitter.Diagnostics()) {
        messages.push_back(diagnostic.message);
    }
    auto sorted = messages;
    std::ranges::sort(sorted);
    CHECK_EQ(std::ranges::unique(sorted).begin(), sorted.end());
    REQUIRE_EQ(messages.size(), 1);
    CHECK_MESSAGE(messages.front().contains("'cmpeq' opcode on 'Point'"), messages.front());
}

// Constants, globals and the read-only pool
//
// The opcodes below are the ones a value has to pass through before anything
// can be done with it, and the store that puts it to use belongs to the next
// group. The programs here therefore still report the opcodes they reach past
// the constant, which is why these cases assert what was emitted for the
// constant rather than that nothing was reported.

TEST_CASE("AArch64 RCU emitter materializes a boolean, a character and a null pointer") {
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            let flag: bool = true;
            let letter: char8 = c8'x';
            let nothing: *int32 = null;
            return 0;
        }
    )");

    const auto objects = AArch64RcuEmitter(package, "test").Generate();
    REQUIRE_EQ(objects.size(), 1);
    const auto words = FunctionWords(objects.front(), "Main");

    // Each is one bit pattern in the register the allocation gave it and one
    // store of the bytes its type occupies: a boolean and a character are
    // single bytes, and a pointer is a doubleword.
    const std::vector<std::uint32_t> expected = {
        0xD2800034, // mov  x20, #1
        0xD2800F14, // mov  x20, #120
        0xD2800014, // mov  x20, #0
    };
    for (const auto word : expected) {
        CHECK_MESSAGE(std::ranges::find(words, word) != words.end(), HexWord(word));
    }
    // The narrow ones store a byte through the W view of that same register,
    // and nothing narrows it first: a byte store reads no more than a byte.
    CHECK(std::ranges::any_of(words, [](const std::uint32_t w) { return (w & 0xFFC003FFU) == 0x39000274U; }));
}

TEST_CASE("AArch64 RCU emitter takes the address of an alloca from the frame pointer") {
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            let value: int = 7;
            return 0;
        }
    )");

    const auto objects = AArch64RcuEmitter(package, "test").Generate();
    const auto words = FunctionWords(objects.front(), "Main");

    const auto found =
        std::ranges::find_if(words, [](const std::uint32_t w) { return FramePointerAddImm(w, 19).has_value(); });
    REQUIRE_MESSAGE(found != words.end(), "the alloca's address is an ADD from X29");
    // The frame record sits at the bottom of the frame, so every local is above
    // it and no alloca is ever reached at a displacement of zero.
    CHECK_GE(*FramePointerAddImm(*found, 19), 16);
}

TEST_CASE("AArch64 RCU emitter loads an encodable float with FMOV and pools the rest") {
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            let near: float64 = 1.5;
            let far: float64 = 1e300;
            let single: float32 = 2.5f32;
            return 0;
        }
    )");

    const auto objects = AArch64RcuEmitter(package, "test").Generate();
    const auto &object = objects.front();
    const auto words = FunctionWords(object, "Main");

    // The two FMOV forms name their value outright and reach no memory at all,
    // and each names the callee-saved vector register the allocation handed the
    // value rather than the scratch one.
    CHECK_MESSAGE(std::ranges::find(words, 0x1E6F1008U) != words.end(), "fmov d8, #1.5");
    CHECK_MESSAGE(std::ranges::find(words, 0x1E209008U) != words.end(), "fmov s8, #2.5");

    // 1e300 is not one of the 256 values FMOV encodes, so it is a doubleword in
    // .rodata reached by ADRP plus a scaled LDR, one relocation on each.
    const auto pooled = RelocsFor(object, RCU_TEXT_IDX, "__f64_0");
    REQUIRE_EQ(pooled.size(), 2);
    CHECK_EQ(pooled[0].type, RcuRelType::AArch64AdrPrelPgHi21);
    CHECK_EQ(pooled[1].type, RcuRelType::AArch64LdstAbsLo12Nc);
    CHECK_EQ(pooled[1].sectionOffset, pooled[0].sectionOffset + 4);
    CHECK_EQ(HexWord(TextWordAt(object, pooled[0].sectionOffset) & 0x9F00001FU), HexWord(0x90000010U)); // adrp x16
    CHECK_EQ(HexWord(TextWordAt(object, pooled[1].sectionOffset) & 0xFFC003FFU),
             HexWord(0xFD400208U)); // ldr d8, [x16]

    // The pooled value is the doubleword the literal denotes, little-endian.
    const auto bytes = RodataOf(object, "__f64_0");
    REQUIRE_EQ(bytes.size(), 8);
    const double expected = 1e300;
    std::uint64_t bits = 0;
    std::memcpy(&bits, &expected, 8);
    for (std::size_t i = 0; i < 8; ++i) {
        CHECK_EQ(bytes[i], static_cast<std::uint8_t>(bits >> (8 * i) & 0xFFU));
    }
}

TEST_CASE("AArch64 RCU emitter interns each distinct string once") {
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            let first = "hi";
            let second = "hi";
            let third = "no";
            return 0;
        }
    )");

    const auto objects = AArch64RcuEmitter(package, "test").Generate();
    const auto &object = objects.front();

    // Two distinct literals, so two symbols and no third: the repeat is the
    // first one's symbol again.
    CHECK(FindSymbol(object, "__str0") != nullptr);
    CHECK(FindSymbol(object, "__str1") != nullptr);
    CHECK(FindSymbol(object, "__str2") == nullptr);
    CHECK_EQ(RodataOf(object, "__str0"), std::vector<std::uint8_t>{'h', 'i', 0});
    CHECK_EQ(RodataOf(object, "__str1"), std::vector<std::uint8_t>{'n', 'o', 0});

    // Both uses of the repeated literal reach it through the same symbol, so it
    // carries two ADRP/ADD pairs rather than one.
    const auto repeated = RelocsFor(object, RCU_TEXT_IDX, "__str0");
    REQUIRE_EQ(repeated.size(), 4);
    for (std::size_t i = 0; i < repeated.size(); i += 2) {
        CHECK_EQ(repeated[i].type, RcuRelType::AArch64AdrPrelPgHi21);
        CHECK_EQ(repeated[i + 1].type, RcuRelType::AArch64AddAbsLo12Nc);
        const std::uint32_t adrp = TextWordAt(object, repeated[i].sectionOffset);
        CHECK_EQ(HexWord(adrp & 0x9F000000U), HexWord(0x90000000U)); // adrp xN
        // The ADD reads and writes the register the ADRP wrote, whichever the
        // allocation gave this literal.
        const std::uint32_t reg = adrp & 0x1FU;
        CHECK_EQ(HexWord(TextWordAt(object, repeated[i + 1].sectionOffset) & 0xFFC003FFU),
                 HexWord(0x91000000U | reg << 5U | reg)); // add xN, xN, #0
    }
}

TEST_CASE("AArch64 RCU emitter writes a constant array into read-only data") {
    const auto package = CompileToAArch64Lir(R"(
        const WORDS: uint32[4] = [1u32, 2u32, 3u32, 0xFFFFFFFFu32];

        func Main() -> int {
            let first = WORDS[0];
            return 0;
        }
    )");

    const auto objects = AArch64RcuEmitter(package, "test").Generate();
    const auto &object = objects.front();

    const std::vector<std::uint8_t> expected = {1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF};
    CHECK_EQ(RodataOf(object, "WORDS"), expected);

    // Reaching it is the same ADRP/ADD pair a string takes, against the
    // constant's own symbol rather than an interned one.
    const auto address = RelocsFor(object, RCU_TEXT_IDX, "WORDS");
    REQUIRE_EQ(address.size(), 2);
    CHECK_EQ(address[0].type, RcuRelType::AArch64AdrPrelPgHi21);
    CHECK_EQ(address[1].type, RcuRelType::AArch64AddAbsLo12Nc);
}

TEST_CASE("AArch64 RCU emitter publishes a constant slice as a header pointing at its elements") {
    // Slice is the standard library's, and this suite compiles no packages, so
    // the declaration the constant needs is written out here.
    const auto package = CompileToAArch64Lir(R"(
        struct Slice<T> { data: *T; length: uint; }

        const TEXT: Slice<char8> = "abc";

        func Main() -> int {
            let length = TEXT.length;
            return 0;
        }
    )");

    const auto objects = AArch64RcuEmitter(package, "test").Generate();
    const auto &object = objects.front();

    CHECK_EQ(RodataOf(object, "TEXT$elements"), std::vector<std::uint8_t>{'a', 'b', 'c', 0});

    // The header is a null data pointer the linker fills in and a length that
    // is already there, so a program reading the length never depends on the
    // relocation having been applied.
    const auto header = RodataOf(object, "TEXT");
    REQUIRE_EQ(header.size(), 16);
    for (std::size_t i = 0; i < 8; ++i) {
        CHECK_EQ(header[i], 0);
    }
    CHECK_EQ(header[8], 3);

    const auto elements = RelocsFor(object, RCU_RODATA_IDX, "TEXT$elements");
    REQUIRE_EQ(elements.size(), 1);
    CHECK_EQ(elements[0].type, RcuRelType::Abs64);
    CHECK_EQ(elements[0].sectionOffset, FindSymbol(object, "TEXT")->value);
}

TEST_CASE("AArch64 RCU emitter gives a scalar constant a symbol in the data section") {
    const auto package = CompileToAArch64Lir(R"(
        pub const LIMIT: int32 = 7;

        func Main() -> int {
            return 0;
        }
    )");

    const auto objects = AArch64RcuEmitter(package, "test").Generate();
    const auto &object = objects.front();

    const RcuSymbol *limit = FindSymbol(object, "LIMIT");
    REQUIRE(limit != nullptr);
    CHECK_EQ(limit->sectionIdx, RCU_DATA_IDX);
    CHECK_EQ(limit->size, 8);
    CHECK_EQ(limit->visibility, RcuSymVis::Global);
    // The value is inlined at every use, so what stands behind the symbol is
    // storage to take the address of rather than the number itself.
    CHECK_EQ(object.sections[RCU_DATA_IDX].data.size(), 8);
}

// Loads, stores and aggregate addressing
//
// Everything below is a whole program the back end lowers without reporting
// anything, so each case checks that as well as the instructions it names: a
// program these opcodes complete is one nothing else in it is missing from.

TEST_CASE("AArch64 RCU emitter loads and stores at the width its type occupies") {
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            var narrow: int8 = 1;
            var wide: uint16 = 2;
            let readNarrow: int8 = narrow;
            let readWide: uint16 = wide;
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    // The width comes from the type and the extension from its signedness: a
    // store names the W view of the register it truncates, a signed load
    // extends into the X one, and an unsigned load into the W one, which
    // zeroes the half of the register above it.
    const std::vector<std::pair<std::uint32_t, const char *>> expected = {
        {0x39000000U, "strb  wN, [xM]"},
        {0x39800000U, "ldrsb xN, [xM]"},
        {0x79000000U, "strh  wN, [xM]"},
        {0x79400000U, "ldrh  wN, [xM]"},
    };
    for (const auto &form : expected) {
        CHECK_MESSAGE(std::ranges::any_of(words,
                                          [&form](const std::uint32_t w) {
                                              return (w & 0xFFC00000U) == form.first && (w >> 10U & 0xFFFU) == 0;
                                          }),
                      form.second);
    }
}

TEST_CASE("AArch64 RCU emitter reaches a field at the offset its layout gives") {
    const auto package = CompileToAArch64Lir(R"(
        struct Pair { first: int; second: int; }

        func Main() -> int {
            var pair = Pair {first: 1, second: 2};
            return pair.second;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    // The second field of a pair of doublewords sits at eight, which is one
    // ADD on the pointer the base register holds.
    CHECK_MESSAGE(std::ranges::any_of(words, [](const std::uint32_t w) { return FieldAddImm(w) == 8U; }),
                  "add xD, xN, #8");
    // The first sits at zero, so it costs no ADD at all: one of any other
    // offset would mean the layout was recomputed rather than read.
    CHECK_FALSE(std::ranges::any_of(words, [](const std::uint32_t w) {
        const auto imm = FieldAddImm(w);
        return imm.has_value() && *imm != 8U;
    }));
}

TEST_CASE("AArch64 RCU emitter scales an index by the width of one element") {
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            let words: int32[4] = [1, 2, 3, 4];
            var pick = words[2];
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    // A four-byte element is a power of two, so the scale is a shift inside
    // the addition and no multiply is emitted at all.
    CHECK_MESSAGE(std::ranges::any_of(words, [](const std::uint32_t w) { return (w & 0xFFE0FC00U) == 0x8B000800U; }),
                  "add xD, xN, xM, lsl #2");
    CHECK_FALSE(std::ranges::any_of(words, [](const std::uint32_t w) { return (w & 0xFFE08000U) == 0x9B000000U; }));
}

TEST_CASE("AArch64 RCU emitter multiplies by an element width no shift reaches") {
    const auto package = CompileToAArch64Lir(R"(
        struct Triple { a: int; b: int; c: int; }

        func Main() -> int {
            var one = Triple {a: 1, b: 2, c: 3};
            let items: Triple[2] = [one, one];
            var pick = items[1];
            return pick.c;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    // Twenty-four bytes is no shift, so the width goes into a register and
    // MADD folds the multiply into the addition it was going to take anyway.
    CHECK_MESSAGE(std::ranges::find(words, 0xD280030CU) != words.end(), "mov x12, #24");
    CHECK_MESSAGE(
        std::ranges::any_of(
            words, [](const std::uint32_t w) { return (w & 0xFFE08000U) == 0x9B000000U && (w >> 16U & 31U) == 12; }),
        "madd xD, xN, x12, xA");
}

TEST_CASE("AArch64 RCU emitter moves an aligned aggregate in pairs") {
    const auto package = CompileToAArch64Lir(R"(
        struct Pair { first: int; second: int; }

        func Main() -> int {
            var pair = Pair {first: 1, second: 2};
            var copy = pair;
            return copy.second;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    // Sixteen bytes at an alignment of eight is one pair each way rather than
    // the four single accesses the descent below would otherwise take.
    CHECK_MESSAGE(std::ranges::any_of(words, [](const std::uint32_t w) { return (w & 0xFFC07C1FU) == 0xA9403009U; }),
                  "ldp x9, x12, [xN, #imm]");
    CHECK_MESSAGE(std::ranges::any_of(words, [](const std::uint32_t w) { return (w & 0xFFC07C1FU) == 0xA9003009U; }),
                  "stp x9, x12, [xN, #imm]");
}

TEST_CASE("AArch64 RCU emitter copies a byte-aligned aggregate a chunk at a time") {
    // Nine bytes with nothing wider than a byte in them, so the whole record
    // has an alignment of one and the pair forms are not available for it.
    const auto package = CompileToAArch64Lir(R"(
        struct Bytes { a: int8; b: int8; c: int8; d: int8; e: int8; f: int8; g: int8; h: int8; i: int8; }

        func Main() -> int {
            var raw = Bytes {a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7, h: 8, i: 9};
            var copy = raw;
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    // The only pairs left in the function are the prologue's and the
    // epilogue's: the frame record, and the callee-saved registers the
    // allocation took, which are saved two at a time for the same reason a
    // copy would have used a pair.
    for (const auto word : words) {
        if (!IsPairAccess(word)) {
            continue;
        }
        const std::uint32_t first = word & 31U;
        const std::uint32_t second = word >> 10U & 31U;
        const bool frameRecord = first == 29 && second == 30;
        const bool calleeSaved = first >= 19 && first <= 28 && second >= 19 && second <= 28;
        const bool prologue = frameRecord || calleeSaved;
        CHECK_MESSAGE(prologue, HexWord(word));
    }
    // Eight of the nine bytes go in one doubleword and the ninth on its own,
    // both through the scratch register a block copy moves through.
    CHECK(std::ranges::any_of(words, [](const std::uint32_t w) { return (w & 0xFFC0001FU) == 0xF9400009U; }));
    CHECK(std::ranges::any_of(words, [](const std::uint32_t w) { return (w & 0xFFC0001FU) == 0x39400009U; }));
}

TEST_CASE("AArch64 RCU emitter reaches a slot past the addressing range through a scratch register") {
    // Enough locals to put the last one past the 32 KiB a scaled doubleword
    // offset reaches, so neither immediate form of LDR can name it.
    // The last of them is an aggregate, which no register holds: every scalar
    // above it lives in the frame only because the allocation ran out of
    // registers, and the copy is what reaches its slot at a displacement.
    std::string body;
    for (int i = 0; i < 4200; ++i) {
        body += std::format("    var v{}: int = {};\n", i, i);
    }
    const auto package = CompileToAArch64Lir(std::format(R"(
        struct Pair {{ first: int; second: int; }}

        func Main() -> int {{
{}            var pair = Pair {{first: 1, second: 2}};
            var copy = pair;
            return copy.second;
        }}
    )",
                                                         body));

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    // The displacement moves into X16, which then addresses at zero — the
    // fallback the encoder's ResolveMemOperand emits, reached here rather than
    // hand-rolled beside the access. Which side of the copy needs it is the
    // frame layout's business, so the access itself is read as either.
    CHECK(std::ranges::any_of(words, [](const std::uint32_t w) { return (w & 0xFF8003FFU) == 0x910003B0U; }));
    CHECK(std::ranges::any_of(words, [](const std::uint32_t w) { return (w & 0xFFBFFFE0U) == 0xF9000200U; }));
}

// Integer arithmetic, bitwise and shift opcodes
//
// Each of these is a whole program too, so an emitter that reported nothing is
// part of what every case checks. The words are the instructions themselves —
// the operands are always X9, X12 and, where a divide needs a third register,
// X10, since every value lives in a stack slot between instructions.

TEST_CASE("AArch64 RCU emitter lowers each binary integer operator to one instruction") {
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            var a: int = 37;
            var b: int = 5;
            var sum = a + b;
            var difference = a - b;
            var product = a * b;
            var conjunction = a & b;
            var disjunction = a | b;
            var exclusive = a ^ b;
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    const std::vector<std::pair<std::uint32_t, const char *>> expected = {
        {0x8B000000U, "add xD, xN, xM"}, {0xCB000000U, "sub xD, xN, xM"}, {0x9B007C00U, "mul xD, xN, xM"},
        {0x8A000000U, "and xD, xN, xM"}, {0xCA000000U, "eor xD, xN, xM"},
    };
    for (const auto &form : expected) {
        CHECK_MESSAGE(HasRegisterForm(words, form.first), form.second);
    }
    // ORR is also how a register-to-register MOV is spelled, so the one this
    // program's `|` produced is the one whose first operand is not the zero
    // register.
    CHECK_MESSAGE(
        std::ranges::any_of(
            words, [](const std::uint32_t w) { return (w & 0xFFE0FC00U) == 0xAA000000U && (w >> 5U & 31U) != 31; }),
        "orr xD, xN, xM");
}

TEST_CASE("AArch64 RCU emitter divides with the instruction the operand's signedness names") {
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            var signedLeft: int = -100;
            var signedRight: int = 7;
            var signedQuotient = signedLeft / signedRight;
            var unsignedLeft: uint = 100;
            var unsignedRight: uint = 7;
            var unsignedQuotient = unsignedLeft / unsignedRight;
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    // The operands arrive extended by their own types, so the divide itself is
    // the only thing that has to know which of the two it is.
    CHECK_MESSAGE(HasRegisterForm(words, 0x9AC00C00U), "sdiv xD, xN, xM");
    CHECK_MESSAGE(HasRegisterForm(words, 0x9AC00800U), "udiv xD, xN, xM");
}

TEST_CASE("AArch64 RCU emitter synthesizes a remainder from a divide and a multiply-subtract") {
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            var left: int = -100;
            var right: int = 7;
            var remainder = left % right;
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    // AArch64 has no remainder instruction: it is the dividend less the
    // quotient times the divisor, and MSUB is that whole expression.
    CHECK_MESSAGE(HasRegisterForm(words, 0x9AC00C00U), "sdiv xD, xN, xM");
    // The quotient goes to X10, which is neither operand, because a remainder
    // needs both of them back — and MSUB reads it there.
    CHECK_MESSAGE(
        std::ranges::any_of(
            words, [](const std::uint32_t w) { return (w & 0xFFE08000U) == 0x9B008000U && (w >> 5U & 31U) == 10; }),
        "msub xD, x10, xM, xA");
    // The multiply is folded into the subtraction rather than standing beside
    // it, so a plain MUL anywhere would mean two instructions where one does.
    CHECK_FALSE(std::ranges::any_of(words, [](const std::uint32_t w) { return (w & 0xFFE0FC00U) == 0x9B007C00U; }));
}

TEST_CASE("AArch64 RCU emitter wraps a narrow result at the width its type occupies") {
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            var small: uint8 = 200;
            var step: uint8 = 100;
            var wrapped = small + step;
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    // Two byte-wide reads, the addition at a whole register's width, and a
    // byte store: 300 wraps to 44 because the byte above it is never written,
    // which is exactly how the x86-64 back end wraps it and costs no masking
    // here either.
    const auto sum = std::ranges::find_if(words, [](const std::uint32_t w) {
        return (w & 0xFFE0FC00U) == 0x8B000000U && (w >> 5U & 31U) == 9 && (w >> 16U & 31U) == 12;
    });
    REQUIRE_MESSAGE(sum != words.end(), "add xD, x9, x12");
    const auto index = static_cast<std::size_t>(sum - words.begin());
    REQUIRE_GE(index, 2);
    REQUIRE_LT(index + 1, words.size());
    // Both operands live in registers the allocation gave them, so what reads
    // one as a byte is UXTB rather than LDRB: the same extension, out of a
    // register rather than out of a slot.
    CHECK_EQ(HexWord(words[index - 2] & 0xFFFFFC1FU), HexWord(0x53001C09U)); // uxtb w9, wN
    CHECK_EQ(HexWord(words[index - 1] & 0xFFFFFC1FU), HexWord(0x53001C0CU)); // uxtb w12, wN
    CHECK_EQ(HexWord(words[index + 1] & 0xFFC00000U), HexWord(0x39000000U)); // strb wD, [xN, #imm]
}

TEST_CASE("AArch64 RCU emitter reads a right shift at the signedness its opcode asks for") {
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            var wide: int32 = -100;
            var amount: int32 = 3;
            var arithmetic = wide >> amount;
            var logical = wide >>> amount;
            var left = wide << amount;
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    // The variable shifts mask the amount to the width of the register they
    // shift, so nothing masks it here and an over-long shift wraps at the same
    // 64 the x86-64 back end's does.
    const auto shift = [&words](const std::uint32_t opcode) {
        return std::ranges::find_if(words, [opcode](const std::uint32_t w) { return (w & 0xFFFFFFE0U) == opcode; });
    };
    const auto arithmetic = shift(0x9ACC2920U); // asr xD, x9, x12
    const auto logical = shift(0x9ACC2520U);    // lsr xD, x9, x12
    REQUIRE_MESSAGE(arithmetic != words.end(), "asr xD, x9, x12");
    REQUIRE_MESSAGE(logical != words.end(), "lsr xD, x9, x12");
    CHECK_MESSAGE(shift(0x9ACC2120U) != words.end(), "lsl xD, x9, x12");

    // What separates the two right shifts is how the value reaches X9 two
    // instructions above each: `>>` sign-extends its operand and `>>>` reads
    // the same signed type as unsigned, which is the whole of the difference
    // between them. Both operands live in a register the allocation gave them,
    // so the extension is out of one rather than out of a slot.
    CHECK_EQ(HexWord(*(arithmetic - 2) & 0xFFFFFC1FU), HexWord(0x93407C09U)); // sxtw x9, wN
    CHECK_EQ(HexWord(*(logical - 2) & 0xFFE0FFFFU), HexWord(0x2A0003E9U));    // mov  w9, wN
}

TEST_CASE("AArch64 RCU emitter negates, complements and tests in one instruction each") {
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            var value: int = 37;
            var negated = -value;
            var complemented = ~value;
            var flag: bool = true;
            var notted = !flag;
            var flipped = ~flag;
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    const std::vector<std::tuple<std::uint32_t, std::uint32_t, const char *>> expected = {
        {0xFFE0FFE0U, 0xCB0003E0U, "neg  xD, xN     — SUB from the zero register"},
        {0xFFE0FFE0U, 0xAA2003E0U, "mvn  xD, xN     — ORN from the zero register"},
        {0xFFFFFC1FU, 0xF100001FU, "cmp  xN, #0"},
        {0xFFFFFFE0U, 0x9A9F17E0U, "cset xD, eq     — the logical negation, as a boolean"},
        {0xFFFFFC00U, 0xD2400000U, "eor  xD, xN, #1 — `~` on a boolean is that negation too"},
    };
    for (const auto &form : expected) {
        const auto mask = std::get<0>(form);
        const auto value = std::get<1>(form);
        CHECK_MESSAGE(std::ranges::any_of(words, [mask, value](const std::uint32_t w) { return (w & mask) == value; }),
                      std::get<2>(form));
    }
    // A boolean is a byte in its slot, so complementing the whole register
    // would leave 0xFE there and read back as true again.
    CHECK_EQ(std::ranges::count_if(words, [](const std::uint32_t w) { return (w & 0xFFE0FFE0U) == 0xAA2003E0U; }), 1);
}

TEST_CASE("AArch64 RCU emitter calls one synthesized exponentiation helper") {
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            var base: int = 5;
            var exponent: int = 3;
            var first = base ** exponent;
            var second = exponent ** base;
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto &object = objects.front();

    // One body, whatever the number of uses, and local to the object: two
    // modules of a package each carry their own rather than sharing one.
    const RcuSymbol *helper = FindSymbol(object, "__rux_ipow");
    REQUIRE(helper != nullptr);
    CHECK_EQ(helper->sectionIdx, RCU_TEXT_IDX);
    CHECK_EQ(helper->visibility, RcuSymVis::Local);
    CHECK_EQ(helper->kind, RcuSymKind::Func);

    // Exponentiation by squaring, with a negative exponent yielding zero and a
    // zero exponent yielding one — the answers the x86-64 helper gives.
    const std::vector<std::uint32_t> expected = {
        0xAA0003E2, // mov  x2, x0        — the base
        0xD2800000, // mov  x0, #0        — a negative exponent yields zero
        0xB7F80101, // tbnz x1, #63, done
        0xD2800020, // mov  x0, #1
        0xB40000C1, // loop: cbz x1, done
        0x36000041, // tbz  w1, #0, square
        0x9B027C00, // mul  x0, x0, x2
        0x9B027C42, // square: mul x2, x2, x2
        0x9341FC21, // asr  x1, x1, #1
        0x17FFFFFB, // b    loop
        0xD65F03C0, // done: ret
    };
    const auto body = FunctionWords(object, "__rux_ipow");
    REQUIRE_EQ(body.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK_EQ(HexWord(body[i]), HexWord(expected[i]));
    }

    // Each use is a BL carrying a branch relocation against that one symbol,
    // since the body is emitted after every function that calls it.
    const auto calls = RelocsFor(object, RCU_TEXT_IDX, "__rux_ipow");
    REQUIRE_EQ(calls.size(), 2);
    for (const auto &call : calls) {
        CHECK_EQ(call.type, RcuRelType::AArch64Call26);
        CHECK_EQ(HexWord(TextWordAt(object, call.sectionOffset) & 0xFC000000U), HexWord(0x94000000U)); // bl
    }
}

TEST_CASE("AArch64 RCU emitter reads a comparison at the signedness of its operands") {
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            var a: int = 3;
            var b: int = 4;
            var below = a < b;
            var atLeast = a >= b;
            var same = a == b;
            var u: uint = 3;
            var v: uint = 4;
            var uBelow = u < v;
            var uAtLeast = u >= v;
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    // One comparison and one CSET each, and the condition is where the
    // signedness of the operands shows: equality reads the same flags either
    // way, and the orderings do not.
    CHECK_EQ(std::ranges::count_if(words, [](const std::uint32_t w) { return (w & 0xFFE0FC1FU) == 0xEB00001FU; }),
             5); // cmp xN, xM
    const std::vector<std::pair<std::uint32_t, const char *>> expected = {
        {0x9A9FA7E0U, "cset xD, lt   — a signed `<`"},
        {0x9A9FB7E0U, "cset xD, ge   — a signed `>=`"},
        {0x9A9F17E0U, "cset xD, eq"},
        {0x9A9F27E0U, "cset xD, lo   — the same two comparisons, unsigned"},
        {0x9A9F37E0U, "cset xD, hs"},
    };
    for (const auto &form : expected) {
        CHECK_MESSAGE(HasCset(words, form.first), form.second);
    }
    // A boolean is one byte of its slot, so the result is stored as one.
    CHECK_EQ(std::ranges::count_if(words, [](const std::uint32_t w) { return (w & 0xFFC00000U) == 0x39000000U; }), 5);
}

TEST_CASE("AArch64 RCU emitter compares floats with conditions a NaN does not satisfy") {
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            var a: float64 = 1.5;
            var b: float64 = 2.5;
            var below = a < b;
            var atMost = a <= b;
            var unequal = a != b;
            var s: float32 = 1.5f32;
            var t: float32 = 2.5f32;
            var above = s > t;
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    // FCMP compares in the register file that holds a float, at the precision
    // the operands are; the general-purpose register only receives the answer.
    CHECK_EQ(std::ranges::count_if(words, [](const std::uint32_t w) { return (w & 0xFFE0FC1FU) == 0x1E602000U; }),
             3); // fcmp dN, dM
    CHECK_EQ(std::ranges::count_if(words, [](const std::uint32_t w) { return (w & 0xFFE0FC1FU) == 0x1E202000U; }),
             1); // fcmp sN, sM

    // MI and LS rather than LT and LE: an unordered comparison leaves C and V
    // set, which LT and LE are satisfied by and these two are not, so `<` and
    // `<=` against a NaN answer false — and `!=` answers true — exactly as the
    // x86-64 back end's ordered/parity pairs do.
    const std::vector<std::pair<std::uint32_t, const char *>> expected = {
        {0x9A9F57E0U, "cset xD, mi   — `<`"},
        {0x9A9F87E0U, "cset xD, ls   — `<=`"},
        {0x9A9F07E0U, "cset xD, ne   — `!=`, the one comparison a NaN satisfies"},
        {0x9A9FD7E0U, "cset xD, gt   — `>`"},
    };
    for (const auto &form : expected) {
        CHECK_MESSAGE(HasCset(words, form.first), form.second);
    }
}

TEST_CASE("AArch64 RCU emitter branches on a boolean and patches both edges") {
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            var a: int = 3;
            var b: int = 4;
            if a < b {
                return 7;
            }
            return 9;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    // A boolean in a register is a value that is zero or is not, so the branch
    // needs no comparison of its own: CBZ takes the edge the condition failed
    // and the B beside it takes the other.
    const auto branch =
        std::ranges::find_if(words, [](const std::uint32_t w) { return (w & 0xFF00001FU) == 0xB4000009U; });
    REQUIRE_MESSAGE(branch != words.end(), "cbz x9, #imm");
    const auto index = static_cast<std::size_t>(branch - words.begin());
    REQUIRE_LT(index + 1, words.size());
    CHECK_EQ(HexWord(words[index - 1] & 0xFFC003FFU), HexWord(0x394003A9U)); // ldrb w9, [x29, #imm]
    CHECK_EQ(HexWord(words[index + 1] & 0xFC000000U), HexWord(0x14000000U)); // b

    // Both displacements were filled in once the blocks they name had offsets,
    // and each one lands on the value that branch's edge returns.
    const auto whenFalse = index + static_cast<std::size_t>(BranchDisplacement(words[index]));
    const auto whenTrue = index + 1 + static_cast<std::size_t>(BranchDisplacement(words[index + 1]));
    REQUIRE_LT(whenFalse, words.size());
    REQUIRE_LT(whenTrue, words.size());
    CHECK_EQ(HexWord(words[whenTrue]), HexWord(0xD28000E9U));  // mov x9, #7
    CHECK_EQ(HexWord(words[whenFalse]), HexWord(0xD2800129U)); // mov x9, #9
}

TEST_CASE("AArch64 RCU emitter closes a loop with a backward branch") {
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            var i: int = 0;
            while i < 10 {
                i += 1;
            }
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    // The back edge is the one branch whose target is behind it, and there is
    // exactly one: what it names is the block that tests the condition, which is
    // the block the conditional branch leaving the loop ends.
    const auto backward = std::ranges::find_if(
        words, [](const std::uint32_t w) { return (w & 0xFC000000U) == 0x14000000U && BranchDisplacement(w) < 0; });
    REQUIRE_MESSAGE(backward != words.end(), "a backward b");
    const auto index = static_cast<std::size_t>(backward - words.begin());
    const auto target = static_cast<std::size_t>(index + BranchDisplacement(words[index]));

    const auto exiting =
        std::ranges::find_if(words, [](const std::uint32_t w) { return (w & 0xFF00001FU) == 0xB4000009U; });
    REQUIRE_MESSAGE(exiting != words.end(), "cbz x9, #imm");
    const auto test = static_cast<std::size_t>(exiting - words.begin());
    REQUIRE_LT(target, test);
    const std::vector<std::uint32_t> condition(words.begin() + static_cast<std::ptrdiff_t>(target),
                                               words.begin() + static_cast<std::ptrdiff_t>(test));
    CHECK_MESSAGE(std::ranges::find(condition, 0xEB0C013FU) != condition.end(), "cmp x9, x12");
    CHECK_MESSAGE(std::ranges::find(condition, 0x9A9FA7E9U) != condition.end(), "cset x9, lt");
}

TEST_CASE("AArch64 RCU emitter widens a conditional branch that cannot reach its block") {
    // A conditional branch keeps nineteen bits of instruction displacement, so
    // it reaches a megabyte of code. Nothing a program is likely to contain puts
    // a block further away than that, and this is what a program that does gets:
    // enough instructions between the branch and the block it skips to that the
    // short form has no encoding at all.
    constexpr std::uint32_t kFiller = 100000;
    LirFunc func;
    func.name = "Main";
    func.isPublic = true;
    func.returnType = TypeRef::MakeInt64();

    LirBlock entry;
    entry.label = "entry";
    entry.instrs.push_back(ConstInstr(0, "true", TypeRef::MakeBool()));
    LirTerminator branch;
    branch.kind = LirTermKind::Branch;
    branch.cond = 0;
    branch.trueTarget = 1;
    branch.falseTarget = 2;
    entry.term = branch;

    LirBlock filler;
    filler.label = "filler";
    for (std::uint32_t i = 0; i < kFiller; ++i) {
        filler.instrs.push_back(ConstInstr(i + 2, "1", TypeRef::MakeInt64()));
    }
    LirTerminator jump;
    jump.kind = LirTermKind::Jump;
    jump.trueTarget = 2;
    filler.term = jump;

    LirBlock exit;
    exit.label = "exit";
    exit.instrs.push_back(ConstInstr(1, "7", TypeRef::MakeInt64()));
    exit.term = ReturnTerm(1);

    func.blocks = {std::move(entry), std::move(filler), std::move(exit)};

    const auto package = PackageOf(std::move(func));
    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    // The branch is the inverse of the one the terminator asked for, jumping
    // over an unconditional branch that carries the target: two instructions
    // rather than one, and twenty-six bits of displacement rather than nineteen.
    const auto widened = std::ranges::find(words, 0xB5000049U); // cbnz x9, #8
    REQUIRE_MESSAGE(widened != words.end(), "cbnz x9, #8");
    const auto index = static_cast<std::size_t>(widened - words.begin());
    REQUIRE_LT(index + 2, words.size());
    CHECK_EQ(HexWord(words[index + 1] & 0xFC000000U), HexWord(0x14000000U)); // b — the false edge
    CHECK_EQ(HexWord(words[index + 2] & 0xFC000000U), HexWord(0x14000000U)); // b — the true edge
    CHECK_MESSAGE(std::ranges::find_if(words, [](const std::uint32_t w) { return (w & 0xFF00001FU) == 0xB4000009U; }) ==
                      words.end(),
                  "no cbz survived the widening");

    // The false edge really was out of reach — a displacement no nineteen-bit
    // field holds — and the true edge is the instruction after the pair.
    CHECK_GT(BranchDisplacement(words[index + 1]), 1 << 18);
    CHECK_EQ(BranchDisplacement(words[index + 2]), 1);
    const auto whenFalse = index + 1 + static_cast<std::size_t>(BranchDisplacement(words[index + 1]));
    REQUIRE_LT(whenFalse, words.size());
    CHECK_EQ(HexWord(words[whenFalse]), HexWord(0xD28000E9U)); // mov x9, #7
}

TEST_CASE("AArch64 RCU emitter breaks a cycle of phi copies through a frame slot") {
    // Two phis in one block naming each other: on the edge that closes the loop
    // the copies exchange two values, which no order of stores performs — the
    // second copy would read what the first one has already overwritten. The
    // shared resolver saves one of them first, and the loop is here because a
    // cycle needs an edge whose source block is the block the phis are in.
    LirFunc func;
    func.name = "Main";
    func.isPublic = true;
    func.returnType = TypeRef::MakeInt64();

    LirBlock entry;
    entry.label = "entry";
    entry.instrs.push_back(ConstInstr(0, "1", TypeRef::MakeInt64()));
    entry.instrs.push_back(ConstInstr(1, "2", TypeRef::MakeInt64()));
    entry.instrs.push_back(ConstInstr(2, "0", TypeRef::MakeInt64()));
    LirTerminator jump;
    jump.kind = LirTermKind::Jump;
    jump.trueTarget = 1;
    entry.term = jump;

    LirBlock loop;
    loop.label = "loop";
    loop.instrs.push_back(PhiInstr(3, {{0, 0}, {4, 1}})); // the two that swap
    loop.instrs.push_back(PhiInstr(4, {{1, 0}, {3, 1}}));
    loop.instrs.push_back(PhiInstr(5, {{2, 0}, {6, 1}})); // and one that does not
    loop.instrs.push_back(ConstInstr(7, "1", TypeRef::MakeInt64()));
    LirInstr add;
    add.op = LirOpcode::Add;
    add.dst = 6;
    add.type = TypeRef::MakeInt64();
    add.srcs = {5, 7};
    loop.instrs.push_back(add);
    loop.instrs.push_back(ConstInstr(8, "5", TypeRef::MakeInt64()));
    LirInstr compare;
    compare.op = LirOpcode::CmpLt;
    compare.dst = 9;
    compare.type = TypeRef::MakeBool();
    compare.srcs = {6, 8};
    loop.instrs.push_back(compare);
    LirTerminator branch;
    branch.kind = LirTermKind::Branch;
    branch.cond = 9;
    branch.trueTarget = 1;
    branch.falseTarget = 2;
    loop.term = branch;

    LirBlock exit;
    exit.label = "exit";
    exit.term = ReturnTerm(3);

    func.blocks = {std::move(entry), std::move(loop), std::move(exit)};

    const auto package = PackageOf(std::move(func));
    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    // The copies belong to the edge rather than to a block, so they sit in the
    // terminator, after a branch that the other edge takes over them.
    const auto over =
        std::ranges::find_if(words, [](const std::uint32_t w) { return (w & 0xFF00001FU) == 0xB4000009U; });
    REQUIRE_MESSAGE(over != words.end(), "cbz x9, #imm");
    const auto index = static_cast<std::size_t>(over - words.begin());
    REQUIRE_EQ(BranchDisplacement(words[index]), 10); // eight words of copies and two branches

    // Four load-and-store pairs where three copies were asked for: the one the
    // cycle does not touch, then the save, then the two that read it.
    std::vector<std::uint32_t> reads;
    std::vector<std::uint32_t> writes;
    for (std::size_t i = index + 1; i < index + 9; i += 2) {
        const auto read = SlotDisplacement(words[i], false);
        const auto write = SlotDisplacement(words[i + 1], true);
        REQUIRE_MESSAGE(read.has_value(), HexWord(words[i]));
        REQUIRE_MESSAGE(write.has_value(), HexWord(words[i + 1]));
        reads.push_back(*read);
        writes.push_back(*write);
    }

    // The save is a copy to a place nothing else in the frame occupies, past
    // every slot because it was reserved after all of them; the copy that would
    // otherwise have read a slot already overwritten reads it instead.
    const std::uint32_t temporary = writes[1];
    CHECK_EQ(reads[3], temporary);
    const std::vector<std::uint32_t> slots = {reads[0], reads[1], reads[2], writes[0], writes[2], writes[3]};
    CHECK_GT(temporary, std::ranges::max(slots));
    CHECK_EQ(reads[1], writes[2]); // saved, then overwritten by the other value
    CHECK_EQ(reads[2], writes[3]); // and that value's own slot takes the saved one
    CHECK_EQ(std::ranges::count(writes, temporary), 1);
}

TEST_CASE("AArch64 RCU emitter lowers a switch to a compare chain and traps where control cannot arrive") {
    // Neither of these reaches a source program yet — the front end lowers a
    // `match` to comparisons of its own and only emits `unreachable` after a
    // panic or a call that does not return — so the LIR is written out here
    // rather than compiled.
    LirFunc func;
    func.name = "Main";
    func.isPublic = true;
    func.returnType = TypeRef::MakeInt64();

    LirBlock entry;
    entry.label = "entry";
    entry.instrs.push_back(ConstInstr(0, "5000", TypeRef::MakeInt64()));
    LirTerminator term;
    term.kind = LirTermKind::Switch;
    term.cond = 0;
    term.defaultTarget = 3;
    term.cases = {{"1", 1}, {"5000", 2}};
    entry.term = term;

    LirBlock first;
    first.label = "first";
    first.instrs.push_back(ConstInstr(1, "10", TypeRef::MakeInt64()));
    first.term = ReturnTerm(1);

    LirBlock second;
    second.label = "second";
    second.instrs.push_back(ConstInstr(2, "20", TypeRef::MakeInt64()));
    second.term = ReturnTerm(2);

    LirBlock unreachable;
    unreachable.label = "unreachable";
    LirTerminator trap;
    trap.kind = LirTermKind::Unreachable;
    unreachable.term = trap;

    func.blocks = {std::move(entry), std::move(first), std::move(second), std::move(unreachable)};

    const auto package = PackageOf(std::move(func));
    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    // A label the arithmetic immediate reaches is one instruction; one it does
    // not is materialized first, which is the encoder's refusal being read
    // rather than a rule about labels restated here.
    const auto reachable = std::ranges::find(words, 0xF100053FU); // cmp x9, #1
    REQUIRE_MESSAGE(reachable != words.end(), "cmp x9, #1");
    const auto materialized = std::ranges::find(words, 0xD282710CU); // mov x12, #5000
    REQUIRE_MESSAGE(materialized != words.end(), "mov x12, #5000");
    const auto index = static_cast<std::size_t>(materialized - words.begin());
    REQUIRE_LT(index + 2, words.size());
    CHECK_EQ(HexWord(words[index + 1]), HexWord(0xEB0C013FU)); // cmp x9, x12

    // Each arm is a branch on equality to its own block, and the default is the
    // fall-through the chain ends in.
    const auto firstArm = static_cast<std::size_t>(reachable - words.begin()) + 1;
    const auto secondArm = index + 2;
    CHECK_EQ(HexWord(words[firstArm] & 0xFF00001FU), HexWord(0x54000000U));  // b.eq
    CHECK_EQ(HexWord(words[secondArm] & 0xFF00001FU), HexWord(0x54000000U)); // b.eq
    const auto whenFirst = firstArm + static_cast<std::size_t>(BranchDisplacement(words[firstArm]));
    const auto whenSecond = secondArm + static_cast<std::size_t>(BranchDisplacement(words[secondArm]));
    REQUIRE_LT(whenFirst, words.size());
    REQUIRE_LT(whenSecond, words.size());
    CHECK_EQ(HexWord(words[whenFirst]), HexWord(0xD2800149U));  // mov x9, #10
    CHECK_EQ(HexWord(words[whenSecond]), HexWord(0xD2800289U)); // mov x9, #20

    // The default block traps: a program that arrives where nothing can arrive
    // stops at the instruction rather than wherever falling through led.
    CHECK_EQ(HexWord(words.back()), HexWord(0x00000000U)); // udf #0
}

// Register allocation
//
// Which values a function keeps in machine registers is the shared linear
// scan's answer; what this back end adds is the pool each register file is
// allocated from and the prologue that preserves it. Every program below is a
// function of no parameters, so the run of frame stores after `mov x29, sp` is
// exactly what the allocation took and nothing else.

TEST_CASE("AArch64 RCU emitter preserves the callee-saved registers it allocated") {
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            var a: int = 1;
            var b: int = 2;
            var c = a + b;
            var d = c * a;
            return c + d;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    const auto saved = SavedRegisters(words);
    CHECK(saved.vector.empty());
    REQUIRE_FALSE(saved.general.empty());
    // X19 through X28 and nothing else: X29 and X30 are the frame record the
    // prologue already wrote, X18 is the platform register no program may
    // touch, and everything below it is a caller's to clobber.
    for (const unsigned reg : saved.general) {
        CHECK_MESSAGE(reg >= 19, reg);
        CHECK_MESSAGE(reg <= 28, reg);
        // What the prologue preserved the epilogue gives back.
        CHECK_MESSAGE(RestoresRegister(words, reg, false), reg);
    }
    // The allocation starts at the bottom of the pool, so the first register a
    // function needs is always X19.
    CHECK_EQ(std::ranges::count(saved.general, 19U), 1);
}

TEST_CASE("AArch64 RCU emitter preserves the low half of every vector register it allocates") {
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            var a: float64 = 1.5;
            var b: float64 = 2.5;
            var c = a + b;
            var d = c * a;
            var e = c - d;
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    const auto saved = SavedRegisters(words);
    REQUIRE_FALSE(saved.vector.empty());
    for (const unsigned reg : saved.vector) {
        CHECK_MESSAGE(reg >= 8, reg);
        CHECK_MESSAGE(reg <= 15, reg);
        CHECK_MESSAGE(RestoresRegister(words, reg, true), reg);
    }
    // A doubleword each, which is the whole of what AAPCS64 asks a callee to
    // preserve of V8 through V15 — and the whole of what this back end puts
    // there, since a float64 is a doubleword and a float32 is half of one.
    CHECK_EQ(std::ranges::count_if(words, [](const std::uint32_t w) { return (w & 0xFFC003E0U) == 0x3D0003A0U; }),
             0); // no str qN, [x29, #imm]
}

TEST_CASE("AArch64 RCU emitter never allocates the platform register") {
    // Fourteen values live at once, which is more than the ten X19 through X28
    // supply: what the pool does not reach stays in the frame rather than
    // reaching past the end of it.
    std::string body;
    std::string sum;
    for (int i = 0; i < 14; ++i) {
        body += std::format("            var v{}: int = {};\n", i, i);
        sum += std::format("{}v{}", i == 0 ? "" : " + ", i);
    }
    const auto package = CompileToAArch64Lir(std::format(R"(
        func Main() -> int {{
{}            return {};
        }}
    )",
                                                         body, sum));

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    const auto saved = SavedRegisters(words);
    CHECK_LE(saved.general.size(), 10);
    CHECK_FALSE(std::ranges::contains(saved.general, 18U));
    for (const unsigned reg : saved.general) {
        CHECK_MESSAGE(reg >= 19, reg);
        CHECK_MESSAGE(reg <= 28, reg);
    }
}

TEST_CASE("AArch64 RCU emitter keeps every value in the frame where control branches") {
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            var a: int = 1;
            var b: int = 2;
            if a < b {
                b = a + b;
            }
            return b;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    // A value in a register would have to be correct at every edge, and a phi
    // lowers to copies between slots here, so a function of more than one block
    // allocates nothing at all — and preserves nothing, which is how a prologue
    // says so.
    const auto saved = SavedRegisters(words);
    CHECK(saved.general.empty());
    CHECK(saved.vector.empty());
}

// AAPCS64 calls
//
// One convention for both sides of every call: the cases below read the caller
// and the callee out of the same object and check that what one wrote is where
// the other looks. Nothing here depends on a calling convention the LIR names,
// because AArch64 has one and a Rux function and a C function are called by it
// alike.

TEST_CASE("Apple AArch64 selects its fixed-argument layout from the target OS") {
    constexpr AArch64CallLayoutPolicy apple = AArch64CallPolicyFor(Target::OS::MacOS);
    constexpr AArch64CallLayoutPolicy ios = AArch64CallPolicyFor(Target::OS::iOS);
    constexpr AArch64CallLayoutPolicy linux = AArch64CallPolicyFor(Target::OS::Linux);
    constexpr AArch64CallLayoutPolicy windows = AArch64CallPolicyFor(Target::OS::Windows);

    CHECK_EQ(apple.StackAlignment(1), 1);
    CHECK_EQ(apple.StackBytes(1), 1);
    CHECK_EQ(apple.FirstGeneralRegister(1, 16), 1);
    CHECK(apple.callerExtendsNarrowIntegers);
    CHECK_EQ(ios.FirstGeneralRegister(1, 16), 1);
    CHECK(ios.compactStackArguments);

    for (const AArch64CallLayoutPolicy policy : {linux, windows}) {
        CHECK_EQ(policy.StackAlignment(1), 8);
        CHECK_EQ(policy.StackBytes(1), 8);
        CHECK_EQ(policy.FirstGeneralRegister(1, 16), 2);
        CHECK_FALSE(policy.callerExtendsNarrowIntegers);
    }
}

TEST_CASE("Apple AArch64 packs fixed stack arguments at natural alignment") {
    auto lowered = CompileToAArch64Lir(R"(
        func Packed(a: int, b: int, c: int, d: int, e: int, f: int, g: int, h: int,
                    i: int8, j: uint16, k: int32, l: int64) -> int64 {
            return l;
        }

        func Main() -> int {
            var result = Packed(1, 2, 3, 4, 5, 6, 7, 8, -9, 10, 11, 12);
            return 0;
        }
    )");

    // Put the definition and use in separate RCU objects. Each module then
    // classifies its own side of the call, as separately compiled packages do.
    LirPackage package;
    package.modules.resize(2);
    package.modules[0].name = "callee.rux";
    package.modules[1].name = "caller.rux";
    for (auto &func : lowered.modules.front().funcs) {
        const std::size_t module = func.name == "Packed" ? 0 : 1;
        package.modules[module].funcs.push_back(std::move(func));
    }

    const auto emitFor = [&package](const Target::OS os) {
        AArch64RcuEmitter emitter(package, "test", os);
        auto objects = emitter.Generate();
        CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
        return objects;
    };
    const auto appleObjects = emitFor(Target::OS::MacOS);
    const auto linuxObjects = emitFor(Target::OS::Linux);
    const auto windowsObjects = emitFor(Target::OS::Windows);

    const auto objectWith = [](const std::vector<RcuFile> &objects, const std::string_view symbol) -> const RcuFile & {
        const auto found = std::ranges::find_if(objects, [symbol](const RcuFile &object) {
            const RcuSymbol *candidate = FindSymbol(object, symbol);
            return candidate != nullptr && candidate->sectionIdx != RCU_SEC_EXTERNAL;
        });
        REQUIRE(found != objects.end());
        return *found;
    };

    const auto outgoingArea = [](const std::vector<std::uint32_t> &words) {
        for (const std::uint32_t word : words) {
            if (const auto bytes = StackPointerAdjustment(word, true)) {
                return *bytes;
            }
        }
        return std::int64_t{0};
    };
    const auto hasStore = [](const std::vector<std::uint32_t> &words, const unsigned width,
                             const std::uint32_t offset) {
        return std::ranges::any_of(words, [width, offset](const std::uint32_t word) {
            return StackArgumentStored(word, width) == std::optional<std::uint32_t>(offset);
        });
    };

    const auto appleCaller = FunctionWords(objectWith(appleObjects, "Main"), "Main");
    CHECK_EQ(outgoingArea(appleCaller), 16);
    CHECK(hasStore(appleCaller, 1, 0));
    CHECK(hasStore(appleCaller, 2, 2));
    CHECK(hasStore(appleCaller, 4, 4));
    CHECK(hasStore(appleCaller, 8, 8));

    // The callee makes the same target-selected walk independently and reads
    // the four values immediately above its valid X29/X30 frame record.
    const auto appleCallee = FunctionWords(objectWith(appleObjects, "Packed"), "Packed");
    const auto frame = PreIndexedFrameSize(appleCallee.front());
    REQUIRE_MESSAGE(frame.has_value(), HexWord(appleCallee.front()));
    const auto hasIncoming = [&appleCallee, frame](const unsigned width, const bool sign, const std::int32_t offset) {
        return std::ranges::any_of(appleCallee, [width, sign, offset, frame](const std::uint32_t word) {
            return IncomingDisplacement(word, width, sign) ==
                   std::optional<std::int32_t>(static_cast<std::int32_t>(*frame) + offset);
        });
    };
    CHECK(hasIncoming(1, true, 0));
    CHECK(hasIncoming(2, false, 2));
    CHECK(hasIncoming(4, true, 4));
    CHECK(hasIncoming(8, true, 8));

    // Linux and non-variadic Windows retain the previous generic AAPCS64
    // doubleword layout for the same LIR call.
    for (const auto *objects : {&linuxObjects, &windowsObjects}) {
        const auto caller = FunctionWords(objectWith(*objects, "Main"), "Main");
        CHECK_EQ(outgoingArea(caller), 32);
        for (std::uint32_t slot = 0; slot < 4; ++slot) {
            CHECK(hasStore(caller, 8, slot * 8));
        }
    }
}

TEST_CASE("Apple AArch64 callers extend fixed narrow integer arguments") {
    const auto package = CompileToAArch64Lir(R"(
        func Narrow(signedByte: int8, unsignedShort: uint16) -> int {
            return 0;
        }

        func Main() -> int {
            return Narrow(-1, 65535);
        }
    )");

    AArch64RcuEmitter emitter(package, "test", Target::OS::MacOS);
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto caller = FunctionWords(objects.front(), "Main");
    const auto call = BranchAndLinkIndex(caller);
    REQUIRE(call.has_value());
    const auto beforeCall = std::ranges::subrange(caller.begin(), caller.begin() + static_cast<std::ptrdiff_t>(*call));

    // SXT[BH] writes the full X register for signed values; UXT[BH] writes W
    // and therefore zeroes the upper half. The destination fields are X0 and
    // W1, the registers in which the declared parameters travel.
    CHECK(
        std::ranges::any_of(beforeCall, [](const std::uint32_t word) { return (word & 0xFFFFFC1FU) == 0x93401C00U; }));
    CHECK(
        std::ranges::any_of(beforeCall, [](const std::uint32_t word) { return (word & 0xFFFFFC1FU) == 0x53003C01U; }));
}

TEST_CASE("AArch64 RCU emitter passes the first eight integer arguments in the registers AAPCS64 names") {
    const auto package = CompileToAArch64Lir(R"(
        func Eight(a: int, b: int, c: int, d: int, e: int, f: int, g: int, h: int) -> int {
            return h;
        }

        func Main() -> int {
            return Eight(1, 2, 3, 4, 5, 6, 7, 8);
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    // The caller fills X0 through X7 in order, and those eight loads are the
    // eight instructions the branch follows.
    const auto caller = FunctionWords(objects.front(), "Main");
    const auto call = BranchAndLinkIndex(caller);
    REQUIRE(call.has_value());
    REQUIRE_GE(*call, 8);
    for (unsigned reg = 0; reg < 8; ++reg) {
        const std::uint32_t word = caller[*call - 8 + reg];
        const auto loaded = ArgumentFilled(word);
        REQUIRE_MESSAGE(loaded.has_value(), HexWord(word));
        CHECK_EQ(*loaded, reg);
    }

    // Eight arguments need no stack of their own, so nothing moves the stack
    // pointer between the prologue that opened the frame and the branch.
    for (const auto word : caller) {
        CHECK_FALSE(StackPointerAdjustment(word, true).has_value());
        CHECK_FALSE(StackPointerAdjustment(word, false).has_value());
    }

    // The callee takes the same eight registers out of them in the same order
    // and before it does anything else, which is what makes every later mention
    // of a parameter a read of wherever it put it.
    const auto callee = FunctionWords(objects.front(), "Eight");
    const auto first = std::ranges::find_if(
        callee, [](const std::uint32_t w) { return ArgumentDrained(w) == std::optional<unsigned>(0); });
    REQUIRE(first != callee.end());
    const auto spills = static_cast<std::size_t>(first - callee.begin());
    REQUIRE_GE(callee.size(), spills + 8);
    for (unsigned reg = 0; reg < 8; ++reg) {
        const std::uint32_t word = callee[spills + reg];
        const auto drained = ArgumentDrained(word);
        REQUIRE_MESSAGE(drained.has_value(), HexWord(word));
        CHECK_EQ(*drained, reg);
    }
}

TEST_CASE("AArch64 RCU emitter sends the ninth argument and everything past it on the stack") {
    const auto package = CompileToAArch64Lir(R"(
        func Ten(a: int, b: int, c: int, d: int, e: int, f: int, g: int, h: int, i: int, j: uint16) -> uint16 {
            return j;
        }

        func Main() -> int {
            var result = Ten(1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    // Two arguments past the eighth take a doubleword each, and the area they
    // sit in is rounded to the sixteen bytes the stack pointer is a multiple of.
    const auto caller = FunctionWords(objects.front(), "Main");
    const auto call = BranchAndLinkIndex(caller);
    REQUIRE(call.has_value());
    // Five instructions open the area and fill it, and the eight argument
    // registers are loaded after them, so the branch is the fourteenth.
    REQUIRE_GE(*call, 13);
    const std::size_t open = *call - 13;
    const auto opened = StackPointerAdjustment(caller[open], true);
    REQUIRE_MESSAGE(opened.has_value(), HexWord(caller[open]));
    CHECK_EQ(*opened, 16);
    REQUIRE_LT(*call + 1, caller.size());
    const auto closed = StackPointerAdjustment(caller[*call + 1], false);
    REQUIRE_MESSAGE(closed.has_value(), HexWord(caller[*call + 1]));
    CHECK_EQ(*closed, 16);

    // Both stack arguments are written a whole doubleword at a time, so the
    // narrow one occupies its slot's low bytes and leaves the next slot alone.
    CHECK_EQ(HexWord(caller[open + 2]), HexWord(0xF90003E9U)); // str x9, [sp]
    CHECK_EQ(HexWord(caller[open + 4]), HexWord(0xF90007E9U)); // str x9, [sp, #8]
    // The narrow one is read out of its own slot at its own width before it is
    // written out at the stack slot's.
    CHECK_EQ(HexWord(caller[open + 3] & 0xFFC003FFU), HexWord(0x794003A9U)); // ldrh w9, [x29]

    // The callee finds them directly above its own frame: the frame record sits
    // at the bottom of the frame, so what the caller wrote at its stack pointer
    // is at X29 plus the frame size.
    const auto callee = FunctionWords(objects.front(), "Ten");
    REQUIRE_GT(callee.size(), 12);
    const auto frame = PreIndexedFrameSize(callee.front());
    REQUIRE_MESSAGE(frame.has_value(), HexWord(callee.front()));
    const auto reads = [&callee](const unsigned width, const std::int32_t displacement) {
        return std::ranges::any_of(callee, [width, displacement](const std::uint32_t w) {
            return IncomingDisplacement(w, width) == std::optional<std::int32_t>(displacement);
        });
    };
    CHECK(reads(8, *frame));
    // A narrow one is read at the width its own type occupies rather than a
    // whole doubleword, since a C caller leaves the bytes above it as it found
    // them.
    CHECK(reads(2, *frame + 8));
}

TEST_CASE("AArch64 RCU emitter returns a value in X0 and extends a narrow one on the way out") {
    const auto package = CompileToAArch64Lir(R"(
        func Byte(a: uint8, b: uint8) -> uint8 {
            return a + b;
        }

        func Short(a: int16) -> int16 {
            return a;
        }

        func Main() -> int {
            var wrapped = Byte(200, 100);
            var negative = Short(-3);
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    // What fills X0 extends by the returned type, which is the whole of what
    // AAPCS64 asks a callee to do: unsigned zeroes the register above the
    // value, signed sign-extends it. The value being returned lives in a
    // register the allocation gave it, so the extension is out of one rather
    // than out of a slot — the same instruction either way.
    const auto byteReturn = FunctionWords(objects.front(), "Byte");
    CHECK_MESSAGE(
        std::ranges::any_of(byteReturn, [](const std::uint32_t w) { return (w & 0xFFFFFC1FU) == 0x53001C00U; }),
        "uxtb w0, wN");
    const auto shortReturn = FunctionWords(objects.front(), "Short");
    CHECK_MESSAGE(
        std::ranges::any_of(shortReturn, [](const std::uint32_t w) { return (w & 0xFFFFFC1FU) == 0x93403C00U; }),
        "sxth x0, wN");

    // The caller keeps what came back and writes only the bytes the type
    // occupies into the local it belongs to, so nothing it does afterwards
    // depends on the bits above them.
    const auto caller = FunctionWords(objects.front(), "Main");
    const auto call = BranchAndLinkIndex(caller);
    REQUIRE(call.has_value());
    REQUIRE_LT(*call + 2, caller.size());
    const auto kept = ArgumentDrained(caller[*call + 1]);
    REQUIRE_MESSAGE(kept == std::optional<unsigned>(0), HexWord(caller[*call + 1]));
    const std::uint32_t result = caller[*call + 1] & 0x1FU;
    CHECK_EQ(HexWord(caller[*call + 2] & 0xFFC0001FU), HexWord(0x39000000U | result)); // strb wN, [xM]
}

TEST_CASE("AArch64 RCU emitter branches to a function this module defines through a relocation") {
    const auto package = CompileToAArch64Lir(R"(
        func Callee() -> int {
            return 7;
        }

        func Main() -> int {
            return Callee();
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto &object = objects.front();

    // The branch is emitted with no displacement at all: where Callee ended up
    // is the relocation's answer rather than this generator's.
    const auto caller = FunctionWords(object, "Main");
    const auto call = BranchAndLinkIndex(caller);
    REQUIRE(call.has_value());
    CHECK_EQ(HexWord(caller[*call]), HexWord(0x94000000U)); // bl #0

    const RcuSymbol *main = FindSymbol(object, "Main");
    REQUIRE(main != nullptr);
    const auto relocs = RelocsFor(object, RCU_TEXT_IDX, "Callee");
    REQUIRE_EQ(relocs.size(), 1);
    CHECK_EQ(relocs.front().type, RcuRelType::AArch64Call26);
    CHECK_EQ(relocs.front().sectionOffset, main->value + *call * 4);
    CHECK_EQ(TextWordAt(object, relocs.front().sectionOffset), 0x94000000U);
}

TEST_CASE("AArch64 RCU emitter calls through a register when the callee is a value") {
    const auto package = CompileToAArch64Lir(R"(
        func Add(a: int, b: int) -> int {
            return a + b;
        }

        func Apply(f: func(int, int) -> int, a: int, b: int) -> int {
            return f(a, b);
        }

        func Main() -> int {
            return Apply(Add, 1, 2);
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    const auto words = FunctionWords(objects.front(), "Apply");
    const auto indirect = std::ranges::find(words, 0xD63F0120U); // blr x9
    REQUIRE_MESSAGE(indirect != words.end(), "blr x9");
    const auto index = static_cast<std::size_t>(indirect - words.begin());
    REQUIRE_GE(index, 3);

    // The address is fetched after the argument registers and into X9 rather
    // than one of them, so fetching it cannot disturb what it is called with.
    CHECK_EQ(ArgumentFilled(words[index - 3]), std::optional<unsigned>(0));
    CHECK_EQ(ArgumentFilled(words[index - 2]), std::optional<unsigned>(1));
    CHECK_EQ(ArgumentFilled(words[index - 1]), std::optional<unsigned>(9));

    // Nothing names a symbol: an indirect call has no target to relocate.
    CHECK_FALSE(BranchAndLinkIndex(words).has_value());
}

TEST_CASE("AArch64 RCU emitter carries the library an extern declaration names to its symbol") {
    const auto package = CompileToAArch64Lir(R"(
        #Link("libc.so.6")
        extern {
            func abs(n: int32) -> int32;
        }

        func Main() -> int {
            var magnitude = abs(-5);
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto &object = objects.front();

    // The declaration was predeclared with its library, so the call site found
    // that symbol rather than creating a second one without it.
    const RcuSymbol *symbol = FindSymbol(object, "abs");
    REQUIRE(symbol != nullptr);
    CHECK_EQ(symbol->kind, RcuSymKind::ExternFunc);
    CHECK_EQ(symbol->visibility, RcuSymVis::Global);
    CHECK_EQ(symbol->sectionIdx, RCU_SEC_EXTERNAL);
    CHECK_EQ(symbol->typeName, "libc.so.6");

    const auto relocs = RelocsFor(object, RCU_TEXT_IDX, "abs");
    REQUIRE_EQ(relocs.size(), 1);
    CHECK_EQ(relocs.front().type, RcuRelType::AArch64Call26);
    CHECK_EQ(TextWordAt(object, relocs.front().sectionOffset), 0x94000000U); // bl #0
}

TEST_CASE("AArch64 RCU emitter ends a path at a call that does not return") {
    const auto package = CompileToAArch64Lir(R"(
        #NoReturn()
        func Die() {
            Die();
        }

        func Main() -> int {
            Die();
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    // The branch is the last thing on the path and the trap is what stands
    // where a fall-through would have been: no epilogue is emitted after a call
    // control does not come back from.
    const auto words = FunctionWords(objects.front(), "Main");
    REQUIRE_GE(words.size(), 2);
    CHECK_EQ(HexWord(words.back()), HexWord(0x00000000U));                          // udf #0
    CHECK_EQ(HexWord(words[words.size() - 2] & 0xFC000000U), HexWord(0x94000000U)); // bl
    CHECK_EQ(std::ranges::count(words, 0xD65F03C0U), 0);                            // ret

    // The function it names is the same shape, and is the whole of it: a frame
    // record, the branch and the trap.
    const auto callee = FunctionWords(objects.front(), "Die");
    REQUIRE_EQ(callee.size(), 4);
    CHECK_EQ(HexWord(callee.back()), HexWord(0x00000000U)); // udf #0
}

TEST_CASE("AArch64 RCU emitter passes the first eight floats in the vector registers and returns in V0") {
    const auto package = CompileToAArch64Lir(R"(
        func Eight(a: float64, b: float64, c: float64, d: float64,
                   e: float64, f: float64, g: float64, h: float64) -> float64 {
            return h;
        }

        func Main() -> int {
            var last = Eight(1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5);
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    // The caller fills D0 through D7 in order, and those eight loads are the
    // eight instructions the branch follows. Nothing touches the stack: the
    // vector file carries all eight on its own.
    const auto caller = FunctionWords(objects.front(), "Main");
    const auto call = BranchAndLinkIndex(caller);
    REQUIRE(call.has_value());
    REQUIRE_GE(*call, 8);
    for (unsigned reg = 0; reg < 8; ++reg) {
        const std::uint32_t word = caller[*call - 8 + reg];
        const auto filled = VectorArgumentFilled(word, 64);
        REQUIRE_MESSAGE(filled.has_value(), HexWord(word));
        CHECK_EQ(*filled, reg);
    }
    for (const auto word : caller) {
        CHECK_FALSE(StackPointerAdjustment(word, true).has_value());
    }

    // The callee spills the same eight, and answers in the register the first
    // argument arrived in.
    const auto callee = FunctionWords(objects.front(), "Eight");
    const std::size_t spills = VectorDrainStart(callee, 64);
    REQUIRE_GE(callee.size(), spills + 8);
    for (unsigned reg = 0; reg < 8; ++reg) {
        const std::uint32_t word = callee[spills + reg];
        const auto drained = VectorArgumentDrained(word, 64);
        REQUIRE_MESSAGE(drained.has_value(), HexWord(word));
        CHECK_EQ(*drained, reg);
    }
    CHECK(std::ranges::any_of(
        callee, [](const std::uint32_t w) { return VectorArgumentFilled(w, 64) == std::optional<unsigned>(0); }));
}

TEST_CASE("AArch64 RCU emitter counts the two register files apart") {
    const auto package = CompileToAArch64Lir(R"(
        func Mixed(a: int, b: float64, c: int, d: float32, e: int) -> float32 {
            return d;
        }

        func Main() -> int {
            var narrow = Mixed(1, 2.5, 3, 4.5f32, 5);
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    // Neither file knows what the other has taken: the three integers are X0
    // through X2 and the two floats are V0 and V1, with the single-precision
    // one read at its own width.
    const auto caller = FunctionWords(objects.front(), "Main");
    const auto call = BranchAndLinkIndex(caller);
    REQUIRE(call.has_value());
    REQUIRE_GE(*call, 5);
    CHECK_EQ(ArgumentFilled(caller[*call - 5]), std::optional<unsigned>(0));
    CHECK_EQ(VectorArgumentFilled(caller[*call - 4], 64), std::optional<unsigned>(0));
    CHECK_EQ(ArgumentFilled(caller[*call - 3]), std::optional<unsigned>(1));
    CHECK_EQ(VectorArgumentFilled(caller[*call - 2], 32), std::optional<unsigned>(1));
    CHECK_EQ(ArgumentFilled(caller[*call - 1]), std::optional<unsigned>(2));

    // A float32 result comes back in S0, which is the same register the second
    // argument was passed in and a different width from it.
    const auto callee = FunctionWords(objects.front(), "Mixed");
    CHECK(std::ranges::any_of(
        callee, [](const std::uint32_t w) { return VectorArgumentFilled(w, 32) == std::optional<unsigned>(0); }));
}

TEST_CASE("AArch64 RCU emitter passes a homogeneous float aggregate in consecutive vector registers") {
    const auto package = CompileToAArch64Lir(R"(
        struct Pair { x: float64; y: float64; }
        struct Quad { a: float32; b: float32; c: float32; d: float32; }

        func TakePair(p: Pair) -> float64 {
            return p.y;
        }

        func TakeQuad(q: Quad) -> float32 {
            return q.d;
        }

        func MakePair(v: float64) -> Pair {
            return Pair { x: v, y: v };
        }

        func Main() -> int {
            var pair = Pair { x: 1.5, y: 2.5 };
            var quad = Quad { a: 1.0f32, b: 2.0f32, c: 3.0f32, d: 4.0f32 };
            var y = TakePair(pair);
            var d = TakeQuad(quad);
            var made = MakePair(3.5);
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    // Two doubles are D0 and D1, read eight bytes apart because that is how
    // wide one member is; four singles are S0 through S3, four bytes apart.
    // No register holds an aggregate, so an HFA is spilled to the frame
    // whatever the allocation did with the scalars around it.
    const auto pairCallee = FunctionWords(objects.front(), "TakePair");
    const std::size_t pairSpills = VectorDrainStart(pairCallee, 64);
    REQUIRE_GE(pairCallee.size(), pairSpills + 2);
    const auto first = VectorSlotAccessOf(pairCallee[pairSpills], 64, true);
    const auto second = VectorSlotAccessOf(pairCallee[pairSpills + 1], 64, true);
    REQUIRE_MESSAGE(first.has_value(), HexWord(pairCallee[pairSpills]));
    REQUIRE_MESSAGE(second.has_value(), HexWord(pairCallee[pairSpills + 1]));
    CHECK_EQ(first->reg, 0);
    CHECK_EQ(second->reg, 1);
    CHECK_EQ(second->displacement, first->displacement + 8);

    const auto quadCallee = FunctionWords(objects.front(), "TakeQuad");
    const std::size_t quadSpills = VectorDrainStart(quadCallee, 32);
    REQUIRE_GE(quadCallee.size(), quadSpills + 4);
    const auto base = VectorSlotAccessOf(quadCallee[quadSpills], 32, true);
    REQUIRE_MESSAGE(base.has_value(), HexWord(quadCallee[quadSpills]));
    for (unsigned member = 0; member < 4; ++member) {
        const auto spilled = VectorSlotAccessOf(quadCallee[quadSpills + member], 32, true);
        REQUIRE_MESSAGE(spilled.has_value(), HexWord(quadCallee[quadSpills + member]));
        CHECK_EQ(spilled->reg, member);
        CHECK_EQ(spilled->displacement, base->displacement + 4 * member);
    }

    // Sixteen bytes of floats come back in two vector registers rather than
    // through memory the caller named: an aggregate this large is returned
    // indirectly only when it is made of something else.
    const auto maker = FunctionWords(objects.front(), "MakePair");
    std::optional<VectorSlotAccess> returnedLow;
    std::optional<VectorSlotAccess> returnedHigh;
    for (std::size_t i = 0; i + 1 < maker.size(); ++i) {
        const auto low = VectorSlotAccessOf(maker[i], 64, false);
        const auto high = VectorSlotAccessOf(maker[i + 1], 64, false);
        if (low && high && low->reg == 0 && high->reg == 1) {
            returnedLow = low;
            returnedHigh = high;
        }
    }
    REQUIRE_MESSAGE(returnedLow.has_value(), "ldr d0, [x29, #imm]");
    REQUIRE_MESSAGE(returnedHigh.has_value(), "ldr d1, [x29, #imm]");
    CHECK_EQ(returnedHigh->displacement, returnedLow->displacement + 8);
    for (const auto word : maker) {
        CHECK_FALSE(ArgumentDrained(word) == std::optional<unsigned>(8));
    }
}

TEST_CASE("AArch64 RCU emitter carries a composite of no more than sixteen bytes in whole registers") {
    const auto package = CompileToAArch64Lir(R"(
        struct Small { a: int32; b: int32; }
        struct Mid { a: int64; b: int32; }

        func TakeSmall(s: Small) -> int32 {
            return s.b;
        }

        func TakeMid(m: Mid) -> int32 {
            return m.b;
        }

        func MakeMid(n: int64) -> Mid {
            return Mid { a: n, b: 7i32 };
        }

        func Main() -> int {
            var small = Small { a: 1i32, b: 2i32 };
            var mid = Mid { a: 3i64, b: 4i32 };
            var fromSmall = TakeSmall(small);
            var fromMid = TakeMid(mid);
            var made = MakeMid(5i64);
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    // Eight bytes are one register, and twelve are two: a composite is broken
    // into whole doublewords, and the second of them carries the four bytes of
    // padding its slot was rounded up by rather than the value beside it.
    const auto smallCallee = FunctionWords(objects.front(), "TakeSmall");
    const std::size_t smallSpills = DrainStart(smallCallee);
    REQUIRE_GE(smallCallee.size(), smallSpills + 2);
    CHECK_FALSE(ArgumentDrained(smallCallee[smallSpills + 1]) == std::optional<unsigned>(1));

    const auto midCallee = FunctionWords(objects.front(), "TakeMid");
    const std::size_t midSpills = DrainStart(midCallee);
    REQUIRE_GE(midCallee.size(), midSpills + 2);
    CHECK_EQ(ArgumentDrained(midCallee[midSpills + 1]), std::optional<unsigned>(1));
    CHECK_EQ(SlotAccessDisplacement(midCallee[midSpills + 1]), SlotAccessDisplacement(midCallee[midSpills]) + 8);

    // The same shape backwards: twelve bytes come back in X0 and X1, and the
    // caller keeps both.
    const auto maker = FunctionWords(objects.front(), "MakeMid");
    bool returnsPair = false;
    for (std::size_t i = 0; i + 1 < maker.size(); ++i) {
        returnsPair = returnsPair || (ArgumentFilled(maker[i]) == std::optional<unsigned>(0) &&
                                      ArgumentFilled(maker[i + 1]) == std::optional<unsigned>(1));
    }
    CHECK(returnsPair);
}

TEST_CASE("AArch64 RCU emitter passes a composite past sixteen bytes as the address of a copy") {
    const auto package = CompileToAArch64Lir(R"(
        struct Big { a: int64; b: int64; c: int64; }

        func TakeBig(b: Big) -> int64 {
            return b.a;
        }

        func Main() -> int {
            var big = Big { a: 1i64, b: 2i64, c: 3i64 };
            var first = TakeBig(big);
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    // Twenty-four bytes of copy, rounded to the sixteen the stack pointer is a
    // multiple of, and the address of that copy is what X0 carries.
    const auto caller = FunctionWords(objects.front(), "Main");
    const auto call = BranchAndLinkIndex(caller);
    REQUIRE(call.has_value());
    REQUIRE_GE(*call, 1);
    CHECK_EQ(StackPointerAddImm(caller[*call - 1], 0), std::optional<std::uint32_t>(0));
    std::optional<std::int64_t> opened;
    for (const auto word : caller) {
        if (const auto adjustment = StackPointerAdjustment(word, true); adjustment.has_value()) {
            opened = adjustment;
            break;
        }
    }
    CHECK_EQ(opened, std::optional<std::int64_t>(32));

    // The callee reads the copy into its own frame once and never writes
    // through the address again, so a parameter it modifies is its own.
    const auto callee = FunctionWords(objects.front(), "TakeBig");
    CHECK(std::ranges::find(callee, 0xAA0003EBU) != callee.end()); // mov x11, x0
    CHECK(std::ranges::any_of(callee, [](const std::uint32_t word) { return IsPairAccess(word); }));
}

TEST_CASE("AArch64 RCU emitter returns a large composite through the memory the caller names in X8") {
    const auto package = CompileToAArch64Lir(R"(
        struct Big { a: int64; b: int64; c: int64; }

        func MakeBig(n: int64) -> Big {
            return Big { a: n, b: n, c: n };
        }

        func Main() -> int {
            var big = MakeBig(4i64);
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    // The caller names the memory before it branches, and keeps nothing
    // afterwards: the callee has already written the whole value there.
    const auto caller = FunctionWords(objects.front(), "Main");
    const auto call = BranchAndLinkIndex(caller);
    REQUIRE(call.has_value());
    REQUIRE_GE(*call, 1);
    CHECK(FramePointerAddImm(caller[*call - 1], 8).has_value());
    REQUIRE_LT(*call + 1, caller.size());
    CHECK_FALSE(ArgumentDrained(caller[*call + 1]) == std::optional<unsigned>(0));

    // The callee keeps that address the way it keeps a parameter, and nothing
    // it returns travels in a register: no load ever fills X0.
    const auto callee = FunctionWords(objects.front(), "MakeBig");
    CHECK(std::ranges::any_of(callee,
                              [](const std::uint32_t w) { return ArgumentDrained(w) == std::optional<unsigned>(8); }));
    for (const auto word : callee) {
        CHECK_FALSE(ArgumentFilled(word) == std::optional<unsigned>(0));
    }
}

TEST_CASE("AArch64 RCU emitter leaves the general-purpose file behind once an argument overflows it") {
    const auto package = CompileToAArch64Lir(R"(
        struct Mid { a: int64; b: int32; }

        func Saturates(a: int, b: int, c: int, d: int, e: int, f: int, g: int, pair: Mid, last: int) -> int {
            return last;
        }

        func Main() -> int {
            var mid = Mid { a: 8i64, b: 9i32 };
            var result = Saturates(1, 2, 3, 4, 5, 6, 7, mid, 10);
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    // Seven integers take X0 through X6, and the composite behind them needs
    // two registers where one is left — so it goes to the stack, and the
    // integer after it follows even though X7 is still free. That is the rule
    // the standard states as saturating the counter, and it is what a caller
    // written argument by argument gets wrong.
    const auto caller = FunctionWords(objects.front(), "Main");
    const auto call = BranchAndLinkIndex(caller);
    REQUIRE(call.has_value());
    REQUIRE_GE(*call, 7);
    for (unsigned reg = 0; reg < 7; ++reg) {
        CHECK_EQ(ArgumentFilled(caller[*call - 7 + reg]), std::optional<unsigned>(reg));
    }
    for (const auto word : caller) {
        CHECK_FALSE(ArgumentFilled(word) == std::optional<unsigned>(7));
    }

    // The callee finds both above its own frame: the composite in the first two
    // doublewords of the caller's area, and the integer in the third.
    const auto callee = FunctionWords(objects.front(), "Saturates");
    const auto frame = PreIndexedFrameSize(callee.front());
    REQUIRE_MESSAGE(frame.has_value(), HexWord(callee.front()));
    const auto found = std::ranges::find_if(callee, [frame](const std::uint32_t word) {
        return IncomingDisplacement(word, 8) == std::optional<std::int32_t>(*frame + 16);
    });
    CHECK_MESSAGE(found != callee.end(), "the ninth argument is read sixteen bytes into the area");
}

TEST_CASE("AArch64 RCU emitter leaves the vector file behind without touching the other one") {
    const auto package = CompileToAArch64Lir(R"(
        struct Pair { x: float64; y: float64; }

        func FivePairs(p: Pair, q: Pair, r: Pair, s: Pair, t: Pair, n: int) -> float64 {
            return t.y;
        }

        func Main() -> int {
            var pair = Pair { x: 1.5, y: 2.5 };
            var last = FivePairs(pair, pair, pair, pair, pair, 42);
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    // Five pairs need ten vector registers where eight remain, so the fifth goes
    // to the stack whole — and the integer behind it still takes X0, because a
    // file that ran out says nothing about the other one.
    const auto caller = FunctionWords(objects.front(), "Main");
    const auto call = BranchAndLinkIndex(caller);
    REQUIRE(call.has_value());
    REQUIRE_GE(*call, 9);
    CHECK_EQ(ArgumentFilled(caller[*call - 1]), std::optional<unsigned>(0));
    for (unsigned member = 0; member < 8; ++member) {
        const std::uint32_t word = caller[*call - 9 + member];
        const auto loaded = VectorSlotAccessOf(word, 64, false);
        REQUIRE_MESSAGE(loaded.has_value(), HexWord(word));
        CHECK_EQ(loaded->reg, member);
    }

    // Sixteen bytes of stack for the pair that did not fit, and nothing more:
    // the integer went to X0 rather than to the area behind it.
    std::optional<std::int64_t> opened;
    for (const auto word : caller) {
        if (const auto adjustment = StackPointerAdjustment(word, true); adjustment.has_value()) {
            opened = adjustment;
            break;
        }
    }
    CHECK_EQ(opened, std::optional<std::int64_t>(16));
}

TEST_CASE("Windows AArch64 sprintf-style variadic calls use consecutive general-purpose slots") {
    const auto package = CompileToAArch64Lir(R"(
        #Link("ucrtbase.dll")
        extern {
            func sprintf(buffer: *char8, format: *char8, ...) -> int32;
        }

        func Main() -> int {
            var buffer = "result";
            var format = "values";
            var written = sprintf(buffer.data, format.data, 2.5, 7, 3.5f32);
            return 0;
        }
    )",
                                             "windows-aarch64");

    AArch64RcuEmitter emitter(package, "test", Target::OS::Windows);
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    const auto caller = FunctionWords(objects.front(), "Main");
    const auto call = BranchAndLinkIndex(caller);
    REQUIRE(call.has_value());

    // The two fixed pointers and all three anonymous arguments occupy one
    // eight-byte slot each. Both floating-point values cross into X registers
    // by bit pattern; no V argument register receives either one.
    const auto beforeCall = std::ranges::subrange(caller.begin(), caller.begin() + static_cast<std::ptrdiff_t>(*call));
    CHECK(std::ranges::any_of(
        beforeCall, [](const std::uint32_t word) { return ArgumentFilled(word) == std::optional<unsigned>(0); }));
    CHECK(std::ranges::any_of(
        beforeCall, [](const std::uint32_t word) { return ArgumentFilled(word) == std::optional<unsigned>(1); }));
    CHECK(std::ranges::any_of(beforeCall, [](const std::uint32_t word) {
        return FloatBitsArgumentFilled(word, 64) == std::optional<unsigned>(2);
    }));
    CHECK(std::ranges::any_of(
        beforeCall, [](const std::uint32_t word) { return ArgumentFilled(word) == std::optional<unsigned>(3); }));
    CHECK(std::ranges::any_of(beforeCall, [](const std::uint32_t word) {
        return FloatBitsArgumentFilled(word, 32) == std::optional<unsigned>(4);
    }));
    for (const auto word : caller) {
        for (const unsigned bits : {32U, 64U}) {
            const auto vector = VectorArgumentFilled(word, bits);
            CHECK(vector.value_or(8) >= 8);
        }
    }
}

TEST_CASE("Windows AArch64 variadic aggregates straddle the register window and stack") {
    const auto package = CompileToAArch64Lir(R"(
        struct FloatPair { x: float64; y: float64; }
        struct IntPair { x: int64; y: int64; }

        #Link("variadic.dll")
        extern {
            func Collect(first: int, ...) -> int32;
        }

        func Main() -> int {
            var floats = FloatPair { x: 1.5, y: 2.5 };
            var integers = IntPair { x: 8i64, y: 9i64 };
            var result = Collect(1, 2, 3, 4, 5, 6, 7, floats, integers);
            return 0;
        }
    )",
                                             "windows-aarch64");

    AArch64RcuEmitter emitter(package, "test", Target::OS::Windows);
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    const auto caller = FunctionWords(objects.front(), "Main");
    const auto call = BranchAndLinkIndex(caller);
    REQUIRE(call.has_value());

    // Seven scalar slots fill X0-X6. The HFA receives no special treatment:
    // its first doubleword fills X7 and its second becomes stack slot zero.
    // The ordinary aggregate follows in stack slots eight and sixteen.
    for (unsigned reg = 0; reg < 8; ++reg) {
        CHECK(std::ranges::any_of(
            caller.begin(), caller.begin() + static_cast<std::ptrdiff_t>(*call),
            [reg](const std::uint32_t word) { return ArgumentFilled(word) == std::optional<unsigned>(reg); }));
    }
    std::vector<std::uint32_t> stackOffsets;
    for (std::size_t i = 0; i < *call; ++i) {
        if (const auto offset = StackArgumentStored(caller[i])) {
            stackOffsets.push_back(*offset);
        }
        CHECK_FALSE(VectorArgumentFilled(caller[i], 64).value_or(8) < 8);
    }
    CHECK_EQ(stackOffsets, std::vector<std::uint32_t>({0, 8, 16}));

    // Twenty-four real argument bytes are rounded so SP remains aligned at the
    // public call boundary.
    CHECK(std::ranges::any_of(caller, [](const std::uint32_t word) {
        return StackPointerAdjustment(word, true) == std::optional<std::int64_t>(32);
    }));
}

TEST_CASE("Windows AArch64 variadic calls copy large aggregates and keep indirect returns in X8") {
    const auto package = CompileToAArch64Lir(R"(
        struct Big { a: int64; b: int64; c: int64; }

        #Link("variadic.dll")
        extern {
            func Transform(scale: float64, ...) -> Big;
        }

        func Main() -> int {
            var input = Big { a: 1i64, b: 2i64, c: 3i64 };
            var output = Transform(2.5, input);
            return 0;
        }
    )",
                                             "windows-aarch64");

    AArch64RcuEmitter emitter(package, "test", Target::OS::Windows);
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    const auto caller = FunctionWords(objects.front(), "Main");
    const auto call = BranchAndLinkIndex(caller);
    REQUIRE(call.has_value());
    REQUIRE_GE(*call, 3);

    // The named float still uses the general file, and the aggregate is copied
    // into the outgoing area with its address in the next consecutive slot.
    CHECK(std::ranges::any_of(
        caller.begin(), caller.begin() + static_cast<std::ptrdiff_t>(*call),
        [](const std::uint32_t word) { return FloatBitsArgumentFilled(word, 64) == std::optional<unsigned>(0); }));
    CHECK(std::ranges::any_of(
        caller.begin(), caller.begin() + static_cast<std::ptrdiff_t>(*call),
        [](const std::uint32_t word) { return StackPointerAddImm(word, 1) == std::optional<std::uint32_t>(0); }));

    // Return classification is independent of the variadic argument variant:
    // the caller names its large result in X8 immediately before the branch.
    CHECK(FramePointerAddImm(caller[*call - 1], 8).has_value());
    REQUIRE_LT(*call + 1, caller.size());
    CHECK_FALSE(ArgumentDrained(caller[*call + 1]) == std::optional<unsigned>(0));
    CHECK(std::ranges::any_of(caller, [](const std::uint32_t word) {
        return StackPointerAdjustment(word, true) == std::optional<std::int64_t>(32);
    }));
}

TEST_CASE("AArch64 RCU emitter passes an anonymous float argument in a vector register") {
    // AAPCS64 states no separate rule for the arguments a variadic declaration
    // does not name: on Linux a float still travels in the vector file, which
    // is where `va_arg` reads it back from. Apple and Windows deviate and
    // neither is reachable through this back end yet.
    const auto package = CompileToAArch64Lir(R"(
        #Link("libc.so.6")
        extern {
            func printf(format: *char8, ...) -> int32;
        }

        func Main() -> int {
            var text = "value";
            var written = printf(text.data, 2.5, 7);
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    const auto caller = FunctionWords(objects.front(), "Main");
    const auto call = BranchAndLinkIndex(caller);
    REQUIRE(call.has_value());
    REQUIRE_GE(*call, 3);
    CHECK_EQ(ArgumentFilled(caller[*call - 3]), std::optional<unsigned>(0));
    const auto anonymous = VectorArgumentFilled(caller[*call - 2], 64);
    REQUIRE_MESSAGE(anonymous.has_value(), HexWord(caller[*call - 2]));
    CHECK_EQ(*anonymous, 0);
    CHECK_EQ(ArgumentFilled(caller[*call - 1]), std::optional<unsigned>(1));
}

// Floating point and conversions
//
// The operands of a floating-point instruction arrive in V16 and V17 and its
// result goes back to the frame, which is the vector-file counterpart of the
// X9/X12 pair the integer opcodes compute in. The precision travels in the
// register: an S operand selects the single-precision form of an instruction
// and a D operand the double-precision one, so the two precisions differ by one
// field of one word and are checked as the same instruction twice.

TEST_CASE("AArch64 RCU emitter lowers each floating-point operator to one instruction") {
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            var wide: float64 = 10.0;
            var other: float64 = 4.0;
            var sum = wide + other;
            var difference = wide - other;
            var product = wide * other;
            var quotient = wide / other;
            var negated = -wide;
            var narrow: float32 = 10.0f32;
            var narrowOther: float32 = 4.0f32;
            var narrowSum = narrow + narrowOther;
            var narrowDifference = narrow - narrowOther;
            var narrowProduct = narrow * narrowOther;
            var narrowQuotient = narrow / narrowOther;
            var narrowNegated = -narrow;
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    const std::vector<std::tuple<std::uint32_t, unsigned, const char *>> expected = {
        {0x1E602800U, 2, "fadd dD, dN, dM"}, {0x1E603800U, 2, "fsub dD, dN, dM"}, {0x1E600800U, 2, "fmul dD, dN, dM"},
        {0x1E601800U, 2, "fdiv dD, dN, dM"}, {0x1E614000U, 1, "fneg dD, dN"},     {0x1E202800U, 2, "fadd sD, sN, sM"},
        {0x1E203800U, 2, "fsub sD, sN, sM"}, {0x1E200800U, 2, "fmul sD, sN, sM"}, {0x1E201800U, 2, "fdiv sD, sN, sM"},
        {0x1E214000U, 1, "fneg sD, sN"},
    };
    for (const auto &form : expected) {
        CHECK_MESSAGE(HasFloatForm(words, std::get<0>(form), std::get<1>(form)), std::get<2>(form));
    }
    // Negation is the instruction and not a constant: the x86-64 back end
    // reaches .rodata for a sign mask to XOR against, and nothing here does.
    CHECK(objects.front().sections[RCU_RODATA_IDX].data.empty());
}

TEST_CASE("AArch64 RCU emitter synthesizes a floating-point remainder from a truncated quotient") {
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            var left: float64 = 10.0;
            var right: float64 = 4.0;
            var remainder = left % right;
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    // The dividend less its quotient truncated toward zero times the divisor,
    // which is three instructions: the truncation happens in the register
    // rather than through a 64-bit integer as it does on x86-64, and the
    // multiply folds into the subtraction that recovers the remainder.
    const auto quotient = std::ranges::find_if(
        words, [](const std::uint32_t w) { return (w & 0xFFE0FC1FU) == (0x1E601800U | 18U); }); // fdiv d18, dN, dM
    REQUIRE_MESSAGE(quotient != words.end(), "fdiv d18, dN, dM");
    const auto index = static_cast<std::size_t>(quotient - words.begin());
    REQUIRE_LT(index + 2, words.size());
    CHECK_EQ(HexWord(words[index + 1]), HexWord(0x1E65C252U));               // frintz d18, d18
    CHECK_EQ(HexWord(words[index + 2] & 0xFFE08000U), HexWord(0x1F408000U)); // fmsub dD, d18, dM, dA
    CHECK_EQ(HexWord(words[index + 2] >> 5U & 0x1FU), HexWord(18U));         // ... from the quotient
    CHECK_FALSE(HasFloatForm(words, 0x1E600800U));                           // fmul dD, dN, dM
}

TEST_CASE("AArch64 RCU emitter converts between the two precisions and no further") {
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            var narrow: float32 = 2.5f32;
            var widened = narrow as float64;
            var narrowed = widened as float32;
            var unchanged = widened as float64;
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    CHECK_MESSAGE(HasFloatForm(words, 0x1E22C000U, 1), "fcvt dD, sN");
    CHECK_MESSAGE(HasFloatForm(words, 0x1E624000U, 1), "fcvt sD, dN");
    // FCVT names the source precision in one field and the destination in
    // another, so a cast to the precision already in hand has no instruction to
    // be — it is the load and the store the two casts above also carry.
    CHECK_EQ(std::ranges::count_if(words, [](const std::uint32_t w) { return (w & 0xFF3F3C00U) == 0x1E220000U; }), 2);
}

TEST_CASE("AArch64 RCU emitter converts between files at the signedness of the integer side") {
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            var value: float64 = 7.9;
            var signedResult = value as int64;
            var unsignedResult = value as uint64;
            var signedSource: int64 = -7;
            var fromSigned = signedSource as float64;
            var unsignedSource: uint64 = 7u64;
            var fromUnsigned = unsignedSource as float64;
            var single = signedSource as float32;
            var narrow: float32 = 2.5f32;
            var fromNarrow = narrow as int64;
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    // Each of the four conversions rounds toward zero or by the current mode as
    // its direction asks, and the signedness of the integer side picks between
    // the pairs — which is what keeps a uint64 above 2^63 an unsigned value
    // rather than the negative one a single signed instruction would give.
    const std::vector<std::pair<std::uint32_t, const char *>> expected = {
        {0x9E780000U, "fcvtzs xD, dN"}, {0x9E790000U, "fcvtzu xD, dN"}, {0x9E620000U, "scvtf  dD, xN"},
        {0x9E630000U, "ucvtf  dD, xN"}, {0x9E220000U, "scvtf  sD, xN"}, {0x9E380000U, "fcvtzs xD, sN"},
    };
    for (const auto &form : expected) {
        CHECK_MESSAGE(HasFloatForm(words, form.first, 1), form.second);
    }
}

TEST_CASE("AArch64 RCU emitter casts between integers with the load and the store alone") {
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            var narrow: int16 = -5i16;
            var widened = narrow as int64;
            var truncated = widened as uint8;
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    // A conversion between integers is never an instruction of its own. A
    // widening one is the sign-extension its source type already asks of every
    // mention, which is SXTH out of the register the allocation gave the
    // int16; a narrowing one is nothing at all, since what writes the
    // destination writes the bytes its type occupies and no more.
    CHECK_MESSAGE(std::ranges::any_of(words, [](const std::uint32_t w) { return (w & 0xFFFFFC00U) == 0x93403C00U; }),
                  "sxth xD, wN");
    // One byte store, which is the uint8 local being written, and nothing that
    // masks the value down to a byte before it.
    CHECK_EQ(std::ranges::count_if(words, [](const std::uint32_t w) { return (w & 0xFFC00000U) == 0x39000000U; }), 1);
    CHECK_FALSE(
        std::ranges::any_of(words, [](const std::uint32_t w) { return (w & 0xFFFFFC00U) == 0x53001C00U; })); // uxtb
}

TEST_CASE("AArch64 RCU emitter moves a float's bits between the register files") {
    // The four names the front end lowers as calls but which are not calls:
    // each is one FMOV, which pairs a word with an S register and a doubleword
    // with a D one and converts nothing on the way.
    const auto package = CompileToAArch64Lir(R"(
        func FloatBits64(value: float64) -> uint64 { return 0u64; }
        func FloatFromBits64(bits: uint64) -> float64 { return 0.0; }
        func FloatBits32(value: float32) -> uint32 { return 0u32; }
        func FloatFromBits32(bits: uint32) -> float32 { return 0.0f32; }

        func Main() -> int {
            var wide: float64 = 2.5;
            var wideBits = FloatBits64(wide);
            var backToWide = FloatFromBits64(wideBits);
            var narrow: float32 = 2.5f32;
            var narrowBits = FloatBits32(narrow);
            var backToNarrow = FloatFromBits32(narrowBits);
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    const std::vector<std::pair<std::uint32_t, const char *>> expected = {
        {0x9E660000U, "fmov xD, dN"},
        {0x9E670000U, "fmov dD, xN"},
        {0x1E260000U, "fmov wD, sN"},
        {0x1E270000U, "fmov sD, wN"},
    };
    for (const auto &form : expected) {
        CHECK_MESSAGE(HasFloatForm(words, form.first, 1), form.second);
    }
    // No branch is taken to any of the four, so the bodies declared above are
    // never reached from here.
    CHECK_FALSE(BranchAndLinkIndex(words).has_value());
}

TEST_CASE("AArch64 RCU emitter calls one synthesized floating-point exponentiation helper") {
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            var base: float64 = 2.0;
            var exponent: float64 = 10.0;
            var first = base ** exponent;
            var second = base ** exponent;
            var narrowBase: float32 = 2.0f32;
            var narrowExponent: float32 = 10.0f32;
            var narrow = narrowBase ** narrowExponent;
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto &object = objects.front();

    // Both helpers are emitted once and after every user function, so each of
    // the three uses is a BL carrying a relocation against a local text symbol.
    const RcuSymbol *wide = FindSymbol(object, "__rux_powf64");
    const RcuSymbol *narrow = FindSymbol(object, "__rux_powf32");
    REQUIRE(wide != nullptr);
    REQUIRE(narrow != nullptr);
    CHECK_EQ(wide->visibility, RcuSymVis::Local);
    CHECK_EQ(narrow->visibility, RcuSymVis::Local);
    CHECK_GT(wide->value, 0);
    CHECK_GT(wide->size, 0);

    const auto wideCalls = RelocsFor(object, RCU_TEXT_IDX, "__rux_powf64");
    const auto narrowCalls = RelocsFor(object, RCU_TEXT_IDX, "__rux_powf32");
    REQUIRE_EQ(wideCalls.size(), 3); // two from Main, one from the f32 helper
    REQUIRE_EQ(narrowCalls.size(), 1);
    for (const auto &reloc : wideCalls) {
        CHECK_EQ(reloc.type, RcuRelType::AArch64Call26);
        CHECK_EQ(HexWord(TextWordAt(object, reloc.sectionOffset)), HexWord(0x94000000U)); // bl with no displacement
    }

    // The single-precision helper widens both arguments, defers, and narrows
    // the answer — which is what keeps its result correctly rounded, and is
    // what makes it the one synthesized body with a frame of its own.
    const auto body = FunctionWords(object, "__rux_powf32");
    const std::vector<std::uint32_t> expected = {
        0xA9BF7BFD, // stp  x29, x30, [sp, #-16]!
        0x910003FD, // mov  x29, sp
        0x1E22C000, // fcvt d0, s0
        0x1E22C021, // fcvt d1, s1
        0x94000000, // bl   __rux_powf64
        0x1E624000, // fcvt s0, d0
        0xA8C17BFD, // ldp  x29, x30, [sp], #16
        0xD65F03C0, // ret
    };
    REQUIRE_EQ(body.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK_EQ(HexWord(body[i]), HexWord(expected[i]));
    }

    // The double-precision body reaches the read-only pool for the constants
    // its two series need and takes X16 as the page register each time, which
    // is the one register a sequence may claim without being given one.
    const auto wideBody = FunctionWords(object, "__rux_powf64");
    CHECK_GT(std::ranges::count_if(wideBody, [](const std::uint32_t w) { return (w & 0x9F00001FU) == 0x90000010U; }),
             20);
}

TEST_CASE("AArch64 RCU emitter reads an enum as the integer its discriminant is") {
    const auto package = CompileToAArch64Lir(R"(
        enum Direction {
            North = 10,
            South = 20
        }

        func Main() -> int {
            var heading = Direction::North;
            var same = heading == Direction::North;
            var value = heading as int;
            var back = 20 as Direction;
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    // An enum value is a bit pattern in a general-purpose register, so its
    // constant, its comparison and its casts are the same instructions an
    // integer's are — which is the whole of what naming its kind would have
    // added, and is why the type is read for what it is not rather than what
    // it is.
    const auto hasMoveWide = [&words](const std::uint32_t value) {
        return std::ranges::any_of(
            words, [value](const std::uint32_t w) { return (w & 0xFFFFFFE0U) == (0xD2800000U | value << 5U); });
    };
    CHECK_MESSAGE(hasMoveWide(10), "mov xD, #10");
    CHECK_MESSAGE(hasMoveWide(20), "mov xD, #20");
    CHECK_MESSAGE(std::ranges::any_of(words, [](const std::uint32_t w) { return (w & 0xFFE0FC1FU) == 0xEB00001FU; }),
                  "cmp xN, xM");
    CHECK_MESSAGE(HasCset(words, 0x9A9F17E0U), "cset xD, eq");
}

// Assertions, inline assembly and static data
//
// A failed assertion is the one construct in this back end that is a property
// of the operating system rather than of the architecture, and an `asm func` is
// the one body it selects no instructions for. The cases below read both and
// verify the static data and vtable relocations emitted beside them.

namespace {
// The two intrinsics an assertion and a panic are written as, and the type
// their message travels in, declared here rather than imported: these cases are
// compiled as a package of one module with no dependencies, and `Core` is where
// all three usually live.
constexpr std::string_view kAssertIntrinsics = R"(
        struct Slice<T> {
            data: *T;
            length: uint;
        }

        intrinsic func Assert(condition: bool, message: Slice<char8>);
        intrinsic func Panic(message: Slice<char8>);
)";

// The words a write to standard error takes once its buffer and length are in
// place: the descriptor, the call number, and the trap that asks for it. Linux
// numbers a write 64 and takes the number in X8.
constexpr std::uint32_t kMovX0StdErr = 0xD2800040U; // mov x0, #2
constexpr std::uint32_t kMovX8Write = 0xD2800808U;  // mov x8, #64
constexpr std::uint32_t kSvc0 = 0xD4000001U;        // svc #0
constexpr std::uint32_t kAdrpX1 = 0x90000001U;      // adrp x1, <symbol>
constexpr std::uint32_t kAddX1Lo12 = 0x91000021U;   // add  x1, x1, #:lo12:<symbol>
constexpr std::uint32_t kLdpX1X2 = 0xA9400941U;     // ldp  x1, x2, [x10]
constexpr std::uint32_t kBrk1 = 0xD4200020U;        // brk  #1

// Windows reaches standard error through KERNEL32. GetStdHandle receives -12
// in X0, and every WriteFile receives its last two arguments in X3 and X4.
constexpr std::uint32_t kSubSp16 = 0xD10043FFU;           // sub sp, sp, #16
constexpr std::uint32_t kMovX0StdErrHandle = 0x92800160U; // mov x0, #-12
constexpr std::uint32_t kMovX3Sp = 0x910003E3U;           // mov x3, sp
constexpr std::uint32_t kMovX4Zero = 0xD2800004U;         // mov x4, #0
constexpr std::uint32_t kBl0 = 0x94000000U;               // bl <import>

// The immediate of `mov xN, #imm` in the one-instruction move-wide form, or
// nothing when the word is some other instruction. A message's length is
// materialized this way, and how long a message is depends on where in this
// file the source it came from sits.
[[nodiscard]] std::optional<std::uint32_t> MoveWideImm(const std::uint32_t word, const unsigned reg) {
    if ((word & 0xFFE0001FU) != (0xD2800000U | reg)) {
        return std::nullopt;
    }
    return word >> 5U & 0xFFFFU;
}

// The index of the one `brk` in a body, which is where an assertion's failure
// path ends and therefore how the words before it are found without counting
// the instructions ahead of them.
[[nodiscard]] std::optional<std::size_t> TrapIndex(const std::vector<std::uint32_t> &words) {
    const auto found = std::ranges::find(words, kBrk1);
    if (found == words.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(found - words.begin());
}

// How many times a run of bytes appears in the read-only section, which is what
// says whether two assertions shared one interned prefix or were each given a
// copy of it.
[[nodiscard]] std::size_t RodataOccurrences(const RcuFile &object, const std::string_view text) {
    const auto &rodata = object.sections[RCU_RODATA_IDX].data;
    const std::string_view bytes(reinterpret_cast<const char *>(rodata.data()), rodata.size());
    std::size_t count = 0;
    for (std::size_t at = bytes.find(text); at != std::string_view::npos; at = bytes.find(text, at + 1)) {
        ++count;
    }
    return count;
}
} // namespace

TEST_CASE("AArch64 RCU emitter writes a failed assertion's three parts and traps") {
    const auto package = CompileToAArch64Lir(std::format(R"(
        {}
        func Main() -> int {{
            Assert(1 == 1, "one");
            return 0;
        }}
    )",
                                                         kAssertIntrinsics));

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto &object = objects.front();
    const auto words = FunctionWords(object, "Main");

    const auto trap = TrapIndex(words);
    REQUIRE_MESSAGE(trap.has_value(), "no brk in the body");
    REQUIRE(*trap >= 19);

    // Read backwards from the trap: the location, the message and the prefix,
    // each of them one write, and each write ending in the same three words.
    const std::size_t location = *trap - 6;
    const std::size_t message = *trap - 11;
    const std::size_t prefix = *trap - 17;
    for (const std::size_t write : {prefix, message, location}) {
        const std::size_t end = write == message ? message + 2 : write + 3;
        CHECK_EQ(HexWord(words[end]), HexWord(kMovX0StdErr));
        CHECK_EQ(HexWord(words[end + 1]), HexWord(kMovX8Write));
        CHECK_EQ(HexWord(words[end + 2]), HexWord(kSvc0));
    }

    // The prefix and the location are addresses this object knows, reached as a
    // page and an offset with both immediates left to their relocations; the
    // lengths are what the two texts actually are.
    CHECK_EQ(HexWord(words[prefix]), HexWord(kAdrpX1));
    CHECK_EQ(HexWord(words[prefix + 1]), HexWord(kAddX1Lo12));
    CHECK_EQ(MoveWideImm(words[prefix + 2], 2), std::string("Assertion failed: ").size());
    CHECK_EQ(HexWord(words[location]), HexWord(kAdrpX1));
    CHECK_EQ(HexWord(words[location + 1]), HexWord(kAddX1Lo12));
    CHECK_MESSAGE(MoveWideImm(words[location + 2], 2).has_value(), HexWord(words[location + 2]));

    // The message is neither: what the operand holds is the address of a
    // `Slice<char8>`, and one LDP takes both of its fields into the two
    // registers the call reads them from.
    CHECK_EQ(ArgumentFilled(words[message]), std::optional<unsigned>(10)); // the address, into X10
    CHECK_EQ(HexWord(words[message + 1]), HexWord(kLdpX1X2));

    // A condition that held branches over the whole of it, landing on the
    // instruction after the trap.
    const std::size_t condition = *trap - 18;
    CHECK_EQ(words[condition] & 0xFF00001FU, 0xB5000009U); // cbnz x9, <after the trap>
    CHECK_EQ(BranchDisplacement(words[condition]), static_cast<std::int32_t>(*trap + 1 - condition));

    // Both texts are in the read-only section, spelled the way the x86-64 back
    // end spells them, and the location names the function, the file and the
    // position the front end recorded.
    CHECK_EQ(RodataOccurrences(object, "Assertion failed: "), 1);
    CHECK_EQ(RodataOccurrences(object, "\n  at Main (test.rux:"), 1);
}

TEST_CASE("AArch64 RCU emitter interns one assertion prefix for every site") {
    const auto package = CompileToAArch64Lir(std::format(R"(
        {}
        func Main() -> int {{
            Assert(1 == 1, "one");
            Assert(2 == 2, "two");
            return 0;
        }}
    )",
                                                         kAssertIntrinsics));

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto &object = objects.front();

    // One prefix behind both sites, and a location of its own for each: the
    // prefix is the same string twice and the two locations differ in the line
    // they name.
    CHECK_EQ(RodataOccurrences(object, "Assertion failed: "), 1);
    CHECK_EQ(RodataOccurrences(object, "\n  at Main (test.rux:"), 2);

    // Two traps and two branches over them, which is what says the second
    // assertion is a path of its own rather than a jump into the first.
    const auto words = FunctionWords(object, "Main");
    CHECK_EQ(std::ranges::count(words, kBrk1), 2);
    CHECK_EQ(std::ranges::count(words, kSvc0), 6);
}

TEST_CASE("AArch64 RCU emitter reaches a panic with no condition and no branch") {
    const auto package = CompileToAArch64Lir(std::format(R"(
        {}
        func Main() -> int {{
            Panic("stop");
            return 0;
        }}
    )",
                                                         kAssertIntrinsics));

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto &object = objects.front();
    const auto words = FunctionWords(object, "Main");

    const auto trap = TrapIndex(words);
    REQUIRE_MESSAGE(trap.has_value(), "no brk in the body");
    CHECK_EQ(std::ranges::count(words, kSvc0), 3);
    CHECK_EQ(RodataOccurrences(object, "Panic: "), 1);
    CHECK_EQ(RodataOccurrences(object, "Assertion failed: "), 0);

    // Nothing is tested and nothing is skipped: a panic always trapped, so no
    // conditional branch stands anywhere in the body.
    for (const std::uint32_t word : words) {
        CHECK_MESSAGE((word & 0xFF000010U) != 0x54000000U, HexWord(word)); // b.<cond>
        CHECK_MESSAGE((word & 0x7E000000U) != 0x34000000U, HexWord(word)); // cbz / cbnz
    }
}

TEST_CASE("AArch64 RCU emitter asks each kernel for a write by its own numbering") {
    const auto package = CompileToAArch64Lir(std::format(R"(
        {}
        func Main() -> int {{
            Assert(1 == 1, "one");
            return 0;
        }}
    )",
                                                         kAssertIntrinsics));

    AArch64RcuEmitter darwin(package, "test", Target::OS::MacOS);
    const auto objects = darwin.Generate();
    CHECK_MESSAGE(darwin.Diagnostics().empty(), JoinMessages(darwin.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    // Darwin numbers a write 4, takes the number in X16 and is asked through a
    // trap of its own, so not one word of the three that surround the buffer is
    // the same as Linux's.
    CHECK_EQ(std::ranges::count(words, 0xD2800090U), 3); // mov x16, #4
    CHECK_EQ(std::ranges::count(words, 0xD4001001U), 3); // svc #0x80
    CHECK_EQ(std::ranges::count(words, kMovX8Write), 0);
    CHECK_EQ(std::ranges::count(words, kSvc0), 0);
    CHECK_EQ(std::ranges::count(words, kMovX0StdErr), 3); // the descriptor is the same
}

TEST_CASE("Windows AArch64 assertions write through KERNEL32 and branch over the failure path") {
    const auto package = CompileToAArch64Lir(std::format(R"(
        {}
        func Main() -> int {{
            Assert(1 == 1, "one");
            return 0;
        }}
    )",
                                                         kAssertIntrinsics),
                                             "windows-aarch64");

    AArch64RcuEmitter emitter(package, "test", Target::OS::Windows);
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto &object = objects.front();
    const auto words = FunctionWords(object, "Main");

    const RcuSymbol *getStdHandle = FindSymbol(object, "GetStdHandle");
    const RcuSymbol *writeFile = FindSymbol(object, "WriteFile");
    REQUIRE(getStdHandle != nullptr);
    REQUIRE(writeFile != nullptr);
    for (const RcuSymbol *symbol : {getStdHandle, writeFile}) {
        CHECK_EQ(symbol->sectionIdx, RCU_SEC_EXTERNAL);
        CHECK_EQ(symbol->kind, RcuSymKind::ExternFunc);
        CHECK_EQ(symbol->visibility, RcuSymVis::Global);
        CHECK_EQ(symbol->typeName, "KERNEL32.DLL");
    }

    const auto getCalls = FunctionRelocs(object, "Main", "GetStdHandle");
    const auto writeCalls = FunctionRelocs(object, "Main", "WriteFile");
    REQUIRE_EQ(getCalls.size(), 3);
    REQUIRE_EQ(writeCalls.size(), 3);
    for (std::size_t i = 0; i < getCalls.size(); ++i) {
        CHECK_EQ(getCalls[i].second, RcuRelType::AArch64Call26);
        CHECK_EQ(writeCalls[i].second, RcuRelType::AArch64Call26);

        const std::size_t get = getCalls[i].first / A64Enc::InstrSize;
        const std::size_t write = writeCalls[i].first / A64Enc::InstrSize;
        REQUIRE(get + 2 < words.size());
        REQUIRE(write < words.size());
        CHECK(get < write);
        CHECK_EQ(HexWord(words[get]), HexWord(kBl0));
        CHECK_EQ(HexWord(words[get + 1]), HexWord(kMovX3Sp));
        CHECK_EQ(HexWord(words[get + 2]), HexWord(kMovX4Zero));
        CHECK_EQ(HexWord(words[write]), HexWord(kBl0));
    }

    // All five WriteFile arguments are visible in the instruction stream: X0
    // comes back from GetStdHandle, static writes materialize X1/X2 through a
    // relocated address and length, the slice loads both together for the
    // dynamic write, and X3/X4 were checked beside every imported call above.
    CHECK_EQ(std::ranges::count(words, kMovX0StdErrHandle), 3);
    CHECK_EQ(std::ranges::count(words, kLdpX1X2), 1);
    CHECK_EQ(std::ranges::count(words, kSubSp16), 1);
    CHECK_EQ(std::ranges::count(words, kSvc0), 0);

    std::vector<const RcuSymbol *> staticTexts;
    for (const auto &symbol : object.symbols) {
        if (symbol.sectionIdx != RCU_RODATA_IDX) {
            continue;
        }
        const auto bytes = RodataOf(object, symbol.name);
        const std::string_view text(reinterpret_cast<const char *>(bytes.data()), bytes.size() - 1);
        if (text == "Assertion failed: " || text.starts_with("\n  at Main (test.rux:")) {
            staticTexts.push_back(&symbol);
        }
    }
    REQUIRE_EQ(staticTexts.size(), 2);
    for (const RcuSymbol *symbol : staticTexts) {
        const auto address = FunctionRelocs(object, "Main", symbol->name);
        REQUIRE_EQ(address.size(), 2);
        CHECK_EQ(address[0].second, RcuRelType::AArch64AdrPrelPgHi21);
        CHECK_EQ(address[1].second, RcuRelType::AArch64AddAbsLo12Nc);
        CHECK_EQ(HexWord(words[address[0].first / A64Enc::InstrSize]), HexWord(kAdrpX1));
        CHECK_EQ(HexWord(words[address[1].first / A64Enc::InstrSize]), HexWord(kAddX1Lo12));
    }

    const auto trap = TrapIndex(words);
    REQUIRE_MESSAGE(trap.has_value(), "no brk in the body");
    const auto held = std::ranges::find_if(words, [](const std::uint32_t word) {
        return (word & 0xFF00001FU) == 0xB5000009U; // cbnz x9, <after the trap>
    });
    REQUIRE(held != words.end());
    const std::size_t heldIndex = static_cast<std::size_t>(held - words.begin());
    CHECK_EQ(BranchDisplacement(*held), static_cast<std::int32_t>(*trap + 1 - heldIndex));
}

TEST_CASE("Windows AArch64 panics use the same imported writer and always trap") {
    const auto package = CompileToAArch64Lir(std::format(R"(
        {}
        func Main() -> int {{
            Panic("stop");
            return 0;
        }}
    )",
                                                         kAssertIntrinsics),
                                             "windows-aarch64");

    AArch64RcuEmitter emitter(package, "test", Target::OS::Windows);
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto &object = objects.front();
    const auto words = FunctionWords(object, "Main");

    CHECK_EQ(FunctionRelocs(object, "Main", "GetStdHandle").size(), 3);
    CHECK_EQ(FunctionRelocs(object, "Main", "WriteFile").size(), 3);
    CHECK_EQ(std::ranges::count(words, kBrk1), 1);
    CHECK_EQ(RodataOccurrences(object, "Panic: "), 1);
    CHECK_EQ(RodataOccurrences(object, "Assertion failed: "), 0);
    for (const std::uint32_t word : words) {
        CHECK_MESSAGE((word & 0xFF000010U) != 0x54000000U, HexWord(word)); // b.<cond>
        CHECK_MESSAGE((word & 0x7E000000U) != 0x34000000U, HexWord(word)); // cbz / cbnz
    }
}

TEST_CASE("AArch64 RCU emitter emits an asm func as a raw blob") {
    const auto package = CompileToAArch64Lir(R"(
        asm func Add(a: int64, b: int64) -> int64 {
            add x0, x0, x1
            ret
        }

        func Main() -> int {
            return Add(40, 2) as int;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto &object = objects.front();

    // Exactly what the program wrote down: no frame record, no frame pointer,
    // no spill of the arguments AAPCS64 already put in the right registers.
    const std::vector<std::uint32_t> expected = {
        0x8B010000, // add x0, x0, x1
        0xD65F03C0, // ret
    };
    const auto words = FunctionWords(object, "Add");
    REQUIRE_EQ(words.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK_EQ(HexWord(words[i]), HexWord(expected[i]));
    }

    // The caller is an ordinary body, so the two live in one .text and the
    // symbol of each names its own bytes.
    const RcuSymbol *symbol = FindSymbol(object, "Add");
    REQUIRE(symbol != nullptr);
    CHECK_EQ(symbol->sectionIdx, RCU_TEXT_IDX);
    CHECK_EQ(symbol->size, expected.size() * 4);
    CHECK(FunctionWords(object, "Main").size() > expected.size());
}

TEST_CASE("AArch64 RCU emitter relocates a symbol an asm func names") {
    const auto package = CompileToAArch64Lir(R"(
        asm func Triple(x: int64) -> int64 {
            mov x1, #3
            mul x0, x0, x1
            ret
        }

        asm func TripleThenAdd(x: int64) -> int64 {
            stp x0, x30, [sp, #-16]!
            bl Triple
            ldp x1, x30, [sp], #16
            add x0, x0, x1
            ret
        }

        func Main() -> int {
            return TripleThenAdd(10) as int;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto &object = objects.front();

    // The branch carries no displacement of its own; the relocation on it names
    // the callee, and the callee is this module's own symbol rather than an
    // extern the reference invented.
    const auto words = FunctionWords(object, "TripleThenAdd");
    const auto branch = BranchAndLinkIndex(words);
    REQUIRE_MESSAGE(branch.has_value(), "no bl in the body");
    CHECK_EQ(BranchDisplacement(words[*branch]), 0);

    const auto relocs = RelocsFor(object, RCU_TEXT_IDX, "Triple");
    REQUIRE_EQ(relocs.size(), 1);
    CHECK_EQ(relocs.front().type, RcuRelType::AArch64Call26);
    const RcuSymbol *callee = FindSymbol(object, "Triple");
    REQUIRE(callee != nullptr);
    CHECK_EQ(callee->sectionIdx, RCU_TEXT_IDX);

    const RcuSymbol *caller = FindSymbol(object, "TripleThenAdd");
    REQUIRE(caller != nullptr);
    CHECK_EQ(relocs.front().sectionOffset, caller->value + *branch * 4);
}

TEST_CASE("AArch64 RCU emitter lays a vtable out as a run of relocated function pointers") {
    const auto package = CompileToAArch64Lir(R"(
        interface Figure {
            func Area() -> int;
        }

        struct Square {
            size: int;
        }

        extend Square : Figure {
            func Area(self) -> int {
                return self.size * self.size;
            }
        }

        func Main() -> int {
            var square = Square { size: 5 };
            var figure: Figure = square;
            return figure.Area();
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto &object = objects.front();

    // A vtable is one read-only symbol holding a doubleword per method, each of
    // them zero in the object and each named by an Abs64 relocation: a slot
    // holds a whole address, which is the one thing on AArch64 that is not a
    // field of an instruction.
    const auto vtable = std::ranges::find_if(object.symbols, [](const RcuSymbol &symbol) {
        return symbol.sectionIdx == RCU_RODATA_IDX && symbol.name.contains("Square") && symbol.name.contains("Figure");
    });
    REQUIRE_MESSAGE(vtable != object.symbols.end(), "no vtable symbol");
    REQUIRE_EQ(vtable->size, 8);
    CHECK_EQ(vtable->value % 8, 0);

    const auto slots = RodataOf(object, vtable->name);
    CHECK(std::ranges::all_of(slots, [](const std::uint8_t byte) { return byte == 0; }));

    const auto relocs = RelocsFor(object, RCU_RODATA_IDX, "Square::Area");
    REQUIRE_EQ(relocs.size(), 1);
    CHECK_EQ(relocs.front().type, RcuRelType::Abs64);
    CHECK_EQ(relocs.front().sectionOffset, vtable->value);
}

// Whole-function images
//
// Representative code-generation paths asserted word for word from the
// prologue to the RET. Every test above names the one instruction the opcode it
// is about must produce and masks away everything the allocation decides; the
// cases below name all of it, so a change in frame layout, in allocation order
// or in the shape of a prologue is visible here even when every masked test
// still passes.
//
// The disassembly beside each word is `llvm-mc -triple=aarch64 -disassemble`
// reading that word back, which is what makes these images reviewable rather
// than a checksum. A deliberate change to the back end will fail them; the fix
// is to read the new image out of the failure and check its disassembly, not to
// delete the case.

TEST_CASE("AArch64 RCU emitter emits every word of a function reading a pooled constant") {
    // A double no FMOV immediate reaches is one .rodata symbol read
    // through an ADRP / LDR pair, and the pair's immediates are zero in the
    // object because the two relocations below are what fill them in.
    const auto package = CompileToAArch64Lir(R"(
        func Ratio() -> float64 {
            return 0.1;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    CheckFunctionImage(objects.front(), "Ratio",
                       {
                           0xA9BE7BFD, // stp  x29, x30, [sp, #-32]!
                           0x910003FD, // mov  x29, sp
                           0xFD000BA8, // str  d8, [x29, #16]
                           0x90000010, // adrp x16, __f64_0
                           0xFD400208, // ldr  d8, [x16, :lo12:__f64_0]
                           0x1E604100, // fmov d0, d8
                           0xFD400BA8, // ldr  d8, [x29, #16]
                           0xA8C27BFD, // ldp  x29, x30, [sp], #32
                           0xD65F03C0, // ret
                       });

    const std::vector<std::pair<std::uint32_t, std::uint16_t>> expected = {
        {12, RcuRelType::AArch64AdrPrelPgHi21},
        {16, RcuRelType::AArch64LdstAbsLo12Nc},
    };
    CHECK(FunctionRelocs(objects.front(), "Ratio", "__f64_0") == expected);
    const auto pooled = RodataOf(objects.front(), "__f64_0");
    CHECK_EQ(pooled, std::vector<std::uint8_t>{0x9A, 0x99, 0x99, 0x99, 0x99, 0x99, 0xB9, 0x3F});
}

TEST_CASE("AArch64 RCU emitter emits every word of a function reading a field of a local aggregate") {
    // The aggregate is a frame slot the alloca's address is taken of,
    // each field is written through that address at the offset the layout gives,
    // and the read is the same address plus the same offset.
    const auto package = CompileToAArch64Lir(R"(
        struct Pair { first: int; second: int; }

        func Second() -> int {
            var pair = Pair {first: 1, second: 2};
            return pair.second;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    CheckFunctionImage(objects.front(), "Second",
                       {
                           0xA9B97BFD, // stp x29, x30, [sp, #-112]!
                           0x910003FD, // mov x29, sp
                           0xA90153B3, // stp x19, x20, [x29, #16]
                           0xF90013B5, // str x21, [x29, #32]
                           0x9100C3B3, // add x19, x29, #48     — the address of `pair`
                           0xAA1303F4, // mov x20, x19
                           0xD2800035, // mov x21, #1
                           0xF9000295, // str x21, [x20]        — pair.first
                           0x91002274, // add x20, x19, #8
                           0xD2800055, // mov x21, #2
                           0xF9000295, // str x21, [x20]        — pair.second
                           0x91002274, // add x20, x19, #8
                           0xF9400293, // ldr x19, [x20]
                           0xAA1303E0, // mov x0, x19
                           0xA94153B3, // ldp x19, x20, [x29, #16]
                           0xF94013B5, // ldr x21, [x29, #32]
                           0xA8C77BFD, // ldp x29, x30, [sp], #112
                           0xD65F03C0, // ret
                       });
}

TEST_CASE("AArch64 RCU emitter emits every word of a function multiplying and adding") {
    // Two arguments arrive in registers, are spilled to the slots their
    // addresses name, and the arithmetic reads them back: one MUL and one ADD,
    // with nothing between them the operators did not ask for.
    const auto package = CompileToAArch64Lir(R"(
        func Combine(a: int, b: int) -> int {
            return a * b + a;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    CheckFunctionImage(objects.front(), "Combine",
                       {
                           0xA9B77BFD, // stp x29, x30, [sp, #-144]!
                           0x910003FD, // mov x29, sp
                           0xA90153B3, // stp x19, x20, [x29, #16]
                           0xA9025BB5, // stp x21, x22, [x29, #32]
                           0xAA0003F3, // mov x19, x0
                           0xAA0103F5, // mov x21, x1
                           0x910123B4, // add x20, x29, #72
                           0xF9000293, // str x19, [x20]        — the slot of `a`
                           0x910163B3, // add x19, x29, #88
                           0xF9000275, // str x21, [x19]        — the slot of `b`
                           0xF9400295, // ldr x21, [x20]
                           0xF9400276, // ldr x22, [x19]
                           0x9B167EB3, // mul x19, x21, x22
                           0xF9400295, // ldr x21, [x20]
                           0x8B150274, // add x20, x19, x21
                           0xAA1403E0, // mov x0, x20
                           0xA94153B3, // ldp x19, x20, [x29, #16]
                           0xA9425BB5, // ldp x21, x22, [x29, #32]
                           0xA8C97BFD, // ldp x29, x30, [sp], #144
                           0xD65F03C0, // ret
                       });
}

TEST_CASE("AArch64 RCU emitter emits every word of a function comparing and branching") {
    // A comparison is CMP and CSET, the boolean it produces is narrowed
    // to the byte its type occupies, and the branch on it is a CBZ over a B —
    // the far edge is the fallthrough and the near one is jumped over. Every
    // value crossing a block boundary is in the frame, so both exits reload what
    // they return.
    const auto package = CompileToAArch64Lir(R"(
        func Clamp(n: int) -> int {
            if (n > 10) {
                return 10;
            }
            return n;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    CheckFunctionImage(objects.front(), "Clamp",
                       {
                           0xA9BB7BFD, // stp  x29, x30, [sp, #-80]!
                           0x910003FD, // mov  x29, sp
                           0xF9000BA0, // str  x0, [x29, #16]
                           0x910083A9, // add  x9, x29, #32
                           0xF9000FA9, // str  x9, [x29, #24]
                           0xF9400FAA, // ldr  x10, [x29, #24]
                           0xF9400BA9, // ldr  x9, [x29, #16]
                           0xF9000149, // str  x9, [x10]        — the slot of `n`
                           0xF9400FAA, // ldr  x10, [x29, #24]
                           0xF9400149, // ldr  x9, [x10]
                           0xF90017A9, // str  x9, [x29, #40]
                           0xD2800149, // mov  x9, #10
                           0xF9001BA9, // str  x9, [x29, #48]
                           0xF94017A9, // ldr  x9, [x29, #40]
                           0xF9401BAC, // ldr  x12, [x29, #48]
                           0xEB0C013F, // cmp  x9, x12
                           0x9A9FD7E9, // cset x9, gt
                           0x3900E3A9, // strb w9, [x29, #56]
                           0x3940E3A9, // ldrb w9, [x29, #56]
                           0xB4000049, // cbz  x9, +8           — over the branch below
                           0x14000007, // b    +28              — the `if` body
                           0xF9400FAA, // ldr  x10, [x29, #24]
                           0xF9400149, // ldr  x9, [x10]
                           0xF90023A9, // str  x9, [x29, #64]
                           0xF94023A0, // ldr  x0, [x29, #64]
                           0xA8C57BFD, // ldp  x29, x30, [sp], #80
                           0xD65F03C0, // ret
                           0xD2800149, // mov  x9, #10
                           0xF90027A9, // str  x9, [x29, #72]
                           0xF94027A0, // ldr  x0, [x29, #72]
                           0xA8C57BFD, // ldp  x29, x30, [sp], #80
                           0xD65F03C0, // ret
                       });
}

TEST_CASE("AArch64 RCU emitter emits every word of a function calling another with two arguments") {
    // The two arguments are materialized where the allocation put them
    // and moved into X0 and X1 at the call, the result is read out of X0, and
    // the callee is named by a CALL26 whose field is zero until it is linked.
    const auto package = CompileToAArch64Lir(R"(
        func Add(a: int, b: int) -> int {
            return a + b;
        }

        func Caller() -> int {
            return Add(1, 2);
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    CheckFunctionImage(objects.front(), "Caller",
                       {
                           0xA9BC7BFD, // stp x29, x30, [sp, #-64]!
                           0x910003FD, // mov x29, sp
                           0xA90153B3, // stp x19, x20, [x29, #16]
                           0xF90013B5, // str x21, [x29, #32]
                           0xD2800033, // mov x19, #1
                           0xD2800054, // mov x20, #2
                           0xAA1303E0, // mov x0, x19
                           0xAA1403E1, // mov x1, x20
                           0x94000000, // bl  Add
                           0xAA0003F5, // mov x21, x0
                           0xAA1503E0, // mov x0, x21
                           0xA94153B3, // ldp x19, x20, [x29, #16]
                           0xF94013B5, // ldr x21, [x29, #32]
                           0xA8C47BFD, // ldp x29, x30, [sp], #64
                           0xD65F03C0, // ret
                       });

    const std::vector<std::pair<std::uint32_t, std::uint16_t>> expected = {{32, RcuRelType::AArch64Call26}};
    CHECK(FunctionRelocs(objects.front(), "Caller", "Add") == expected);
}

TEST_CASE("AArch64 RCU emitter emits every word of a function taking a float pair in the vector registers") {
    // A composite of two doubles is a homogeneous aggregate, so it
    // arrives in D0 and D1 rather than through memory; the prologue writes the
    // pair into a frame slot and the body reads the fields back out of it as it
    // would any other aggregate.
    const auto package = CompileToAArch64Lir(R"(
        struct Vec2 { x: float64; y: float64; }

        func Sum(v: Vec2) -> float64 {
            return v.x + v.y;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    CheckFunctionImage(objects.front(), "Sum",
                       {
                           0xA9B77BFD, // stp  x29, x30, [sp, #-144]!
                           0x910003FD, // mov  x29, sp
                           0xA90153B3, // stp  x19, x20, [x29, #16]
                           0x6D0227A8, // stp  d8, d9, [x29, #32]
                           0xFD001BAA, // str  d10, [x29, #48]
                           0xFD001FA0, // str  d0, [x29, #56]    — v.x, as it arrived
                           0xFD0023A1, // str  d1, [x29, #64]    — v.y, as it arrived
                           0x910143B3, // add  x19, x29, #80
                           0x9100E3AB, // add  x11, x29, #56
                           0xA9403169, // ldp  x9, x12, [x11]
                           0xA9003269, // stp  x9, x12, [x19]    — the pair, copied in one go
                           0xAA1303F4, // mov  x20, x19
                           0xFD400288, // ldr  d8, [x20]
                           0x91002274, // add  x20, x19, #8
                           0xFD400289, // ldr  d9, [x20]
                           0x1E69290A, // fadd d10, d8, d9
                           0x1E604140, // fmov d0, d10
                           0xA94153B3, // ldp  x19, x20, [x29, #16]
                           0x6D4227A8, // ldp  d8, d9, [x29, #32]
                           0xFD401BAA, // ldr  d10, [x29, #48]
                           0xA8C97BFD, // ldp  x29, x30, [sp], #144
                           0xD65F03C0, // ret
                       });
}

TEST_CASE("AArch64 RCU emitter emits every word of a function widening an integer to a double") {
    // A cast between the two register files is one SCVTF, and its
    // signedness is the integer side's.
    const auto package = CompileToAArch64Lir(R"(
        func Widen(n: int) -> float64 {
            return n as float64;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    CheckFunctionImage(objects.front(), "Widen",
                       {
                           0xA9BB7BFD, // stp   x29, x30, [sp, #-80]!
                           0x910003FD, // mov   x29, sp
                           0xA90153B3, // stp   x19, x20, [x29, #16]
                           0xFD0013A8, // str   d8, [x29, #32]
                           0xAA0003F3, // mov   x19, x0
                           0x9100E3B4, // add   x20, x29, #56
                           0xF9000293, // str   x19, [x20]
                           0xF9400293, // ldr   x19, [x20]
                           0x9E620268, // scvtf d8, x19
                           0x1E604100, // fmov  d0, d8
                           0xA94153B3, // ldp   x19, x20, [x29, #16]
                           0xFD4013A8, // ldr   d8, [x29, #32]
                           0xA8C57BFD, // ldp   x29, x30, [sp], #80
                           0xD65F03C0, // ret
                       });
}

TEST_CASE("AArch64 RCU emitter emits every word of an asm func") {
    // An `asm func` is the body the source wrote and nothing else: no
    // prologue opens a frame the body did not ask for, and no epilogue follows
    // the RET the body already wrote.
    const auto package = CompileToAArch64Lir(R"(
        asm func AddAsm(a: int64, b: int64) -> int64 {
            add x0, x0, x1
            ret
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    CheckFunctionImage(objects.front(), "AddAsm",
                       {
                           0x8B010000, // add x0, x0, x1
                           0xD65F03C0, // ret
                       });
}

TEST_CASE("AArch64 RCU emitter emits every word of a function whose values live across two calls") {
    // Everything live across a call is in a callee-saved register the
    // prologue preserved and the epilogue restored, which is what the allocator
    // is for: four of them here, in two pairs, and no spill of a live value into
    // the frame between the calls.
    const auto package = CompileToAArch64Lir(R"(
        func Add(a: int, b: int) -> int {
            return a + b;
        }

        func Twice(n: int) -> int {
            let once = Add(n, 1);
            return Add(once, 2);
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));

    CheckFunctionImage(objects.front(), "Twice",
                       {
                           0xA9B77BFD, // stp x29, x30, [sp, #-144]!
                           0x910003FD, // mov x29, sp
                           0xA90153B3, // stp x19, x20, [x29, #16]
                           0xA9025BB5, // stp x21, x22, [x29, #32]
                           0xAA0003F3, // mov x19, x0
                           0x910103B4, // add x20, x29, #64
                           0xF9000293, // str x19, [x20]        — the slot of `n`
                           0x910143B3, // add x19, x29, #80     — the slot of `once`
                           0xF9400295, // ldr x21, [x20]
                           0xD2800034, // mov x20, #1
                           0xAA1503E0, // mov x0, x21
                           0xAA1403E1, // mov x1, x20
                           0x94000000, // bl  Add
                           0xAA0003F6, // mov x22, x0
                           0xF9000276, // str x22, [x19]
                           0xF9400274, // ldr x20, [x19]
                           0xD2800053, // mov x19, #2
                           0xAA1403E0, // mov x0, x20
                           0xAA1303E1, // mov x1, x19
                           0x94000000, // bl  Add
                           0xAA0003F5, // mov x21, x0
                           0xAA1503E0, // mov x0, x21
                           0xA94153B3, // ldp x19, x20, [x29, #16]
                           0xA9425BB5, // ldp x21, x22, [x29, #32]
                           0xA8C97BFD, // ldp x29, x30, [sp], #144
                           0xD65F03C0, // ret
                       });

    const std::vector<std::pair<std::uint32_t, std::uint16_t>> expected = {
        {48, RcuRelType::AArch64Call26},
        {76, RcuRelType::AArch64Call26},
    };
    CHECK(FunctionRelocs(objects.front(), "Twice", "Add") == expected);
}
