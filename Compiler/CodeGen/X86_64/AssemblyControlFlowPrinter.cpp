#include "CodeGen/X86_64/AssemblyControlFlowPrinter.h"

#include "CodeGen/RuntimeFailure.h"
#include "CodeGen/X86_64/AssemblyInstructionPrinter.h"
#include "CodeGen/X86_64/AssemblyModulePrinter.h"

#include <format>
#include <vector>

namespace Rux {
using namespace Layout;

namespace {
class FunctionAssemblyPrinter {
public:
    FunctionAssemblyPrinter(AssemblyModulePrinter &modulePrinter, const LayoutMap &layouts,
                            const std::unordered_set<std::string> &interfaceNames, const Target::OS targetOs)
        : modulePrinter(modulePrinter)
        , layouts(layouts)
        , interfaceNames(interfaceNames)
        , targetOs(targetOs) {
    }

    void EmitFunction(const LirFunc &function) {
        currentFunction = function.name;
        const X86_64FramePlan framePlan = PlanX86_64Frame(function, layouts, interfaceNames, targetOs);
        AssemblyInstructionPrinter instructionPrinter(modulePrinter, framePlan, layouts, interfaceNames, targetOs);
        instructionPrinter.EmitFunctionSetup(function);

        for (std::uint32_t index = 0; index < function.blocks.size(); ++index) {
            GenBlock(index, function.blocks[index], function, instructionPrinter);
        }
        TB();
    }

private:
    AssemblyModulePrinter &modulePrinter;
    const LayoutMap &layouts;
    const std::unordered_set<std::string> &interfaceNames;
    Target::OS targetOs;
    std::string currentFunction;

    void TI(const std::string_view instruction) {
        modulePrinter.TextInstruction(instruction);
    }

    void TL(const std::string_view label) {
        modulePrinter.TextLabel(label);
    }

    void TC(const std::string_view comment) {
        modulePrinter.TextComment(comment);
    }

    void TB() {
        modulePrinter.TextBlank();
    }

    [[nodiscard]] std::string InternString(const std::string &value) {
        return modulePrinter.InternString(value);
    }

    void NeedExtern(const std::string &name) {
        modulePrinter.DeclareExtern(name);
    }

    [[nodiscard]] std::string BlockLabel(const std::uint32_t index, const std::string &label) const {
        if (index == 0) {
            return currentFunction;
        }
        return "." + currentFunction + "_" + label;
    }

    void EmitPhiMoves(std::uint32_t fromBlock, std::uint32_t toBlock, AssemblyInstructionPrinter &instructionPrinter) {
        const auto &phiMoves = instructionPrinter.FramePlan().PhiMoves();
        const auto from = phiMoves.find(fromBlock);
        if (from == phiMoves.end()) {
            return;
        }
        const auto to = from->second.find(toBlock);
        if (to == from->second.end()) {
            return;
        }

        const std::vector<PhiMoveStep> steps = ResolvePhiMoves(to->second);
        const std::int32_t temporaryOffset = instructionPrinter.FramePlan().PhiTemporaryOffset();
        for (const auto &step : steps) {
            if (step.kind == PhiMoveStep::Kind::SaveDestination) {
                const int size = instructionPrinter.SizeOfRuntime(step.type);
                instructionPrinter.LoadA(step.dst, step.type);
                if (size == 16) {
                    TI(std::format("{:<8}qword [rbp - {}], rax", "mov", temporaryOffset));
                    TI(std::format("{:<8}qword [rbp - {}], rdx", "mov", temporaryOffset - 8));
                }
                else if (IsFloat(step.type)) {
                    TI(std::format("{:<8}{} [rbp - {}], xmm0", size == 4 ? "movss" : "movsd",
                                   size == 4 ? "dword" : "qword", temporaryOffset));
                }
                else {
                    TI(std::format("{:<8}qword [rbp - {}], rax", "mov", temporaryOffset));
                }
            }
            else if (step.sourceIsTemporary) {
                const int size = instructionPrinter.SizeOfRuntime(step.type);
                if (size == 16) {
                    TI(std::format("{:<8}rax, qword [rbp - {}]", "mov", temporaryOffset));
                    TI(std::format("{:<8}rdx, qword [rbp - {}]", "mov", temporaryOffset - 8));
                }
                else if (IsFloat(step.type)) {
                    TI(std::format("{:<8}xmm0, {} [rbp - {}]", size == 4 ? "movss" : "movsd",
                                   size == 4 ? "dword" : "qword", temporaryOffset));
                }
                else {
                    TI(std::format("{:<8}rax, qword [rbp - {}]", "mov", temporaryOffset));
                }
                instructionPrinter.StoreA(step.dst, step.type);
            }
            else {
                instructionPrinter.LoadA(step.src, step.type);
                instructionPrinter.StoreA(step.dst, step.type);
            }
        }
    }

    void GenBlock(const std::uint32_t index, const LirBlock &block, const LirFunc &function,
                  AssemblyInstructionPrinter &instructionPrinter) {
        const std::string label = BlockLabel(index, block.label);
        if (index != 0) {
            TB();
            TL(label);
        }
        for (const auto &instruction : block.instrs) {
            GenInstruction(instruction, instructionPrinter);
        }
        if (block.term) {
            GenTerminator(index, *block.term, function, instructionPrinter);
        }
        else {
            TI("nop    ; missing terminator");
        }
    }

    void GenInstruction(const LirInstr &instruction, AssemblyInstructionPrinter &instructionPrinter) {
        if (instructionPrinter.EmitArithmetic(instruction) || instructionPrinter.EmitMemory(instruction)) {
            return;
        }

        switch (instruction.op) {
        case LirOpcode::Assert:
        case LirOpcode::Panic: {
            const bool isAssertion = instruction.op == LirOpcode::Assert;
            if (instruction.srcs.size() < (isAssertion ? 2 : 1)) {
                break;
            }
            std::string okLabel;
            if (isAssertion) {
                instructionPrinter.LoadA(instruction.srcs[0], TypeRef::MakeBool());
                TI("test    rax, rax");
                okLabel = modulePrinter.CreateLocalLabel(std::format(".{}_assert_ok_", currentFunction));
                TI(std::format("{:<8}{}", "jnz", okLabel));
            }

            const LirReg messageRegister = instruction.srcs[isAssertion ? 1 : 0];
            const RuntimeFailureLayout layout = BuildRuntimeFailureLayout(
                isAssertion ? RuntimeFailureKind::Assertion : RuntimeFailureKind::Panic, instruction.sourceFunction,
                instruction.sourceFile, instruction.sourceLine, instruction.sourceColumn);

            if (instructionPrinter.IsWin64Convention(CallingConvention::Default)) {
                NeedExtern("GetStdHandle");
                NeedExtern("WriteFile");
                TI("sub     rsp, 48");
                TI("mov     qword [rsp + 32], 0");

                const auto prepareWrite = [&]() {
                    TI("mov     ecx, -12");
                    TI("call    GetStdHandle");
                    TI("mov     rcx, rax");
                    TI("lea     r9, [rsp + 40]");
                };
                const auto writeStatic = [&](const std::string &value) {
                    prepareWrite();
                    const std::string label = InternString(value);
                    TI(std::format("{:<8}rdx, [rel {}]", "lea", label));
                    TI(std::format("{:<8}r8d, {}", "mov", value.size()));
                    TI("call    WriteFile");
                };

                writeStatic(layout.prefix);
                prepareWrite();
                instructionPrinter.LoadA(messageRegister, TypeRef::MakePointer(TypeRef::MakeNamed("Slice<char8>")));
                TI("mov     r10, rax");
                TI("mov     rdx, [r10]");
                TI("mov     r8, [r10 + 8]");
                TI("call    WriteFile");
                writeStatic(layout.location);
            }
            else {
                const int syscallNumber = targetOs == Target::OS::MacOS ? 0x0200'0004
                                        : targetOs == Target::OS::Linux ? 1
                                                                        : 4;
                const auto writeStatic = [&](const std::string &value) {
                    const std::string label = InternString(value);
                    TI(std::format("{:<8}rsi, [rel {}]", "lea", label));
                    TI(std::format("{:<8}edx, {}", "mov", value.size()));
                    TI("mov     edi, 2");
                    TI(std::format("{:<8}eax, {}", "mov", syscallNumber));
                    TI("syscall");
                };

                writeStatic(layout.prefix);
                instructionPrinter.LoadA(messageRegister, TypeRef::MakePointer(TypeRef::MakeNamed("Slice<char8>")));
                TI("mov     r10, rax");
                TI("mov     rsi, [r10]");
                TI("mov     rdx, [r10 + 8]");
                TI("mov     edi, 2");
                TI(std::format("{:<8}eax, {}", "mov", syscallNumber));
                TI("syscall");
                writeStatic(layout.location);
            }

            TI("ud2");
            if (isAssertion) {
                TL(okLabel);
            }
            break;
        }
        case LirOpcode::Call:
            EmitCall(instruction.strArg, instruction.srcs, instruction.dst, instruction.type, instruction.callConv,
                     instructionPrinter);
            break;
        case LirOpcode::CallIndirect:
            EmitCallIndirect(instruction.srcs, instruction.dst, instruction.type, instruction.callConv,
                             instructionPrinter);
            break;
        default:
            TC(std::format("unsupported LIR opcode {} ({})", LirOpcodeName(instruction.op),
                           static_cast<int>(instruction.op)));
            break;
        }
    }

    void EmitCall(const std::string &callee, const std::vector<LirReg> &arguments, const LirReg destination,
                  const TypeRef &returnType, const CallingConvention convention,
                  AssemblyInstructionPrinter &instructionPrinter) {
        const std::vector<LirReg> stackArguments = EmitCallArguments(arguments, convention, instructionPrinter);
        const bool win64 = instructionPrinter.IsWin64Convention(convention);
        const int stackBytes = win64 ? 32 + AlignUp(static_cast<int>(stackArguments.size()) * 8, 16)
                                     : AlignUp(static_cast<int>(stackArguments.size()) * 8, 16);
        if (stackBytes > 0) {
            TI(std::format("sub     rsp, {}", stackBytes));
            StoreStackArguments(stackArguments, win64, instructionPrinter);
        }
        TI(std::format("{:<8}{}", "call", callee));
        if (stackBytes > 0) {
            TI(std::format("add     rsp, {}", stackBytes));
        }
        if (destination != LirNoReg && !returnType.IsOpaque()) {
            instructionPrinter.StoreA(destination, returnType);
        }
    }

    void EmitCallIndirect(const std::vector<LirReg> &sources, const LirReg destination, const TypeRef &returnType,
                          const CallingConvention convention, AssemblyInstructionPrinter &instructionPrinter) {
        if (sources.empty()) {
            return;
        }
        const LirReg callee = sources[0];
        const std::vector<LirReg> arguments(sources.begin() + 1, sources.end());
        const std::vector<LirReg> stackArguments = EmitCallArguments(arguments, convention, instructionPrinter);
        const bool win64 = instructionPrinter.IsWin64Convention(convention);
        const int stackBytes = win64 ? 32 + AlignUp(static_cast<int>(stackArguments.size()) * 8, 16)
                                     : AlignUp(static_cast<int>(stackArguments.size()) * 8, 16);
        if (stackBytes > 0) {
            TI(std::format("sub     rsp, {}", stackBytes));
            StoreStackArguments(stackArguments, win64, instructionPrinter);
        }
        TI(std::format("{:<8}r10, qword [rbp - {}]", "mov", instructionPrinter.FramePlan().SlotOffsets().at(callee)));
        TI("call    r10");
        if (stackBytes > 0) {
            TI(std::format("add     rsp, {}", stackBytes));
        }
        if (destination != LirNoReg && !returnType.IsOpaque()) {
            instructionPrinter.StoreA(destination, returnType);
        }
    }

    void StoreStackArguments(const std::vector<LirReg> &arguments, const bool win64,
                             AssemblyInstructionPrinter &instructionPrinter) {
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            const auto &registerTypes = instructionPrinter.FramePlan().RegisterTypes();
            const TypeRef type =
                registerTypes.contains(arguments[index]) ? registerTypes.at(arguments[index]) : TypeRef::MakeInt64();
            instructionPrinter.LoadA(arguments[index], type);
            const std::size_t offset = win64 ? 32 + index * 8 : index * 8;
            TI(std::format("{:<8}qword [rsp + {}], rax", "mov", offset));
        }
    }

    [[nodiscard]] std::vector<LirReg> EmitCallArguments(const std::vector<LirReg> &arguments,
                                                        const CallingConvention convention,
                                                        AssemblyInstructionPrinter &instructionPrinter) {
        const bool win64 = instructionPrinter.IsWin64Convention(convention);
        const auto *integerRegisters = win64 ? kWin64IntArgRegs : kIntArgRegs;
        const int maximumIntegerRegisters = win64 ? 4 : 6;
        std::vector<LirReg> stackArguments;
        if (win64) {
            for (int index = 0; index < static_cast<int>(arguments.size()); ++index) {
                const LirReg argument = arguments[index];
                const auto &registerTypes = instructionPrinter.FramePlan().RegisterTypes();
                const TypeRef type =
                    registerTypes.contains(argument) ? registerTypes.at(argument) : TypeRef::MakeInt64();
                if (index < 4) {
                    if (IsFloat(type)) {
                        const int size = SizeOf(type);
                        TI(std::format("{:<8}{}, {} [rbp - {}]", size == 4 ? "movss" : "movsd", kFltArgRegs[index],
                                       size == 4 ? "dword" : "qword",
                                       instructionPrinter.FramePlan().SlotOffsets().at(argument)));
                    }
                    else {
                        instructionPrinter.LoadA(argument, type);
                        TI(std::format("{:<8}{}, rax", "mov", integerRegisters[index]));
                    }
                }
                else {
                    stackArguments.push_back(argument);
                }
            }
        }
        else {
            int integerIndex = 0;
            int floatIndex = 0;
            for (const LirReg argument : arguments) {
                const auto &registerTypes = instructionPrinter.FramePlan().RegisterTypes();
                const TypeRef type =
                    registerTypes.contains(argument) ? registerTypes.at(argument) : TypeRef::MakeInt64();
                if (IsFloat(type)) {
                    if (floatIndex < 8) {
                        const int size = SizeOf(type);
                        TI(std::format("{:<8}{}, {} [rbp - {}]", size == 4 ? "movss" : "movsd", kFltArgRegs[floatIndex],
                                       size == 4 ? "dword" : "qword",
                                       instructionPrinter.FramePlan().SlotOffsets().at(argument)));
                        ++floatIndex;
                    }
                    else {
                        stackArguments.push_back(argument);
                    }
                }
                else if (integerIndex < maximumIntegerRegisters) {
                    instructionPrinter.LoadA(argument, type);
                    TI(std::format("{:<8}{}, rax", "mov", integerRegisters[integerIndex]));
                    ++integerIndex;
                }
                else {
                    stackArguments.push_back(argument);
                }
            }
        }
        return stackArguments;
    }

    [[nodiscard]] bool HasPhiMoves(const std::uint32_t from, const std::uint32_t to,
                                   const AssemblyInstructionPrinter &instructionPrinter) const {
        const auto &phiMoves = instructionPrinter.FramePlan().PhiMoves();
        const auto block = phiMoves.find(from);
        return block != phiMoves.end() && block->second.contains(to);
    }

    void GenTerminator(const std::uint32_t blockIndex, const LirTerminator &terminator, const LirFunc &function,
                       AssemblyInstructionPrinter &instructionPrinter) {
        switch (terminator.kind) {
        case LirTermKind::Jump:
            EmitPhiMoves(blockIndex, terminator.trueTarget, instructionPrinter);
            TI(std::format("{:<8}{}", "jmp",
                           BlockLabel(terminator.trueTarget, function.blocks[terminator.trueTarget].label)));
            break;
        case LirTermKind::Branch: {
            const auto &registerTypes = instructionPrinter.FramePlan().RegisterTypes();
            const TypeRef conditionType =
                registerTypes.contains(terminator.cond) ? registerTypes.at(terminator.cond) : TypeRef::MakeBool();
            instructionPrinter.LoadA(terminator.cond, conditionType);
            TI("test    rax, rax");
            const std::string trueLabel =
                BlockLabel(terminator.trueTarget, function.blocks[terminator.trueTarget].label);
            const std::string falseLabel =
                BlockLabel(terminator.falseTarget, function.blocks[terminator.falseTarget].label);
            const bool truePhi = HasPhiMoves(blockIndex, terminator.trueTarget, instructionPrinter);
            const bool falsePhi = HasPhiMoves(blockIndex, terminator.falseTarget, instructionPrinter);
            if (!truePhi && !falsePhi) {
                TI(std::format("{:<8}{}", "jz", falseLabel));
                TI(std::format("{:<8}{}", "jmp", trueLabel));
            }
            else {
                const std::string trampolineTrue = std::format(".{}_br{}_t", currentFunction, blockIndex);
                const std::string trampolineFalse = std::format(".{}_br{}_f", currentFunction, blockIndex);
                TI(std::format("{:<8}{}", "jz", trampolineFalse));
                TL(trampolineTrue);
                EmitPhiMoves(blockIndex, terminator.trueTarget, instructionPrinter);
                TI(std::format("{:<8}{}", "jmp", trueLabel));
                TL(trampolineFalse);
                EmitPhiMoves(blockIndex, terminator.falseTarget, instructionPrinter);
                TI(std::format("{:<8}{}", "jmp", falseLabel));
            }
            break;
        }
        case LirTermKind::Return:
            if (terminator.retVal && *terminator.retVal != LirNoReg) {
                instructionPrinter.LoadReturnValue(*terminator.retVal, terminator.retType);
            }
            EmitEpilogue(instructionPrinter);
            break;
        case LirTermKind::Switch: {
            const auto &registerTypes = instructionPrinter.FramePlan().RegisterTypes();
            const TypeRef conditionType =
                registerTypes.contains(terminator.cond) ? registerTypes.at(terminator.cond) : TypeRef::MakeInt64();
            instructionPrinter.LoadA(terminator.cond, conditionType);
            for (const auto &switchCase : terminator.cases) {
                TI(std::format("{:<8}rax, {}", "cmp", switchCase.value));
                TI(std::format("{:<8}{}", "je",
                               BlockLabel(switchCase.target, function.blocks[switchCase.target].label)));
            }
            EmitPhiMoves(blockIndex, terminator.defaultTarget, instructionPrinter);
            TI(std::format("{:<8}{}", "jmp",
                           BlockLabel(terminator.defaultTarget, function.blocks[terminator.defaultTarget].label)));
            break;
        }
        case LirTermKind::Unreachable:
            TI("ud2");
            break;
        }
    }

    void EmitEpilogue(const AssemblyInstructionPrinter &instructionPrinter) {
        const auto &usedPhysicalRegisters = instructionPrinter.FramePlan().UsedPhysicalRegisters();
        if (!usedPhysicalRegisters.empty()) {
            const std::int32_t remainingFrame = instructionPrinter.FramePlan().FrameSize() -
                                                static_cast<std::int32_t>(usedPhysicalRegisters.size() * 8);
            if (remainingFrame > 0) {
                TI(std::format("add     rsp, {}", remainingFrame));
            }
            for (auto registerIndex = usedPhysicalRegisters.rbegin(); registerIndex != usedPhysicalRegisters.rend();
                 ++registerIndex) {
                TI(std::format("pop     {}", instructionPrinter.PhysicalRegisterName(*registerIndex)));
            }
            TI("pop     rbp");
        }
        else {
            TI("leave");
        }
        TI("ret");
    }
};

} // namespace

AssemblyControlFlowPrinter::AssemblyControlFlowPrinter(AssemblyModulePrinter &modulePrinter, const LayoutMap &layouts,
                                                       const std::unordered_set<std::string> &interfaceNames,
                                                       const Target::OS inputTargetOs)
    : modulePrinter(modulePrinter)
    , layouts(layouts)
    , interfaceNames(interfaceNames)
    , targetOs(inputTargetOs) {
}

void AssemblyControlFlowPrinter::EmitFunction(const LirFunc &function) {
    FunctionAssemblyPrinter printer(modulePrinter, layouts, interfaceNames, targetOs);
    printer.EmitFunction(function);
}
} // namespace Rux
