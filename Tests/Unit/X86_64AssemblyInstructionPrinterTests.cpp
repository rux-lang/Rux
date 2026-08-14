#include "CodeGen/X86_64/AssemblyInstructionPrinter.h"
#include "CodeGen/X86_64/AssemblyModulePrinter.h"

#include <doctest.h>
#include <format>
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
} // namespace

TEST_CASE("x86-64 assembly instruction printing owns setup scalar and memory text") {
    const TypeRef int64 = TypeRef::MakeInt64();
    LirFunc function;
    function.name = "ScalarMemory";
    function.callConv = CallingConvention::SysV;
    function.params = {{0, int64, "left"}, {1, int64, "right"}};

    LirInstr constant = Instruction(LirOpcode::Const, 2, int64);
    constant.strArg = "4";
    const LirInstr add = Instruction(LirOpcode::Add, 3, int64, {0, 1});
    const LirInstr alloca = Instruction(LirOpcode::Alloca, 4, int64);
    const LirInstr store = Instruction(LirOpcode::Store, LirNoReg, int64, {3, 4});
    const LirInstr load = Instruction(LirOpcode::Load, 5, int64, {4});
    const LirInstr cast = Instruction(LirOpcode::Cast, 6, TypeRef::MakeFloat64(), {5});
    LirInstr globalAddress = Instruction(LirOpcode::GlobalAddr, 7, int64);
    globalAddress.strArg = "GlobalValue";
    LirInstr stringAddress = Instruction(LirOpcode::StringAddr, 8, TypeRef::MakePointer(TypeRef::MakeChar8()));
    stringAddress.strArg = "ok";

    LirBlock block;
    block.instrs = {constant, add, alloca, store, load, cast, globalAddress, stringAddress};
    block.term.emplace();
    block.term->kind = LirTermKind::Return;
    function.blocks.push_back(std::move(block));

    const Layout::LayoutMap layouts;
    const std::unordered_set<std::string> interfaceNames;
    const X86_64FramePlan framePlan = PlanX86_64Frame(function, layouts, interfaceNames, Target::OS::Linux);
    AssemblyModulePrinter modulePrinter(Target::OS::Linux);
    AssemblyInstructionPrinter printer(modulePrinter, framePlan, layouts, interfaceNames, Target::OS::Linux);

    printer.EmitFunctionSetup(function);
    CHECK(printer.EmitMemory(constant));
    CHECK(printer.EmitArithmetic(add));
    CHECK(printer.EmitMemory(alloca));
    CHECK(printer.EmitMemory(store));
    CHECK(printer.EmitMemory(load));
    CHECK(printer.EmitArithmetic(cast));
    CHECK(printer.EmitMemory(globalAddress));
    CHECK(printer.EmitMemory(stringAddress));

    const LirInstr call = Instruction(LirOpcode::Call, LirNoReg, TypeRef::MakeOpaque());
    CHECK_FALSE(printer.EmitArithmetic(call));
    CHECK_FALSE(printer.EmitMemory(call));

    const std::string output = modulePrinter.Finalize();
    CHECK(output.find("    ; ── ScalarMemory ─\nScalarMemory:\n    push    rbp") != std::string::npos);
    CHECK(output.find(std::format("mov     qword [rbp - {}], rdi", framePlan.SlotOffsets().at(0))) !=
          std::string::npos);
    CHECK(output.find("add     rax, r10") != std::string::npos);
    CHECK(output.find("lea     rax, [rbp - ") != std::string::npos);
    CHECK(output.find("mov     qword [r11], rax") != std::string::npos);
    CHECK(output.find("cvtsi2sd  xmm0, rax") != std::string::npos);
    CHECK(output.find("lea     rax, [rel GlobalValue]") != std::string::npos);
    CHECK(output.find("db    111, 107, 0") != std::string::npos);
}
