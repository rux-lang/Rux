#include "CodeGen/X86_64/FramePlan.h"
#include "CodeGen/X86_64/RcuEmitter.h"
#include "Linker/Linker.h"
#include "System/Process.h"
#include "Target/Platform.h"

#include <atomic>
#include <doctest.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Rux;

namespace {
class WideProgram {
public:
    explicit WideProgram(const TypeRef inputType)
        : type(inputType) {
        failures = Constant(TypeRef::MakeInt32(), "0");
    }

    [[nodiscard]] LirReg Constant(const TypeRef &constantType, std::string value) {
        LirInstr instruction;
        instruction.op = LirOpcode::Const;
        instruction.dst = next++;
        instruction.type = constantType;
        instruction.strArg = std::move(value);
        instructions.push_back(std::move(instruction));
        return next - 1;
    }

    [[nodiscard]] LirReg Binary(const LirOpcode op, const LirReg left, const LirReg right, const TypeRef &resultType) {
        LirInstr instruction;
        instruction.op = op;
        instruction.dst = next++;
        instruction.type = resultType;
        instruction.srcs = {left, right};
        instructions.push_back(std::move(instruction));
        return next - 1;
    }

    [[nodiscard]] LirReg Unary(const LirOpcode op, const LirReg value, const TypeRef &resultType) {
        LirInstr instruction;
        instruction.op = op;
        instruction.dst = next++;
        instruction.type = resultType;
        instruction.srcs = {value};
        instructions.push_back(std::move(instruction));
        return next - 1;
    }

    [[nodiscard]] LirReg Call(std::string name, const LirReg argument, const TypeRef &resultType) {
        LirInstr instruction;
        instruction.op = LirOpcode::Call;
        instruction.dst = next++;
        instruction.type = resultType;
        instruction.srcs = {argument};
        instruction.strArg = std::move(name);
        instructions.push_back(std::move(instruction));
        return next - 1;
    }

    void Expect(const LirReg actual, std::string expected) {
        const LirReg expectedValue = Constant(type, std::move(expected));
        const LirReg matches = Binary(LirOpcode::CmpEq, actual, expectedValue, TypeRef::MakeBool());
        Record(matches);
    }

    void ExpectTrue(const LirReg actual) {
        Record(actual);
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

    TypeRef type;

private:
    void Record(const LirReg matches) {
        const LirReg mismatch = Unary(LirOpcode::Not, matches, TypeRef::MakeBool());
        const LirReg mismatchCode = Unary(LirOpcode::Cast, mismatch, TypeRef::MakeInt32());
        const LirReg weight = Constant(TypeRef::MakeInt32(), std::to_string(1U << checkIndex++));
        const LirReg weighted = Binary(LirOpcode::Mul, mismatchCode, weight, TypeRef::MakeInt32());
        failures = Binary(LirOpcode::Add, failures, weighted, TypeRef::MakeInt32());
    }

    std::vector<LirInstr> instructions;
    LirReg next = 0;
    LirReg failures = LirNoReg;
    std::uint32_t checkIndex = 0;
};

[[nodiscard]] LirPackage WideArithmeticPackage() {
    const TypeRef uint128 = TypeRef::MakePrimitive(TypeRef::Kind::UInt128);
    WideProgram program(uint128);
    const LirReg zero = program.Constant(uint128, "0");
    const LirReg one = program.Constant(uint128, "1");
    const LirReg maximum = program.Constant(uint128, "340282366920938463463374607431768211455");
    const LirReg ten = program.Constant(uint128, "10");

    program.Expect(program.Binary(LirOpcode::Add, maximum, one, uint128), "0");
    program.Expect(program.Binary(LirOpcode::Sub, zero, one, uint128), "340282366920938463463374607431768211455");
    program.Expect(program.Unary(LirOpcode::BitNot, zero, uint128), "340282366920938463463374607431768211455");
    program.Expect(program.Binary(LirOpcode::And, maximum, ten, uint128), "10");
    program.Expect(program.Binary(LirOpcode::Or, one, ten, uint128), "11");
    program.Expect(program.Binary(LirOpcode::Xor, maximum, ten, uint128), "340282366920938463463374607431768211445");

    const LirReg twoTo64PlusOne = program.Constant(uint128, "18446744073709551617");
    program.Expect(program.Binary(LirOpcode::Mul, twoTo64PlusOne, twoTo64PlusOne, uint128), "36893488147419103233");
    program.Expect(program.Binary(LirOpcode::Mul, maximum, maximum, uint128), "1");
    program.Expect(program.Binary(LirOpcode::Div, maximum, ten, uint128), "34028236692093846346337460743176821145");
    program.Expect(program.Binary(LirOpcode::Mod, maximum, ten, uint128), "5");

    const LirReg shift65 = program.Constant(TypeRef::MakeUInt64(), "65");
    const LirReg shift127 = program.Constant(TypeRef::MakeUInt64(), "127");
    const LirReg topBit = program.Constant(uint128, "170141183460469231731687303715884105728");
    program.Expect(program.Binary(LirOpcode::Shl, one, shift65, uint128), "36893488147419103232");
    program.Expect(program.Binary(LirOpcode::Lshr, topBit, shift127, uint128), "1");

    const LirReg three = program.Constant(uint128, "3");
    const LirReg twenty = program.Constant(uint128, "20");
    program.Expect(program.Binary(LirOpcode::Pow, three, twenty, uint128), "3486784401");
    program.ExpectTrue(program.Binary(LirOpcode::CmpGt, maximum, one, TypeRef::MakeBool()));

    const TypeRef int128 = TypeRef::MakePrimitive(TypeRef::Kind::Int128);
    program.type = int128;
    const LirReg negativeHundred = program.Constant(int128, "-100");
    const LirReg seven = program.Constant(int128, "7");
    program.Expect(program.Binary(LirOpcode::Div, negativeHundred, seven, int128), "-14");
    program.Expect(program.Binary(LirOpcode::Mod, negativeHundred, seven, int128), "-2");
    program.Expect(program.Unary(LirOpcode::Neg, negativeHundred, int128), "100");
    const LirReg minimum = program.Constant(int128, "-170141183460469231731687303715884105728");
    const LirReg negativeOne = program.Constant(int128, "-1");
    program.ExpectTrue(program.Binary(LirOpcode::CmpLt, minimum, negativeOne, TypeRef::MakeBool()));
    program.Expect(program.Binary(LirOpcode::Shr, minimum, shift127, int128), "-1");
    const LirReg negativeOne64 = program.Constant(TypeRef::MakeInt64(), "-1");
    program.Expect(program.Unary(LirOpcode::Cast, negativeOne64, int128), "-1");
    const LirReg narrowed = program.Unary(LirOpcode::Cast, negativeOne, TypeRef::MakeInt64());
    const LirReg expectedNarrow = program.Constant(TypeRef::MakeInt64(), "-1");
    program.ExpectTrue(program.Binary(LirOpcode::CmpEq, narrowed, expectedNarrow, TypeRef::MakeBool()));
    program.ExpectTrue(program.Unary(LirOpcode::Cast, negativeOne, TypeRef::MakeBool()));
    program.Expect(program.Call("EchoWide", negativeHundred, int128), "-100");

    const TypeRef uint512 = TypeRef::MakePrimitive(TypeRef::Kind::UInt512);
    constexpr std::string_view UInt512Maximum =
        "1340780792994259709957402499820584612747936582059239337772356144372176403007354697680187429816690342"
        "7690031858186486050853753882811946569946433649006084095";
    program.type = uint512;
    const LirReg maximum512 = program.Constant(uint512, std::string(UInt512Maximum));
    const LirReg one512 = program.Constant(uint512, "1");
    program.Expect(program.Binary(LirOpcode::Add, maximum512, one512, uint512), "0");
    program.Expect(program.Call("EchoWide512", maximum512, uint512), std::string(UInt512Maximum));

    LirFunc echo;
    echo.name = "EchoWide";
    echo.params.push_back({0, int128, "value"});
    echo.returnType = int128;
    LirBlock echoBlock;
    echoBlock.label = "entry";
    LirInstr echoAlloca;
    echoAlloca.op = LirOpcode::Alloca;
    echoAlloca.dst = 1;
    echoAlloca.type = int128;
    LirInstr echoStore;
    echoStore.op = LirOpcode::Store;
    echoStore.type = int128;
    echoStore.srcs = {0, 1};
    LirInstr echoLoad;
    echoLoad.op = LirOpcode::Load;
    echoLoad.dst = 2;
    echoLoad.type = int128;
    echoLoad.srcs = {1};
    echoBlock.instrs = {echoAlloca, echoStore, echoLoad};
    echoBlock.term.emplace();
    echoBlock.term->kind = LirTermKind::Return;
    echoBlock.term->retVal = 2;
    echoBlock.term->retType = int128;
    echo.blocks.push_back(std::move(echoBlock));

    LirFunc echo512;
    echo512.name = "EchoWide512";
    echo512.params.push_back({0, uint512, "value"});
    echo512.returnType = uint512;
    LirBlock echo512Block;
    echo512Block.label = "entry";
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

    LirModule module;
    module.name = "WideInteger.rux";
    module.funcs.push_back(std::move(echo));
    module.funcs.push_back(std::move(echo512));
    module.funcs.push_back(program.Finish());
    LirPackage package;
    package.modules.push_back(std::move(module));
    return package;
}
} // namespace

TEST_CASE("x86-64 frame plan gives 512-bit operations full-width homes and scratch") {
    const TypeRef uint512 = TypeRef::MakePrimitive(TypeRef::Kind::UInt512);
    LirFunc function;
    function.name = "Wide";
    function.returnType = uint512;
    LirBlock block;
    LirInstr left;
    left.op = LirOpcode::Const;
    left.dst = 0;
    left.type = uint512;
    left.strArg = "1";
    LirInstr right = left;
    right.dst = 1;
    LirInstr divide;
    divide.op = LirOpcode::Div;
    divide.dst = 2;
    divide.type = uint512;
    divide.srcs = {0, 1};
    block.instrs = {left, right, divide};
    block.term.emplace();
    block.term->kind = LirTermKind::Return;
    block.term->retVal = 2;
    block.term->retType = uint512;
    function.blocks.push_back(std::move(block));

    const X86_64FramePlan plan = PlanX86_64Frame(function, {}, {}, Target::OS::Linux);
    CHECK_EQ(plan.SlotOffsets().at(1) - plan.SlotOffsets().at(0), 64);
    CHECK_EQ(plan.SlotOffsets().at(2) - plan.SlotOffsets().at(1), 64);
    CHECK_EQ(plan.WideTemporarySize(), 64);
    CHECK_EQ(plan.WideTemporaryOffset(1) - plan.WideTemporaryOffset(0), 64);
    CHECK_EQ(plan.WideTemporaryOffset(2) - plan.WideTemporaryOffset(1), 64);
    CHECK_EQ(plan.FrameSize() % 16, 0);
}

TEST_CASE("x86-64 RCU executes wide integer arithmetic and comparisons") {
    const LirPackage package = WideArithmeticPackage();
    RcuEmitter sysvEmitter(package, "WideInteger", Target::OS::Linux);
    const auto sysvObjects = sysvEmitter.Generate();
    REQUIRE(sysvEmitter.Diagnostics().empty());
    REQUIRE_EQ(sysvObjects.size(), 1);

    RcuEmitter emitter(package, "WideInteger", Target::OS::Windows);
    auto objects = emitter.Generate();
    REQUIRE(emitter.Diagnostics().empty());
    REQUIRE_EQ(objects.size(), 1);

#if RUX_OS_WINDOWS && RUX_ARCH_X86_64
    static std::atomic_uint64_t sequence = 0;
    const std::filesystem::path output =
        std::filesystem::temp_directory_path() / ("rux-wide-x86-64-" + std::to_string(++sequence) + ".exe");
    std::error_code error;
    std::filesystem::remove(output, error);
    Linker linker(std::move(objects), "WideInteger", {}, ArtifactKind::Executable, Target::OS::Windows,
                  Target::Arch::X86_64);
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
