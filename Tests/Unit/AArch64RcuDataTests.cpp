// AArch64 RCU literals, static data, memory addressing and aggregate copies.

#include "AArch64RcuEmitterTestSupport.h"
#include "CodeGen/AArch64/RcuEmitter.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <doctest.h>
#include <format>
#include <string>
#include <utility>
#include <vector>

using namespace Rux;
using namespace Rux::Testing;

// Constants, globals and the read-only pool
//
// The opcodes below are the ones a value has to pass through before anything
// can be done with it, and the store that puts it to use belongs to the next
// group. The programs here therefore still report the opcodes they reach past
// the constant, which is why these cases assert what was emitted for the
// constant rather than that nothing was reported.

TEST_CASE("AArch64 RCU emitter writes every byte of an aggregate constant") {
    // A payload-carrying variant written as its empty case is one constant whose type is not a register value. The
    // literal names the low bytes and says nothing about the rest, so the rest is written as zero: leaving it would
    // hand the slot whatever the last value there happened to be, which is the sort of difference that shows up as
    // a test passing on one target and not the other.
    const auto package = CompileToAArch64Lir(R"(
        variant Parcel {
            Nothing,
            Count(int64)
        }

        func Take(parcel: Parcel) -> int64 {
            return 0i64;
        }

        func Main() -> int {
            return Take(Parcel::Nothing) as int;
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    REQUIRE_EQ(objects.size(), 1);

    const auto words = FunctionWords(objects.front(), "Main");
    // STR (immediate, unsigned offset) at 64 bits is 1111'1001'00 with the source register in the low five bits, so
    // a store naming register 31 is a store of the zero register.
    const auto zeroStores = std::ranges::count_if(
        words, [](const std::uint32_t word) { return (word & 0xFFC0'0000U) == 0xF900'0000U && (word & 0x1FU) == 31U; });
    CHECK_MESSAGE(zeroStores > 0, "the padding of an aggregate constant must be written, not left");
}

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
