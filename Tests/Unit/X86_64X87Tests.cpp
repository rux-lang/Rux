// x86-64 x87 encoding and float80 lowering.
//
// Every expected byte string follows the Intel SDM opcode tables and was cross-checked against clang's integrated
// assembler in Intel syntax with the `{disp32}` prefix, which forces the disp32 forms this encoder always emits.

#include "CodeGen/X86_64/AssemblyPrinter.h"
#include "CodeGen/X86_64/Encoder.h"
#include "CodeGen/X86_64/FramePlan.h"
#include "CodeGen/X86_64/RcuEmitter.h"
#include "Linker/Linker.h"
#include "Numeric/FloatParsing.h"
#include "Numeric/SoftwareFloat.h"
#include "System/Process.h"
#include "Target/FloatLowering.h"
#include "Target/Platform.h"
#include "TemporaryDirectory.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <doctest.h>
#include <filesystem>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Rux;

namespace {
using Bytes = std::vector<std::uint8_t>;
using Gpr = X64Enc::Gpr;

template <typename Emit>
[[nodiscard]] Bytes Encode(Emit emit) {
    Bytes data;
    const X64Enc encoder(data);
    emit(encoder);
    return data;
}
} // namespace

TEST_CASE("x87 loads and stores address rbp-relative frame slots with a disp32") {
    // [rbp - 32]: ModRM mod=10 r/m=101 with the /digit in reg, then E0 FF FF FF.
    CHECK(Encode([](const X64Enc &e) { e.FldTbyte(-32); }) == Bytes{0xDB, 0xAD, 0xE0, 0xFF, 0xFF, 0xFF});
    CHECK(Encode([](const X64Enc &e) { e.FstpTbyte(-32); }) == Bytes{0xDB, 0xBD, 0xE0, 0xFF, 0xFF, 0xFF});
    CHECK(Encode([](const X64Enc &e) { e.FldQword(-32); }) == Bytes{0xDD, 0x85, 0xE0, 0xFF, 0xFF, 0xFF});
    CHECK(Encode([](const X64Enc &e) { e.FstpQword(-32); }) == Bytes{0xDD, 0x9D, 0xE0, 0xFF, 0xFF, 0xFF});
    CHECK(Encode([](const X64Enc &e) { e.FldDword(-32); }) == Bytes{0xD9, 0x85, 0xE0, 0xFF, 0xFF, 0xFF});
    CHECK(Encode([](const X64Enc &e) { e.FstpDword(-32); }) == Bytes{0xD9, 0x9D, 0xE0, 0xFF, 0xFF, 0xFF});
    CHECK(Encode([](const X64Enc &e) { e.FildQword(-32); }) == Bytes{0xDF, 0xAD, 0xE0, 0xFF, 0xFF, 0xFF});
    CHECK(Encode([](const X64Enc &e) { e.FildDword(-32); }) == Bytes{0xDB, 0x85, 0xE0, 0xFF, 0xFF, 0xFF});
}

TEST_CASE("x87 rbp forms are the base-register forms with rbp as the base") {
    CHECK(Encode([](const X64Enc &e) { e.FldTbyte(0x120); }) ==
          Encode([](const X64Enc &e) { e.FldTbyteAt(Gpr::Rbp, 0x120); }));
    CHECK(Encode([](const X64Enc &e) { e.FstpQword(-8); }) ==
          Encode([](const X64Enc &e) { e.FstpQwordAt(Gpr::Rbp, -8); }));
    CHECK(Encode([](const X64Enc &e) { e.FildDword(4); }) ==
          Encode([](const X64Enc &e) { e.FildDwordAt(Gpr::Rbp, 4); }));
}

TEST_CASE("x87 base-register forms add REX.B for r8-r15 and a SIB byte for rsp and r12") {
    // fld tbyte [r10 + 16]
    CHECK(Encode([](const X64Enc &e) { e.FldTbyteAt(Gpr::R10, 16); }) ==
          Bytes{0x41, 0xDB, 0xAA, 0x10, 0x00, 0x00, 0x00});
    // fstp tbyte [rsp + 8]
    CHECK(Encode([](const X64Enc &e) { e.FstpTbyteAt(Gpr::Rsp, 8); }) ==
          Bytes{0xDB, 0xBC, 0x24, 0x08, 0x00, 0x00, 0x00});
    // fstp tbyte [r11 - 8]
    CHECK(Encode([](const X64Enc &e) { e.FstpTbyteAt(Gpr::R11, -8); }) ==
          Bytes{0x41, 0xDB, 0xBB, 0xF8, 0xFF, 0xFF, 0xFF});
    // fld tbyte [r12 + 0x100]: r12 shares rsp's r/m encoding, so it needs the SIB byte too.
    CHECK(Encode([](const X64Enc &e) { e.FldTbyteAt(Gpr::R12, 0x100); }) ==
          Bytes{0x41, 0xDB, 0xAC, 0x24, 0x00, 0x01, 0x00, 0x00});
    // fld tbyte [r13 + 0x100]: r13 shares rbp's r/m encoding, which mod=10 makes an ordinary base.
    CHECK(Encode([](const X64Enc &e) { e.FldTbyteAt(Gpr::R13, 0x100); }) ==
          Bytes{0x41, 0xDB, 0xAD, 0x00, 0x01, 0x00, 0x00});
    // fld qword [rax + 0x12345678]
    CHECK(Encode([](const X64Enc &e) { e.FldQwordAt(Gpr::Rax, 0x12345678); }) ==
          Bytes{0xDD, 0x80, 0x78, 0x56, 0x34, 0x12});
    // fstp qword [r15 + 24]
    CHECK(Encode([](const X64Enc &e) { e.FstpQwordAt(Gpr::R15, 24); }) ==
          Bytes{0x41, 0xDD, 0x9F, 0x18, 0x00, 0x00, 0x00});
    // fld dword [rdi + 4]
    CHECK(Encode([](const X64Enc &e) { e.FldDwordAt(Gpr::Rdi, 4); }) == Bytes{0xD9, 0x87, 0x04, 0x00, 0x00, 0x00});
    // fstp dword [rbx + 0]
    CHECK(Encode([](const X64Enc &e) { e.FstpDwordAt(Gpr::Rbx, 0); }) == Bytes{0xD9, 0x9B, 0x00, 0x00, 0x00, 0x00});
    // fild qword [rsp + 16]
    CHECK(Encode([](const X64Enc &e) { e.FildQwordAt(Gpr::Rsp, 16); }) ==
          Bytes{0xDF, 0xAC, 0x24, 0x10, 0x00, 0x00, 0x00});
    // fild dword [r9 - 4]
    CHECK(Encode([](const X64Enc &e) { e.FildDwordAt(Gpr::R9, -4); }) ==
          Bytes{0x41, 0xDB, 0x81, 0xFC, 0xFF, 0xFF, 0xFF});
}

TEST_CASE("x87 popping arithmetic uses the Intel operand order") {
    // Intel FSUBP ST(1), ST(0) is DE E9 and computes ST(1) - ST(0). AT&T `fsubp %st, %st(1)` names DE E1 instead.
    CHECK(Encode([](const X64Enc &e) { e.FaddpSt1(); }) == Bytes{0xDE, 0xC1});
    CHECK(Encode([](const X64Enc &e) { e.FsubpSt1(); }) == Bytes{0xDE, 0xE9});
    CHECK(Encode([](const X64Enc &e) { e.FsubrpSt1(); }) == Bytes{0xDE, 0xE1});
    CHECK(Encode([](const X64Enc &e) { e.FmulpSt1(); }) == Bytes{0xDE, 0xC9});
    CHECK(Encode([](const X64Enc &e) { e.FdivpSt1(); }) == Bytes{0xDE, 0xF9});
    CHECK(Encode([](const X64Enc &e) { e.FdivrpSt1(); }) == Bytes{0xDE, 0xF1});
    CHECK(Encode([](const X64Enc &e) { e.Fprem(); }) == Bytes{0xD9, 0xF8});
    CHECK(Encode([](const X64Enc &e) { e.Fchs(); }) == Bytes{0xD9, 0xE0});
}

TEST_CASE("x87 status, comparison and stack-register forms") {
    CHECK(Encode([](const X64Enc &e) { e.FnstswAx(); }) == Bytes{0xDF, 0xE0});
    CHECK(Encode([](const X64Enc &e) { e.FucomipSt(1); }) == Bytes{0xDF, 0xE9});
    CHECK(Encode([](const X64Enc &e) { e.FucomipSt(3); }) == Bytes{0xDF, 0xEB});
    CHECK(Encode([](const X64Enc &e) { e.FstpSt(0); }) == Bytes{0xDD, 0xD8});
    CHECK(Encode([](const X64Enc &e) { e.FstpSt(1); }) == Bytes{0xDD, 0xD9});
    CHECK(Encode([](const X64Enc &e) { e.FstpSt(7); }) == Bytes{0xDD, 0xDF});
    CHECK(Encode([](const X64Enc &e) { e.FldSt(0); }) == Bytes{0xD9, 0xC0});
    CHECK(Encode([](const X64Enc &e) { e.FldSt(1); }) == Bytes{0xD9, 0xC1});
    CHECK(Encode([](const X64Enc &e) { e.FxchSt(1); }) == Bytes{0xD9, 0xC9});
    CHECK(Encode([](const X64Enc &e) { e.FxchSt(2); }) == Bytes{0xD9, 0xCA});
}

TEST_CASE("x87 control word and the integer helpers around it") {
    CHECK(Encode([](const X64Enc &e) { e.Fnstcw(-32); }) == Bytes{0xD9, 0xBD, 0xE0, 0xFF, 0xFF, 0xFF});
    CHECK(Encode([](const X64Enc &e) { e.Fldcw(-30); }) == Bytes{0xD9, 0xAD, 0xE2, 0xFF, 0xFF, 0xFF});
    CHECK(Encode([](const X64Enc &e) { e.MovWordStoreImm16(-32, 0x037F); }) ==
          Bytes{0x66, 0xC7, 0x85, 0xE0, 0xFF, 0xFF, 0xFF, 0x7F, 0x03});
    CHECK(Encode([](const X64Enc &e) { e.MovDwordStoreImm32(-20, 0); }) ==
          Bytes{0xC7, 0x85, 0xEC, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00});
    CHECK(Encode([](const X64Enc &e) { e.MovDwordStoreImm32(-20, -2); }) ==
          Bytes{0xC7, 0x85, 0xEC, 0xFF, 0xFF, 0xFF, 0xFE, 0xFF, 0xFF, 0xFF});
    CHECK(Encode([](const X64Enc &e) { e.TestAhImm8(0x04); }) == Bytes{0xF6, 0xC4, 0x04});
}

TEST_CASE("x87 encodings compose into a precision-pinned subtraction and an fprem loop") {
    // fnstcw [rbp-48]; mov word [rbp-46], 0x037F; fldcw [rbp-46]; fld a; fld b; fsubp (a - b); fstp result;
    // zero the padding; fldcw [rbp-48].
    const Bytes subtraction = Encode([](const X64Enc &e) {
        e.Fnstcw(-48);
        e.MovWordStoreImm16(-46, 0x037F);
        e.Fldcw(-46);
        e.FldTbyte(-16);
        e.FldTbyte(-32);
        e.FsubpSt1();
        e.FstpTbyte(-64);
        e.MovWordStoreImm16(-54, 0);
        e.MovDwordStoreImm32(-52, 0);
        e.Fldcw(-48);
    });
    const Bytes expected{0xD9, 0xBD, 0xD0, 0xFF, 0xFF, 0xFF,                         // fnstcw [rbp-48]
                         0x66, 0xC7, 0x85, 0xD2, 0xFF, 0xFF, 0xFF, 0x7F, 0x03,       // mov word [rbp-46], 0x037F
                         0xD9, 0xAD, 0xD2, 0xFF, 0xFF, 0xFF,                         // fldcw [rbp-46]
                         0xDB, 0xAD, 0xF0, 0xFF, 0xFF, 0xFF,                         // fld tbyte [rbp-16]
                         0xDB, 0xAD, 0xE0, 0xFF, 0xFF, 0xFF,                         // fld tbyte [rbp-32]
                         0xDE, 0xE9,                                                 // fsubp st(1), st
                         0xDB, 0xBD, 0xC0, 0xFF, 0xFF, 0xFF,                         // fstp tbyte [rbp-64]
                         0x66, 0xC7, 0x85, 0xCA, 0xFF, 0xFF, 0xFF, 0x00, 0x00,       // mov word [rbp-54], 0
                         0xC7, 0x85, 0xCC, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, // mov dword [rbp-52], 0
                         0xD9, 0xAD, 0xD0, 0xFF, 0xFF, 0xFF};                        // fldcw [rbp-48]
    CHECK(subtraction == expected);

    // loop: fprem; fnstsw ax; test ah, 4; jnz loop. The backward rel32 is patched like any other branch.
    Bytes loop;
    const X64Enc encoder(loop);
    const std::uint32_t top = encoder.Size();
    encoder.Fprem();
    encoder.FnstswAx();
    encoder.TestAhImm8(0x04);
    std::uint32_t patch = 0;
    encoder.Jnz(patch);
    encoder.Patch32(patch, static_cast<std::int32_t>(top) - static_cast<std::int32_t>(patch + 4));
    encoder.FstpSt(1);
    CHECK(loop == Bytes{0xD9, 0xF8, 0xDF, 0xE0, 0xF6, 0xC4, 0x04, 0x0F, 0x85, 0xF3, 0xFF, 0xFF, 0xFF, 0xDD, 0xD9});
}

// ---- float80 lowering ----------------------------------------------------------------------------------------------

namespace {
[[nodiscard]] TypeRef Primitive(const TypeRef::Kind kind) {
    return TypeRef::MakePrimitive(kind);
}

const TypeRef Extended = Primitive(TypeRef::Kind::Float80);

/// `%n = op %0[, %1]` over the function's parameters, returned.
[[nodiscard]] LirFunc SingleInstruction(const LirOpcode op, const std::vector<TypeRef> &parameterTypes,
                                        const TypeRef &resultType) {
    LirFunc function;
    function.name = "Operation";
    function.isPublic = true;
    function.returnType = resultType;
    LirInstr instruction;
    instruction.op = op;
    instruction.type = resultType;
    for (LirReg reg = 0; reg < parameterTypes.size(); ++reg) {
        function.params.push_back({reg, parameterTypes[reg], "p" + std::to_string(reg)});
        instruction.srcs.push_back(reg);
    }
    instruction.dst = static_cast<LirReg>(parameterTypes.size());
    LirBlock block;
    block.label = "entry";
    block.instrs = {instruction};
    block.term.emplace();
    block.term->kind = LirTermKind::Return;
    block.term->retVal = instruction.dst;
    block.term->retType = resultType;
    function.blocks.push_back(std::move(block));
    return function;
}

[[nodiscard]] LirPackage PackageOf(std::vector<LirFunc> functions) {
    LirModule module;
    module.name = "X87.rux";
    module.funcs = std::move(functions);
    LirPackage package;
    package.modules.push_back(std::move(module));
    return package;
}

[[nodiscard]] Bytes TextOf(const LirFunc &function, const Target::OS os) {
    const LirPackage package = PackageOf({function}); // the emitter keeps a reference
    RcuEmitter emitter(package, "X87", os);
    const auto objects = emitter.Generate();
    REQUIRE(emitter.Diagnostics().empty());
    REQUIRE_EQ(objects.size(), 1);
    return objects.front().sections.at(RCU_TEXT_IDX).data;
}

[[nodiscard]] bool Contains(const Bytes &text, const Bytes &sequence) {
    return std::ranges::search(text, sequence).begin() != text.end();
}

/// The frame addresses the lowering uses, read from the same plan the RCU emitter builds.
struct Frame {
    explicit Frame(const LirFunc &function, const Target::OS os)
        : plan(PlanX86_64Frame(function, {}, {}, os)) {
    }

    [[nodiscard]] std::int32_t Slot(const LirReg reg) const {
        return -plan.SlotOffsets().at(reg);
    }

    [[nodiscard]] std::int32_t ControlWords() const {
        return -plan.WideTemporaryOffset(0);
    }

    [[nodiscard]] std::int32_t Spill() const {
        return -plan.WideTemporaryOffset(1);
    }

    X86_64FramePlan plan;
};

void BeginRounding(const X64Enc &e, const std::int32_t controlWords) {
    e.Fnstcw(controlWords);
    e.MovWordStoreImm16(controlWords + 2, 0x037F);
    e.Fldcw(controlWords + 2);
}

void PopFloat80(const X64Enc &e, const std::int32_t slot) {
    e.FstpTbyte(slot);
    e.MovWordStoreImm16(slot + 10, 0);
    e.MovDwordStoreImm32(slot + 12, 0);
}

constexpr Target::OS Systems[] = {Target::OS::Windows, Target::OS::Linux};
} // namespace

TEST_CASE("float80 is computed by the x87 unit on x86-64 only") {
    using namespace Target;
    CHECK_EQ(FloatLoweringFor(TypeRef::Kind::Float80, Arch::X86_64), FloatLowering::X87);
    CHECK_EQ(FloatLoweringFor(TypeRef::Kind::Float80, Arch::AArch64), FloatLowering::Software);
    CHECK_EQ(FloatLoweringFor(TypeRef::Kind::Float128, Arch::X86_64), FloatLowering::Software);
    CHECK_EQ(FloatLoweringFor(TypeRef::Kind::Float64, Arch::X86_64), FloatLowering::Native);
}

TEST_CASE("x87 lowering pins 64-bit precision around float80 arithmetic and zeroes the padding") {
    struct Case {
        LirOpcode op;
        void (X64Enc::*combine)() const;
    };

    for (const Case operation : {Case{LirOpcode::Add, &X64Enc::FaddpSt1}, Case{LirOpcode::Sub, &X64Enc::FsubpSt1},
                                 Case{LirOpcode::Mul, &X64Enc::FmulpSt1}, Case{LirOpcode::Div, &X64Enc::FdivpSt1}}) {
        for (const Target::OS os : Systems) {
            CAPTURE(static_cast<int>(operation.op));
            CAPTURE(static_cast<int>(os));
            const LirFunc function = SingleInstruction(operation.op, {Extended, Extended}, Extended);
            const Frame frame(function, os);
            CHECK_EQ(frame.plan.WideTemporarySize(), 16);
            // fnstcw; fld left; fld right; op; fstp tbyte result; zero bytes 10-15; fldcw (the caller's word).
            const Bytes expected = Encode([&](const X64Enc &e) {
                BeginRounding(e, frame.ControlWords());
                e.FldTbyte(frame.Slot(0));
                e.FldTbyte(frame.Slot(1));
                (e.*operation.combine)();
                PopFloat80(e, frame.Slot(2));
                e.Fldcw(frame.ControlWords());
            });
            CHECK(Contains(TextOf(function, os), expected));
        }
    }
}

TEST_CASE("x87 remainder loops on fprem's C2 flag and leaves the control word alone") {
    for (const Target::OS os : Systems) {
        CAPTURE(static_cast<int>(os));
        const LirFunc function = SingleInstruction(LirOpcode::Mod, {Extended, Extended}, Extended);
        const Frame frame(function, os);
        Bytes expected;
        const X64Enc e(expected);
        e.FldTbyte(frame.Slot(1)); // divisor
        e.FldTbyte(frame.Slot(0)); // dividend
        const std::uint32_t top = e.Size();
        e.Fprem();
        e.FnstswAx();
        e.TestAhImm8(0x04);
        std::uint32_t patch = 0;
        e.Jnz(patch);
        e.Patch32(patch, static_cast<std::int32_t>(top) - static_cast<std::int32_t>(patch + 4));
        e.FstpSt(1);
        PopFloat80(e, frame.Slot(2));
        const Bytes text = TextOf(function, os);
        CHECK(Contains(text, expected));
        CHECK_FALSE(Contains(text, Encode([&](const X64Enc &x) { x.Fnstcw(frame.ControlWords()); })));
    }
}

TEST_CASE("float32 and float64 remainders run the same exact fprem loop, not an SSE formula") {
    for (const TypeRef &type : {Primitive(TypeRef::Kind::Float32), Primitive(TypeRef::Kind::Float64)}) {
        for (const Target::OS os : Systems) {
            CAPTURE(type.ToString());
            CAPTURE(static_cast<int>(os));
            const bool float32 = type->kind == TypeRef::Kind::Float32;
            const LirFunc function = SingleInstruction(LirOpcode::Mod, {type, type}, type);
            const Frame frame(function, os);
            // Allocated SSE values pass through a scratch slot because x87 accepts only memory operands.
            REQUIRE_FALSE(frame.plan.PhysicalRegisters().contains(0));
            REQUIRE_FALSE(frame.plan.PhysicalRegisters().contains(2));
            REQUIRE(frame.plan.FloatRegisters().contains(0));
            REQUIRE(frame.plan.FloatRegisters().contains(1));
            REQUIRE(frame.plan.FloatRegisters().contains(2));
            CHECK_GE(frame.plan.WideTemporarySize(), 8);
            Bytes expected;
            const X64Enc e(expected);
            for (const auto reg : {1u, 0u}) { // Divisor, then dividend.
                e.MovXmm(0, frame.plan.FloatRegisters().at(reg));
                float32 ? e.MovssXmm0Store(frame.Spill()) : e.MovsdXmm0Store(frame.Spill());
                float32 ? e.FldDword(frame.Spill()) : e.FldQword(frame.Spill());
            }
            const std::uint32_t top = e.Size();
            e.Fprem();
            e.FnstswAx();
            e.TestAhImm8(0x04);
            std::uint32_t patch = 0;
            e.Jnz(patch);
            e.Patch32(patch, static_cast<std::int32_t>(top) - static_cast<std::int32_t>(patch + 4));
            e.FstpSt(1);
            float32 ? e.FstpDword(frame.Spill()) : e.FstpQword(frame.Spill());
            float32 ? e.MovssXmm0Load(frame.Spill()) : e.MovsdXmm0Load(frame.Spill());
            e.MovXmm(frame.plan.FloatRegisters().at(2), 0);
            const Bytes text = TextOf(function, os);
            CHECK(Contains(text, expected));
            // Exact, so the control word is left alone; and nothing of the truncated-quotient formula remains.
            CHECK_FALSE(Contains(text, Encode([&](const X64Enc &x) { x.Fnstcw(frame.ControlWords()); })));
            CHECK_FALSE(Contains(text, float32 ? Bytes{0xF3, 0x0F, 0x2C} : Bytes{0xF2, 0x48, 0x0F, 0x2C})); // cvtts?2si
            CHECK_FALSE(Contains(text, float32 ? Bytes{0xF3, 0x0F, 0x5E} : Bytes{0xF2, 0x0F, 0x5E}));       // divs?
        }
    }
}

TEST_CASE("x87 comparisons use fucomip with the SSE unordered patterns and pop both operands") {
    struct Case {
        LirOpcode op;
        void (X64Enc::*set)() const;
        bool unorderedTrue;
    };

    for (const Case comparison :
         {Case{LirOpcode::CmpEq, &X64Enc::SeteAl, false}, Case{LirOpcode::CmpNe, &X64Enc::SetneAl, true},
          Case{LirOpcode::CmpLt, &X64Enc::SetbAl, false}, Case{LirOpcode::CmpLe, &X64Enc::SetbeAl, false},
          Case{LirOpcode::CmpGt, &X64Enc::SetaAl, false}, Case{LirOpcode::CmpGe, &X64Enc::SetaeAl, false}}) {
        for (const Target::OS os : Systems) {
            CAPTURE(static_cast<int>(comparison.op));
            CAPTURE(static_cast<int>(os));
            const LirFunc function = SingleInstruction(comparison.op, {Extended, Extended}, TypeRef::MakeBool());
            const Frame frame(function, os);
            // A bool result still gets scratch: its operands are float80.
            CHECK_EQ(frame.plan.WideTemporarySize(), 16);
            const Bytes expected = Encode([&](const X64Enc &e) {
                e.FldTbyte(frame.Slot(1));
                e.FldTbyte(frame.Slot(0));
                e.FucomipSt(1);
                e.FstpSt(0);
                (e.*comparison.set)();
                comparison.unorderedTrue ? e.SetpDl() : e.SetnpDl();
                comparison.unorderedTrue ? e.OrAlDl() : e.AndAlDl();
                e.MovzxRaxAl();
            });
            CHECK(Contains(TextOf(function, os), expected));
        }
    }
}

TEST_CASE("x87 conversions widen machine floats exactly and round to them under the pinned control word") {
    const TypeRef float32 = Primitive(TypeRef::Kind::Float32);
    const TypeRef float64 = Primitive(TypeRef::Kind::Float64);
    for (const Target::OS os : Systems) {
        CAPTURE(static_cast<int>(os));
        const LirFunc widen64 = SingleInstruction(LirOpcode::Cast, {float64}, Extended);
        const Frame widen64Frame(widen64, os);
        const Bytes widen64Text = TextOf(widen64, os);
        CHECK(Contains(widen64Text, Encode([&](const X64Enc &e) {
                           e.FldQword(widen64Frame.Slot(0));
                           PopFloat80(e, widen64Frame.Slot(1));
                       })));
        CHECK_FALSE(Contains(widen64Text, Encode([&](const X64Enc &e) { e.Fnstcw(widen64Frame.ControlWords()); })));

        const LirFunc widen32 = SingleInstruction(LirOpcode::Cast, {float32}, Extended);
        const Frame widen32Frame(widen32, os);
        CHECK(Contains(TextOf(widen32, os), Encode([&](const X64Enc &e) {
                           e.FldDword(widen32Frame.Slot(0));
                           PopFloat80(e, widen32Frame.Slot(1));
                       })));

        const LirFunc narrow64 = SingleInstruction(LirOpcode::Cast, {Extended}, float64);
        const Frame narrow64Frame(narrow64, os);
        CHECK_EQ(narrow64Frame.plan.WideTemporarySize(), 16);
        CHECK(Contains(TextOf(narrow64, os), Encode([&](const X64Enc &e) {
                           BeginRounding(e, narrow64Frame.ControlWords());
                           e.FldTbyte(narrow64Frame.Slot(0));
                           e.FstpQword(narrow64Frame.Slot(1));
                           e.Fldcw(narrow64Frame.ControlWords());
                       })));

        const LirFunc narrow32 = SingleInstruction(LirOpcode::Cast, {Extended}, float32);
        const Frame narrow32Frame(narrow32, os);
        CHECK(Contains(TextOf(narrow32, os), Encode([&](const X64Enc &e) {
                           BeginRounding(e, narrow32Frame.ControlWords());
                           e.FldTbyte(narrow32Frame.Slot(0));
                           e.FstpDword(narrow32Frame.Slot(1));
                           e.Fldcw(narrow32Frame.ControlWords());
                       })));
    }
}

TEST_CASE("x87 converts narrow signed integers through a sign-extended 64-bit spill") {
    for (const Target::OS os : Systems) {
        CAPTURE(static_cast<int>(os));
        const LirFunc function = SingleInstruction(LirOpcode::Cast, {TypeRef::MakeInt16()}, Extended);
        const Frame frame(function, os);
        const auto physical = frame.plan.PhysicalRegisters().find(0);
        const Bytes expected = Encode([&](const X64Enc &e) {
            if (physical != frame.plan.PhysicalRegisters().end()) {
                e.MovRaxPhysReg(physical->second);
                e.MovsxRaxAx();
            }
            else {
                e.MovsxRaxWord(frame.Slot(0));
            }
            e.MovRaxStore(frame.Spill());
            e.FildQword(frame.Spill());
            PopFloat80(e, frame.Slot(1));
        });
        CHECK(Contains(TextOf(function, os), expected));
    }
}

namespace {
const FloatFormat &FormatOf(const std::uint32_t bits) {
    return *FindFloatFormat(bits);
}

[[nodiscard]] FloatEncoding Parse(const std::string_view text, const std::uint32_t bits = 80) {
    return *ParseFloatEncoding(text, FormatOf(bits));
}

/// A straight-line `Main` returning a bitmask of failed checks. Each float check stores the value through an alloca
/// and reloads its storage as an unsigned integer, so it compares every byte, float80 padding included, against the
/// bits `SoftwareFloat` folds.
class X87Program {
public:
    [[nodiscard]] LirReg Constant(const TypeRef &type, std::string text) {
        LirInstr instruction;
        instruction.op = LirOpcode::Const;
        instruction.type = type;
        instruction.strArg = std::move(text);
        return Add(std::move(instruction));
    }

    [[nodiscard]] LirReg Float80Constant(const FloatEncoding &value) {
        return Constant(Extended, FormatFloatEncoding(value));
    }

    [[nodiscard]] LirReg Operation(const LirOpcode op, const std::vector<LirReg> &sources, const TypeRef &type) {
        LirInstr instruction;
        instruction.op = op;
        instruction.type = type;
        instruction.srcs = sources;
        return Add(std::move(instruction));
    }

    void ExpectBits(const LirReg value, const TypeRef &type, const FloatEncoding &expected) {
        const std::uint32_t bytes = expected.Format().storageBytes;
        const TypeRef storage = bytes == 16 ? Primitive(TypeRef::Kind::UInt128)
                              : bytes == 8  ? TypeRef::MakeUInt64()
                                            : TypeRef::MakeUInt32();
        const std::uint64_t high = bytes == 16 ? expected.Bits().Word64(1) : 0;
        const std::uint64_t words[] = {expected.Bits().Word64(0), high};
        const std::string text = WideInteger::FromWords(std::span(words, bytes == 16 ? 2 : 1), bytes * 8).ToDecimal();

        LirInstr alloca;
        alloca.op = LirOpcode::Alloca;
        alloca.type = type;
        const LirReg address = Add(std::move(alloca));
        LirInstr store;
        store.op = LirOpcode::Store;
        store.type = type;
        store.srcs = {value, address};
        instructions.push_back(std::move(store));
        LirInstr load;
        load.op = LirOpcode::Load;
        load.type = storage;
        load.srcs = {address};
        const LirReg loaded = Add(std::move(load));
        Record(Operation(LirOpcode::CmpEq, {loaded, Constant(storage, text)}, TypeRef::MakeBool()));
    }

    void Expect(const LirReg condition, const bool expected) {
        Record(expected ? condition : Operation(LirOpcode::Not, {condition}, TypeRef::MakeBool()));
    }

    [[nodiscard]] LirFunc Finish() {
        LirBlock block;
        block.label = "entry";
        block.instrs = std::move(instructions);
        block.term.emplace();
        block.term->kind = LirTermKind::Return;
        block.term->retVal = failures;
        block.term->retType = TypeRef::MakeInt32();
        LirFunc function;
        function.name = "Main";
        function.isPublic = true;
        function.returnType = TypeRef::MakeInt32();
        function.blocks.push_back(std::move(block));
        return function;
    }

    [[nodiscard]] std::uint32_t Checks() const {
        return checks;
    }

private:
    LirReg Add(LirInstr instruction) {
        instruction.dst = next++;
        instructions.push_back(std::move(instruction));
        return instructions.back().dst;
    }

    void Record(const LirReg matches) {
        if (failures == LirNoReg) {
            failures = Constant(TypeRef::MakeInt32(), "0");
        }
        const LirReg mismatch = Operation(LirOpcode::Not, {matches}, TypeRef::MakeBool());
        const LirReg code = Operation(LirOpcode::Cast, {mismatch}, TypeRef::MakeInt32());
        const LirReg weight = Constant(TypeRef::MakeInt32(), std::to_string(1U << checks++));
        failures =
            Operation(LirOpcode::Add, {failures, Operation(LirOpcode::Mul, {code, weight}, TypeRef::MakeInt32())},
                      TypeRef::MakeInt32());
    }

    std::vector<LirInstr> instructions;
    LirReg next = 0;
    LirReg failures = LirNoReg;
    std::uint32_t checks = 0;
};

[[nodiscard]] LirFunc X87Checks() {
    const TypeRef float32 = Primitive(TypeRef::Kind::Float32);
    const TypeRef float64 = Primitive(TypeRef::Kind::Float64);
    X87Program program;
    const auto value = [&](const std::string_view text) { return program.Float80Constant(Parse(text)); };
    const auto binary = [&](const LirOpcode op, const LirReg left, const LirReg right) {
        return program.Operation(op, {left, right}, Extended);
    };

    // Exact only at 64-bit precision: the Win64 default of 53 bits would round both to 1.
    const LirReg one = value("1");
    program.ExpectBits(binary(LirOpcode::Add, one, value("0x1p-63")), Extended, Parse("0x1.0000000000000002p0"));
    program.ExpectBits(binary(LirOpcode::Sub, one, value("0x1p-64")), Extended, Parse("0x1.fffffffffffffffep-1"));
    // A tie at 64-bit precision rounds to even.
    program.ExpectBits(binary(LirOpcode::Add, one, value("0x1p-64")), Extended, Parse("1"));
    program.ExpectBits(binary(LirOpcode::Add, value("0x1.0000000000000002p0"), value("0x1p-64")), Extended,
                       Parse("0x1.0000000000000004p0"));

    const LirReg three = value("3");
    const LirReg third = binary(LirOpcode::Div, one, three);
    program.ExpectBits(third, Extended, DivideFloat(Parse("1"), Parse("3")));
    program.ExpectBits(binary(LirOpcode::Mul, value("1.1"), value("1.1")), Extended,
                       MultiplyFloat(Parse("1.1"), Parse("1.1")));
    // Gradual underflow and overflow.
    program.ExpectBits(binary(LirOpcode::Div, value("0x1p-16445"), value("2")), Extended, Parse("0"));
    program.ExpectBits(binary(LirOpcode::Mul, value("0x1.fffffffffffffffep16383"), value("2")), Extended,
                       FloatEncoding::Infinity(FormatOf(80)));
    // fprem needs many partial steps for a 2^1000 exponent gap; the remainder keeps the dividend's sign.
    program.ExpectBits(binary(LirOpcode::Mod, value("0x1p1000"), three), Extended,
                       RemainderFloat(Parse("0x1p1000"), Parse("3")));
    program.ExpectBits(binary(LirOpcode::Mod, value("-7.5"), value("2")), Extended, Parse("-1.5"));
    program.ExpectBits(binary(LirOpcode::Mod, value("5"), value("infinity")), Extended, Parse("5"));

    program.ExpectBits(program.Operation(LirOpcode::Cast, {program.Constant(float64, "0.1")}, Extended), Extended,
                       ConvertFloat(Parse("0.1", 64), FormatOf(80)));
    program.ExpectBits(program.Operation(LirOpcode::Cast, {program.Constant(float32, "0.1")}, Extended), Extended,
                       ConvertFloat(Parse("0.1", 32), FormatOf(80)));
    program.ExpectBits(program.Operation(LirOpcode::Cast, {third}, float64), float64,
                       ConvertFloat(DivideFloat(Parse("1"), Parse("3")), FormatOf(64)));
    program.ExpectBits(program.Operation(LirOpcode::Cast, {third}, float32), float32,
                       ConvertFloat(DivideFloat(Parse("1"), Parse("3")), FormatOf(32)));
    // Halfway between two float64 values: ties to even, rounded once.
    program.ExpectBits(program.Operation(LirOpcode::Cast, {value("0x1.00000000000008p0")}, float64), float64,
                       Parse("1", 64));

    const auto fromInteger = [&](const TypeRef &type, const std::string &text, const std::int64_t integer) {
        const LirReg converted = program.Operation(LirOpcode::Cast, {program.Constant(type, text)}, Extended);
        program.ExpectBits(
            converted, Extended,
            IntegerToFloat(WideInteger::FromUnsigned(static_cast<std::uint64_t>(integer), 64), true, FormatOf(80)));
    };
    fromInteger(TypeRef::MakeInt8(), "-128", -128);
    fromInteger(TypeRef::MakeInt16(), "-12345", -12345);
    fromInteger(TypeRef::MakeInt32(), "-5", -5);
    fromInteger(TypeRef::MakeInt64(), "-9223372036854775807", -9223372036854775807);

    const LirReg nan = value("nan");
    const auto compare = [&](const LirOpcode op, const LirReg left, const LirReg right) {
        return program.Operation(op, {left, right}, TypeRef::MakeBool());
    };
    program.Expect(compare(LirOpcode::CmpLt, one, three), true);
    program.Expect(compare(LirOpcode::CmpGt, one, three), false);
    program.Expect(compare(LirOpcode::CmpGe, three, three), true);
    program.Expect(compare(LirOpcode::CmpEq, value("0"), value("-0")), true);
    program.Expect(compare(LirOpcode::CmpLe, nan, nan), false);
    program.Expect(compare(LirOpcode::CmpEq, nan, nan), false);
    program.Expect(compare(LirOpcode::CmpNe, nan, one), true);
    program.Expect(compare(LirOpcode::CmpNe, three, three), false);
    REQUIRE_LE(program.Checks(), 31);
    return program.Finish();
}

/// float32 and float64 remainders on the fprem loop, each against the bits `RemainderFloat` folds: quotients past
/// 2^31 and 2^63, where the truncated-quotient formula overflowed its integer, ones below that it rounded, signed
/// zeros, an infinite divisor, subnormals and the widest exponent gaps.
[[nodiscard]] LirFunc RemainderChecks() {
    X87Program program;
    const auto check = [&](const std::uint32_t bits, const std::string_view left, const std::string_view right) {
        const TypeRef type = Primitive(bits == 32 ? TypeRef::Kind::Float32 : TypeRef::Kind::Float64);
        const FloatEncoding dividend = Parse(left, bits);
        const FloatEncoding divisor = Parse(right, bits);
        const LirReg remainder = program.Operation(LirOpcode::Mod,
                                                   {program.Constant(type, FormatFloatEncoding(dividend)),
                                                    program.Constant(type, FormatFloatEncoding(divisor))},
                                                   type);
        program.ExpectBits(remainder, type, RemainderFloat(dividend, divisor));
    };
    for (const std::uint32_t bits : {32U, 64U}) {
        check(bits, "30000000000", "7");
        check(bits, "1e20", "3");
        check(bits, "-7.5", "2");
        check(bits, "-6", "3");
        check(bits, "1", "0.1");
        check(bits, "0.1", "0.01");
        check(bits, "-5.25", "infinity");
        check(bits, "0x1p-140", "3");
    }
    check(32, "0x1.fffffep127", "3");
    check(32, "0x1.fffffep127", "0x1.4p-147");
    check(32, "0x1p-126", "0x1.4p-147");
    check(64, "0x1.fffffffffffffp1023", "3");
    check(64, "1e308", "1e-308");
    check(64, "0x1.fffffffffffffp1023", "0x1.8p-1073");
    check(64, "0x1p-1050", "0x1.8p-1062");
    REQUIRE_LE(program.Checks(), 31);
    return program.Finish();
}

/// Emits `main` for both conventions and, on a Windows x86-64 host, links and runs it: each set bit of the exit code
/// names a failed check, in the order the function records them.
void CheckRuns(const LirFunc &main) {
    const LirPackage package = PackageOf({main});
    RcuEmitter sysvEmitter(package, "X87", Target::OS::Linux);
    REQUIRE_EQ(sysvEmitter.Generate().size(), 1);
    REQUIRE(sysvEmitter.Diagnostics().empty());

    RcuEmitter emitter(package, "X87", Target::OS::Windows);
    auto objects = emitter.Generate();
    REQUIRE(emitter.Diagnostics().empty());
    REQUIRE_EQ(objects.size(), 1);

#if RUX_OS_WINDOWS && RUX_ARCH_X86_64
    static std::atomic_uint64_t sequence = 0;
    const std::filesystem::path output =
        Rux::Testing::TestTemporaryDirectory() / ("rux-x87-" + std::to_string(++sequence) + ".exe");
    std::error_code error;
    std::filesystem::remove(output, error);
    Linker linker(std::move(objects), "X87", {}, ArtifactKind::Executable, Target::OS::Windows, Target::Arch::X86_64);
    const bool linked = linker.Link(output);
    const std::string linkMessage = linker.Errors().empty() ? "link failed" : linker.Errors().front().message;
    REQUIRE_MESSAGE(linked, linkMessage);
    const auto result = System::RunCaptured(output);
    std::filesystem::remove(output, error);
    REQUIRE(result.has_value());
    CHECK_EQ(result->exitCode, 0);
#else
    CHECK_FALSE(objects.front().sections.at(RCU_TEXT_IDX).data.empty());
#endif
}
} // namespace

TEST_CASE("x86-64 RCU executes float80 arithmetic, comparisons and conversions on the x87 unit") {
    CheckRuns(X87Checks());
}

TEST_CASE("x86-64 RCU executes float32 and float64 remainders exactly, as constant folding computes them") {
    CheckRuns(RemainderChecks());
}

// ---- assembly text -------------------------------------------------------------------------------------------------

namespace {
/// One function holding every x87 form the lowering emits: the four rounding operations, the fprem loop, an ordered
/// and the unordered-true comparison, both widening conversions and both narrowing ones.
[[nodiscard]] LirFunc X87TextFunction() {
    const TypeRef float32 = Primitive(TypeRef::Kind::Float32);
    const TypeRef float64 = Primitive(TypeRef::Kind::Float64);
    LirFunc function;
    function.name = "X87Text";
    function.isPublic = true;
    function.returnType = float64;
    function.params = {
        {0, Extended, "left"}, {1, Extended, "right"}, {2, float64, "wide"}, {3, TypeRef::MakeInt16(), "small"}};
    const auto instruction = [](const LirOpcode op, const LirReg dst, const TypeRef &type, std::vector<LirReg> srcs) {
        LirInstr result;
        result.op = op;
        result.dst = dst;
        result.type = type;
        result.srcs = std::move(srcs);
        return result;
    };
    LirBlock block;
    block.label = "entry";
    block.instrs = {
        instruction(LirOpcode::Add, 4, Extended, {0, 1}),
        instruction(LirOpcode::Sub, 5, Extended, {0, 1}),
        instruction(LirOpcode::Mul, 6, Extended, {0, 1}),
        instruction(LirOpcode::Div, 7, Extended, {0, 1}),
        instruction(LirOpcode::Mod, 8, Extended, {0, 1}),
        instruction(LirOpcode::CmpLt, 9, TypeRef::MakeBool(), {0, 1}),
        instruction(LirOpcode::CmpNe, 10, TypeRef::MakeBool(), {0, 1}),
        instruction(LirOpcode::Cast, 11, Extended, {2}),
        instruction(LirOpcode::Cast, 12, float64, {4}),
        instruction(LirOpcode::Cast, 13, float32, {4}),
        instruction(LirOpcode::Cast, 14, Extended, {3}),
    };
    block.term.emplace();
    block.term->kind = LirTermKind::Return;
    block.term->retVal = 12;
    block.term->retType = float64;
    function.blocks.push_back(std::move(block));
    return function;
}

/// The lines as the module printer writes them: indented four spaces, one per line.
[[nodiscard]] std::string Lines(const std::vector<std::string> &lines) {
    std::string text;
    for (const auto &line : lines) {
        text += "    " + line + "\n";
    }
    return text;
}

void Append(std::vector<std::string> &lines, std::vector<std::string> more) {
    for (auto &line : more) {
        lines.push_back(std::move(line));
    }
}
} // namespace

TEST_CASE("x86-64 assembly text spells the x87 lowering on the frame addresses the object code uses") {
    for (const Target::OS os : Systems) {
        CAPTURE(static_cast<int>(os));
        const LirFunc function = X87TextFunction();
        const std::string text = AssemblyPrinter(PackageOf({function}), os).Generate();
        const X86_64FramePlan plan = PlanX86_64Frame(function, {}, {}, os);
        const auto slot = [&](const LirReg reg) { return plan.SlotOffsets().at(reg); };
        const std::int32_t controlWords = plan.WideTemporaryOffset(0);
        const std::int32_t spill = plan.WideTemporaryOffset(1);
        const auto contains = [&](const std::string &expected) {
            CAPTURE(expected);
            CHECK(text.find(expected) != std::string::npos);
        };
        const std::vector<std::string> begin = {std::format("fnstcw  word [rbp - {}]", controlWords),
                                                std::format("mov     word [rbp - {}], 0x037F", controlWords - 2),
                                                std::format("fldcw   word [rbp - {}]", controlWords - 2)};
        const std::string end = std::format("fldcw   word [rbp - {}]", controlWords);
        const auto popFloat80 = [&](const LirReg reg) {
            return std::vector<std::string>{std::format("fstp    tword [rbp - {}]", slot(reg)),
                                            std::format("mov     word [rbp - {}], 0", slot(reg) - 10),
                                            std::format("mov     dword [rbp - {}], 0", slot(reg) - 12)};
        };

        // Each rounding operation: pin the control word, push left then right, combine into st1 and pop, store the
        // result with its padding zeroed, restore the caller's word.
        const std::pair<LirReg, std::string_view> arithmetic[] = {
            {4, "faddp"}, {5, "fsubp"}, {6, "fmulp"}, {7, "fdivp"}};
        for (const auto &[result, opcode] : arithmetic) {
            CAPTURE(opcode);
            std::vector<std::string> lines = begin;
            lines.push_back(std::format("fld     tword [rbp - {}]", slot(0)));
            lines.push_back(std::format("fld     tword [rbp - {}]", slot(1)));
            lines.push_back(std::format("{:<8}st1, st0", opcode));
            Append(lines, popFloat80(result));
            lines.push_back(end);
            contains(Lines(lines));
        }

        // The remainder pushes the divisor first, loops on C2 and drops the divisor. It is exact, so it never touches
        // the control word.
        const std::string loopStart =
            Lines({std::format("fld     tword [rbp - {}]", slot(1)), std::format("fld     tword [rbp - {}]", slot(0))});
        const std::size_t remainder = text.find(loopStart);
        REQUIRE(remainder != std::string::npos);
        const std::size_t label = remainder + loopStart.size();
        const std::size_t colon = text.find(":\n", label);
        REQUIRE(colon != std::string::npos);
        const std::string loop = text.substr(label, colon - label);
        CHECK(loop.starts_with(".x87_fprem_"));
        std::vector<std::string> loopBody = {"fprem", "fnstsw  ax", "test    ah, 4", "jnz     " + loop, "fstp    st1"};
        Append(loopBody, popFloat80(8));
        CHECK_EQ(text.compare(colon + 2, Lines(loopBody).size(), Lines(loopBody)), 0);
        CHECK(text.find(Lines(loopBody) + Lines({end})) == std::string::npos);

        // Comparisons push right then left so st0 holds the left operand, pop both, and read the flags as UCOMISD's.
        const std::vector<std::string> compare = {std::format("fld     tword [rbp - {}]", slot(1)),
                                                  std::format("fld     tword [rbp - {}]", slot(0)), "fucomip st0, st1",
                                                  "fstp    st0"};
        std::vector<std::string> less = compare;
        Append(less, {"setb    al", "setnp   dl", "and     al, dl", "movzx   rax, al"});
        contains(Lines(less));
        std::vector<std::string> notEqual = compare;
        Append(notEqual, {"setne   al", "setp    dl", "or      al, dl", "movzx   rax, al"});
        contains(Lines(notEqual));

        // Widening is exact and left unwrapped; narrowing rounds under the pinned word.
        std::vector<std::string> widen = {std::format("fld     qword [rbp - {}]", slot(2))};
        Append(widen, popFloat80(11));
        contains(Lines(widen));
        CHECK(text.find(Lines(begin) + Lines(widen)) == std::string::npos);

        const auto narrow = [&](const LirReg result, const std::string_view width) {
            std::vector<std::string> lines = begin;
            lines.push_back(std::format("fld     tword [rbp - {}]", slot(4)));
            const bool physical = plan.PhysicalRegisters().contains(result);
            lines.push_back(std::format("fstp    {} [rbp - {}]", width, physical ? spill : slot(result)));
            return Lines(lines);
        };
        contains(narrow(12, "qword"));
        contains(narrow(13, "dword"));

        // A narrow signed integer is sign-extended into the 64-bit spill slot `fild` reads.
        std::vector<std::string> integer = {std::format("mov     qword [rbp - {}], rax", spill),
                                            std::format("fild    qword [rbp - {}]", spill)};
        Append(integer, popFloat80(14));
        contains(Lines(integer));
        if (!plan.PhysicalRegisters().contains(3)) {
            contains(Lines({std::format("movsx   rax, word [rbp - {}]", slot(3))}) + Lines(integer));
        }

        // Nothing falls through to the SSE or integer spellings.
        CHECK(text.find("addsd") == std::string::npos);
        CHECK(text.find("ucomisd") == std::string::npos);
        CHECK(text.find("imul") == std::string::npos);
    }
}
