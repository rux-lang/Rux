#include "CodeGen/AArch64/CallLayout.h"
#include "CodeGen/AArch64/Encoder.h"
#include "CodeGen/AArch64/FramePlan.h"
#include "CodeGen/AArch64/RcuEmitter.h"

#include <array>
#include <doctest.h>
#include <string>
#include <utility>
#include <vector>

using namespace Rux;

namespace {
[[nodiscard]] LirInstr Constant(const LirReg destination, const TypeRef &type, std::string value) {
    LirInstr instruction;
    instruction.op = LirOpcode::Const;
    instruction.dst = destination;
    instruction.type = type;
    instruction.strArg = std::move(value);
    return instruction;
}

[[nodiscard]] LirInstr Binary(const LirOpcode opcode, const LirReg destination, const TypeRef &type, const LirReg left,
                              const LirReg right) {
    LirInstr instruction;
    instruction.op = opcode;
    instruction.dst = destination;
    instruction.type = type;
    instruction.srcs = {left, right};
    return instruction;
}

[[nodiscard]] LirPackage WidePackage() {
    const TypeRef uint128 = TypeRef::MakePrimitive(TypeRef::Kind::UInt128);
    const TypeRef int128 = TypeRef::MakePrimitive(TypeRef::Kind::Int128);
    const TypeRef uint512 = TypeRef::MakePrimitive(TypeRef::Kind::UInt512);

    LirFunc echo128;
    echo128.name = "Echo128";
    echo128.params.push_back({0, uint128, "value"});
    echo128.returnType = uint128;
    LirBlock echo128Block;
    LirInstr echoAlloca;
    echoAlloca.op = LirOpcode::Alloca;
    echoAlloca.dst = 1;
    echoAlloca.type = uint128;
    LirInstr echoStore;
    echoStore.op = LirOpcode::Store;
    echoStore.type = uint128;
    echoStore.srcs = {0, 1};
    LirInstr echoLoad;
    echoLoad.op = LirOpcode::Load;
    echoLoad.dst = 2;
    echoLoad.type = uint128;
    echoLoad.srcs = {1};
    echo128Block.instrs = {echoAlloca, echoStore, echoLoad};
    echo128Block.term.emplace();
    echo128Block.term->kind = LirTermKind::Return;
    echo128Block.term->retVal = 2;
    echo128Block.term->retType = uint128;
    echo128.blocks.push_back(std::move(echo128Block));

    LirFunc echo512;
    echo512.name = "Echo512";
    echo512.params.push_back({0, uint512, "value"});
    echo512.returnType = uint512;
    LirBlock echo512Block;
    LirInstr echo512Alloca = echoAlloca;
    echo512Alloca.type = uint512;
    LirInstr echo512Store = echoStore;
    echo512Store.type = uint512;
    LirInstr echo512Load = echoLoad;
    echo512Load.type = uint512;
    echo512Block.instrs = {echo512Alloca, echo512Store, echo512Load};
    echo512Block.term.emplace();
    echo512Block.term->kind = LirTermKind::Return;
    echo512Block.term->retVal = 2;
    echo512Block.term->retType = uint512;
    echo512.blocks.push_back(std::move(echo512Block));

    LirFunc exercise;
    exercise.name = "Exercise";
    exercise.returnType = uint128;
    LirBlock block;
    block.instrs = {
        Constant(0, uint128, "340282366920938463463374607431768211455"),
        Constant(1, uint128, "10"),
        Constant(2, TypeRef::MakeUInt64(), "65"),
        Constant(3, int128, "-100"),
        Constant(4, int128, "7"),
        Constant(5, uint512,
                 "1340780792994259709957402499820584612747936582059239337772356144372176403007354697680187"
                 "4298166903427690031858186486050853753882811946569946433649006084095"),
        Constant(28, TypeRef::MakeInt64(), "-7"),
        Binary(LirOpcode::Add, 6, uint128, 0, 1),
        Binary(LirOpcode::Sub, 7, uint128, 0, 1),
        Binary(LirOpcode::Mul, 8, uint128, 0, 1),
        Binary(LirOpcode::Div, 9, int128, 3, 4),
        Binary(LirOpcode::Mod, 10, int128, 3, 4),
        Binary(LirOpcode::And, 12, uint128, 0, 1),
        Binary(LirOpcode::Or, 13, uint128, 0, 1),
        Binary(LirOpcode::Xor, 14, uint128, 0, 1),
        Binary(LirOpcode::Shl, 15, uint128, 1, 2),
        Binary(LirOpcode::Shr, 16, int128, 3, 2),
        Binary(LirOpcode::Lshr, 17, uint128, 0, 2),
        Binary(LirOpcode::CmpEq, 18, TypeRef::MakeBool(), 6, 7),
        Binary(LirOpcode::CmpLt, 19, TypeRef::MakeBool(), 3, 4),
        Binary(LirOpcode::CmpGe, 20, TypeRef::MakeBool(), 0, 1),
    };
    LirInstr negate;
    negate.op = LirOpcode::Neg;
    negate.dst = 21;
    negate.type = int128;
    negate.srcs = {3};
    block.instrs.push_back(negate);
    LirInstr complement = negate;
    complement.op = LirOpcode::BitNot;
    complement.dst = 22;
    complement.type = uint128;
    complement.srcs = {0};
    block.instrs.push_back(complement);
    LirInstr truth = complement;
    truth.op = LirOpcode::Not;
    truth.dst = 23;
    truth.type = uint128;
    block.instrs.push_back(truth);
    LirInstr widen;
    widen.op = LirOpcode::Cast;
    widen.dst = 24;
    widen.type = int128;
    widen.srcs = {28};
    block.instrs.push_back(widen);
    LirInstr narrow = widen;
    narrow.dst = 25;
    narrow.type = TypeRef::MakeInt64();
    narrow.srcs = {3};
    block.instrs.push_back(narrow);
    LirInstr call128;
    call128.op = LirOpcode::Call;
    call128.dst = 26;
    call128.type = uint128;
    call128.srcs = {0};
    call128.strArg = "Echo128";
    block.instrs.push_back(call128);
    LirInstr call512 = call128;
    call512.dst = 27;
    call512.type = uint512;
    call512.srcs = {5};
    call512.strArg = "Echo512";
    block.instrs.push_back(call512);
    block.term.emplace();
    block.term->kind = LirTermKind::Return;
    block.term->retVal = 26;
    block.term->retType = uint128;
    exercise.blocks.push_back(std::move(block));

    LirModule module;
    module.name = "WideAArch64.rux";
    module.funcs = {std::move(echo128), std::move(echo512), std::move(exercise)};
    LirPackage package;
    package.modules.push_back(std::move(module));
    return package;
}
} // namespace

TEST_CASE("AArch64 encoder carries across multiword arithmetic") {
    std::vector<std::uint8_t> bytes;
    A64Enc encoder(bytes);
    CHECK_EQ(encoder.Adc(A64::Xn(0), A64::Xn(1), A64::Xn(2)), A64Status::Ok);
    CHECK_EQ(encoder.Adcs(A64::Xn(0), A64::Xn(1), A64::Xn(2)), A64Status::Ok);
    CHECK_EQ(encoder.Sbc(A64::Xn(0), A64::Xn(1), A64::Xn(2)), A64Status::Ok);
    CHECK_EQ(encoder.Sbcs(A64::Xn(0), A64::Xn(1), A64::Xn(2)), A64Status::Ok);
    CHECK_EQ(encoder.WordAt(0), 0x9A020020);
    CHECK_EQ(encoder.WordAt(4), 0xBA020020);
    CHECK_EQ(encoder.WordAt(8), 0xDA020020);
    CHECK_EQ(encoder.WordAt(12), 0xFA020020);
    CHECK_EQ(encoder.Adc(A64::Sp, A64::Xn(1), A64::Xn(2)), A64Status::InvalidRegister);
}

TEST_CASE("AArch64 call layouts transport wide integers through aggregate ABI lanes") {
    const TypeRef uint128 = TypeRef::MakePrimitive(TypeRef::Kind::UInt128);
    const TypeRef uint512 = TypeRef::MakePrimitive(TypeRef::Kind::UInt512);
    for (const Target::OS os : {Target::OS::Linux, Target::OS::MacOS, Target::OS::Windows}) {
        const AArch64CallPlanner planner({}, {}, {}, os);
        const AArch64CallLayout layout = planner.PlanArguments({TypeRef::MakeUInt64(), uint128, uint512});
        REQUIRE_EQ(layout.args.size(), 3);
        CHECK_EQ(layout.args[1].kind, AArch64ArgumentLocation::Kind::General);
        CHECK_EQ(layout.args[1].first, os == Target::OS::MacOS ? 1 : 2);
        CHECK_EQ(layout.args[1].count, 2);
        CHECK(layout.args[2].byReference);
        CHECK_EQ(layout.args[2].first, os == Target::OS::MacOS ? 3 : 4);
        CHECK_EQ(layout.args[2].copyBytes, 64);
        CHECK_EQ(planner.PlanResult(uint128).first, 0);
        CHECK_EQ(planner.PlanResult(uint128).count, 2);
        CHECK(planner.ReturnsInMemory(uint512));
    }
}

TEST_CASE("AArch64 frame plan reserves full-width wide integer homes and scratch") {
    const TypeRef uint512 = TypeRef::MakePrimitive(TypeRef::Kind::UInt512);
    LirFunc function;
    function.name = "Divide";
    function.returnType = uint512;
    LirBlock block;
    block.instrs = {Constant(0, uint512, "100"), Constant(1, uint512, "7"), Binary(LirOpcode::Div, 2, uint512, 0, 1)};
    block.term.emplace();
    block.term->kind = LirTermKind::Return;
    block.term->retVal = 2;
    block.term->retType = uint512;
    function.blocks.push_back(std::move(block));
    const AArch64FramePlan plan = PlanAArch64Frame(function, {}, {}, {}, Target::OS::Linux);
    CHECK_EQ(plan.SlotOffsets().at(1) - plan.SlotOffsets().at(0), 64);
    CHECK_EQ(plan.SlotOffsets().at(2) - plan.SlotOffsets().at(1), 64);
    CHECK_EQ(plan.WideTemporarySize(), 64);
    CHECK_EQ(plan.WideTemporaryOffset(1) - plan.WideTemporaryOffset(0), 64);
    CHECK_EQ(plan.WideTemporaryOffset(2) - plan.WideTemporaryOffset(1), 64);
}

TEST_CASE("AArch64 RCU lowers every wide integer operation for each platform ABI") {
    const LirPackage package = WidePackage();
    for (const Target::OS os : {Target::OS::Linux, Target::OS::MacOS, Target::OS::Windows}) {
        AArch64RcuEmitter emitter(package, "WideAArch64", os);
        const auto objects = emitter.Generate();
        CHECK_MESSAGE(emitter.Diagnostics().empty(), (os == Target::OS::Windows ? "Windows" : "AAPCS64"));
        REQUIRE_EQ(objects.size(), 1);
        CHECK_FALSE(objects.front().sections.at(RCU_TEXT_IDX).data.empty());
    }
}
