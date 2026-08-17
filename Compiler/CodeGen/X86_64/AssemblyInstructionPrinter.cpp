// Per-instruction assembly text, including the register and operand-size
// spellings x86-64 syntax requires.

#include "CodeGen/X86_64/AssemblyInstructionPrinter.h"

#include "CodeGen/X86_64/AssemblyModulePrinter.h"
#include "CodeGen/X86_64/RuntimeHelpers.h"
#include "Target/Platform.h"

#include <algorithm>
#include <format>

namespace Rux {
using namespace Layout;

namespace {
/// The accumulator register's name at a given width — `al`, `ax`, `eax`, `rax` — since x86-64 spells the same register
/// differently depending on the operand size.
std::string_view GprA(const int bytes) {
    switch (bytes) {
    case 1:
        return "al";
    case 2:
        return "ax";
    case 4:
        return "eax";
    default:
        return "rax";
    }
}

/// The size keyword a memory operand needs when the width is not implied by a register operand, as in `qword ptr [rbp -
/// 8]`.
std::string_view PtrSize(const int bytes) {
    switch (bytes) {
    case 1:
        return "byte";
    case 2:
        return "word";
    case 4:
        return "dword";
    default:
        return "qword";
    }
}
} // namespace

AssemblyInstructionPrinter::AssemblyInstructionPrinter(AssemblyModulePrinter &inputModulePrinter,
                                                       const X86_64FramePlan &inputFramePlan,
                                                       const LayoutMap &inputLayouts,
                                                       const std::unordered_set<std::string> &inputInterfaceNames,
                                                       const Target::OS inputTargetOs)
    : modulePrinter(inputModulePrinter)
    , framePlan(inputFramePlan)
    , layouts(inputLayouts)
    , interfaceNames(inputInterfaceNames)
    , targetOs(inputTargetOs) {
}

const X86_64FramePlan &AssemblyInstructionPrinter::FramePlan() const {
    return framePlan;
}

bool AssemblyInstructionPrinter::IsWin64Convention(const CallingConvention convention) const {
    CallingConvention resolved = convention;
    if (resolved == CallingConvention::Default) {
        resolved = PlatformDefaultConvention(targetOs, Target::Arch::X86_64);
    }
    else {
        resolved = ResolveCConvention(resolved, targetOs, Target::Arch::X86_64);
    }
    return resolved == CallingConvention::Win64;
}

int AssemblyInstructionPrinter::SizeOfRuntime(const TypeRef &type) const {
    return RuntimeSizeOf(type, layouts, interfaceNames);
}

bool AssemblyInstructionPrinter::IsWin64AddressParameter(const TypeRef &type) const {
    if (type.kind != TypeRef::Kind::Named) {
        return false;
    }
    const std::string base = BaseTypeName(type.name);
    return base == "Slice" || interfaceNames.contains(base);
}

bool AssemblyInstructionPrinter::IsRegPointerTo(const LirReg reg, const TypeRef &pointee) const {
    const auto &registerTypes = framePlan.RegisterTypes();
    const auto type = registerTypes.find(reg);
    return type != registerTypes.end() && type->second.kind == TypeRef::Kind::Pointer && !type->second.inner.empty() &&
           type->second.inner[0] == pointee;
}

std::string_view AssemblyInstructionPrinter::PhysicalRegisterName(const int index) const {
    switch (index) {
    case 0:
        return "rbx";
    case 1:
        return "r12";
    case 2:
        return "r13";
    case 3:
        return "r14";
    case 4:
        return "r15";
    default:
        return "rbx";
    }
}

void AssemblyInstructionPrinter::LoadA(const LirReg reg, const TypeRef &type) {
    const auto physical = framePlan.PhysicalRegisters().find(reg);
    if (physical != framePlan.PhysicalRegisters().end()) {
        modulePrinter.TextInstruction(std::format("{:<8}rax, {}", "mov", PhysicalRegisterName(physical->second)));
        const int size = SizeOfRuntime(type);
        if (size > 0 && size < 8) {
            if (type.IsSigned()) {
                if (size == 4) {
                    modulePrinter.TextInstruction("movsxd  rax, eax");
                }
                else if (size == 2) {
                    modulePrinter.TextInstruction("movsx   rax, ax");
                }
                else {
                    modulePrinter.TextInstruction("movsx   rax, al");
                }
            }
            else if (size == 4) {
                modulePrinter.TextInstruction("mov     eax, eax");
            }
            else if (size == 2) {
                modulePrinter.TextInstruction("movzx   rax, ax");
            }
            else {
                modulePrinter.TextInstruction("movzx   rax, al");
            }
        }
        return;
    }

    const int size = SizeOfRuntime(type);
    const int offset = framePlan.SlotOffsets().at(reg);
    if (size == 16) {
        modulePrinter.TextInstruction(std::format("{:<8}rax, qword [rbp - {}]", "mov", offset));
        modulePrinter.TextInstruction(std::format("{:<8}rdx, qword [rbp - {}]", "mov", offset - 8));
    }
    else if (IsFloat(type)) {
        modulePrinter.TextInstruction(
            std::format("{:<8}xmm0, {} [rbp - {}]", size == 4 ? "movss" : "movsd", PtrSize(size), offset));
    }
    else if (size == 8 || size == 0) {
        modulePrinter.TextInstruction(std::format("{:<8}rax, qword [rbp - {}]", "mov", offset));
    }
    else if (type.IsSigned()) {
        modulePrinter.TextInstruction(
            std::format("{:<8}rax, {} [rbp - {}]", size == 4 ? "movsxd" : "movsx", PtrSize(size), offset));
    }
    else if (size == 4) {
        modulePrinter.TextInstruction(std::format("{:<8}eax, dword [rbp - {}]", "mov", offset));
    }
    else {
        modulePrinter.TextInstruction(std::format("{:<8}rax, {} [rbp - {}]", "movzx", PtrSize(size), offset));
    }
}

void AssemblyInstructionPrinter::LoadB(const LirReg reg, const TypeRef &type) {
    const auto physical = framePlan.PhysicalRegisters().find(reg);
    if (physical != framePlan.PhysicalRegisters().end()) {
        modulePrinter.TextInstruction(std::format("{:<8}r10, {}", "mov", PhysicalRegisterName(physical->second)));
        const int size = SizeOfRuntime(type);
        if (size > 0 && size < 8) {
            if (type.IsSigned()) {
                if (size == 4) {
                    modulePrinter.TextInstruction("movsxd  r10, r10d");
                }
                else if (size == 2) {
                    modulePrinter.TextInstruction("movsx   r10, r10w");
                }
                else {
                    modulePrinter.TextInstruction("movsx   r10, r10b");
                }
            }
            else if (size == 4) {
                modulePrinter.TextInstruction("mov     r10d, r10d");
            }
            else if (size == 2) {
                modulePrinter.TextInstruction("movzx   r10, r10w");
            }
            else {
                modulePrinter.TextInstruction("movzx   r10, r10b");
            }
        }
        return;
    }

    const int size = SizeOfRuntime(type);
    const std::int32_t offset = framePlan.SlotOffsets().at(reg);
    if (IsFloat(type)) {
        modulePrinter.TextInstruction(
            std::format("{:<8}xmm1, {} [rbp - {}]", size == 4 ? "movss" : "movsd", PtrSize(size), offset));
    }
    else if (size == 8 || size == 0) {
        modulePrinter.TextInstruction(std::format("{:<8}r10, qword [rbp - {}]", "mov", offset));
    }
    else if (type.IsSigned()) {
        modulePrinter.TextInstruction(
            std::format("{:<8}r10, {} [rbp - {}]", size == 4 ? "movsxd" : "movsx", PtrSize(size), offset));
    }
    else if (size == 4) {
        modulePrinter.TextInstruction(std::format("{:<8}r10d, dword [rbp - {}]", "mov", offset));
    }
    else {
        modulePrinter.TextInstruction(std::format("{:<8}r10, {} [rbp - {}]", "movzx", PtrSize(size), offset));
    }
}

void AssemblyInstructionPrinter::StoreA(const LirReg reg, const TypeRef &type) {
    const auto physical = framePlan.PhysicalRegisters().find(reg);
    if (physical != framePlan.PhysicalRegisters().end()) {
        modulePrinter.TextInstruction(std::format("{:<8}{}, rax", "mov", PhysicalRegisterName(physical->second)));
        return;
    }

    const int size = SizeOfRuntime(type);
    const int offset = framePlan.SlotOffsets().at(reg);
    if (size == 16) {
        modulePrinter.TextInstruction(std::format("{:<8}qword [rbp - {}], rax", "mov", offset));
        modulePrinter.TextInstruction(std::format("{:<8}qword [rbp - {}], rdx", "mov", offset - 8));
    }
    else if (IsFloat(type)) {
        modulePrinter.TextInstruction(
            std::format("{:<8}{} [rbp - {}], xmm0", size == 4 ? "movss" : "movsd", PtrSize(size), offset));
    }
    else {
        const int storeSize = size > 0 ? size : 8;
        modulePrinter.TextInstruction(
            std::format("{:<8}{} [rbp - {}], {}", "mov", PtrSize(storeSize), offset, GprA(storeSize)));
    }
}

/// Copies an aggregate of any size a chunk at a time, the way the object emitter does. A slot offset names the end of
/// its region, so byte `offset` of the value sits at `[rbp - (slot - offset)]`. Sizing the copy from the type rather
/// than special-casing sixteen bytes matters: anything wider silently copied only its first eight before.
void AssemblyInstructionPrinter::CopyAggregateFromR10ToSlot(const std::int32_t destinationSlot, const int size) const {
    int offset = 0;
    for (const int chunk : {8, 4, 2, 1}) {
        while (offset + chunk <= size) {
            modulePrinter.TextInstruction(
                offset == 0 ? std::format("{:<8}{}, {} [r10]", "mov", GprA(chunk), PtrSize(chunk))
                            : std::format("{:<8}{}, {} [r10 + {}]", "mov", GprA(chunk), PtrSize(chunk), offset));
            modulePrinter.TextInstruction(
                std::format("{:<8}{} [rbp - {}], {}", "mov", PtrSize(chunk), destinationSlot - offset, GprA(chunk)));
            offset += chunk;
        }
    }
}

void AssemblyInstructionPrinter::CopyAggregateFromSlotToR11(const std::int32_t sourceSlot, const int size) const {
    int offset = 0;
    for (const int chunk : {8, 4, 2, 1}) {
        while (offset + chunk <= size) {
            modulePrinter.TextInstruction(
                std::format("{:<8}{}, {} [rbp - {}]", "mov", GprA(chunk), PtrSize(chunk), sourceSlot - offset));
            modulePrinter.TextInstruction(
                offset == 0 ? std::format("{:<8}{} [r11], {}", "mov", PtrSize(chunk), GprA(chunk))
                            : std::format("{:<8}{} [r11 + {}], {}", "mov", PtrSize(chunk), offset, GprA(chunk)));
            offset += chunk;
        }
    }
}

void AssemblyInstructionPrinter::LoadReturnValue(const LirReg reg, const TypeRef &type) {
    const int size = SizeOfRuntime(type);
    if (IsRegPointerTo(reg, type) && (size == 1 || size == 2 || size == 4 || size == 8 || size == 16)) {
        const auto physical = framePlan.PhysicalRegisters().find(reg);
        if (physical != framePlan.PhysicalRegisters().end()) {
            modulePrinter.TextInstruction(std::format("{:<8}r10, {}", "mov", PhysicalRegisterName(physical->second)));
        }
        else {
            modulePrinter.TextInstruction(
                std::format("{:<8}r10, qword [rbp - {}]", "mov", framePlan.SlotOffsets().at(reg)));
        }
        if (size == 16) {
            modulePrinter.TextInstruction(std::format("{:<8}rax, qword [r10]", "mov"));
            modulePrinter.TextInstruction(std::format("{:<8}rdx, qword [r10 + 8]", "mov"));
        }
        else if (size == 8) {
            modulePrinter.TextInstruction(std::format("{:<8}rax, qword [r10]", "mov"));
        }
        else if (size == 4) {
            modulePrinter.TextInstruction(std::format("{:<8}eax, dword [r10]", "mov"));
        }
        else {
            modulePrinter.TextInstruction(std::format("{:<8}eax, {} [r10]", "movzx", PtrSize(size)));
        }
        return;
    }
    LoadA(reg, type);
}

void AssemblyInstructionPrinter::EmitFunctionSetup(const LirFunc &function) {
    modulePrinter.TextBlank();
    modulePrinter.TextComment(std::format("── {} ─", function.name));
    modulePrinter.TextLabel(function.name);
    modulePrinter.TextInstruction("push    rbp");
    modulePrinter.TextInstruction("mov     rbp, rsp");
    for (const int index : framePlan.UsedPhysicalRegisters()) {
        modulePrinter.TextInstruction(std::format("push    {}", PhysicalRegisterName(index)));
    }
    const std::int32_t remainingFrame =
        framePlan.FrameSize() - static_cast<std::int32_t>(framePlan.UsedPhysicalRegisters().size() * 8);
    if (remainingFrame > 0) {
        modulePrinter.TextInstruction(std::format("sub     rsp, {}", remainingFrame));
    }

    if (IsWin64Convention(CallingConvention::Default)) {
        int argumentIndex = 0;
        for (const auto &parameter : function.params) {
            const int size = SizeOf(parameter.type);
            const int offset = framePlan.SlotOffsets().at(parameter.reg);
            if (argumentIndex < 4) {
                if (IsWin64AddressParameter(parameter.type)) {
                    modulePrinter.TextInstruction(
                        std::format("mov     qword [rbp - {}], {}", offset, kWin64IntArgRegs[argumentIndex]));
                }
                else if (IsFloat(parameter.type)) {
                    modulePrinter.TextInstruction(std::format("{:<8}{} [rbp - {}], {}", size == 4 ? "movss" : "movsd",
                                                              PtrSize(size), offset, kFltArgRegs[argumentIndex]));
                }
                else {
                    modulePrinter.TextInstruction(std::format("{:<8}{} [rbp - {}], {}", "mov",
                                                              PtrSize(std::max(size, 1)), offset,
                                                              kWin64IntArgRegs[argumentIndex]));
                }
                ++argumentIndex;
            }
            else {
                const std::int32_t stackArgumentOffset = 48 + (argumentIndex - 4) * 8;
                if (IsWin64AddressParameter(parameter.type)) {
                    modulePrinter.TextInstruction(std::format("mov     rax, qword [rbp + {}]", stackArgumentOffset));
                    modulePrinter.TextInstruction(std::format("mov     qword [rbp - {}], rax", offset));
                }
                else if (IsFloat(parameter.type)) {
                    modulePrinter.TextInstruction(std::format("{:<8}xmm0, {} [rbp + {}]", size == 4 ? "movss" : "movsd",
                                                              PtrSize(size), stackArgumentOffset));
                    modulePrinter.TextInstruction(
                        std::format("{:<8}{} [rbp - {}], xmm0", size == 4 ? "movss" : "movsd", PtrSize(size), offset));
                }
                else {
                    modulePrinter.TextInstruction(
                        std::format("mov     rax, {} [rbp + {}]", PtrSize(std::max(size, 1)), stackArgumentOffset));
                    modulePrinter.TextInstruction(
                        std::format("mov     {} [rbp - {}], rax", PtrSize(std::max(size, 1)), offset));
                }
                ++argumentIndex;
            }
        }
    }
    else {
        int integerArgumentIndex = 0;
        int floatArgumentIndex = 0;
        for (const auto &parameter : function.params) {
            const int size = SizeOf(parameter.type);
            const int offset = framePlan.SlotOffsets().at(parameter.reg);
            if (IsFloat(parameter.type)) {
                if (floatArgumentIndex < 8) {
                    modulePrinter.TextInstruction(std::format("{:<8}{} [rbp - {}], {}", size == 4 ? "movss" : "movsd",
                                                              PtrSize(size), offset, kFltArgRegs[floatArgumentIndex]));
                    ++floatArgumentIndex;
                }
            }
            else if (integerArgumentIndex < 6) {
                modulePrinter.TextInstruction(std::format("{:<8}{} [rbp - {}], {}", "mov", PtrSize(std::max(size, 1)),
                                                          offset, kIntArgRegs[integerArgumentIndex]));
                ++integerArgumentIndex;
            }
        }
    }

    for (const auto &parameter : function.params) {
        const auto physical = framePlan.PhysicalRegisters().find(parameter.reg);
        if (physical == framePlan.PhysicalRegisters().end()) {
            continue;
        }
        const int size = IsWin64AddressParameter(parameter.type) ? 8 : SizeOfRuntime(parameter.type);
        const int offset = framePlan.SlotOffsets().at(parameter.reg);
        if (size == 8 || size == 0) {
            modulePrinter.TextInstruction(std::format("{:<8}rax, qword [rbp - {}]", "mov", offset));
        }
        else if (parameter.type.IsSigned()) {
            modulePrinter.TextInstruction(
                std::format("{:<8}rax, {} [rbp - {}]", size == 4 ? "movsxd" : "movsx", PtrSize(size), offset));
        }
        else if (size == 4) {
            modulePrinter.TextInstruction(std::format("{:<8}eax, dword [rbp - {}]", "mov", offset));
        }
        else {
            modulePrinter.TextInstruction(std::format("{:<8}rax, {} [rbp - {}]", "movzx", PtrSize(size), offset));
        }
        modulePrinter.TextInstruction(std::format("{:<8}{}, rax", "mov", PhysicalRegisterName(physical->second)));
    }
}

int AssemblyInstructionPrinter::ResolveFieldOffset(const LirReg base, const std::string &fieldName) const {
    const auto type = framePlan.RegisterTypes().find(base);
    if (type == framePlan.RegisterTypes().end()) {
        return 0;
    }
    return FieldOffsetOf(type->second, fieldName, layouts, interfaceNames);
}

bool AssemblyInstructionPrinter::EmitArithmetic(const LirInstr &instruction) {
    switch (instruction.op) {
    case LirOpcode::Add:
    case LirOpcode::Sub:
    case LirOpcode::And:
    case LirOpcode::Or:
    case LirOpcode::Xor: {
        const TypeRef &type = instruction.type;
        LoadA(instruction.srcs[0], type);
        LoadB(instruction.srcs[1], type);
        std::string_view opcode;
        if (IsFloat(type)) {
            const bool float32 = type.kind == TypeRef::Kind::Float32;
            switch (instruction.op) {
            case LirOpcode::Add:
                opcode = float32 ? "addss" : "addsd";
                break;
            case LirOpcode::Sub:
                opcode = float32 ? "subss" : "subsd";
                break;
            default:
                opcode = float32 ? "addss" : "addsd";
                break;
            }
            modulePrinter.TextInstruction(std::format("{:<8}xmm0, xmm1", opcode));
        }
        else {
            switch (instruction.op) {
            case LirOpcode::Add:
                opcode = "add";
                break;
            case LirOpcode::Sub:
                opcode = "sub";
                break;
            case LirOpcode::And:
                opcode = "and";
                break;
            case LirOpcode::Or:
                opcode = "or";
                break;
            default:
                opcode = "xor";
                break;
            }
            modulePrinter.TextInstruction(std::format("{:<8}rax, r10", opcode));
        }
        StoreA(instruction.dst, type);
        return true;
    }
    case LirOpcode::Mul: {
        const TypeRef &type = instruction.type;
        LoadA(instruction.srcs[0], type);
        LoadB(instruction.srcs[1], type);
        modulePrinter.TextInstruction(
            IsFloat(type) ? (type.kind == TypeRef::Kind::Float32 ? "mulss   xmm0, xmm1" : "mulsd   xmm0, xmm1")
                          : "imul    rax, r10");
        StoreA(instruction.dst, type);
        return true;
    }
    case LirOpcode::Div:
    case LirOpcode::Mod: {
        const TypeRef &type = instruction.type;
        LoadA(instruction.srcs[0], type);
        LoadB(instruction.srcs[1], type);
        if (IsFloat(type)) {
            modulePrinter.TextInstruction(type.kind == TypeRef::Kind::Float32 ? "divss   xmm0, xmm1"
                                                                              : "divsd   xmm0, xmm1");
        }
        else {
            if (type.IsSigned()) {
                modulePrinter.TextInstruction("cqo");
                modulePrinter.TextInstruction("idiv    r10");
            }
            else {
                modulePrinter.TextInstruction("xor     rdx, rdx");
                modulePrinter.TextInstruction("div     r10");
            }
            if (instruction.op == LirOpcode::Mod) {
                modulePrinter.TextInstruction("mov     rax, rdx");
            }
        }
        StoreA(instruction.dst, type);
        return true;
    }
    case LirOpcode::Pow: {
        const TypeRef &type = instruction.type;
        const int shadowSpace = IsWin64Convention(CallingConvention::Default) ? 32 : 0;
        if (shadowSpace > 0) {
            modulePrinter.TextInstruction(std::format("sub     rsp, {}", shadowSpace));
        }
        if (IsFloat(type)) {
            const bool float32 = type.kind == TypeRef::Kind::Float32;
            modulePrinter.RequestHelper(float32 ? X86_64RuntimeHelper::FloatPower32
                                                : X86_64RuntimeHelper::FloatPower64);
            LoadA(instruction.srcs[0], type);
            LoadB(instruction.srcs[1], type);
            modulePrinter.TextInstruction(float32 ? "call    __rux_powf32" : "call    __rux_powf64");
        }
        else {
            modulePrinter.RequestHelper(X86_64RuntimeHelper::IntegerPower);
            LoadA(instruction.srcs[0], type);
            LoadB(instruction.srcs[1], type);
            modulePrinter.TextInstruction("mov     rcx, rax");
            modulePrinter.TextInstruction("mov     rdx, r10");
            modulePrinter.TextInstruction("call    __rux_ipow");
        }
        if (shadowSpace > 0) {
            modulePrinter.TextInstruction(std::format("add     rsp, {}", shadowSpace));
        }
        StoreA(instruction.dst, type);
        return true;
    }
    case LirOpcode::Shl:
    case LirOpcode::Shr:
    case LirOpcode::Lshr: {
        const TypeRef &type = instruction.type;
        LoadA(instruction.srcs[0], instruction.op == LirOpcode::Lshr ? UnsignedIntegerType(type) : type);
        modulePrinter.TextInstruction(
            std::format("{:<8}r11, qword [rbp - {}]", "mov", framePlan.SlotOffsets().at(instruction.srcs[1])));
        modulePrinter.TextInstruction("mov     rcx, r11");
        if (const bool right = instruction.op == LirOpcode::Shr || instruction.op == LirOpcode::Lshr;
            right && type.IsSigned() && instruction.op != LirOpcode::Lshr) {
            modulePrinter.TextInstruction("sar     rax, cl");
        }
        else if (right) {
            modulePrinter.TextInstruction("shr     rax, cl");
        }
        else {
            modulePrinter.TextInstruction("shl     rax, cl");
        }
        StoreA(instruction.dst, type);
        return true;
    }
    case LirOpcode::Neg: {
        const TypeRef &type = instruction.type;
        LoadA(instruction.srcs[0], type);
        if (IsFloat(type)) {
            if (type.kind == TypeRef::Kind::Float32) {
                const std::string label = modulePrinter.InternFloat32("0x80000000");
                modulePrinter.TextInstruction(std::format("movss   xmm1, dword [rel {}]", label));
                modulePrinter.TextInstruction("xorps   xmm0, xmm1");
            }
            else {
                const std::string label = modulePrinter.InternFloat64("0x8000000000000000");
                modulePrinter.TextInstruction(std::format("movsd   xmm1, qword [rel {}]", label));
                modulePrinter.TextInstruction("xorpd   xmm0, xmm1");
            }
        }
        else {
            modulePrinter.TextInstruction("neg     rax");
        }
        StoreA(instruction.dst, type);
        return true;
    }
    case LirOpcode::Not: {
        LoadA(instruction.srcs[0], instruction.type);
        modulePrinter.TextInstruction("test    rax, rax");
        modulePrinter.TextInstruction("setz    al");
        modulePrinter.TextInstruction("movzx   rax, al");
        StoreA(instruction.dst, TypeRef::MakeBool());
        return true;
    }
    case LirOpcode::BitNot:
        LoadA(instruction.srcs[0], instruction.type);
        modulePrinter.TextInstruction("not     rax");
        StoreA(instruction.dst, instruction.type);
        return true;
    case LirOpcode::CmpEq:
    case LirOpcode::CmpNe:
    case LirOpcode::CmpLt:
    case LirOpcode::CmpLe:
    case LirOpcode::CmpGt:
    case LirOpcode::CmpGe: {
        const auto &registerTypes = framePlan.RegisterTypes();
        const TypeRef &lhsType =
            registerTypes.contains(instruction.srcs[0]) ? registerTypes.at(instruction.srcs[0]) : instruction.type;
        LoadA(instruction.srcs[0], lhsType);
        LoadB(instruction.srcs[1], lhsType);
        std::string_view setOpcode;
        if (IsFloat(lhsType)) {
            modulePrinter.TextInstruction(lhsType.kind == TypeRef::Kind::Float32 ? "ucomiss xmm0, xmm1"
                                                                                 : "ucomisd xmm0, xmm1");
            switch (instruction.op) {
            case LirOpcode::CmpEq:
                setOpcode = "sete";
                break;
            case LirOpcode::CmpNe:
                setOpcode = "setne";
                break;
            case LirOpcode::CmpLt:
                setOpcode = "setb";
                break;
            case LirOpcode::CmpLe:
                setOpcode = "setbe";
                break;
            case LirOpcode::CmpGt:
                setOpcode = "seta";
                break;
            default:
                setOpcode = "setae";
                break;
            }
        }
        else {
            modulePrinter.TextInstruction("cmp     rax, r10");
            const bool signedType = lhsType.IsSigned();
            switch (instruction.op) {
            case LirOpcode::CmpEq:
                setOpcode = "sete";
                break;
            case LirOpcode::CmpNe:
                setOpcode = "setne";
                break;
            case LirOpcode::CmpLt:
                setOpcode = signedType ? "setl" : "setb";
                break;
            case LirOpcode::CmpLe:
                setOpcode = signedType ? "setle" : "setbe";
                break;
            case LirOpcode::CmpGt:
                setOpcode = signedType ? "setg" : "seta";
                break;
            default:
                setOpcode = signedType ? "setge" : "setae";
                break;
            }
        }
        modulePrinter.TextInstruction(std::format("{:<8}al", setOpcode));
        modulePrinter.TextInstruction("movzx   rax, al");
        StoreA(instruction.dst, TypeRef::MakeBool());
        return true;
    }
    case LirOpcode::Cast: {
        const TypeRef &destinationType = instruction.type;
        const auto source = framePlan.RegisterTypes().find(instruction.srcs[0]);
        const TypeRef sourceType = source == framePlan.RegisterTypes().end() ? destinationType : source->second;
        const bool sourceFloat = IsFloat(sourceType);
        const bool destinationFloat = IsFloat(destinationType);
        LoadA(instruction.srcs[0], sourceType);
        if (sourceFloat && !destinationFloat) {
            modulePrinter.TextInstruction(sourceType.kind == TypeRef::Kind::Float32 ? "cvttss2si rax, xmm0"
                                                                                    : "cvttsd2si rax, xmm0");
        }
        else if (!sourceFloat && destinationFloat) {
            modulePrinter.TextInstruction(destinationType.kind == TypeRef::Kind::Float32 ? "cvtsi2ss  xmm0, rax"
                                                                                         : "cvtsi2sd  xmm0, rax");
        }
        else if (sourceFloat && destinationFloat) {
            if (sourceType.kind == TypeRef::Kind::Float32 && destinationType.kind != TypeRef::Kind::Float32) {
                modulePrinter.TextInstruction("cvtss2sd  xmm0, xmm0");
            }
            else if (sourceType.kind != TypeRef::Kind::Float32 && destinationType.kind == TypeRef::Kind::Float32) {
                modulePrinter.TextInstruction("cvtsd2ss  xmm0, xmm0");
            }
        }
        StoreA(instruction.dst, destinationType);
        return true;
    }
    default:
        return false;
    }
}

bool AssemblyInstructionPrinter::EmitMemory(const LirInstr &instruction) {
    switch (instruction.op) {
    case LirOpcode::Const: {
        if (instruction.dst == LirNoReg) {
            return true;
        }
        const TypeRef &type = instruction.type;
        const int size = SizeOf(type);
        if (type.kind == TypeRef::Kind::Str) {
            const std::string label = modulePrinter.InternString(instruction.strArg);
            modulePrinter.TextInstruction(std::format("{:<8}rax, {}", "lea", label));
        }
        else if (type.kind == TypeRef::Kind::Float32) {
            const std::string label = modulePrinter.InternFloat32(instruction.strArg);
            modulePrinter.TextInstruction(std::format("{:<8}xmm0, dword [rel {}]", "movss", label));
            modulePrinter.TextInstruction(
                std::format("{:<8}dword [rbp - {}], xmm0", "movss", framePlan.SlotOffsets().at(instruction.dst)));
            return true;
        }
        else if (type.kind == TypeRef::Kind::Float64) {
            const std::string label = modulePrinter.InternFloat64(instruction.strArg);
            modulePrinter.TextInstruction(std::format("{:<8}xmm0, qword [rel {}]", "movsd", label));
            modulePrinter.TextInstruction(
                std::format("{:<8}qword [rbp - {}], xmm0", "movsd", framePlan.SlotOffsets().at(instruction.dst)));
            return true;
        }
        else if (type.kind == TypeRef::Kind::Bool) {
            const std::string value = instruction.strArg == "true" || instruction.strArg == "1" ? "1" : "0";
            modulePrinter.TextInstruction(std::format("{:<8}rax, {}", "mov", value));
        }
        else {
            modulePrinter.TextInstruction(
                std::format("{:<8}rax, {}", "mov", instruction.strArg.empty() ? "0" : instruction.strArg));
        }
        StoreA(instruction.dst, size > 0 ? type : TypeRef::MakeInt64());
        return true;
    }
    case LirOpcode::Alloca: {
        const std::int32_t dataOffset = framePlan.AllocaDataOffsets().at(instruction.dst);
        modulePrinter.TextInstruction(std::format("{:<8}rax, [rbp - {}]", "lea", dataOffset));
        StoreA(instruction.dst, TypeRef::MakePointer(instruction.type));
        return true;
    }
    case LirOpcode::Load: {
        const TypeRef &type = instruction.type;
        const int size = SizeOfRuntime(type);
        if (!instruction.strArg.empty()) {
            modulePrinter.TextInstruction(std::format("{:<8}rax, [rel {}]", "mov", instruction.strArg));
        }
        else {
            const LirReg pointer = instruction.srcs[0];
            const auto physical = framePlan.PhysicalRegisters().find(pointer);
            if (physical != framePlan.PhysicalRegisters().end()) {
                modulePrinter.TextInstruction(
                    std::format("{:<8}r10, {}", "mov", PhysicalRegisterName(physical->second)));
            }
            else {
                modulePrinter.TextInstruction(
                    std::format("{:<8}r10, qword [rbp - {}]", "mov", framePlan.SlotOffsets().at(pointer)));
            }
            // Anything wider than a register is an aggregate: no scalar reaches nine bytes. Copying it a chunk at a
            // time covers every width, where keying on exactly sixteen left a twenty-four byte value copying its
            // first eight bytes and silently dropping the rest.
            if (size > 8) {
                CopyAggregateFromR10ToSlot(framePlan.SlotOffsets().at(instruction.dst), size);
                return true;
            }
            if (IsFloat(type)) {
                modulePrinter.TextInstruction(
                    std::format("{:<8}xmm0, {} [r10]", size == 4 ? "movss" : "movsd", PtrSize(size)));
                modulePrinter.TextInstruction(std::format("{:<8}{} [rbp - {}], xmm0", size == 4 ? "movss" : "movsd",
                                                          PtrSize(size), framePlan.SlotOffsets().at(instruction.dst)));
                return true;
            }
            if (size == 8 || size == 0) {
                modulePrinter.TextInstruction(std::format("{:<8}rax, qword [r10]", "mov"));
            }
            else if (type.IsSigned()) {
                modulePrinter.TextInstruction(
                    std::format("{:<8}rax, {} [r10]", size == 4 ? "movsxd" : "movsx", PtrSize(size)));
            }
            else if (size == 4) {
                modulePrinter.TextInstruction(std::format("{:<8}eax, dword [r10]", "mov"));
            }
            else {
                modulePrinter.TextInstruction(std::format("{:<8}rax, {} [r10]", "movzx", PtrSize(size)));
            }
        }
        StoreA(instruction.dst, size > 0 ? type : TypeRef::MakeInt64());
        return true;
    }
    case LirOpcode::Store: {
        const LirReg value = instruction.srcs[0];
        const LirReg pointer = instruction.srcs[1];
        const TypeRef &type = instruction.type;
        const int size = SizeOfRuntime(type);
        const auto physical = framePlan.PhysicalRegisters().find(pointer);
        if (physical != framePlan.PhysicalRegisters().end()) {
            modulePrinter.TextInstruction(std::format("{:<8}r11, {}", "mov", PhysicalRegisterName(physical->second)));
        }
        else {
            modulePrinter.TextInstruction(
                std::format("{:<8}r11, qword [rbp - {}]", "mov", framePlan.SlotOffsets().at(pointer)));
        }
        if (size > 8) {
            // As in Load above: any width, a chunk at a time, rather than sixteen bytes exactly.
            CopyAggregateFromSlotToR11(framePlan.SlotOffsets().at(value), size);
        }
        else if (IsFloat(type)) {
            modulePrinter.TextInstruction(std::format("{:<8}xmm0, {} [rbp - {}]", size == 4 ? "movss" : "movsd",
                                                      PtrSize(size), framePlan.SlotOffsets().at(value)));
            modulePrinter.TextInstruction(
                std::format("{:<8}{} [r11], xmm0", size == 4 ? "movss" : "movsd", PtrSize(size)));
        }
        else {
            const int storeSize = size > 0 ? size : 8;
            LoadA(value, type);
            modulePrinter.TextInstruction(std::format("{:<8}{} [r11], {}", "mov", PtrSize(storeSize), GprA(storeSize)));
        }
        return true;
    }
    case LirOpcode::FieldPtr: {
        const LirReg base = instruction.srcs[0];
        LoadA(base, framePlan.RegisterTypes().at(base));
        if (const int fieldOffset = ResolveFieldOffset(base, instruction.strArg); fieldOffset != 0) {
            modulePrinter.TextInstruction(std::format("{:<8}rax, [rax + {}]", "lea", fieldOffset));
        }
        StoreA(instruction.dst, TypeRef::MakePointer(instruction.type));
        return true;
    }
    case LirOpcode::IndexPtr: {
        const LirReg base = instruction.srcs[0];
        const LirReg index = instruction.srcs[1];
        int elementSize = instruction.type.kind == TypeRef::Kind::Pointer && !instruction.type.inner.empty()
                            ? SizeOfRuntime(instruction.type.inner[0])
                            : 8;
        elementSize = std::max(elementSize, 1);
        LoadA(base, framePlan.RegisterTypes().at(base));
        LoadB(index, framePlan.RegisterTypes().at(index));
        modulePrinter.TextInstruction(std::format("{:<8}r11, r10, {}", "imul", elementSize));
        modulePrinter.TextInstruction("add     rax, r11");
        StoreA(instruction.dst, TypeRef::MakePointer(instruction.type));
        return true;
    }
    case LirOpcode::Phi:
        return true;
    case LirOpcode::GlobalAddr:
        modulePrinter.TextInstruction(std::format("{:<8}rax, [rel {}]", "lea", instruction.strArg));
        StoreA(instruction.dst, TypeRef::MakePointer(instruction.type));
        return true;
    case LirOpcode::StringAddr: {
        const TypeRef elementType = instruction.type.inner.empty() ? TypeRef::MakeChar8() : instruction.type.inner[0];
        const std::string label =
            modulePrinter.InternString(EncodeStringLiteral(instruction.strArg, SizeOfRuntime(elementType)));
        modulePrinter.TextInstruction(std::format("{:<8}rax, [rel {}]", "lea", label));
        StoreA(instruction.dst, instruction.type);
        return true;
    }
    default:
        return false;
    }
}
} // namespace Rux
