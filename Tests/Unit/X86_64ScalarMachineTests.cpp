#include "CodeGen/X86_64/Encoder.h"
#include "CodeGen/X86_64/ScalarMachine.h"

#include <doctest.h>

using namespace Rux;

namespace {
LirFunc Arithmetic(const TypeRef &type, const LirOpcode operation, const bool keepLeft = true) {
    LirFunc function;
    function.name = "Arithmetic";
    function.params = {{0, type, "left"}, {1, type, "right"}};
    function.returnType = type;
    LirInstr instruction;
    instruction.op = operation;
    instruction.type = type;
    instruction.dst = 2;
    instruction.srcs = {0, 1};
    LirBlock block;
    block.label = "entry";
    block.instrs.push_back(instruction);
    if (keepLeft) {
        LirInstr use;
        use.op = LirOpcode::Cast;
        use.type = type;
        use.dst = 3;
        use.srcs = {0};
        block.instrs.push_back(use);
    }
    block.term.emplace();
    block.term->kind = LirTermKind::Return;
    block.term->retVal = 2;
    block.term->retType = type;
    function.blocks.push_back(std::move(block));
    return function;
}
} // namespace

TEST_CASE("Selected scalar integer operations encode architectural register operands") {
    const std::vector<std::pair<LirOpcode, std::vector<std::uint8_t>>> cases = {
        {LirOpcode::Add, {0x49, 0x89, 0xDD, 0x4D, 0x01, 0xE5}},
        {LirOpcode::Sub, {0x49, 0x89, 0xDD, 0x4D, 0x29, 0xE5}},
        {LirOpcode::Mul, {0x49, 0x89, 0xDD, 0x4D, 0x0F, 0xAF, 0xEC}},
        {LirOpcode::And, {0x49, 0x89, 0xDD, 0x4D, 0x21, 0xE5}},
        {LirOpcode::Or, {0x49, 0x89, 0xDD, 0x4D, 0x09, 0xE5}},
        {LirOpcode::Xor, {0x49, 0x89, 0xDD, 0x4D, 0x31, 0xE5}},
    };
    for (const auto &[operation, expected] : cases) {
        const auto function = Arithmetic(TypeRef::MakeUInt64(), operation);
        const auto plan = PlanX86_64Frame(function, {}, {}, Target::OS::Windows);
        const auto selected = SelectScalarMachineInstruction(function.blocks[0].instrs[0], plan);
        REQUIRE(selected.has_value());
        CHECK_EQ(selected->destination, 13);
        CHECK_EQ(selected->left, 3);
        CHECK_EQ(selected->right, 12);
        std::vector<std::uint8_t> bytes;
        X64Enc encoder(bytes);
        encoder.EmitScalarMachineInstruction(*selected);
        CHECK(bytes == expected);
    }
}

TEST_CASE("Selected scalar floating operations retain SSE width and operand order") {
    const std::vector<std::pair<LirOpcode, std::uint8_t>> operations = {
        {LirOpcode::Add, 0x58}, {LirOpcode::Sub, 0x5C}, {LirOpcode::Mul, 0x59}, {LirOpcode::Div, 0x5E}};
    for (const bool single : {false, true}) {
        for (const auto &[operation, opcode] : operations) {
            const auto function = Arithmetic(single ? TypeRef::MakeFloat32() : TypeRef::MakeFloat64(), operation);
            const auto plan = PlanX86_64Frame(function, {}, {}, Target::OS::Windows);
            const auto selected = SelectScalarMachineInstruction(function.blocks[0].instrs[0], plan);
            REQUIRE(selected.has_value());
            std::vector<std::uint8_t> bytes;
            X64Enc encoder(bytes);
            encoder.EmitScalarMachineInstruction(*selected);
            const std::vector<std::uint8_t> expected = {
                0x45, 0x0F, 0x28, 0xD0, static_cast<std::uint8_t>(single ? 0xF3 : 0xF2), 0x45, 0x0F, opcode, 0xD1};
            CHECK(bytes == expected);
        }
    }
}

TEST_CASE("Scalar machine selection retains narrow fixed-register and spilled fallbacks") {
    for (const auto &type : {TypeRef::MakeInt8(), TypeRef::MakeUInt32(), TypeRef::MakeInt64()}) {
        const auto function = Arithmetic(type, type == TypeRef::MakeInt64() ? LirOpcode::Div : LirOpcode::Add);
        const auto plan = PlanX86_64Frame(function, {}, {}, Target::OS::Windows);
        CHECK_FALSE(SelectScalarMachineInstruction(function.blocks[0].instrs[0], plan).has_value());
    }
    auto function = Arithmetic(TypeRef::MakeUInt64(), LirOpcode::Add);
    for (LirReg reg = 4; reg < 10; ++reg)
        function.params.push_back({reg, TypeRef::MakeUInt64(), "extra"});
    function.blocks[0].instrs[0].srcs[1] = 9;
    const auto plan = PlanX86_64Frame(function, {}, {}, Target::OS::Windows);
    CHECK_FALSE(SelectScalarMachineInstruction(function.blocks[0].instrs[0], plan).has_value());
}

TEST_CASE("x86 scalar arithmetic coalesces a dying left input without a physical copy") {
    for (const auto &type : {TypeRef::MakeUInt64(), TypeRef::MakeFloat32(), TypeRef::MakeFloat64()}) {
        const auto function = Arithmetic(type, LirOpcode::Sub, false);
        const auto plan = PlanX86_64Frame(function, {}, {}, Target::OS::Windows);
        const auto selected = SelectScalarMachineInstruction(function.blocks[0].instrs[0], plan);
        REQUIRE(selected.has_value());
        CHECK_EQ(selected->destination, selected->left);
        CHECK_NE(selected->destination, selected->right);
        std::vector<std::uint8_t> bytes;
        X64Enc encoder(bytes);
        encoder.EmitScalarMachineInstruction(*selected);
        CHECK_EQ(bytes.size(), type.IsFloat() ? 5 : 3);
    }
}
