// AArch64 floating-point lowering and inline-assembly RCU emission.
//
// The expected words below came from `llvm-mc -triple=aarch64 -show-encoding`
// on the instruction named beside each, so a disagreement here is a
// disagreement with a second implementation rather than with someone's reading
// of the ARM manual.

#include "AArch64RcuEmitterTestSupport.h"
#include "CodeGen/AArch64/Encoder.h"
#include "CodeGen/AArch64/RcuEmitter.h"

#include <algorithm>
#include <cstdint>
#include <doctest.h>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

using namespace Rux;
using namespace Rux::Testing;

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

// Inline assembly
//
// An `asm func` is the one body this back end selects no instructions for.
// The cases below verify its raw bytes and relocations.

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
