// AArch64 vtable layout and complete-function word images.

#include "AArch64RcuEmitterTestSupport.h"
#include "CodeGen/AArch64/RcuEmitter.h"

#include <algorithm>
#include <cstdint>
#include <doctest.h>
#include <utility>
#include <vector>

using namespace Rux;
using namespace Rux::Testing;

// Vtable layout

TEST_CASE("AArch64 RCU emitter lays a vtable out as a run of relocated function pointers") {
    const auto package = CompileToAArch64Lir(R"(
        interface Figure {
            func Area() -> int;
        }

        struct Square {
            size: int;
        }

        extend Square : Figure {
            func Area(self: &Square) -> int {
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
