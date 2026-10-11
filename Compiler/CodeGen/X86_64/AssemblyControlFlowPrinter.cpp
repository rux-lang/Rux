// Assembly text for branches, labels and calls — the parts of a function whose
// printed form depends on the block structure around them. Like the instruction
// text, it mirrors the object emitter (`X86_64CallEmitter`,
// `X86_64TerminatorEmitter` and the module emitter's runtime-failure path).

#include "CodeGen/X86_64/AssemblyControlFlowPrinter.h"

#include "CodeGen/IntegerLiteral.h"
#include "CodeGen/RuntimeFailure.h"
#include "CodeGen/X86_64/AssemblyInstructionPrinter.h"
#include "CodeGen/X86_64/AssemblyModulePrinter.h"
#include "CodeGen/X86_64/CodeAlignment.h"
#include "CodeGen/X86_64/MachineEmission.h"
#include "CodeGen/X86_64/MachineSelection.h"
#include "Target/AsmInstr.h"

#include <format>
#include <string>
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

    bool tracesPanics = false;

    void EmitFunction(const LirFunc &function) {
        currentFunction = function.name;
        TI(std::format("align {}, db 0x90", X86CodeAlignment));
        if (function.isAsm) {
            EmitAsmFunction(function);
            return;
        }
        const X86_64FramePlan framePlan = PlanX86_64Frame(function, layouts, interfaceNames, targetOs);
        AssemblyInstructionPrinter instructionPrinter(modulePrinter, framePlan, layouts, interfaceNames, targetOs);
        instructionPrinter.EmitFunctionSetup(function);

        const auto alignedBlocks = X86AlignedBlocks(function);
        for (std::uint32_t index = 0; index < function.blocks.size(); ++index) {
            if (alignedBlocks[index]) {
                TI(std::format("align {}, db 0x90", X86CodeAlignment));
            }
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

    /// One operand of an `asm func` instruction, in the syntax it was written in.
    [[nodiscard]] static std::string AsmOperandText(const AsmOperand &operand) {
        switch (operand.kind) {
        case AsmOperand::Kind::Reg:
        case AsmOperand::Kind::Sym:
            return operand.name;
        case AsmOperand::Kind::Imm:
            return std::to_string(operand.imm);
        case AsmOperand::Kind::Mem: {
            std::string address;
            const auto add = [&](const std::string &term) { address += (address.empty() ? "" : " + ") + term; };
            if (!operand.memSym.empty()) {
                add("rel " + operand.memSym);
            }
            if (!operand.memBase.empty()) {
                add(operand.memBase);
            }
            if (!operand.memIndex.empty()) {
                add(operand.memScale == 1 ? operand.memIndex
                                          : std::format("{}*{}", operand.memIndex, operand.memScale));
            }
            if (operand.imm < 0) {
                address += std::format("{}{}", address.empty() ? "-" : " - ", -operand.imm);
            }
            else if (operand.imm > 0 || address.empty()) {
                add(std::to_string(operand.imm));
            }
            const std::string_view size = operand.memSize == 1 ? "byte "
                                        : operand.memSize == 2 ? "word "
                                        : operand.memSize == 4 ? "dword "
                                        : operand.memSize == 8 ? "qword "
                                                               : "";
            return std::format("{}[{}]", size, address);
        }
        default:
            return {};
        }
    }

    /// An `asm func` is its body as written: the object emitter encodes it as a raw blob, with no prologue, epilogue
    /// or frame.
    void EmitAsmFunction(const LirFunc &function) {
        TB();
        TC(std::format("── {} ─", function.name));
        TL(function.name);
        for (const AsmInstr &instruction : function.asmBody) {
            if (!instruction.labelDef.empty()) {
                TL(instruction.labelDef);
                continue;
            }
            std::string operands;
            for (const AsmOperand &operand : instruction.operands) {
                operands += (operands.empty() ? "" : ", ") + AsmOperandText(operand);
            }
            TI(operands.empty() ? instruction.mnemonic : std::format("{:<7} {}", instruction.mnemonic, operands));
        }
    }

    [[nodiscard]] std::string BlockLabel(const std::uint32_t index, const std::string &label) const {
        if (index == 0) {
            return currentFunction;
        }
        return "." + currentFunction + "_" + label;
    }

    /// The moves a phi edge makes, as `X86_64TerminatorEmitter::EmitPhiMoves` makes them: a value wider than two words
    /// word by word between slots, anything narrower through rax (rax:rdx, xmm0) and the operand moves.
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

        const auto I = [&](const std::string_view opcode, const std::string &operands) {
            instructionPrinter.Instruction(opcode, operands);
        };
        const auto rbp = [](const std::string_view size, const std::int32_t displacement) {
            return AssemblyInstructionPrinter::Rbp(size, displacement);
        };
        const std::int32_t temporary = -instructionPrinter.FramePlan().PhiTemporaryOffset();
        for (const auto &step : ResolvePhiMoves(to->second)) {
            const int size = instructionPrinter.SizeOfRuntime(step.type);
            if (size > 16) {
                const std::int32_t source = step.kind == PhiMoveStep::Kind::SaveDestination
                                              ? instructionPrinter.Disp(step.dst)
                                          : step.sourceIsTemporary ? temporary
                                                                   : instructionPrinter.Disp(step.src);
                const std::int32_t destination =
                    step.kind == PhiMoveStep::Kind::SaveDestination ? temporary : instructionPrinter.Disp(step.dst);
                for (int offset = 0; offset < size; offset += 8) {
                    I("mov", "rax, " + rbp("qword", source + offset));
                    I("mov", rbp("qword", destination + offset) + ", rax");
                }
                continue;
            }
            const int scalarSize = size > 0 ? size : 8;
            const bool float32 = step.type.kind == TypeRef::Kind::Float32;
            if (step.kind == PhiMoveStep::Kind::SaveDestination) {
                instructionPrinter.LoadA(step.dst, step.type);
                if (size > 8) {
                    I("mov", rbp("qword", temporary) + ", rax");
                    I("mov", rbp("qword", temporary + 8) + ", rdx");
                }
                else if (IsFloat(step.type)) {
                    I(float32 ? "movss" : "movsd", rbp(float32 ? "dword" : "qword", temporary) + ", xmm0");
                }
                else {
                    const std::string_view width = scalarSize == 8 ? "qword"
                                                 : scalarSize == 4 ? "dword"
                                                 : scalarSize == 2 ? "word"
                                                                   : "byte";
                    const std::string_view accumulator = scalarSize == 8 ? "rax"
                                                       : scalarSize == 4 ? "eax"
                                                       : scalarSize == 2 ? "ax"
                                                                         : "al";
                    I("mov", std::format("{}, {}", rbp(width, temporary), accumulator));
                }
                continue;
            }
            if (!step.sourceIsTemporary) {
                instructionPrinter.LoadA(step.src, step.type);
                instructionPrinter.StoreA(step.dst, step.type);
                continue;
            }
            if (size > 8) {
                I("mov", "rax, " + rbp("qword", temporary));
                I("mov", "r10, " + rbp("qword", temporary + 8));
                I("mov", "rdx, r10");
            }
            else if (IsFloat(step.type)) {
                I(float32 ? "movss" : "movsd", "xmm0, " + rbp(float32 ? "dword" : "qword", temporary));
            }
            else if (scalarSize == 8) {
                I("mov", "rax, " + rbp("qword", temporary));
            }
            else if (step.type.IsSigned()) {
                I(scalarSize == 4 ? "movsxd" : "movsx", "rax, " + rbp(scalarSize == 4   ? "dword"
                                                                      : scalarSize == 2 ? "word"
                                                                                        : "byte",
                                                                      temporary));
            }
            else if (scalarSize == 4) {
                I("mov", "eax, " + rbp("dword", temporary));
            }
            else {
                I("movzx", "rax, " + rbp(scalarSize == 2 ? "word" : "byte", temporary));
            }
            instructionPrinter.StoreA(step.dst, step.type);
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
        if (instructionPrinter.EmitArithmetic(instruction) || instructionPrinter.EmitMemory(instruction) ||
            instructionPrinter.EmitAtomic(instruction) || instructionPrinter.EmitThreadLocal(instruction)) {
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

            // As the module emitter writes them: WriteFile to the standard error handle on Windows, the write
            // system call to descriptor 2 elsewhere.
            if (targetOs == Target::OS::Windows) {
                NeedExtern("GetStdHandle");
                NeedExtern("WriteFile");
                TI("sub     rsp, 48");

                const auto prepareWrite = [&]() {
                    TI("mov     eax, -12");
                    TI("mov     rcx, rax");
                    TI("call    GetStdHandle");
                    TI("mov     rcx, rax");
                    TI("lea     r9, [rsp + 40]");
                    TI("mov     qword [rsp + 32], 0");
                };
                const auto writeStatic = [&](const std::string &value) {
                    prepareWrite();
                    const std::string label = InternString(value);
                    TI(std::format("{:<8}rax, [rel {}]", "lea", label));
                    TI("mov     rdx, rax");
                    TI(std::format("{:<8}eax, {}", "mov", value.size()));
                    TI("mov     r8, rax");
                    TI("call    WriteFile");
                };

                writeStatic(layout.prefix);
                prepareWrite();
                instructionPrinter.LoadA(messageRegister,
                                         TypeRef::MakePointer(TypeRef::MakeText(TypeRef::Kind::Char8)));
                TI("mov     r10, rax");
                TI("mov     rdx, qword [r10]");
                TI("mov     r8, qword [r10 + 8]");
                TI("call    WriteFile");
                writeStatic(layout.location);
                if (tracesPanics) {
                    TI("call    __rux_backtrace");
                }
            }
            else {
                const int syscallNumber = targetOs == Target::OS::Linux ? 1
                                        : targetOs == Target::OS::MacOS ? 0x0200'0004
                                                                        : 4;
                const auto writeStatic = [&](const std::string &value) {
                    const std::string label = InternString(value);
                    TI(std::format("{:<8}rax, [rel {}]", "lea", label));
                    TI("mov     rsi, rax");
                    TI(std::format("{:<8}edx, {}", "mov", value.size()));
                    TI("mov     edi, 2");
                    TI(std::format("{:<8}eax, {}", "mov", syscallNumber));
                    TI("syscall");
                };

                writeStatic(layout.prefix);
                instructionPrinter.LoadA(messageRegister,
                                         TypeRef::MakePointer(TypeRef::MakeText(TypeRef::Kind::Char8)));
                TI("mov     r10, rax");
                TI("mov     rsi, qword [r10]");
                TI("mov     rdx, qword [r10 + 8]");
                TI("mov     edi, 2");
                TI(std::format("{:<8}eax, {}", "mov", syscallNumber));
                TI("syscall");
                writeStatic(layout.location);
                if (tracesPanics) {
                    TI("call    __rux_backtrace");
                }
            }

            TI("ud2");
            if (isAssertion) {
                TL(okLabel);
            }
            break;
        }
        case LirOpcode::FramePointer:
            TI("mov     rax, rbp");
            instructionPrinter.StoreA(instruction.dst, instruction.type);
            break;
        case LirOpcode::WriteStandardError:
            if (targetOs == Target::OS::Windows) {
                NeedExtern("GetStdHandle");
                NeedExtern("WriteFile");
                TI("sub     rsp, 48");
                TI("mov     eax, -12");
                TI("mov     rcx, rax");
                TI("call    GetStdHandle");
                TI("mov     rcx, rax");
                TI("lea     r9, [rsp + 40]");
                TI("mov     qword [rsp + 32], 0");
                instructionPrinter.LoadA(instruction.srcs[0], TypeRef::MakePointer(TypeRef::MakeChar8()));
                TI("mov     rdx, rax");
                instructionPrinter.LoadA(instruction.srcs[1], TypeRef::MakeUInt64());
                TI("mov     r8, rax");
                TI("call    WriteFile");
                TI("add     rsp, 48");
            }
            else {
                const int syscallNumber = targetOs == Target::OS::Linux ? 1
                                        : targetOs == Target::OS::MacOS ? 0x0200'0004
                                                                        : 4;
                instructionPrinter.LoadA(instruction.srcs[0], TypeRef::MakePointer(TypeRef::MakeChar8()));
                TI("mov     rsi, rax");
                instructionPrinter.LoadA(instruction.srcs[1], TypeRef::MakeUInt64());
                TI("mov     rdx, rax");
                TI("mov     edi, 2");
                TI(std::format("{:<8}eax, {}", "mov", syscallNumber));
                TI("syscall");
            }
            break;
        case LirOpcode::Call:
            if (!EmitBitCast(instruction, instructionPrinter)) {
                EmitPreparedCall(instruction, instruction.srcs, false, instructionPrinter);
            }
            break;
        case LirOpcode::CallIndirect:
            if (!instruction.srcs.empty()) {
                const std::vector<LirReg> arguments(instruction.srcs.begin() + 1, instruction.srcs.end());
                EmitPreparedCall(instruction, arguments, true, instructionPrinter);
            }
            break;
        default:
            TC(std::format("unsupported LIR opcode {} ({})", LirOpcodeName(instruction.op),
                           static_cast<int>(instruction.op)));
            break;
        }
    }

    // ---- calls, as `X86_64CallEmitter` makes them ------------------------------------------------------------------

    [[nodiscard]] static TypeRef ArgumentType(const LirReg argument, const AssemblyInstructionPrinter &printer) {
        const auto &types = printer.FramePlan().RegisterTypes();
        return types.contains(argument) ? types.at(argument) : TypeRef::MakeInt64();
    }

    [[nodiscard]] static bool IsTwoWordAggregate(const TypeRef &type, const AssemblyInstructionPrinter &printer) {
        return printer.IsAggregate(type) && printer.SizeOfRuntime(type) > 8 && printer.SizeOfRuntime(type) <= 16;
    }

    [[nodiscard]] static std::string Rsp(const std::int32_t offset, const std::string_view size = "qword") {
        return offset == 0 ? std::format("{} [rsp]", size) : std::format("{} [rsp + {}]", size, offset);
    }

    /// The outgoing area a call reserves: the Win64 home area and stack arguments, or the System V stack arguments.
    [[nodiscard]] static int CallFrameSize(const std::vector<LirReg> &arguments, const bool win64, const int startIndex,
                                           const AssemblyInstructionPrinter &printer) {
        if (win64) {
            const std::size_t count = arguments.size() + static_cast<std::size_t>(startIndex);
            return AlignUp(static_cast<int>(32 + (count > 4 ? count - 4 : 0) * 8), 16);
        }
        int integerIndex = startIndex;
        int floatIndex = 0;
        std::size_t stackArguments = 0;
        for (const LirReg argument : arguments) {
            const TypeRef type = ArgumentType(argument, printer);
            if (IsFloat(type)) {
                floatIndex < 8 ? ++floatIndex : ++stackArguments;
            }
            else if (printer.IsSysVMemoryAggregate(type)) {
                stackArguments += static_cast<std::size_t>(AlignUp(printer.SizeOfRuntime(type), 8) / 8);
            }
            else if (IsTwoWordAggregate(type, printer)) {
                if (integerIndex <= 4) {
                    integerIndex += 2;
                }
                else {
                    stackArguments += 2;
                }
            }
            else {
                integerIndex < 6 ? ++integerIndex : ++stackArguments;
            }
        }
        return AlignUp(static_cast<int>(stackArguments * 8), 16);
    }

    /// System V stack arguments, written before any register is loaded.
    static void StoreSysVStackArguments(const std::vector<LirReg> &arguments, const int startIndex,
                                        AssemblyInstructionPrinter &printer) {
        int integerIndex = startIndex;
        int floatIndex = 0;
        int stackIndex = 0;
        for (const LirReg argument : arguments) {
            const TypeRef type = ArgumentType(argument, printer);
            const std::int32_t displacement = printer.Disp(argument);
            if (printer.IsSysVMemoryAggregate(type)) {
                for (int offset = 0; offset < AlignUp(printer.SizeOfRuntime(type), 8); offset += 8) {
                    printer.Instruction("mov",
                                        "rax, " + AssemblyInstructionPrinter::Rbp("qword", displacement + offset));
                    printer.Instruction("mov", Rsp(stackIndex++ * 8) + ", rax");
                }
                continue;
            }
            if (IsTwoWordAggregate(type, printer)) {
                if (integerIndex <= 4) {
                    integerIndex += 2;
                }
                else {
                    for (int offset = 0; offset < 16; offset += 8) {
                        printer.Instruction("mov",
                                            "rax, " + AssemblyInstructionPrinter::Rbp("qword", displacement + offset));
                        printer.Instruction("mov", Rsp(stackIndex++ * 8) + ", rax");
                    }
                }
                continue;
            }
            bool onStack = false;
            if (IsFloat(type)) {
                onStack = floatIndex >= 8;
                ++floatIndex;
            }
            else {
                onStack = integerIndex >= 6;
                ++integerIndex;
            }
            if (!onStack) {
                continue;
            }
            printer.LoadA(argument, type);
            const std::int32_t offset = stackIndex++ * 8;
            if (IsFloat(type)) {
                const bool float32 = SizeOf(type) == 4;
                printer.Instruction(float32 ? "movss" : "movsd", Rsp(offset, float32 ? "dword" : "qword") + ", xmm0");
            }
            else {
                printer.Instruction("mov", Rsp(offset) + ", rax");
            }
        }
    }

    static void EmitArguments(const std::vector<LirReg> &arguments, const bool win64, const int startIndex,
                              AssemblyInstructionPrinter &printer) {
        const auto &physicalRegisters = printer.FramePlan().PhysicalRegisters();
        if (win64) {
            int index = startIndex;
            for (const LirReg argument : arguments) {
                const TypeRef type = ArgumentType(argument, printer);
                const std::int32_t displacement = printer.Disp(argument);
                const bool float32 = SizeOf(type) == 4;
                if (index >= 4) {
                    const std::int32_t stackOffset = 32 + (index - 4) * 8;
                    if (IsFloat(type)) {
                        // Moved as raw bits through rax rather than through xmm0, which by now holds the first
                        // argument.
                        printer.LoadA(argument, float32 ? TypeRef::MakeUInt32() : TypeRef::MakeUInt64());
                    }
                    else if (printer.IsWin64ByRefAggregate(type)) {
                        printer.Instruction("lea", "rax, " + AssemblyInstructionPrinter::Rbp(displacement));
                    }
                    else {
                        printer.LoadA(argument, type);
                    }
                    printer.Instruction("mov", Rsp(stackOffset) + ", rax");
                    ++index;
                    continue;
                }
                const std::string_view reg = kWin64IntArgRegs[index];
                if (IsFloat(type)) {
                    if (const auto found = printer.FramePlan().FloatRegisters().find(argument);
                        found != printer.FramePlan().FloatRegisters().end()) {
                        printer.Instruction("movaps", std::format("{}, xmm{}", kFltArgRegs[index], found->second));
                    }
                    else {
                        printer.Instruction(
                            float32 ? "movss" : "movsd",
                            std::format("{}, {}", kFltArgRegs[index],
                                        AssemblyInstructionPrinter::Rbp(float32 ? "dword" : "qword", displacement)));
                    }
                }
                else if (printer.IsWin64ByRefAggregate(type)) {
                    // A value wider than a register, or of an odd width, is passed by the address of its home.
                    printer.Instruction("lea",
                                        std::format("{}, {}", reg, AssemblyInstructionPrinter::Rbp(displacement)));
                }
                else if (type.kind == TypeRef::Kind::Pointer && !type.inner.empty() &&
                         printer.IsWin64ByRefAggregate(type.inner[0]) && !physicalRegisters.contains(argument)) {
                    printer.Instruction(
                        "mov", std::format("{}, {}", reg, AssemblyInstructionPrinter::Rbp("qword", displacement)));
                }
                else {
                    printer.LoadA(argument, type);
                    printer.Instruction("mov", std::format("{}, rax", reg));
                }
                ++index;
            }
            return;
        }

        int integerIndex = startIndex;
        int floatIndex = 0;
        for (const LirReg argument : arguments) {
            const TypeRef type = ArgumentType(argument, printer);
            const std::int32_t displacement = printer.Disp(argument);
            if (IsFloat(type)) {
                if (floatIndex < 8) {
                    const bool float32 = SizeOf(type) == 4;
                    if (const auto found = printer.FramePlan().FloatRegisters().find(argument);
                        found != printer.FramePlan().FloatRegisters().end()) {
                        printer.Instruction("movaps", std::format("{}, xmm{}", kFltArgRegs[floatIndex], found->second));
                    }
                    else {
                        printer.Instruction(
                            float32 ? "movss" : "movsd",
                            std::format("{}, {}", kFltArgRegs[floatIndex],
                                        AssemblyInstructionPrinter::Rbp(float32 ? "dword" : "qword", displacement)));
                    }
                    ++floatIndex;
                }
            }
            else if (printer.IsSysVMemoryAggregate(type)) {
                // Already written to its stack slots, and it takes no register.
            }
            else if (IsTwoWordAggregate(type, printer)) {
                if (integerIndex <= 4) {
                    for (int offset = 0; offset < 16; offset += 8) {
                        printer.Instruction("mov",
                                            "rax, " + AssemblyInstructionPrinter::Rbp("qword", displacement + offset));
                        printer.Instruction("mov", std::format("{}, rax", kIntArgRegs[integerIndex++]));
                    }
                }
            }
            else if (integerIndex < 6) {
                printer.LoadA(argument, type);
                printer.Instruction("mov", std::format("{}, rax", kIntArgRegs[integerIndex++]));
            }
        }
        // System V wants AL to count the vector registers a variadic call uses; it is harmless for a fixed call.
        printer.Instruction("mov", std::format("eax, {}", floatIndex));
    }

    static void StoreReturnValue(const LirReg destination, const TypeRef &type, AssemblyInstructionPrinter &printer) {
        if (const int size = printer.SizeOfRuntime(type); size > 8 && size <= 16) {
            printer.Instruction("mov", AssemblyInstructionPrinter::Rbp("qword", printer.Disp(destination)) + ", rax");
            printer.Instruction("mov",
                                AssemblyInstructionPrinter::Rbp("qword", printer.Disp(destination) + 8) + ", rdx");
        }
        else {
            printer.StoreA(destination, type);
        }
    }

    /// The bit-cast intrinsics the emitter inlines instead of calling.
    static bool EmitBitCast(const LirInstr &instruction, AssemblyInstructionPrinter &printer) {
        if (instruction.srcs.size() != 1) {
            return false;
        }
        const LirReg source = instruction.srcs[0];
        if (instruction.strArg == "FloatBits64") {
            printer.LoadA(source, TypeRef::MakeFloat64());
            printer.Instruction("movq", "rax, xmm0");
        }
        else if (instruction.strArg == "FloatFromBits64") {
            printer.LoadA(source, TypeRef::MakeInt64());
            printer.Instruction("movq", "xmm0, rax");
        }
        else if (instruction.strArg == "FloatBits32") {
            printer.LoadA(source, TypeRef::MakeFloat32());
            printer.Instruction("movd", "eax, xmm0");
        }
        else if (instruction.strArg == "FloatFromBits32") {
            printer.LoadA(source, TypeRef::MakeInt32());
            printer.Instruction("movd", "xmm0, eax");
        }
        else {
            return false;
        }
        if (instruction.dst != LirNoReg && !instruction.type.IsOpaque()) {
            StoreReturnValue(instruction.dst, instruction.type, printer);
        }
        return true;
    }

    void EmitPreparedCall(const LirInstr &instruction, const std::vector<LirReg> &arguments, const bool indirect,
                          AssemblyInstructionPrinter &printer) {
        const bool win64 = printer.IsWin64Convention(instruction.callConv);
        // A result no register pair returns is written through a pointer the caller passes first.
        const bool hiddenReturn =
            instruction.dst != LirNoReg &&
            (win64 ? printer.IsWin64ByRefAggregate(instruction.type) : printer.IsSysVMemoryAggregate(instruction.type));
        const int startIndex = hiddenReturn ? 1 : 0;
        const int frameSize = CallFrameSize(arguments, win64, startIndex, printer);
        if (frameSize > 0) {
            TI(std::format("sub     rsp, {}", frameSize));
        }
        if (hiddenReturn) {
            const std::string destination = AssemblyInstructionPrinter::Rbp(printer.Disp(instruction.dst));
            if (win64) {
                printer.Instruction("lea", "rcx, " + destination);
            }
            else {
                printer.Instruction("lea", "rax, " + destination);
                printer.Instruction("mov", "rdi, rax");
            }
        }
        if (!win64) {
            StoreSysVStackArguments(arguments, startIndex, printer);
        }
        EmitArguments(arguments, win64, startIndex, printer);
        if (indirect) {
            const LirReg callee = instruction.srcs.front();
            if (const auto physical = printer.FramePlan().PhysicalRegisters().find(callee);
                physical != printer.FramePlan().PhysicalRegisters().end()) {
                printer.Instruction("mov", std::format("r10, {}", printer.PhysicalRegisterName(physical->second)));
            }
            else {
                printer.Instruction("mov", "r10, " + AssemblyInstructionPrinter::Rbp("qword", printer.Disp(callee)));
            }
            TI("call    r10");
        }
        else {
            TI(std::format("{:<8}{}", "call", instruction.strArg));
        }
        if (frameSize > 0) {
            TI(std::format("add     rsp, {}", frameSize));
        }
        if (instruction.dst != LirNoReg && !instruction.type.IsOpaque() && !hiddenReturn) {
            StoreReturnValue(instruction.dst, instruction.type, printer);
        }
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
            if (terminator.trueTarget != blockIndex + 1) {
                TI(std::format("{:<8}{}", "jmp",
                               BlockLabel(terminator.trueTarget, function.blocks[terminator.trueTarget].label)));
            }
            break;
        case LirTermKind::Branch: {
            const auto &registerTypes = instructionPrinter.FramePlan().RegisterTypes();
            auto condition = X86Condition::NotEqual;
            const auto &comparisons = instructionPrinter.FramePlan().BranchComparisons();
            if (const auto found = comparisons.find(terminator.cond); found != comparisons.end()) {
                const auto &comparison = found->second;
                instructionPrinter.LoadA(comparison.right, comparison.type);
                TI("mov     r10, rax");
                instructionPrinter.LoadA(comparison.left, comparison.type);
                TI("cmp     rax, r10");
                condition = comparison.condition;
            }
            else {
                const TypeRef conditionType =
                    registerTypes.contains(terminator.cond) ? registerTypes.at(terminator.cond) : TypeRef::MakeBool();
                instructionPrinter.LoadA(terminator.cond, conditionType);
                TI("test    rax, rax");
            }
            const std::string trueLabel =
                BlockLabel(terminator.trueTarget, function.blocks[terminator.trueTarget].label);
            const std::string falseLabel =
                BlockLabel(terminator.falseTarget, function.blocks[terminator.falseTarget].label);
            const bool truePhi = HasPhiMoves(blockIndex, terminator.trueTarget, instructionPrinter);
            const bool falsePhi = HasPhiMoves(blockIndex, terminator.falseTarget, instructionPrinter);
            if (!truePhi && !falsePhi) {
                if (terminator.falseTarget == blockIndex + 1) {
                    TI(std::format("{:<8}{}", ConditionMnemonic(condition), trueLabel));
                }
                else {
                    TI(std::format("{:<8}{}", ConditionMnemonic(Inverse(condition)), falseLabel));
                    if (terminator.trueTarget != blockIndex + 1) {
                        TI(std::format("{:<8}{}", "jmp", trueLabel));
                    }
                }
            }
            else {
                const std::string trampolineTrue = std::format(".{}_br{}_t", currentFunction, blockIndex);
                const std::string trampolineFalse = std::format(".{}_br{}_f", currentFunction, blockIndex);
                TI(std::format("{:<8}{}", ConditionMnemonic(Inverse(condition)), trampolineFalse));
                TL(trampolineTrue);
                EmitPhiMoves(blockIndex, terminator.trueTarget, instructionPrinter);
                TI(std::format("{:<8}{}", "jmp", trueLabel));
                TL(trampolineFalse);
                EmitPhiMoves(blockIndex, terminator.falseTarget, instructionPrinter);
                if (terminator.falseTarget != blockIndex + 1) {
                    TI(std::format("{:<8}{}", "jmp", falseLabel));
                }
            }
            break;
        }
        case LirTermKind::Return:
            if (terminator.retVal && *terminator.retVal != LirNoReg) {
                // A result returned through the caller's pointer is copied there; anything else is loaded into rax
                // (rax:rdx, xmm0).
                if (instructionPrinter.FramePlan().HiddenReturnOffset() != 0) {
                    instructionPrinter.StoreHiddenReturnValue(*terminator.retVal, terminator.retType);
                }
                else {
                    instructionPrinter.LoadReturnValue(*terminator.retVal, terminator.retType);
                }
            }
            EmitEpilogue(instructionPrinter);
            break;
        case LirTermKind::Switch: {
            const auto &registerTypes = instructionPrinter.FramePlan().RegisterTypes();
            const TypeRef conditionType =
                registerTypes.contains(terminator.cond) ? registerTypes.at(terminator.cond) : TypeRef::MakeInt64();
            instructionPrinter.LoadA(terminator.cond, conditionType);
            for (const auto &switchCase : terminator.cases) {
                // The case value as the emitter encodes it: the literal's bits, as a sign-extended imm32.
                TI(std::format("{:<8}rax, {}", "cmp",
                               static_cast<std::int32_t>(ParseIntegerLiteralBits(switchCase.value).value_or(0))));
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
        for (const auto &[reg, offset] : instructionPrinter.FramePlan().FloatSaves()) {
            TI(std::format("movdqu  xmm{}, {}", reg, AssemblyInstructionPrinter::Rbp("oword", -offset)));
        }
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
    if (machineSelection && !function.isAsm) {
        const X86_64Machine::SelectionContext context{layouts, interfaceNames, targetOs, tracesPanics};
        if (auto selected = X86_64Machine::SelectFunction(function, context)) {
            if (const auto emitted = X86_64Machine::EmitFunction(*selected, targetOs, true)) {
                modulePrinter.TextInstruction(std::format("align {}, db 0x90", X86CodeAlignment));
                modulePrinter.TextBlank();
                modulePrinter.TextComment(std::format("── {} (machine selection) ─", function.name));
                modulePrinter.TextLabel(function.name);
                for (const auto &line : emitted->assembly) {
                    modulePrinter.TextLine(line);
                }
                modulePrinter.TextBlank();
                return;
            }
        }
    }
    FunctionAssemblyPrinter printer(modulePrinter, layouts, interfaceNames, targetOs);
    printer.tracesPanics = tracesPanics;
    printer.EmitFunction(function);
}
} // namespace Rux
