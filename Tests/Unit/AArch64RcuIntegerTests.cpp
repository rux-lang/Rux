// AArch64 RCU integer arithmetic, comparisons, shifts and casts.

#include "AArch64RcuEmitterTestSupport.h"
#include "CodeGen/AArch64/RcuEmitter.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <doctest.h>
#include <tuple>
#include <utility>
#include <vector>

using namespace Rux;
using namespace Rux::Testing;

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

// Integer casts and enum representations

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
