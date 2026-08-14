// The AArch64 RCU back end: the object it produces for a whole function, and
// the reports it makes for everything it does not lower yet.
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

// Inline assembly and static data
//
// An `asm func` is the one body this back end selects no instructions for.
// The cases below verify its raw bytes and relocations, then the static data
// and vtable relocations emitted beside ordinary functions.

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
