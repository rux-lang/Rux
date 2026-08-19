#include "CodeGen/X86_64/AssemblyControlFlowPrinter.h"
#include "CodeGen/X86_64/AssemblyModulePrinter.h"

#include <doctest.h>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace Rux;

namespace {
[[nodiscard]] LirInstr Instruction(const LirOpcode opcode, const LirReg destination, const TypeRef &type,
                                   std::vector<LirReg> sources = {}) {
    LirInstr instruction;
    instruction.op = opcode;
    instruction.dst = destination;
    instruction.type = type;
    instruction.srcs = std::move(sources);
    return instruction;
}

[[nodiscard]] LirTerminator Return(const LirReg value, const TypeRef &type) {
    LirTerminator terminator;
    terminator.kind = LirTermKind::Return;
    terminator.retVal = value;
    terminator.retType = type;
    return terminator;
}
} // namespace

TEST_CASE("x86-64 assembly control-flow printing owns calls phi edges and terminators") {
    const TypeRef int64 = TypeRef::MakeInt64();
    LirFunc function;
    function.name = "Control";
    function.callConv = CallingConvention::SysV;
    function.params = {
        {0, TypeRef::MakeBool(), "condition"}, {1, int64, "argument"}, {2, TypeRef::MakePointer(int64), "callee"}};

    LirInstr directCall = Instruction(LirOpcode::Call, 3, int64, {1});
    directCall.strArg = "Direct";
    directCall.callConv = CallingConvention::SysV;
    LirInstr indirectCall = Instruction(LirOpcode::CallIndirect, 4, int64, {2, 3});
    indirectCall.callConv = CallingConvention::SysV;
    LirBlock entry;
    entry.label = "entry";
    entry.instrs = {directCall, indirectCall};
    entry.term.emplace();
    entry.term->kind = LirTermKind::Branch;
    entry.term->cond = 0;
    entry.term->trueTarget = 1;
    entry.term->falseTarget = 2;

    LirInstr trueValue = Instruction(LirOpcode::Const, 5, int64);
    trueValue.strArg = "10";
    LirBlock trueBlock;
    trueBlock.label = "true";
    trueBlock.instrs.push_back(std::move(trueValue));
    trueBlock.term.emplace();
    trueBlock.term->kind = LirTermKind::Jump;
    trueBlock.term->trueTarget = 3;

    LirInstr falseValue = Instruction(LirOpcode::Const, 6, int64);
    falseValue.strArg = "20";
    LirBlock falseBlock;
    falseBlock.label = "false";
    falseBlock.instrs.push_back(std::move(falseValue));
    falseBlock.term.emplace();
    falseBlock.term->kind = LirTermKind::Jump;
    falseBlock.term->trueTarget = 3;

    LirInstr phi = Instruction(LirOpcode::Phi, 7, int64);
    phi.phiPreds = {{5, 1}, {6, 2}};
    LirBlock join;
    join.label = "join";
    join.instrs.push_back(std::move(phi));
    join.term.emplace();
    join.term->kind = LirTermKind::Switch;
    join.term->cond = 7;
    join.term->cases.push_back({.value = "10", .target = 4});
    join.term->defaultTarget = 5;

    LirBlock returnBlock;
    returnBlock.label = "return";
    returnBlock.term = Return(7, int64);

    LirBlock unreachable;
    unreachable.label = "unreachable";
    unreachable.term.emplace();
    unreachable.term->kind = LirTermKind::Unreachable;

    function.blocks = {std::move(entry), std::move(trueBlock),   std::move(falseBlock),
                       std::move(join),  std::move(returnBlock), std::move(unreachable)};

    AssemblyModulePrinter modulePrinter(Target::OS::Linux);
    const Layout::LayoutMap layouts;
    const std::unordered_set<std::string> interfaceNames;
    AssemblyControlFlowPrinter printer(modulePrinter, layouts, interfaceNames, Target::OS::Linux);
    printer.EmitFunction(function);

    const std::string output = modulePrinter.Finalize();
    CHECK(output.find("mov     rdi, rax\n    call    Direct") != std::string::npos);
    CHECK(output.find("call    r10") != std::string::npos);
    CHECK(output.find("jz      .Control_false") != std::string::npos);
    CHECK(output.find("jmp     .Control_join") != std::string::npos);
    CHECK(output.find("cmp     rax, 10\n    je      .Control_return") != std::string::npos);
    CHECK(output.find("leave\n    ret") != std::string::npos);
    CHECK(output.find(".Control_unreachable:\n    ud2") != std::string::npos);
    CHECK(output.find("unsupported LIR opcode") == std::string::npos);
}

TEST_CASE("x86-64 assembly control-flow printing owns runtime failure paths") {
    LirFunc function;
    function.name = "Failures";
    function.callConv = CallingConvention::SysV;
    function.params = {{0, TypeRef::MakeBool(), "condition"},
                       {1, TypeRef::MakePointer(TypeRef::MakeNamed("Slice<char8>")), "message"}};

    LirInstr assertion = Instruction(LirOpcode::Assert, LirNoReg, TypeRef::MakeOpaque(), {0, 1});
    assertion.sourceFunction = "Failures";
    assertion.sourceFile = "failure.rux";
    assertion.sourceLine = 4;
    assertion.sourceColumn = 8;
    LirInstr panic = Instruction(LirOpcode::Panic, LirNoReg, TypeRef::MakeOpaque(), {1});
    panic.sourceFunction = assertion.sourceFunction;
    panic.sourceFile = assertion.sourceFile;
    panic.sourceLine = 5;
    panic.sourceColumn = 12;

    LirBlock block;
    block.instrs = {std::move(assertion), std::move(panic)};
    block.term.emplace();
    block.term->kind = LirTermKind::Unreachable;
    function.blocks.push_back(std::move(block));

    AssemblyModulePrinter modulePrinter(Target::OS::Linux);
    const Layout::LayoutMap layouts;
    const std::unordered_set<std::string> interfaceNames;
    AssemblyControlFlowPrinter printer(modulePrinter, layouts, interfaceNames, Target::OS::Linux);
    printer.EmitFunction(function);

    const std::string output = modulePrinter.Finalize();
    CHECK(output.find("Failures_assert_ok_") != std::string::npos);
    CHECK(output.find("Assertion failed: ") == std::string::npos);
    CHECK(output.find("db    65, 115, 115, 101, 114, 116, 105, 111, 110, 32, 102, 97, 105, 108, 101, 100, 58, 32") !=
          std::string::npos);
    CHECK(output.find("db    80, 97, 110, 105, 99, 58, 32") != std::string::npos);
    CHECK(output.find("syscall") != std::string::npos);
}

TEST_CASE("x86-64 assembly control-flow printing comments invalid LIR explicitly") {
    LirFunc function;
    function.name = "Invalid";
    function.callConv = CallingConvention::SysV;
    LirBlock block;
    block.instrs.push_back(Instruction(static_cast<LirOpcode>(999), LirNoReg, TypeRef::MakeOpaque()));
    function.blocks.push_back(std::move(block));

    AssemblyModulePrinter modulePrinter(Target::OS::Linux);
    const Layout::LayoutMap layouts;
    const std::unordered_set<std::string> interfaceNames;
    AssemblyControlFlowPrinter printer(modulePrinter, layouts, interfaceNames, Target::OS::Linux);
    printer.EmitFunction(function);

    const std::string output = modulePrinter.Finalize();
    CHECK(output.find("; unsupported LIR opcode ? (999)") != std::string::npos);
    CHECK(output.find("nop    ; missing terminator") != std::string::npos);
}

TEST_CASE("a Win64 float argument on the stack is placed without disturbing the register arguments") {
    // The first four arguments live in xmm0-xmm3 by the time the fifth is placed, so the fifth cannot be routed
    // through xmm0 on its way to the stack: doing that overwrote the first argument, and the callee saw one value
    // twice. The bits travel through rax instead, which is an argument register under neither convention.
    const TypeRef float32 = TypeRef::MakeFloat32();
    LirFunc function;
    function.name = "Place";
    function.callConv = CallingConvention::Win64;
    function.params = {{0, float32, "a"}, {1, float32, "b"}, {2, float32, "c"}, {3, float32, "d"}, {4, float32, "e"}};

    LirInstr call = Instruction(LirOpcode::Call, 5, float32, {0, 1, 2, 3, 4});
    call.strArg = "Callee";
    call.callConv = CallingConvention::Win64;
    LirBlock entry;
    entry.label = "entry";
    entry.instrs.push_back(std::move(call));
    entry.term = Return(5, float32);
    function.blocks = {std::move(entry)};

    AssemblyModulePrinter modulePrinter(Target::OS::Windows);
    const Layout::LayoutMap layouts;
    const std::unordered_set<std::string> interfaceNames;
    AssemblyControlFlowPrinter printer(modulePrinter, layouts, interfaceNames, Target::OS::Windows);
    printer.EmitFunction(function);

    const std::string output = modulePrinter.Finalize();
    const std::size_t callSite = output.find("call    Callee");
    REQUIRE(callSite != std::string::npos);
    // Once the last register argument is in place, nothing before the call may touch a vector register: the four of
    // them are the call's first four arguments and are already loaded.
    const std::size_t lastRegisterArgument = output.rfind("movss   xmm3,", callSite);
    REQUIRE(lastRegisterArgument != std::string::npos);
    const std::string placement = output.substr(lastRegisterArgument, callSite - lastRegisterArgument);
    CHECK_EQ(placement.find("xmm0"), std::string::npos);
    CHECK_EQ(placement.find("xmm1"), std::string::npos);
    CHECK_EQ(placement.find("xmm2"), std::string::npos);
    // The fifth argument reaches its slot as raw bits instead.
    CHECK(placement.find("mov     eax, dword") != std::string::npos);
    CHECK(placement.find("mov     qword [rsp + 32], rax") != std::string::npos);
}
