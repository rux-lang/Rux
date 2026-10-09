// Per-instruction assembly text, including the register and operand-size
// spellings x86-64 syntax requires.

#include "CodeGen/X86_64/AssemblyInstructionPrinter.h"

#include "CodeGen/X86_64/AssemblyModulePrinter.h"
#include "Target/FloatLowering.h"
#include "Target/Platform.h"

#include <algorithm>
#include <format>

namespace Rux {
using namespace Layout;

namespace {
/// The accumulator register's name at a given width — `al`, `ax`, `eax`, `rax` — since x86-64 spells the same
/// register differently depending on the operand size.
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

[[nodiscard]] bool IsWideBool(const TypeRef &type) {
    return type.IsBool() && IsWideScalar(type);
}

/// The text of an instruction that reads or produces a wide bool, mirroring the object emitter's word-by-word lowering
/// and its canonical form: 0 or 1 in word 0, zeros in every word above. The ordinary paths would treat the value as one
/// register, storing a single word of a constant and inverting every word under `~`. Returns false for an instruction
/// that touches no wide bool, or for one the ordinary paths already spell correctly.
[[nodiscard]] bool PrintWideBool(AssemblyInstructionPrinter &printer, AssemblyModulePrinter &text,
                                 const LirInstr &instruction) {
    const auto &types = printer.FramePlan().RegisterTypes();
    const auto source = instruction.srcs.empty() ? types.end() : types.find(instruction.srcs[0]);
    const TypeRef operandType = source == types.end() ? instruction.type : source->second;
    if (instruction.dst == LirNoReg || (!IsWideBool(operandType) && !IsWideBool(instruction.type))) {
        return false;
    }
    const auto &slots = printer.FramePlan().SlotOffsets();
    // A slot offset names the end of its region, so word `index` of a value sits eight bytes nearer the frame base.
    const auto word = [&](const LirReg reg, const int index) {
        return std::format("qword [rbp - {}]", slots.at(reg) - index * 8);
    };
    const auto emit = [&](const std::string_view opcode, const std::string &operands) {
        text.TextInstruction(std::format("{:<8}{}", opcode, operands));
    };
    // Stores the truth value in rax, 0 or 1, as `resultType`, zero-filling the words of a wide one.
    const auto storeTruth = [&](const TypeRef &resultType) {
        if (!IsWideScalar(resultType)) {
            printer.StoreA(instruction.dst, resultType);
            return;
        }
        emit("mov", word(instruction.dst, 0) + ", rax");
        for (int index = 1; index < SizeOf(resultType) / 8; ++index) {
            emit("mov", word(instruction.dst, index) + ", 0");
        }
    };
    const int words = SizeOf(IsWideBool(operandType) ? operandType : instruction.type) / 8;

    switch (instruction.op) {
    case LirOpcode::Const:
        emit("mov", std::format("rax, {}", instruction.strArg == "true" || instruction.strArg == "1" ? 1 : 0));
        storeTruth(instruction.type);
        return true;
    case LirOpcode::And:
    case LirOpcode::Or:
    case LirOpcode::Xor: {
        const std::string_view opcode = instruction.op == LirOpcode::And ? "and"
                                      : instruction.op == LirOpcode::Or  ? "or"
                                                                         : "xor";
        for (int index = 0; index < words; ++index) {
            emit("mov", "rax, " + word(instruction.srcs[0], index));
            emit(opcode, "rax, " + word(instruction.srcs[1], index));
            emit("mov", word(instruction.dst, index) + ", rax");
        }
        return true;
    }
    case LirOpcode::BitNot:
        // Only the truth bit flips; the zero words above it are copied.
        for (int index = 0; index < words; ++index) {
            emit("mov", "rax, " + word(instruction.srcs[0], index));
            if (index == 0) {
                emit("xor", "rax, 1");
            }
            emit("mov", word(instruction.dst, index) + ", rax");
        }
        return true;
    case LirOpcode::Not:
    case LirOpcode::CmpEq:
    case LirOpcode::CmpNe: {
        // Every word is folded into rax: the value's own words for `!`, the words where the two sides differ for an
        // equality.
        emit("xor", "eax, eax");
        for (int index = 0; index < words; ++index) {
            if (instruction.op == LirOpcode::Not) {
                emit("or", "rax, " + word(instruction.srcs[0], index));
                continue;
            }
            emit("mov", "r10, " + word(instruction.srcs[0], index));
            emit("xor", "r10, " + word(instruction.srcs[1], index));
            emit("or", "rax, r10");
        }
        emit("test", "rax, rax");
        emit(instruction.op == LirOpcode::CmpNe ? "setne" : "sete", "al");
        emit("movzx", "rax, al");
        storeTruth(instruction.op == LirOpcode::Not && instruction.type.IsBool() ? instruction.type
                                                                                 : TypeRef::MakeBool());
        return true;
    }
    case LirOpcode::Cast: {
        const TypeRef &destinationType = instruction.type;
        if (destinationType.IsBool()) {
            // The source's truth comes from the words the source has, never from the destination's width.
            if (IsWideScalar(operandType)) {
                emit("xor", "eax, eax");
                for (int index = 0; index < SizeOf(operandType) / 8; ++index) {
                    emit("or", "rax, " + word(instruction.srcs[0], index));
                }
            }
            else {
                printer.LoadA(instruction.srcs[0], operandType);
            }
            if (!operandType.IsBool()) {
                emit("test", "rax, rax");
                emit("setne", "al");
                emit("movzx", "rax, al");
            }
            storeTruth(destinationType);
            return true;
        }
        if (IsWideScalar(destinationType)) {
            // A wide bool widens or narrows to a wide integer like the unsigned value it holds.
            const int destinationWords = SizeOf(destinationType) / 8;
            for (int index = 0; index < destinationWords; ++index) {
                if (index < words) {
                    emit("mov", "rax, " + word(instruction.srcs[0], index));
                    emit("mov", word(instruction.dst, index) + ", rax");
                }
                else {
                    emit("mov", word(instruction.dst, index) + ", 0");
                }
            }
            return true;
        }
        emit("mov", "rax, " + word(instruction.srcs[0], 0));
        if (IsNativeFloat(destinationType)) {
            emit(destinationType.kind == TypeRef::Kind::Float32 ? "cvtsi2ss" : "cvtsi2sd", "xmm0, rax");
        }
        printer.StoreA(instruction.dst, destinationType);
        return true;
    }
    default:
        return false;
    }
}

[[nodiscard]] bool IsFloat80(const TypeRef &type) {
    return type.kind == TypeRef::Kind::Float80;
}

/// The text of the x87 slot traffic for one instruction, mirroring `X87Stack` in `X87Emitter.cpp`: every operand is
/// pushed from its frame slot and the one result popped back, so the register stack is empty between instructions.
/// The first wide temporary holds the caller's control word with the pinned one after it; the second is the spill slot
/// a value reaches the x87 unit through when it has no frame slot of the right width.
class X87Text {
public:
    X87Text(AssemblyInstructionPrinter &printer, AssemblyModulePrinter &text)
        : printer(printer)
        , text(text)
        , controlWords(printer.FramePlan().WideTemporaryOffset(0))
        , spill(printer.FramePlan().WideTemporaryOffset(1)) {
    }

    void Emit(const std::string_view opcode, const std::string_view operands = {}) const {
        text.TextInstruction(operands.empty() ? std::string(opcode) : std::format("{:<8}{}", opcode, operands));
    }

    /// Saves the caller's control word and selects 64-bit precision with round to nearest.
    void BeginRounding() const {
        Emit("fnstcw", std::format("word [rbp - {}]", controlWords));
        Emit("mov", std::format("word [rbp - {}], 0x037F", controlWords - 2));
        Emit("fldcw", std::format("word [rbp - {}]", controlWords - 2));
    }

    /// Restores the control word `BeginRounding` saved.
    void EndRounding() const {
        Emit("fldcw", std::format("word [rbp - {}]", controlWords));
    }

    /// Pushes `reg`: a float80 as it is, a machine float widened exactly, a signed integer through `fild` after it is
    /// sign-extended to 64 bits in the spill slot.
    void Push(const LirReg reg, const TypeRef &type) const {
        const bool physical = printer.FramePlan().PhysicalRegisters().contains(reg);
        if (IsFloat80(type)) {
            Emit("fld", std::format("tword [rbp - {}]", Slot(reg)));
            return;
        }
        if (IsFloat(type)) {
            const std::int32_t address = physical ? Spill(reg, type) : Slot(reg);
            Emit("fld", std::format("{} [rbp - {}]", type.kind == TypeRef::Kind::Float32 ? "dword" : "qword", address));
            return;
        }
        Emit("fild", std::format("qword [rbp - {}]", !physical && SizeOf(type) == 8 ? Slot(reg) : Spill(reg, type)));
    }

    /// Pops the x87 top into `reg`, rounding to `type` under the current control word. A float80 destination gets its
    /// six padding bytes zeroed.
    void Pop(const LirReg reg, const TypeRef &type) const {
        if (IsFloat80(type)) {
            const std::int32_t slot = Slot(reg);
            Emit("fstp", std::format("tword [rbp - {}]", slot));
            Emit("mov", std::format("word [rbp - {}], 0", slot - 10));
            Emit("mov", std::format("dword [rbp - {}], 0", slot - 12));
            return;
        }
        const std::string_view width = type.kind == TypeRef::Kind::Float32 ? "dword" : "qword";
        if (!printer.FramePlan().PhysicalRegisters().contains(reg)) {
            Emit("fstp", std::format("{} [rbp - {}]", width, Slot(reg)));
            return;
        }
        Emit("fstp", std::format("{} [rbp - {}]", width, spill));
        Emit("mov",
             std::format("{}, {} [rbp - {}]", type.kind == TypeRef::Kind::Float32 ? "eax" : "rax", width, spill));
        printer.StoreA(reg, type);
    }

private:
    [[nodiscard]] std::int32_t Slot(const LirReg reg) const {
        return printer.FramePlan().SlotOffsets().at(reg);
    }

    /// Writes `reg` to the spill slot as a 64-bit value and returns the slot.
    [[nodiscard]] std::int32_t Spill(const LirReg reg, const TypeRef &type) const {
        printer.LoadA(reg, type);
        Emit("mov", std::format("qword [rbp - {}], rax", spill));
        return spill;
    }

    AssemblyInstructionPrinter &printer;
    AssemblyModulePrinter &text;
    std::int32_t controlWords;
    std::int32_t spill;
};

/// The text of a float80 instruction the object emitter lowers to x87 (`X86_64FunctionEmitter::EmitX87`), in the same
/// order and on the same frame addresses. Returns false for anything that lowering leaves to the other paths.
[[nodiscard]] bool PrintX87(AssemblyInstructionPrinter &printer, AssemblyModulePrinter &text,
                            const LirInstr &instruction) {
    if (Target::FloatLoweringFor(TypeRef::Kind::Float80, Target::Arch::X86_64) != Target::FloatLowering::X87 ||
        instruction.dst == LirNoReg || instruction.srcs.empty()) {
        return false;
    }
    const auto &types = printer.FramePlan().RegisterTypes();
    const TypeRef sourceType = types.contains(instruction.srcs[0]) ? types.at(instruction.srcs[0]) : instruction.type;
    const TypeRef &type = instruction.type;
    const X87Text x87(printer, text);

    switch (instruction.op) {
    case LirOpcode::Add:
    case LirOpcode::Sub:
    case LirOpcode::Mul:
    case LirOpcode::Div: {
        if (!IsFloat80(type) || instruction.srcs.size() != 2) {
            return false;
        }
        const std::string_view opcode = instruction.op == LirOpcode::Add ? "faddp"
                                      : instruction.op == LirOpcode::Sub ? "fsubp"
                                      : instruction.op == LirOpcode::Mul ? "fmulp"
                                                                         : "fdivp";
        x87.BeginRounding();
        x87.Push(instruction.srcs[0], type);
        x87.Push(instruction.srcs[1], type);
        // Intel operand order: st1 = st1 op st0, then pop, so the left operand pushed first is the minuend.
        x87.Emit(opcode, "st1, st0");
        x87.Pop(instruction.dst, type);
        x87.EndRounding();
        return true;
    }
    case LirOpcode::Mod: {
        if (!IsFloat80(type) || instruction.srcs.size() != 2) {
            return false;
        }
        // st1 = divisor, st0 = dividend; `fprem` leaves C2 (bit 2 of ah after `fnstsw`) set while the reduction is
        // partial. The remainder is exact, so the control word is left alone.
        x87.Push(instruction.srcs[1], type);
        x87.Push(instruction.srcs[0], type);
        const std::string loop = text.CreateLocalLabel(".x87_fprem_");
        text.TextLabel(loop);
        x87.Emit("fprem");
        x87.Emit("fnstsw", "ax");
        x87.Emit("test", "ah, 4");
        x87.Emit("jnz", loop);
        x87.Emit("fstp", "st1");
        x87.Pop(instruction.dst, type);
        return true;
    }
    case LirOpcode::CmpEq:
    case LirOpcode::CmpNe:
    case LirOpcode::CmpLt:
    case LirOpcode::CmpLe:
    case LirOpcode::CmpGt:
    case LirOpcode::CmpGe: {
        if (!IsFloat80(sourceType) || instruction.srcs.size() != 2) {
            return false;
        }
        x87.Push(instruction.srcs[1], sourceType);
        x87.Push(instruction.srcs[0], sourceType);
        x87.Emit("fucomip", "st0, st1");
        x87.Emit("fstp", "st0");
        // The flags read like UCOMISD's: an unordered result sets PF, which makes every comparison but `!=` false.
        std::string_view setOpcode;
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
        const bool unorderedTrue = instruction.op == LirOpcode::CmpNe;
        x87.Emit(setOpcode, "al");
        x87.Emit(unorderedTrue ? "setp" : "setnp", "dl");
        x87.Emit(unorderedTrue ? "or" : "and", "al, dl");
        x87.Emit("movzx", "rax, al");
        printer.StoreA(instruction.dst, TypeRef::MakeBool());
        return true;
    }
    case LirOpcode::Cast:
        if (IsFloat80(type) &&
            (IsFloat(sourceType) || (sourceType.IsInteger() && sourceType.IsSigned() && SizeOf(sourceType) <= 8))) {
            // Every float32, float64 and 64-bit signed integer is exact in float80: nothing rounds.
            x87.Push(instruction.srcs[0], sourceType);
            x87.Pop(instruction.dst, type);
            return true;
        }
        if (IsFloat80(sourceType) && IsFloat(type)) {
            x87.BeginRounding();
            x87.Push(instruction.srcs[0], sourceType);
            x87.Pop(instruction.dst, type);
            x87.EndRounding();
            return true;
        }
        return false;
    default:
        return false;
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

bool AssemblyInstructionPrinter::IsAggregate(const TypeRef &type) const {
    if (IsWideValue(type)) {
        return true;
    }
    if (type.IsRange()) {
        return true;
    }
    // A slice, text included, is a 16-byte {data, length} view whose shape the runtime fixes rather than a declaration.
    if (type.IsSlice()) {
        return true;
    }
    switch (type.kind) {
    case TypeRef::Kind::Tuple:
    case TypeRef::Kind::Array:
    // A native sum, optional, or fallible is a tag followed by its widest payload.
    case TypeRef::Kind::Sum:
    case TypeRef::Kind::Optional:
    case TypeRef::Kind::Fallible:
        return true;
    case TypeRef::Kind::Named: {
        const std::string base = BaseTypeName(type.name);
        return interfaceNames.contains(base) || layouts.contains(base) || (!type.inner.empty() && SizeOf(type) > 8);
    }
    default:
        return false;
    }
}

bool AssemblyInstructionPrinter::IsWin64AddressParameter(const TypeRef &type) const {
    if (type.IsSlice()) {
        return true;
    }
    if (type.kind != TypeRef::Kind::Named) {
        return false;
    }
    const std::string base = BaseTypeName(type.name);
    return interfaceNames.contains(base);
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
        if (size > 0 && size < 8 && !IsAggregate(type)) {
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
    if (size > 8 && size <= 16) {
        modulePrinter.TextInstruction(std::format("{:<8}rax, qword [rbp - {}]", "mov", offset));
        modulePrinter.TextInstruction(std::format("{:<8}rdx, qword [rbp - {}]", "mov", offset - 8));
    }
    else if (IsFloat(type)) {
        modulePrinter.TextInstruction(
            std::format("{:<8}xmm0, {} [rbp - {}]", size == 4 ? "movss" : "movsd", PtrSize(size), offset));
    }
    else if (size == 8 || size == 0 ||
             (IsAggregate(type) && (size > 0 && size < 8 && size != 1 && size != 2 && size != 4))) {
        // An odd-width aggregate (3, 5, 6, 7) has no scalar width; its slot is padded to a word, so the whole
        // word is the lossless move -- the buckets below would keep one byte of it.
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
        if (size > 0 && size < 8 && !IsAggregate(type)) {
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
    else if (size == 8 || size == 0 ||
             (IsAggregate(type) && (size > 0 && size < 8 && size != 1 && size != 2 && size != 4))) {
        // The odd-width aggregate again: the padded slot word is the lossless move.
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
    if (size > 8 && size <= 16) {
        modulePrinter.TextInstruction(std::format("{:<8}qword [rbp - {}], rax", "mov", offset));
        modulePrinter.TextInstruction(std::format("{:<8}qword [rbp - {}], rdx", "mov", offset - 8));
    }
    else if (IsFloat(type)) {
        modulePrinter.TextInstruction(
            std::format("{:<8}{} [rbp - {}], xmm0", size == 4 ? "movss" : "movsd", PtrSize(size), offset));
    }
    else {
        // An odd-width aggregate stores its whole padded slot word; see LoadA.
        const int storeSize =
            (size > 0 && !(IsAggregate(type) && (size > 0 && size < 8 && size != 1 && size != 2 && size != 4))) ? size
                                                                                                                : 8;
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
    if (IsRegPointerTo(reg, type) && (size == 1 || size == 2 || size == 4 || size == 8 || (size > 8 && size <= 16))) {
        const auto physical = framePlan.PhysicalRegisters().find(reg);
        if (physical != framePlan.PhysicalRegisters().end()) {
            modulePrinter.TextInstruction(std::format("{:<8}r10, {}", "mov", PhysicalRegisterName(physical->second)));
        }
        else {
            modulePrinter.TextInstruction(
                std::format("{:<8}r10, qword [rbp - {}]", "mov", framePlan.SlotOffsets().at(reg)));
        }
        if (size > 8 && size <= 16) {
            modulePrinter.TextInstruction(std::format("{:<8}rax, qword [r10]", "mov"));
            if (size == 16) {
                modulePrinter.TextInstruction("mov     rdx, qword [r10 + 8]");
            }
            else {
                modulePrinter.TextInstruction("xor     edx, edx");
                for (int offset = size - 1; offset >= 8; --offset) {
                    modulePrinter.TextInstruction("shl     rdx, 8");
                    modulePrinter.TextInstruction(std::format("movzx   ecx, byte [r10 + {}]", offset));
                    modulePrinter.TextInstruction("or      rdx, rcx");
                }
            }
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

    if (IsWin64Convention(function.callConv)) {
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
        int integerArgumentIndex = framePlan.HiddenReturnOffset() != 0 ? 1 : 0;
        int floatArgumentIndex = 0;
        int stackOffset = 16;
        if (framePlan.HiddenReturnOffset() != 0) {
            modulePrinter.TextInstruction(std::format("mov     qword [rbp - {}], rdi", framePlan.HiddenReturnOffset()));
        }
        for (const auto &parameter : function.params) {
            const int size = SizeOfRuntime(parameter.type);
            const int offset = framePlan.SlotOffsets().at(parameter.reg);
            const bool aggregate = IsAggregate(parameter.type);
            const int words = aggregate ? std::max(1, AlignUp(size, 8) / 8) : 1;
            if (IsFloat(parameter.type) && floatArgumentIndex < 8) {
                modulePrinter.TextInstruction(std::format("{:<8}{} [rbp - {}], {}", size == 4 ? "movss" : "movsd",
                                                          PtrSize(size), offset, kFltArgRegs[floatArgumentIndex++]));
            }
            else if (!IsFloat(parameter.type) && words <= 2 && integerArgumentIndex + words <= 6) {
                for (int word = 0; word < words; ++word) {
                    modulePrinter.TextInstruction(std::format("mov     qword [rbp - {}], {}", offset - word * 8,
                                                              kIntArgRegs[integerArgumentIndex++]));
                }
            }
            else {
                for (int word = 0; word < words; ++word) {
                    modulePrinter.TextInstruction(std::format("mov     rax, qword [rbp + {}]", stackOffset));
                    modulePrinter.TextInstruction(std::format("mov     qword [rbp - {}], rax", offset - word * 8));
                    stackOffset += 8;
                }
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
    if (PrintX87(*this, modulePrinter, instruction) || PrintWideBool(*this, modulePrinter, instruction)) {
        return true;
    }
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
        // As in the object emitter, `~` on a bool is logical negation, which keeps it 0 or 1.
        modulePrinter.TextInstruction(instruction.type.IsBool() ? "xor     rax, 1" : "not     rax");
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
            const bool single = sourceType.kind == TypeRef::Kind::Float32;
            const std::string_view truncate = single ? "cvttss2si rax, xmm0" : "cvttsd2si rax, xmm0";
            if (destinationType.IsInteger() && !destinationType.IsSigned() && SizeOf(destinationType) == 8) {
                // As in the binary emitter: a value of 2^63 or more is reduced below it and its top bit restored.
                const std::string bound = single ? modulePrinter.InternFloat32("9223372036854775808.0")
                                                 : modulePrinter.InternFloat64("9223372036854775808.0");
                const std::string large = modulePrinter.CreateLocalLabel("u64.large");
                const std::string done = modulePrinter.CreateLocalLabel("u64.done");
                modulePrinter.TextInstruction(single ? std::format("movss   xmm1, dword [rel {}]", bound)
                                                     : std::format("movsd   xmm1, qword [rel {}]", bound));
                modulePrinter.TextInstruction(single ? "ucomiss xmm0, xmm1" : "ucomisd xmm0, xmm1");
                modulePrinter.TextInstruction(std::format("jae     {}", large));
                modulePrinter.TextInstruction(truncate);
                modulePrinter.TextInstruction(std::format("jmp     {}", done));
                modulePrinter.TextLabel(large);
                modulePrinter.TextInstruction(single ? "subss   xmm0, xmm1" : "subsd   xmm0, xmm1");
                modulePrinter.TextInstruction(truncate);
                modulePrinter.TextInstruction("btc     rax, 63");
                modulePrinter.TextLabel(done);
            }
            else {
                modulePrinter.TextInstruction(truncate);
            }
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
        if (instruction.dst == LirNoReg || PrintWideBool(*this, modulePrinter, instruction)) {
            return true;
        }
        const TypeRef &type = instruction.type;
        const int size = SizeOf(type);
        if (type.kind == TypeRef::Kind::Float32) {
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
        else if (type.IsBool()) {
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
            if (size > 8 || (IsAggregate(type) && (size > 0 && size < 8 && size != 1 && size != 2 && size != 4))) {
                // Wider than a register, or an odd width no scalar move spells: chunked, because a rounded-up
                // load would read past the object and a rounded-down one would lose its tail.
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
        // A value occupying no bytes has nothing to move, and the eight-byte fallback below is for a width the back
        // end could not work out, which is a different thing. Storing a word for a zero-sized field wrote over
        // whatever followed it -- for a node whose value sits between its key and its links, that is the links.
        if (size == 0 && type.kind == TypeRef::Kind::Named) {
            return true;
        }
        const auto physical = framePlan.PhysicalRegisters().find(pointer);
        if (physical != framePlan.PhysicalRegisters().end()) {
            modulePrinter.TextInstruction(std::format("{:<8}r11, {}", "mov", PhysicalRegisterName(physical->second)));
        }
        else {
            modulePrinter.TextInstruction(
                std::format("{:<8}r11, qword [rbp - {}]", "mov", framePlan.SlotOffsets().at(pointer)));
        }
        if (size > 8 || (IsAggregate(type) && (size > 0 && size < 8 && size != 1 && size != 2 && size != 4))) {
            // As in Load above: any width, a chunk at a time, rather than sixteen bytes exactly -- and the odd
            // widths (3, 5, 6, 7), which no scalar store spells without touching what follows the object.
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
