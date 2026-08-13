#include "Lowering/HirToLir/CheckedLirBuilder.h"

#include <doctest.h>

using namespace Rux;

TEST_CASE("Checked LIR builder rejects undefined register uses") {
    LirFunc function;
    CheckedLirBuilder builder(function);
    CHECK(builder.SelectBlock(builder.CreateBlock("entry")));

    const LirReg undefined = builder.AllocateRegister();
    LirInstr use;
    use.op = LirOpcode::Neg;
    use.dst = builder.AllocateRegister();
    use.srcs = {undefined};
    CHECK_FALSE(builder.Insert(use));
    CHECK(function.blocks[0].instrs.empty());

    LirInstr define;
    define.op = LirOpcode::Const;
    define.dst = undefined;
    define.strArg = "1";
    CHECK(builder.Insert(define));
    CHECK(builder.Insert(use));
    CHECK(function.blocks[0].instrs.size() == 2);

    CHECK_FALSE(builder.Insert(define));
    CHECK(function.blocks[0].instrs.size() == 2);
}

TEST_CASE("Checked LIR builder rejects a second terminator") {
    LirFunc function;
    CheckedLirBuilder builder(function);
    const auto entry = builder.CreateBlock("entry");
    const auto exit = builder.CreateBlock("exit");
    REQUIRE(builder.SelectBlock(entry));

    LirTerminator jump;
    jump.kind = LirTermKind::Jump;
    jump.trueTarget = exit;
    CHECK(builder.Terminate(jump));
    CHECK_FALSE(builder.Terminate(jump));

    LirInstr instruction;
    instruction.op = LirOpcode::Const;
    instruction.dst = builder.AllocateRegister();
    CHECK_FALSE(builder.Insert(instruction));
    REQUIRE(function.blocks[entry].term);
    CHECK(function.blocks[entry].term->trueTarget == exit);
}

TEST_CASE("Checked LIR builder defines parameters before use") {
    LirFunc function;
    CheckedLirBuilder builder(function);
    REQUIRE(builder.SelectBlock(builder.CreateBlock("entry")));
    const LirReg parameter = builder.DefineParameter();
    const LirReg result = builder.AllocateRegister();

    LirInstr instruction;
    instruction.op = LirOpcode::Neg;
    instruction.dst = result;
    instruction.srcs = {parameter};
    CHECK(builder.Insert(instruction));

    LirTerminator ret;
    ret.kind = LirTermKind::Return;
    ret.retVal = result;
    CHECK(builder.Terminate(ret));
}

TEST_CASE("Checked LIR opcode mapping has no arithmetic fallback") {
    CHECK(CheckedLirBuilder::BinaryOpcode(TokenKind::Plus) == LirOpcode::Add);
    CHECK(CheckedLirBuilder::CompoundOpcode(TokenKind::MinusAssign) == LirOpcode::Sub);
    CHECK_FALSE(CheckedLirBuilder::BinaryOpcode(TokenKind::Assign));
    CHECK_FALSE(CheckedLirBuilder::CompoundOpcode(TokenKind::Plus));
}
