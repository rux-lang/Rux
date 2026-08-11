// The AArch64 RCU back end: the object it produces for a whole function, and
// the reports it makes for everything it does not lower yet.
//
// The expected words below came from `llvm-mc -triple=aarch64 -show-encoding`
// on the instruction named beside each, so a disagreement here is a
// disagreement with a second implementation rather than with someone's reading
// of the ARM manual.

#include "CodeGen/AArch64/RcuEmitter.h"
#include "Driver/BuildTarget.h"
#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <algorithm>
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
LirPackage CompileToAArch64Lir(const std::string &source) {
    Lexer lexer(source, "test.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());

    const TargetContext target = Driver::TargetContextForTriple("linux-aarch64");
    Parser parser(std::move(lexed.tokens), "test.rux", target.arch);
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    std::vector<Module *> modules = {&parsed.module};
    SemanticAnalyzer analyzer(modules, {}, "test", "linux");
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

// The immediate of `add x9, x29, #imm`, or nothing when the word is some other
// instruction. An alloca's address is the frame pointer plus a displacement,
// and which displacement is the frame layout's business rather than this test's.
[[nodiscard]] std::optional<std::uint32_t> FramePointerAddImm(const std::uint32_t word) {
    if ((word & 0xFFC003FFU) != 0x910003A9U) {
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

// The register `ldr xN, [x29, #imm]` loads, or nothing when the word is some
// other instruction. An argument register is read back out of the load that
// filled it rather than out of a whole expected word, since which slot the
// value came from is the frame layout's business rather than this test's.
[[nodiscard]] std::optional<unsigned> SlotLoadRegister(const std::uint32_t word) {
    if ((word & 0xFFC003E0U) != 0xF94003A0U) {
        return std::nullopt;
    }
    return word & 0x1FU;
}

// The register `str xN, [x29, #imm]` stores, on the same terms: this is how a
// parameter spill is read back.
[[nodiscard]] std::optional<unsigned> SlotStoreRegister(const std::uint32_t word) {
    if ((word & 0xFFC003E0U) != 0xF90003A0U) {
        return std::nullopt;
    }
    return word & 0x1FU;
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

// The byte displacement an `ldr x9, [x29, #imm]` or an `ldrh w9, [x29, #imm]`
// names, which is how an incoming stack argument is read back at the width its
// own type occupies. The immediate counts units of that width.
[[nodiscard]] std::optional<std::int32_t> IncomingDisplacement(const std::uint32_t word, const unsigned width) {
    const std::uint32_t opcode = width == 8 ? 0xF94003A9U : 0x794003A9U;
    if ((word & 0xFFC003FFU) != opcode) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>((word >> 10U & 0xFFFU) * width);
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

    // One 8-byte slot above the 16-byte frame record rounds the frame to 32.
    const std::vector<std::uint32_t> expected = {
        0xA9BE7BFD, // stp  x29, x30, [sp, #-32]!
        0x910003FD, // mov  x29, sp
        0xD2800549, // mov  x9, #42
        0xF9000BA9, // str  x9, [x29, #16]
        0xF9400BA0, // ldr  x0, [x29, #16]
        0xA8C27BFD, // ldp  x29, x30, [sp], #32
        0xD65F03C0, // ret
    };
    const auto words = FunctionWords(object, "Main");
    REQUIRE_EQ(words.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK_EQ(HexWord(words[i]), HexWord(expected[i]));
    }
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
}

TEST_CASE("AArch64 RCU emitter reports an unimplemented opcode by name") {
    // A widening conversion, which reaches the back end as the `cast` opcode
    // that Task 26 lowers.
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            var narrow: int32 = 1;
            var wide: int = narrow as int;
            return wide;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_EQ(objects.size(), 1);

    const auto reports = JoinMessages(emitter.Diagnostics());
    CHECK_MESSAGE(reports.contains("'cast' opcode"), reports);
    CHECK_MESSAGE(reports.contains("not implemented yet"), reports);
    CHECK_MESSAGE(reports.contains("'Main'"), reports);
    for (const auto &diagnostic : emitter.Diagnostics()) {
        CHECK(diagnostic.IsError());
    }
}

TEST_CASE("AArch64 RCU emitter names each unimplemented construct once") {
    // Three conversions, which are three instructions of the one opcode this
    // back end does not lower yet: what a report names is the construct, so
    // reaching it again says nothing new.
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            var narrow: int32 = 1;
            var first: int = narrow as int;
            var second: int = narrow as int;
            var third: int64 = narrow as int64;
            return 0;
        }
    )");

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
    CHECK_MESSAGE(messages.front().contains("'cast' opcode"), messages.front());
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

    // Each is one bit pattern in X9 and one store of the bytes its type
    // occupies: a boolean and a character are single bytes, and a pointer is a
    // doubleword.
    const std::vector<std::uint32_t> expected = {
        0xD2800029, // mov  x9, #1
        0xD2800F09, // mov  x9, #120
        0xD2800009, // mov  x9, #0
    };
    for (const auto word : expected) {
        CHECK_MESSAGE(std::ranges::find(words, word) != words.end(), HexWord(word));
    }
    // The narrow ones store a byte through the W view of the same register.
    CHECK(std::ranges::any_of(words, [](const std::uint32_t w) { return (w & 0xFFC003FFU) == 0x390003A9U; }));
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
        std::ranges::find_if(words, [](const std::uint32_t w) { return FramePointerAddImm(w).has_value(); });
    REQUIRE_MESSAGE(found != words.end(), "the alloca's address is an ADD from X29");
    // The frame record sits at the bottom of the frame, so every local is above
    // it and no alloca is ever reached at a displacement of zero.
    CHECK_GE(*FramePointerAddImm(*found), 16);
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

    // The two FMOV forms name their value outright and reach no memory at all.
    CHECK_MESSAGE(std::ranges::find(words, 0x1E6F1010U) != words.end(), "fmov d16, #1.5");
    CHECK_MESSAGE(std::ranges::find(words, 0x1E209010U) != words.end(), "fmov s16, #2.5");

    // 1e300 is not one of the 256 values FMOV encodes, so it is a doubleword in
    // .rodata reached by ADRP plus a scaled LDR, one relocation on each.
    const auto pooled = RelocsFor(object, RCU_TEXT_IDX, "__f64_0");
    REQUIRE_EQ(pooled.size(), 2);
    CHECK_EQ(pooled[0].type, RcuRelType::AArch64AdrPrelPgHi21);
    CHECK_EQ(pooled[1].type, RcuRelType::AArch64LdstAbsLo12Nc);
    CHECK_EQ(pooled[1].sectionOffset, pooled[0].sectionOffset + 4);
    CHECK_EQ(HexWord(TextWordAt(object, pooled[0].sectionOffset) & 0x9F00001FU), HexWord(0x90000010U)); // adrp x16
    CHECK_EQ(HexWord(TextWordAt(object, pooled[1].sectionOffset) & 0xFFC003FFU),
             HexWord(0xFD400210U)); // ldr d16, [x16]

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
        CHECK_EQ(HexWord(TextWordAt(object, repeated[i].sectionOffset) & 0x9F00001FU), HexWord(0x90000009U)); // adrp x9
        CHECK_EQ(HexWord(TextWordAt(object, repeated[i + 1].sectionOffset) & 0xFFC003FFU),
                 HexWord(0x91000129U)); // add x9, x9, #0
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
    const std::vector<std::uint32_t> expected = {
        0x39000149, // strb  w9, [x10]
        0x39800149, // ldrsb x9, [x10]
        0x79000149, // strh  w9, [x10]
        0x79400149, // ldrh  w9, [x10]
    };
    for (const auto word : expected) {
        CHECK_MESSAGE(std::ranges::find(words, word) != words.end(), HexWord(word));
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
    CHECK_MESSAGE(std::ranges::find(words, 0x91002129U) != words.end(), "add x9, x9, #8");
    // The first sits at zero, so it costs no instruction at all: an ADD of any
    // other offset would mean the layout was recomputed rather than read.
    CHECK_FALSE(std::ranges::any_of(
        words, [](const std::uint32_t w) { return (w & 0xFFC003FFU) == 0x91000129U && (w >> 10U & 0xFFFU) != 8; }));
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
    CHECK_MESSAGE(std::ranges::find(words, 0x8B0A0929U) != words.end(), "add x9, x9, x10, lsl #2");
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
    CHECK_MESSAGE(std::ranges::find(words, 0x9B0C2549U) != words.end(), "madd x9, x10, x12, x9");
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
    CHECK_MESSAGE(std::ranges::find(words, 0xA9403149U) != words.end(), "ldp x9, x12, [x10]");
    CHECK_MESSAGE(std::ranges::find(words, 0xA9003149U) != words.end(), "stp x9, x12, [x10]");
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

    // The only pairs left in the function are the frame record's, which is the
    // prologue's and the epilogue's business rather than the copy's.
    for (const auto word : words) {
        if (!IsPairAccess(word)) {
            continue;
        }
        CHECK_EQ(word & 31U, 29);        // x29
        CHECK_EQ(word >> 10U & 31U, 30); // x30
    }
    // Eight of the nine bytes go in one doubleword and the ninth on its own.
    CHECK(std::ranges::any_of(words, [](const std::uint32_t w) { return (w & 0xFFC003E0U) == 0xF9400140U; }));
    CHECK(std::ranges::any_of(words, [](const std::uint32_t w) { return (w & 0xFFC003E0U) == 0x39400140U; }));
}

TEST_CASE("AArch64 RCU emitter reaches a slot past the addressing range through a scratch register") {
    // Enough locals to put the last one past the 32 KiB a scaled doubleword
    // offset reaches, so neither immediate form of LDR can name it.
    std::string body;
    for (int i = 0; i < 4200; ++i) {
        body += std::format("    var v{}: int = {};\n", i, i);
    }
    const auto package = CompileToAArch64Lir(std::format(R"(
        func Main() -> int {{
{}            return v4199;
        }}
    )",
                                                         body));

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    const auto words = FunctionWords(objects.front(), "Main");

    // The displacement moves into X16, which then addresses at zero — the
    // fallback the encoder's ResolveMemOperand emits, reached here rather than
    // hand-rolled beside the access.
    CHECK(std::ranges::any_of(words, [](const std::uint32_t w) { return (w & 0xFF8003FFU) == 0x910003B0U; }));
    CHECK(std::ranges::any_of(words, [](const std::uint32_t w) { return (w & 0xFFFFFFE0U) == 0xF9400200U; }));
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

    const std::vector<std::uint32_t> expected = {
        0x8B0C0129, // add x9, x9, x12
        0xCB0C0129, // sub x9, x9, x12
        0x9B0C7D29, // mul x9, x9, x12
        0x8A0C0129, // and x9, x9, x12
        0xAA0C0129, // orr x9, x9, x12
        0xCA0C0129, // eor x9, x9, x12
    };
    for (const auto word : expected) {
        CHECK_MESSAGE(std::ranges::find(words, word) != words.end(), HexWord(word));
    }
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
    CHECK_MESSAGE(std::ranges::find(words, 0x9ACC0D2AU) != words.end(), "sdiv x10, x9, x12");
    CHECK_MESSAGE(std::ranges::find(words, 0x9ACC092AU) != words.end(), "udiv x10, x9, x12");
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
    CHECK_MESSAGE(std::ranges::find(words, 0x9ACC0D2AU) != words.end(), "sdiv x10, x9, x12");
    CHECK_MESSAGE(std::ranges::find(words, 0x9B0CA549U) != words.end(), "msub x9, x10, x12, x9");
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

    // Two byte loads, the addition at a whole register's width, and a byte
    // store: 300 wraps to 44 because the byte above it is never written, which
    // is exactly how the x86-64 back end wraps it and costs no masking here
    // either.
    const auto sum = std::ranges::find(words, 0x8B0C0129U); // add x9, x9, x12
    REQUIRE_MESSAGE(sum != words.end(), "add x9, x9, x12");
    const auto index = static_cast<std::size_t>(sum - words.begin());
    REQUIRE_GE(index, 2);
    REQUIRE_LT(index + 1, words.size());
    CHECK_EQ(HexWord(words[index - 2] & 0xFFC003FFU), HexWord(0x394003A9U)); // ldrb w9, [x29, #imm]
    CHECK_EQ(HexWord(words[index - 1] & 0xFFC003FFU), HexWord(0x394003ACU)); // ldrb w12, [x29, #imm]
    CHECK_EQ(HexWord(words[index + 1] & 0xFFC003FFU), HexWord(0x390003A9U)); // strb w9, [x29, #imm]
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
    const auto arithmetic = std::ranges::find(words, 0x9ACC2929U); // asr x9, x9, x12
    const auto logical = std::ranges::find(words, 0x9ACC2529U);    // lsr x9, x9, x12
    REQUIRE_MESSAGE(arithmetic != words.end(), "asr x9, x9, x12");
    REQUIRE_MESSAGE(logical != words.end(), "lsr x9, x9, x12");
    CHECK_MESSAGE(std::ranges::find(words, 0x9ACC2129U) != words.end(), "lsl x9, x9, x12");

    // What separates the two right shifts is the load two instructions above
    // each: `>>` sign-extends its operand and `>>>` reads the same signed type
    // as unsigned, which is the whole of the difference between them.
    const auto signedLoad = *(arithmetic - 2);
    const auto unsignedLoad = *(logical - 2);
    CHECK_EQ(HexWord(signedLoad & 0xFFC003FFU), HexWord(0xB98003A9U));   // ldrsw x9, [x29, #imm]
    CHECK_EQ(HexWord(unsignedLoad & 0xFFC003FFU), HexWord(0xB94003A9U)); // ldr   w9, [x29, #imm]
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

    const std::vector<std::uint32_t> expected = {
        0xCB0903E9, // neg  x9, x9        — SUB from the zero register
        0xAA2903E9, // mvn  x9, x9        — ORN from the zero register
        0xF100013F, // cmp  x9, #0
        0x9A9F17E9, // cset x9, eq        — the logical negation, as a boolean
        0xD2400129, // eor  x9, x9, #1    — `~` on a boolean is that negation too
    };
    for (const auto word : expected) {
        CHECK_MESSAGE(std::ranges::find(words, word) != words.end(), HexWord(word));
    }
    // A boolean is a byte in its slot, so complementing the whole register
    // would leave 0xFE there and read back as true again.
    CHECK_EQ(std::ranges::count(words, 0xAA2903E9U), 1);
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
    CHECK_EQ(std::ranges::count(words, 0xEB0C013FU), 5); // cmp  x9, x12
    const std::vector<std::uint32_t> expected = {
        0x9A9FA7E9, // cset x9, lt   — a signed `<`
        0x9A9FB7E9, // cset x9, ge   — a signed `>=`
        0x9A9F17E9, // cset x9, eq
        0x9A9F27E9, // cset x9, lo   — the same two comparisons, unsigned
        0x9A9F37E9, // cset x9, hs
    };
    for (const auto word : expected) {
        CHECK_MESSAGE(std::ranges::find(words, word) != words.end(), HexWord(word));
    }
    // A boolean is one byte of its slot, so the result is stored as one.
    CHECK_EQ(std::ranges::count_if(words, [](const std::uint32_t w) { return (w & 0xFFC003FFU) == 0x390003A9U; }), 5);
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
    CHECK_EQ(std::ranges::count(words, 0x1E712200U), 3); // fcmp d16, d17
    CHECK_EQ(std::ranges::count(words, 0x1E312200U), 1); // fcmp s16, s17

    // MI and LS rather than LT and LE: an unordered comparison leaves C and V
    // set, which LT and LE are satisfied by and these two are not, so `<` and
    // `<=` against a NaN answer false — and `!=` answers true — exactly as the
    // x86-64 back end's ordered/parity pairs do.
    const std::vector<std::uint32_t> expected = {
        0x9A9F57E9, // cset x9, mi   — `<`
        0x9A9F87E9, // cset x9, ls   — `<=`
        0x9A9F07E9, // cset x9, ne   — `!=`, the one comparison a NaN satisfies
        0x9A9FD7E9, // cset x9, gt   — `>`
    };
    for (const auto word : expected) {
        CHECK_MESSAGE(std::ranges::find(words, word) != words.end(), HexWord(word));
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
    // panic or a call that does not return, which is Task 27's — so the LIR is
    // written out here rather than compiled.
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

// AAPCS64 calls
//
// One convention for both sides of every call: the cases below read the caller
// and the callee out of the same object and check that what one wrote is where
// the other looks. Nothing here depends on a calling convention the LIR names,
// because AArch64 has one and a Rux function and a C function are called by it
// alike.

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
        const auto loaded = SlotLoadRegister(word);
        REQUIRE_MESSAGE(loaded.has_value(), HexWord(word));
        CHECK_EQ(*loaded, reg);
    }

    // Eight arguments need no stack of their own, so nothing moves the stack
    // pointer between the prologue that opened the frame and the branch.
    for (const auto word : caller) {
        CHECK_FALSE(StackPointerAdjustment(word, true).has_value());
        CHECK_FALSE(StackPointerAdjustment(word, false).has_value());
    }

    // The callee spills the same eight registers into its frame before it does
    // anything else, which is what makes every later mention of a parameter a
    // read of a slot.
    const auto callee = FunctionWords(objects.front(), "Eight");
    REQUIRE_GT(callee.size(), 10);
    for (unsigned reg = 0; reg < 8; ++reg) {
        const std::uint32_t word = callee[2 + reg];
        const auto stored = SlotStoreRegister(word);
        REQUIRE_MESSAGE(stored.has_value(), HexWord(word));
        CHECK_EQ(*stored, reg);
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
    CHECK_EQ(IncomingDisplacement(callee[10], 8), std::optional<std::int32_t>(*frame));
    // A narrow one is read at the width its own type occupies rather than a
    // whole doubleword, since a C caller leaves the bytes above it as it found
    // them.
    CHECK_EQ(IncomingDisplacement(callee[12], 2), std::optional<std::int32_t>(*frame + 8));
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

    // The load that fills X0 extends by the returned type, which is the whole
    // of what AAPCS64 asks a callee to do: unsigned zeroes the register above
    // the value, signed sign-extends it.
    const auto byteReturn = FunctionWords(objects.front(), "Byte");
    CHECK_EQ(HexWord(byteReturn[byteReturn.size() - 3] & 0xFFC003E0U), HexWord(0x394003A0U)); // ldrb w0, [x29]
    const auto shortReturn = FunctionWords(objects.front(), "Short");
    CHECK_EQ(HexWord(shortReturn[shortReturn.size() - 3] & 0xFFC003E0U), HexWord(0x798003A0U)); // ldrsh x0, [x29]

    // The caller keeps only the bytes the type occupies, so nothing it does
    // afterwards depends on the bits above them.
    const auto caller = FunctionWords(objects.front(), "Main");
    const auto call = BranchAndLinkIndex(caller);
    REQUIRE(call.has_value());
    CHECK_EQ(HexWord(caller[*call + 1] & 0xFFC003E0U), HexWord(0x390003A0U)); // strb w0, [x29]
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
    CHECK_EQ(SlotLoadRegister(words[index - 3]), std::optional<unsigned>(0));
    CHECK_EQ(SlotLoadRegister(words[index - 2]), std::optional<unsigned>(1));
    CHECK_EQ(SlotLoadRegister(words[index - 1]), std::optional<unsigned>(9));

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

TEST_CASE("AArch64 RCU emitter emits no call at all when one argument cannot be placed") {
    // A float travels in the vector file, which is Task 25's, and the ninth
    // argument is where this one sits — so a generator that placed what it
    // could would have opened a stack area and filled eight registers for a
    // call that must not happen.
    const auto package = CompileToAArch64Lir(R"(
        func Mixed(a: int, b: int, c: int, d: int, e: int, f: int, g: int, h: int, i: float64, j: int) -> int {
            return j;
        }

        func Main() -> int {
            var result = Mixed(1, 2, 3, 4, 5, 6, 7, 8, 9.5, 10);
            return 0;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    const auto reports = JoinMessages(emitter.Diagnostics());
    CHECK_MESSAGE(reports.contains("passing an argument of type 'float64'"), reports);
    CHECK_MESSAGE(reports.contains("'Main'"), reports);

    const auto words = FunctionWords(objects.front(), "Main");
    CHECK_FALSE(BranchAndLinkIndex(words).has_value());
    for (const auto word : words) {
        CHECK_FALSE(StackPointerAdjustment(word, true).has_value());
    }
}
