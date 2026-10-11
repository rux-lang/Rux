// Register allocation over machine IR (`CodeGen/Machine/RegisterAllocator.h`).
//
// Each function is built by hand with a tiny opcode set and, after allocation, executed by an interpreter over a
// physical register file and frame memory. A wrong assignment, a lost spill or a clobbered value changes the result,
// so the checks hold for any coloring the allocator is free to choose.

#include "CodeGen/Machine/RegisterAllocator.h"

#include <bit>
#include <cstdint>
#include <doctest.h>
#include <map>
#include <vector>

using namespace Rux;
using namespace Rux::Machine;

namespace {
enum TestOp : std::uint16_t {
    MoveImmediate,
    Copy,
    AddTo,
    LoadSlot,
    StoreSlot,
    Clobber,
    Jump,
    BranchIfNonZero,
    Return,
};

constexpr Reg Result = 0;
constexpr Reg Argument = 1;

[[nodiscard]] TargetRegisters FourRegisters() {
    TargetRegisters target;
    // Register 0 carries the result and register 1 an argument; 0 to 3 are caller-saved and 4 to 5 callee-saved.
    target.allocationOrder[0] = {0, 1, 2, 3, 4, 5};
    for (const auto reg : target.allocationOrder[0])
        target.classMask[0] |= RegisterBit(reg);
    target.calleeSaved = RegisterBit(4) | RegisterBit(5);
    return target;
}

class Slots final : public SpillCodeFactory {
public:
    [[nodiscard]] Instruction Load(const Reg destination, RegClass, const std::int32_t frameObject) const override {
        Instruction instruction;
        instruction.opcode = LoadSlot;
        Address address;
        address.frameObject = frameObject;
        instruction.operands = {Operand::Def(destination), Operand::Mem(address)};
        return instruction;
    }

    [[nodiscard]] Instruction Store(const Reg source, RegClass, const std::int32_t frameObject) const override {
        Instruction instruction;
        instruction.opcode = StoreSlot;
        Address address;
        address.frameObject = frameObject;
        instruction.operands = {Operand::Mem(address), Operand::Use(source)};
        return instruction;
    }

    [[nodiscard]] std::int32_t SpillSize(RegClass) const override {
        return 8;
    }
};

[[nodiscard]] Instruction Make(const TestOp op, std::vector<Operand> operands) {
    Instruction instruction;
    instruction.opcode = op;
    instruction.operands = std::move(operands);
    instruction.isCopy = op == Copy;
    instruction.rematerializable = op == MoveImmediate;
    return instruction;
}

/// Runs an allocated function: every operand must name a physical register by now.
[[nodiscard]] std::int64_t Run(const Function &function, const std::int64_t argument) {
    std::map<Reg, std::int64_t> registers;
    std::map<std::int32_t, std::int64_t> slots;
    registers[Argument] = argument;
    std::uint32_t block = 0;
    for (std::size_t steps = 0; steps < 100000; ++steps) {
        bool jumped = false;
        for (const auto &instruction : function.blocks[block].instructions) {
            const auto &operands = instruction.operands;
            for (const auto &operand : operands)
                REQUIRE((operand.kind != Operand::Kind::Register || IsPhysical(operand.reg)));
            switch (instruction.opcode) {
            case MoveImmediate:
                registers[operands[0].reg] = operands[1].immediate;
                break;
            case Copy:
                registers[operands[0].reg] = registers[operands[1].reg];
                break;
            case AddTo:
                registers[operands[0].reg] += registers[operands[1].reg];
                break;
            case LoadSlot:
                registers[operands[0].reg] = slots[operands[1].address.frameObject];
                break;
            case StoreSlot:
                slots[operands[0].address.frameObject] = registers[operands[1].reg];
                break;
            case Clobber:
                for (std::uint64_t bits = instruction.implicitDefs; bits != 0; bits &= bits - 1)
                    registers[static_cast<Reg>(std::countr_zero(bits))] = -999;
                break;
            case Jump:
                block = operands[0].target;
                jumped = true;
                break;
            case BranchIfNonZero:
                if (registers[operands[0].reg] != 0) {
                    block = operands[1].target;
                    jumped = true;
                }
                break;
            case Return:
                return registers[Result];
            default:
                FAIL("unknown opcode");
            }
            if (jumped)
                break;
        }
        if (!jumped)
            FAIL("fell off a block");
    }
    FAIL("did not return");
    return 0;
}
} // namespace

TEST_CASE("machine allocation coalesces copies and keeps values across clobbers") {
    Function function;
    function.blocks.resize(1);
    const Reg value = function.NewVirtual(RegClass::General);
    const Reg copy = function.NewVirtual(RegClass::General);
    auto &instructions = function.blocks[0].instructions;
    instructions.push_back(Make(Copy, {Operand::Def(value), Operand::Use(Argument)}));
    instructions.push_back(Make(Copy, {Operand::Def(copy), Operand::Use(value)}));
    Instruction clobber = Make(Clobber, {});
    clobber.implicitDefs = RegisterBit(0) | RegisterBit(1) | RegisterBit(2) | RegisterBit(3);
    instructions.push_back(clobber);
    instructions.push_back(Make(Copy, {Operand::Def(Result), Operand::Use(copy)}));
    Instruction ret = Make(Return, {});
    ret.implicitUses = RegisterBit(Result);
    instructions.push_back(ret);

    const auto result = AllocateRegisters(function, FourRegisters(), Slots());
    REQUIRE(result.succeeded);
    CHECK(result.spilledRegisters == 0);
    // Live across the clobber, the value can only sit in a callee-saved register.
    CHECK((result.usedCalleeSaved & (RegisterBit(4) | RegisterBit(5))) != 0);
    CHECK(Run(function, 42) == 42);
    // The copy between the two virtual registers coalesced away.
    CHECK(function.blocks[0].instructions.size() <= 5);
}

TEST_CASE("machine allocation spills under pressure and still computes the same sum") {
    Function function;
    function.blocks.resize(1);
    auto &instructions = function.blocks[0].instructions;
    std::vector<Reg> values;
    for (int index = 1; index <= 12; ++index) {
        const Reg reg = function.NewVirtual(RegClass::General);
        values.push_back(reg);
        instructions.push_back(Make(MoveImmediate, {Operand::Def(reg), Operand::Imm(index * 1000)}));
    }
    // Every value is live at once here, three times what the register file holds.
    const Reg sum = function.NewVirtual(RegClass::General);
    instructions.push_back(Make(Copy, {Operand::Def(sum), Operand::Use(Argument)}));
    for (const auto reg : values)
        instructions.push_back(Make(AddTo, {Operand::UseDef(sum), Operand::Use(reg)}));
    for (const auto reg : values)
        instructions.push_back(Make(AddTo, {Operand::UseDef(sum), Operand::Use(reg)}));
    instructions.push_back(Make(Copy, {Operand::Def(Result), Operand::Use(sum)}));
    Instruction ret = Make(Return, {});
    ret.implicitUses = RegisterBit(Result);
    instructions.push_back(ret);

    const auto result = AllocateRegisters(function, FourRegisters(), Slots());
    REQUIRE(result.succeeded);
    CHECK(result.spilledRegisters > 0);
    CHECK(Run(function, 5) == 5 + 2 * 78000);
}

TEST_CASE("machine allocation keeps loop-carried values apart") {
    // counter = argument; total = 0; do { total += counter; counter += -1 } while (counter != 0); return total
    Function function;
    function.blocks.resize(3);
    const Reg counter = function.NewVirtual(RegClass::General);
    const Reg total = function.NewVirtual(RegClass::General);
    const Reg step = function.NewVirtual(RegClass::General);
    auto &entry = function.blocks[0];
    entry.instructions.push_back(Make(Copy, {Operand::Def(counter), Operand::Use(Argument)}));
    entry.instructions.push_back(Make(MoveImmediate, {Operand::Def(total), Operand::Imm(0)}));
    entry.instructions.push_back(Make(Jump, {Operand::BlockTarget(1)}));
    entry.successors = {1};
    auto &loop = function.blocks[1];
    loop.loopDepth = 1;
    loop.instructions.push_back(Make(AddTo, {Operand::UseDef(total), Operand::Use(counter)}));
    loop.instructions.push_back(Make(MoveImmediate, {Operand::Def(step), Operand::Imm(-1)}));
    loop.instructions.push_back(Make(AddTo, {Operand::UseDef(counter), Operand::Use(step)}));
    loop.instructions.push_back(Make(BranchIfNonZero, {Operand::Use(counter), Operand::BlockTarget(1)}));
    loop.instructions.push_back(Make(Jump, {Operand::BlockTarget(2)}));
    loop.successors = {1, 2};
    auto &exit = function.blocks[2];
    exit.instructions.push_back(Make(Copy, {Operand::Def(Result), Operand::Use(total)}));
    Instruction ret = Make(Return, {});
    ret.implicitUses = RegisterBit(Result);
    exit.instructions.push_back(ret);

    const auto result = AllocateRegisters(function, FourRegisters(), Slots());
    REQUIRE(result.succeeded);
    CHECK(result.spilledRegisters == 0);
    CHECK(Run(function, 10) == 55);
}
