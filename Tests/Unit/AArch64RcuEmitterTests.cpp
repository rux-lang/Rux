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
    // Written so the sum survives constant folding and reaches the back end as
    // an `add` rather than as one more constant.
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            var total: int = 1;
            total = total + 2;
            return total;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_EQ(objects.size(), 1);

    const auto reports = JoinMessages(emitter.Diagnostics());
    CHECK_MESSAGE(reports.contains("'add' opcode"), reports);
    CHECK_MESSAGE(reports.contains("not implemented yet"), reports);
    CHECK_MESSAGE(reports.contains("'Main'"), reports);
    for (const auto &diagnostic : emitter.Diagnostics()) {
        CHECK(diagnostic.IsError());
    }
}

TEST_CASE("AArch64 RCU emitter names each unimplemented construct once") {
    const auto package = CompileToAArch64Lir(R"(
        func Main() -> int {
            var total: int = 0;
            for i in 0..8 {
                total = total + i;
            }
            return total;
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
    CHECK_MESSAGE(!messages.empty(), "a loop reaches opcodes this back end does not lower yet");
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
