// Per-instruction assembly text, including the register and operand-size spellings x86-64 syntax requires.
//
// Every path here is the text of one object-emitter path — `X86_64FunctionEmitter` (FunctionEmitter.cpp,
// WideIntegerEmitter.cpp, SoftwareFloatEmitter.cpp, X87Emitter.cpp) and the module emitter's prologue and operand moves
// (RcuEmitter.cpp, ModuleValues.cpp) — in the same order and on the same frame addresses. A divergence is a bug in
// whichever side is wrong: the text is only worth reading if it is what the object code does.

#include "CodeGen/X86_64/AssemblyInstructionPrinter.h"

#include "CodeGen/IntegerLiteral.h"
#include "CodeGen/X86_64/AssemblyModulePrinter.h"
#include "CodeGen/X86_64/ScalarAddress.h"
#include "CodeGen/X86_64/ScalarCast.h"
#include "CodeGen/X86_64/ScalarMachine.h"
#include "CodeGen/X86_64/ScalarMemory.h"
#include "Numeric/FloatFormat.h"
#include "Numeric/FloatParsing.h"
#include "Target/FloatLowering.h"
#include "Target/Platform.h"
#include "Types/PrimitiveCatalog.h"

#include <algorithm>
#include <format>
#include <limits>
#include <vector>

namespace Rux {
using namespace Layout;

namespace {
/// The size keyword of a scalar move the emitter makes at `bytes`: a quadword, doubleword or word at exactly those
/// widths and a byte at any other, which is where the emitter's own width buckets send them.
std::string_view Width(const int bytes) {
    switch (bytes) {
    case 8:
        return "qword";
    case 4:
        return "dword";
    case 2:
        return "word";
    default:
        return "byte";
    }
}

/// The accumulator at the width `Width` names.
std::string_view Accumulator(const int bytes) {
    switch (bytes) {
    case 8:
        return "rax";
    case 4:
        return "eax";
    case 2:
        return "ax";
    default:
        return "al";
    }
}

/// `[base + offset]`, or `[base]` at offset zero.
std::string Indexed(const std::string_view size, const std::string_view base, const std::int32_t offset) {
    return offset == 0 ? std::format("{} [{}]", size, base) : std::format("{} [{} + {}]", size, base, offset);
}

/// An imm64 operand, spelled as the bit pattern the register receives.
std::string Imm64(const std::uint64_t value) {
    return std::format("0x{:X}", value);
}

[[nodiscard]] bool IsFloat80(const TypeRef &type) {
    return type->kind == TypeRef::Kind::Float80;
}

/// The text of the x87 slot traffic for one instruction, mirroring `X87Stack` in `X87Emitter.cpp`: every operand is
/// pushed from its frame slot and the one result popped back, so the register stack is empty between instructions.
/// The first wide temporary holds the caller's control word with the pinned one after it; the second is the spill slot
/// a value reaches the x87 unit through when it has no frame slot of the right width.
class X87Text {
public:
    X87Text(AssemblyInstructionPrinter &printer)
        : printer(printer)
        , controlWords(-printer.FramePlan().WideTemporaryOffset(0))
        , spill(-printer.FramePlan().WideTemporaryOffset(1)) {
    }

    void Emit(const std::string_view opcode, const std::string_view operands = {}) const {
        printer.Instruction(opcode, operands);
    }

    /// Saves the caller's control word and selects 64-bit precision with round to nearest.
    void BeginRounding() const {
        Emit("fnstcw", AssemblyInstructionPrinter::Rbp("word", controlWords));
        Emit("mov", AssemblyInstructionPrinter::Rbp("word", controlWords + 2) + ", 0x037F");
        Emit("fldcw", AssemblyInstructionPrinter::Rbp("word", controlWords + 2));
    }

    /// Restores the control word `BeginRounding` saved.
    void EndRounding() const {
        Emit("fldcw", AssemblyInstructionPrinter::Rbp("word", controlWords));
    }

    /// Pushes `reg`: a float80 as it is, a machine float widened exactly, a signed integer through `fild` after it is
    /// sign-extended to 64 bits in the spill slot.
    void Push(const LirReg reg, const TypeRef &type) const {
        const bool physical =
            printer.FramePlan().PhysicalRegisters().contains(reg) || printer.FramePlan().FloatRegisters().contains(reg);
        if (IsFloat80(type)) {
            Emit("fld", AssemblyInstructionPrinter::Rbp("tword", printer.Disp(reg)));
            return;
        }
        if (IsFloat(type)) {
            const std::int32_t address = physical ? Spill(reg, type) : printer.Disp(reg);
            Emit("fld",
                 AssemblyInstructionPrinter::Rbp(type->kind == TypeRef::Kind::Float32 ? "dword" : "qword", address));
            return;
        }
        Emit("fild", AssemblyInstructionPrinter::Rbp("qword", !physical && SizeOf(type) == 8 ? printer.Disp(reg)
                                                                                             : Spill(reg, type)));
    }

    /// Pops the x87 top into `reg`, rounding to `type` under the current control word. A float80 destination gets its
    /// six padding bytes zeroed.
    void Pop(const LirReg reg, const TypeRef &type) const {
        if (IsFloat80(type)) {
            const std::int32_t slot = printer.Disp(reg);
            Emit("fstp", AssemblyInstructionPrinter::Rbp("tword", slot));
            Emit("mov", AssemblyInstructionPrinter::Rbp("word", slot + 10) + ", 0");
            Emit("mov", AssemblyInstructionPrinter::Rbp("dword", slot + 12) + ", 0");
            return;
        }
        const bool float32 = type->kind == TypeRef::Kind::Float32;
        const std::string_view width = float32 ? "dword" : "qword";
        if (!printer.FramePlan().PhysicalRegisters().contains(reg) &&
            !printer.FramePlan().FloatRegisters().contains(reg)) {
            Emit("fstp", AssemblyInstructionPrinter::Rbp(width, printer.Disp(reg)));
            return;
        }
        Emit("fstp", AssemblyInstructionPrinter::Rbp(width, spill));
        Emit(float32 ? "movss" : "movsd", "xmm0, " + AssemblyInstructionPrinter::Rbp(width, spill));
        printer.StoreA(reg, type);
    }

private:
    /// Writes `reg` to the spill slot as a 64-bit value and returns the slot.
    [[nodiscard]] std::int32_t Spill(const LirReg reg, const TypeRef &type) const {
        printer.LoadA(reg, type);
        if (IsFloat(type)) {
            const bool single = type->kind == TypeRef::Kind::Float32;
            Emit(single ? "movss" : "movsd",
                 AssemblyInstructionPrinter::Rbp(single ? "dword" : "qword", spill) + ", xmm0");
        }
        else {
            Emit("mov", AssemblyInstructionPrinter::Rbp("qword", spill) + ", rax");
        }
        return spill;
    }

    AssemblyInstructionPrinter &printer;
    std::int32_t controlWords;
    std::int32_t spill;
};

/// The `setcc` text for the flags an SSE `ucomis*` or an x87 `fucomip` leaves: an unordered result sets PF, which makes
/// every comparison but `!=` false. Mirrors the emitter's SSE comparison and `SetComparison` in `X87Emitter.cpp`.
void PrintFloatComparison(const AssemblyInstructionPrinter &printer, const LirOpcode op) {
    std::string_view setOpcode;
    switch (op) {
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
    const bool unorderedTrue = op == LirOpcode::CmpNe;
    printer.Instruction(setOpcode, "al");
    printer.Instruction(unorderedTrue ? "setp" : "setnp", "dl");
    printer.Instruction(unorderedTrue ? "or" : "and", "al, dl");
}

/// The `fprem` loop of a float32, float64 or float80 remainder, mirroring `EmitRemainder` in `X87Emitter.cpp`: st1 =
/// divisor, st0 = dividend; `fprem` leaves C2 (bit 2 of ah after `fnstsw`) set while the reduction is partial. The
/// remainder is exact, so the control word is left alone.
void PrintRemainder(AssemblyInstructionPrinter &printer, const X87Text &x87, const LirInstr &instruction) {
    x87.Push(instruction.srcs[1], instruction.type);
    x87.Push(instruction.srcs[0], instruction.type);
    const std::string loop = printer.ModulePrinter().CreateLocalLabel(".x87_fprem_");
    printer.ModulePrinter().TextLabel(loop);
    x87.Emit("fprem");
    x87.Emit("fnstsw", "ax");
    x87.Emit("test", "ah, 4");
    x87.Emit("jnz", loop);
    x87.Emit("fstp", "st1");
    x87.Pop(instruction.dst, instruction.type);
}

/// The text of an instruction the object emitter lowers to x87 (`X86_64FunctionEmitter::EmitX87`), in the same order
/// and on the same frame addresses: float80 arithmetic, comparisons and conversions, and every float32 and float64
/// remainder. Returns false for anything that lowering leaves to the other paths.
[[nodiscard]] bool PrintX87(AssemblyInstructionPrinter &printer, const LirInstr &instruction) {
    if (instruction.dst == LirNoReg || instruction.srcs.empty()) {
        return false;
    }
    const X87Text x87(printer);
    if (instruction.op == LirOpcode::Mod && IsFloat(instruction.type) && instruction.srcs.size() == 2) {
        PrintRemainder(printer, x87, instruction);
        return true;
    }
    if (Target::FloatLoweringFor(TypeRef::Kind::Float80, Target::Arch::X86_64) != Target::FloatLowering::X87) {
        return false;
    }
    const auto &types = printer.FramePlan().RegisterTypes();
    const TypeRef sourceType = types.contains(instruction.srcs[0]) ? types.at(instruction.srcs[0]) : instruction.type;
    const TypeRef &type = instruction.type;

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
    case LirOpcode::Mod:
        if (!IsFloat80(type) || instruction.srcs.size() != 2) {
            return false;
        }
        PrintRemainder(printer, x87, instruction);
        return true;
    case LirOpcode::CmpEq:
    case LirOpcode::CmpNe:
    case LirOpcode::CmpLt:
    case LirOpcode::CmpLe:
    case LirOpcode::CmpGt:
    case LirOpcode::CmpGe:
        if (!IsFloat80(sourceType) || instruction.srcs.size() != 2) {
            return false;
        }
        x87.Push(instruction.srcs[1], sourceType);
        x87.Push(instruction.srcs[0], sourceType);
        x87.Emit("fucomip", "st0, st1");
        x87.Emit("fstp", "st0");
        PrintFloatComparison(printer, instruction.op);
        x87.Emit("movzx", "rax, al");
        printer.StoreA(instruction.dst, TypeRef::MakeBool());
        return true;
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

AssemblyModulePrinter &AssemblyInstructionPrinter::ModulePrinter() const {
    return modulePrinter;
}

void AssemblyInstructionPrinter::Instruction(const std::string_view opcode, const std::string_view operands) const {
    modulePrinter.TextInstruction(operands.empty() ? std::string(opcode) : std::format("{:<7} {}", opcode, operands));
}

std::string AssemblyInstructionPrinter::Rbp(const std::int32_t displacement) {
    return displacement < 0 ? std::format("[rbp - {}]", -static_cast<std::int64_t>(displacement))
                            : std::format("[rbp + {}]", displacement);
}

std::string AssemblyInstructionPrinter::Rbp(const std::string_view size, const std::int32_t displacement) {
    return std::format("{} {}", size, Rbp(displacement));
}

std::int32_t AssemblyInstructionPrinter::Disp(const LirReg reg) const {
    return -framePlan.SlotOffsets().at(reg);
}

std::string AssemblyInstructionPrinter::Label(const std::string_view purpose) const {
    return modulePrinter.CreateLocalLabel(std::format(".{}_", purpose));
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
    switch (type->kind) {
    case TypeRef::Kind::Tuple:
    case TypeRef::Kind::Array:
    // A native sum, optional, or fallible is a tag followed by its widest payload.
    case TypeRef::Kind::Sum:
    case TypeRef::Kind::Optional:
    case TypeRef::Kind::Fallible:
        return true;
    case TypeRef::Kind::Named: {
        const std::string base = BaseTypeName(type->name);
        return interfaceNames.contains(base) || layouts.contains(base) || (!type->inner.empty() && SizeOf(type) > 8);
    }
    default:
        return false;
    }
}

bool AssemblyInstructionPrinter::IsWin64ByRefAggregate(const TypeRef &type) const {
    if (!IsAggregate(type)) {
        return false;
    }
    const int size = SizeOfRuntime(type);
    return size > 0 && size != 1 && size != 2 && size != 4 && size != 8;
}

bool AssemblyInstructionPrinter::IsSysVMemoryAggregate(const TypeRef &type) const {
    return IsAggregate(type) && SizeOfRuntime(type) > 16;
}

bool AssemblyInstructionPrinter::IsWin64AddressParameter(const TypeRef &type) const {
    if (type.IsSlice()) {
        return true;
    }
    if (type->kind != TypeRef::Kind::Named) {
        return false;
    }
    const std::string base = BaseTypeName(type->name);
    return interfaceNames.contains(base);
}

bool AssemblyInstructionPrinter::IsRegPointerTo(const LirReg reg, const TypeRef &pointee) const {
    const auto &registerTypes = framePlan.RegisterTypes();
    const auto type = registerTypes.find(reg);
    return type != registerTypes.end() && type->second->kind == TypeRef::Kind::Pointer &&
           !type->second->inner.empty() && type->second->inner[0] == pointee;
}

std::string_view AssemblyInstructionPrinter::PhysicalRegisterName(const int index) const {
    if (index < 0 || index >= static_cast<int>(X86AllocatedGprs.size()))
        return "rbx";
    return X86GeneralRegisterName(X86AllocatedGprs[static_cast<std::size_t>(index)]);
}

// ---- operand moves (ModuleValues.cpp) ----------------------------------------------------------------------------

void AssemblyInstructionPrinter::LoadA(const LirReg reg, const TypeRef &type) {
    if (const auto found = framePlan.FloatRegisters().find(reg); found != framePlan.FloatRegisters().end()) {
        if (IsFloat(type)) {
            Instruction("movaps", std::format("xmm0, xmm{}", found->second));
        }
        else {
            Instruction(SizeOfRuntime(type) == 8 ? "movq" : "movd",
                        std::format("{}, xmm{}", SizeOfRuntime(type) == 8 ? "rax" : "eax", found->second));
        }
        return;
    }
    const int size = SizeOfRuntime(type);
    if (const auto physical = framePlan.PhysicalRegisters().find(reg);
        physical != framePlan.PhysicalRegisters().end()) {
        Instruction("mov", std::format("rax, {}", PhysicalRegisterName(physical->second)));
        if (size > 0 && size < 8 && !IsAggregate(type)) {
            if (type.IsSigned()) {
                Instruction(size == 4 ? "movsxd" : "movsx", size == 4 ? "rax, eax" : size == 2 ? "rax, ax" : "rax, al");
            }
            else if (size == 4) {
                Instruction("mov", "eax, eax");
            }
            else {
                Instruction("movzx", size == 2 ? "rax, ax" : "rax, al");
            }
        }
        return;
    }
    const std::int32_t displacement = Disp(reg);
    if (IsAggregate(type) && size > 0 && size < 8 && size != 1 && size != 2 && size != 4) {
        // A 3-, 5-, 6- or 7-byte aggregate has no scalar width; its slot is padded to a word, so the whole word is the
        // lossless move.
        Instruction("mov", "rax, " + Rbp("qword", displacement));
        return;
    }
    if (size > 8 && size <= 16) {
        Instruction("mov", "rax, " + Rbp("qword", displacement));
        Instruction("mov", "r10, " + Rbp("qword", displacement + 8));
        Instruction("mov", "rdx, r10");
    }
    else if (IsFloat(type)) {
        const bool float32 = type->kind == TypeRef::Kind::Float32;
        Instruction(float32 ? "movss" : "movsd", "xmm0, " + Rbp(float32 ? "dword" : "qword", displacement));
    }
    else if (size == 8 || size == 0) {
        Instruction("mov", "rax, " + Rbp("qword", displacement));
    }
    else if (type.IsSigned()) {
        Instruction(size == 4 ? "movsxd" : "movsx", "rax, " + Rbp(Width(size), displacement));
    }
    else if (size == 4) {
        Instruction("mov", "eax, " + Rbp("dword", displacement));
    }
    else {
        Instruction("movzx", "rax, " + Rbp(size == 2 ? "word" : "byte", displacement));
    }
}

void AssemblyInstructionPrinter::LoadB(const LirReg reg, const TypeRef &type) {
    if (const auto found = framePlan.FloatRegisters().find(reg); found != framePlan.FloatRegisters().end()) {
        if (IsFloat(type)) {
            Instruction("movaps", std::format("xmm1, xmm{}", found->second));
        }
        else {
            Instruction(SizeOfRuntime(type) == 8 ? "movq" : "movd",
                        std::format("{}, xmm{}", SizeOfRuntime(type) == 8 ? "r10" : "r10d", found->second));
        }
        return;
    }
    const int size = SizeOfRuntime(type);
    if (const auto physical = framePlan.PhysicalRegisters().find(reg);
        physical != framePlan.PhysicalRegisters().end()) {
        Instruction("mov", std::format("r10, {}", PhysicalRegisterName(physical->second)));
        if (size > 0 && size < 8 && !IsAggregate(type)) {
            if (type.IsSigned()) {
                Instruction(size == 4 ? "movsxd" : "movsx", size == 4   ? "r10, r10d"
                                                            : size == 2 ? "r10, r10w"
                                                                        : "r10, r10b");
            }
            else if (size == 4) {
                Instruction("mov", "r10d, r10d");
            }
            else {
                Instruction("movzx", size == 2 ? "r10, r10w" : "r10, r10b");
            }
        }
        return;
    }
    const std::int32_t displacement = Disp(reg);
    if (IsAggregate(type) && size > 0 && size < 8 && size != 1 && size != 2 && size != 4) {
        Instruction("mov", "r10, " + Rbp("qword", displacement));
        return;
    }
    if (IsFloat(type)) {
        const bool float32 = type->kind == TypeRef::Kind::Float32;
        Instruction(float32 ? "movss" : "movsd", "xmm1, " + Rbp(float32 ? "dword" : "qword", displacement));
    }
    else if (size == 8 || size == 0) {
        Instruction("mov", "r10, " + Rbp("qword", displacement));
    }
    else if (type.IsSigned()) {
        Instruction(size == 4 ? "movsxd" : "movsx", "r10, " + Rbp(Width(size), displacement));
    }
    else if (size == 4) {
        Instruction("mov", "r10d, " + Rbp("dword", displacement));
    }
    else {
        Instruction("movzx", "r10, " + Rbp(size == 2 ? "word" : "byte", displacement));
    }
}

void AssemblyInstructionPrinter::StoreA(const LirReg reg, const TypeRef &type) {
    if (const auto found = framePlan.FloatRegisters().find(reg); found != framePlan.FloatRegisters().end()) {
        Instruction("movaps", std::format("xmm{}, xmm0", found->second));
        return;
    }
    if (const auto physical = framePlan.PhysicalRegisters().find(reg);
        physical != framePlan.PhysicalRegisters().end()) {
        Instruction("mov", std::format("{}, rax", PhysicalRegisterName(physical->second)));
        return;
    }
    StoreStack(reg, type);
}

void AssemblyInstructionPrinter::StoreStack(const LirReg reg, const TypeRef &type) {
    const int size = SizeOfRuntime(type);
    const std::int32_t displacement = Disp(reg);
    if (size > 8 && size <= 16) {
        Instruction("mov", Rbp("qword", displacement) + ", rax");
        Instruction("mov", Rbp("qword", displacement + 8) + ", rdx");
    }
    else if (IsFloat(type)) {
        const bool float32 = type->kind == TypeRef::Kind::Float32;
        Instruction(float32 ? "movss" : "movsd", Rbp(float32 ? "dword" : "qword", displacement) + ", xmm0");
    }
    else {
        int storeSize = size > 0 ? size : 8;
        if (IsAggregate(type) && storeSize < 8 && storeSize != 1 && storeSize != 2 && storeSize != 4) {
            // The odd-width aggregate's slot is padded to a word, so the word is stored.
            storeSize = 8;
        }
        Instruction("mov", std::format("{}, {}", Rbp(Width(storeSize), displacement), Accumulator(storeSize)));
    }
}

void AssemblyInstructionPrinter::LoadChunkFromR10(const std::int32_t offset, const int size) const {
    switch (size) {
    case 8:
        Instruction("mov", "rax, " + Indexed("qword", "r10", offset));
        break;
    case 4:
        Instruction("mov", "eax, " + Indexed("dword", "r10", offset));
        break;
    default:
        Instruction("movzx", "eax, " + Indexed(size == 2 ? "word" : "byte", "r10", offset));
        break;
    }
}

void AssemblyInstructionPrinter::StoreChunkToR11(const std::int32_t offset, const int size) const {
    Instruction("mov", std::format("{}, {}", Indexed(Width(size), "r11", offset), Accumulator(size)));
}

/// Copies an aggregate of any size a chunk at a time, the way the object emitter does: sizing the copy from the type
/// rather than special-casing sixteen bytes matters, since anything wider silently copied only its first eight before.
void AssemblyInstructionPrinter::CopyAggregateFromR10ToStack(const std::int32_t destination, const int size) const {
    std::int32_t offset = 0;
    for (const int chunk : {8, 4, 2, 1}) {
        while (offset + chunk <= size) {
            LoadChunkFromR10(offset, chunk);
            Instruction("mov", std::format("{}, {}", Rbp(Width(chunk), destination + offset), Accumulator(chunk)));
            offset += chunk;
        }
    }
}

void AssemblyInstructionPrinter::CopyAggregateFromR10ToR11(const int size) {
    std::int32_t offset = 0;
    for (const int chunk : {8, 4, 2, 1}) {
        while (offset + chunk <= size) {
            LoadChunkFromR10(offset, chunk);
            StoreChunkToR11(offset, chunk);
            offset += chunk;
        }
    }
}

void AssemblyInstructionPrinter::StoreHiddenReturnValue(const LirReg reg, const TypeRef &type) {
    Instruction("mov", "r11, " + Rbp("qword", -framePlan.HiddenReturnOffset()));
    if (IsRegPointerTo(reg, type)) {
        if (const auto physical = framePlan.PhysicalRegisters().find(reg);
            physical != framePlan.PhysicalRegisters().end()) {
            Instruction("mov", std::format("r10, {}", PhysicalRegisterName(physical->second)));
        }
        else {
            Instruction("mov", "r10, " + Rbp("qword", Disp(reg)));
        }
    }
    else {
        Instruction("lea", "r10, " + Rbp(Disp(reg)));
    }
    CopyAggregateFromR10ToR11(SizeOfRuntime(type));
    Instruction("mov", "rax, r11");
}

void AssemblyInstructionPrinter::LoadReturnValue(const LirReg reg, const TypeRef &type) {
    const int size = SizeOfRuntime(type);
    if (IsRegPointerTo(reg, type) && (size == 1 || size == 2 || size == 4 || size == 8 || (size > 8 && size <= 16))) {
        if (const auto physical = framePlan.PhysicalRegisters().find(reg);
            physical != framePlan.PhysicalRegisters().end()) {
            Instruction("mov", std::format("r10, {}", PhysicalRegisterName(physical->second)));
        }
        else {
            Instruction("mov", "r10, " + Rbp("qword", Disp(reg)));
        }
        if (size > 8 && size <= 16) {
            // A returned pointee can end at the last byte of an allocation: only its live tail bytes are packed.
            if (size < 16) {
                Instruction("mov", "eax, 0");
                for (int offset = size - 1; offset >= 8; --offset) {
                    Instruction("shl", "rax, 8");
                    Instruction("movzx", "edx, " + Indexed("byte", "r10", offset));
                    Instruction("or", "rax, rdx");
                }
                Instruction("mov", "rdx, rax");
            }
            else {
                Instruction("mov", "rdx, " + Indexed("qword", "r10", 8));
            }
            Instruction("mov", "rax, qword [r10]");
        }
        else {
            LoadChunkFromR10(0, size);
        }
        return;
    }
    if (size > 8 && size <= 16) {
        Instruction("mov", "rax, " + Rbp("qword", Disp(reg)));
        Instruction("mov", "r10, " + Rbp("qword", Disp(reg) + 8));
        Instruction("mov", "rdx, r10");
    }
    else {
        LoadA(reg, type);
    }
}

// ---- prologue (RcuEmitter.cpp, GenFunc) --------------------------------------------------------------------------

void AssemblyInstructionPrinter::EmitStackAllocation(std::int32_t bytes) const {
    constexpr std::int32_t pageSize = 4096;
    while (bytes > pageSize) {
        Instruction("sub", std::format("rsp, {}", pageSize));
        Instruction("test", "qword [rsp], rax");
        bytes -= pageSize;
    }
    if (bytes > 0) {
        Instruction("sub", std::format("rsp, {}", bytes));
        if (bytes == pageSize) {
            Instruction("test", "qword [rsp], rax");
        }
    }
}

void AssemblyInstructionPrinter::EmitFunctionSetup(const LirFunc &function) {
    modulePrinter.TextBlank();
    modulePrinter.TextComment(std::format("── {} ─", function.name));
    modulePrinter.TextLabel(function.name);
    Instruction("push", "rbp");
    Instruction("mov", "rbp, rsp");
    for (const int index : framePlan.UsedPhysicalRegisters()) {
        Instruction("push", PhysicalRegisterName(index));
    }
    EmitStackAllocation(framePlan.FrameSize() -
                        static_cast<std::int32_t>(framePlan.UsedPhysicalRegisters().size() * 8));
    for (const auto &[reg, offset] : framePlan.FloatSaves()) {
        Instruction("movdqu", std::format("{}, xmm{}", Rbp("oword", -offset), reg));
    }

    // The incoming argument registers and stack slots are spilled to the parameters' frame slots. A value no register
    // holds whole arrives by address under Win64, and is copied into the slot here, so the body reads its own copy.
    const bool win64 = IsWin64Convention(function.callConv);
    int integerIndex = 0;
    int floatIndex = 0;
    int stackIndex = 0;
    int win64Index = 0;
    if (framePlan.HiddenReturnOffset() != 0) {
        if (win64) {
            Instruction("mov", Rbp("qword", -framePlan.HiddenReturnOffset()) + ", rcx");
            win64Index = 1;
        }
        else {
            Instruction("mov", Rbp("qword", -framePlan.HiddenReturnOffset()) + ", rdi");
            integerIndex = 1;
        }
    }
    for (const auto &parameter : function.params) {
        const int size = SizeOf(parameter.type);
        const std::int32_t displacement = Disp(parameter.reg);
        const std::string_view floatMove = size == 4 ? "movss" : "movsd";
        const std::string_view floatWidth = size == 4 ? "dword" : "qword";
        if (win64) {
            if (win64Index >= 4) {
                // Above the return address, the saved rbp and the 32-byte home area.
                const std::int32_t stackArgument = 48 + (win64Index - 4) * 8;
                if (IsWin64AddressParameter(parameter.type)) {
                    Instruction("mov", "rax, " + Rbp("qword", stackArgument));
                    Instruction("mov", Rbp("qword", displacement) + ", rax");
                }
                else if (IsWin64ByRefAggregate(parameter.type)) {
                    Instruction("mov", "r10, " + Rbp("qword", stackArgument));
                    CopyAggregateFromR10ToStack(displacement, SizeOfRuntime(parameter.type));
                }
                else if (IsFloat(parameter.type)) {
                    Instruction(floatMove, "xmm0, " + Rbp(floatWidth, stackArgument));
                    Instruction(floatMove, Rbp(floatWidth, displacement) + ", xmm0");
                }
                else {
                    Instruction("mov", "rax, " + Rbp("qword", stackArgument));
                    StoreStack(parameter.reg, parameter.type);
                }
                ++win64Index;
                continue;
            }
            const std::string_view argument = kWin64IntArgRegs[win64Index];
            if (IsWin64AddressParameter(parameter.type)) {
                Instruction("mov", std::format("rax, {}", argument));
                Instruction("mov", Rbp("qword", displacement) + ", rax");
            }
            else if (IsWin64ByRefAggregate(parameter.type)) {
                Instruction("mov", std::format("r10, {}", argument));
                CopyAggregateFromR10ToStack(displacement, SizeOfRuntime(parameter.type));
            }
            else if (IsFloat(parameter.type)) {
                Instruction(floatMove, std::format("{}, {}", Rbp(floatWidth, displacement), kFltArgRegs[win64Index]));
            }
            else {
                Instruction("mov", std::format("rax, {}", argument));
                StoreStack(parameter.reg, parameter.type);
            }
            ++win64Index;
            continue;
        }
        if (IsSysVMemoryAggregate(parameter.type)) {
            const int words = AlignUp(SizeOfRuntime(parameter.type), 8);
            for (int offset = 0; offset < words; offset += 8) {
                Instruction("mov", "rax, " + Rbp("qword", 16 + stackIndex++ * 8));
                Instruction("mov", Rbp("qword", displacement + offset) + ", rax");
            }
        }
        else if (IsFloat(parameter.type)) {
            if (floatIndex < 8) {
                Instruction(floatMove, std::format("{}, {}", Rbp(floatWidth, displacement), kFltArgRegs[floatIndex]));
                ++floatIndex;
            }
            else {
                const std::int32_t stackArgument = 16 + stackIndex++ * 8;
                Instruction(floatMove, "xmm0, " + Rbp(floatWidth, stackArgument));
                Instruction(floatMove, Rbp(floatWidth, displacement) + ", xmm0");
            }
        }
        else if (IsAggregate(parameter.type) && SizeOfRuntime(parameter.type) > 8 &&
                 SizeOfRuntime(parameter.type) <= 16) {
            if (integerIndex <= 4) {
                Instruction("mov", std::format("{}, {}", Rbp("qword", displacement), kIntArgRegs[integerIndex++]));
                Instruction("mov", std::format("{}, {}", Rbp("qword", displacement + 8), kIntArgRegs[integerIndex++]));
            }
            else {
                Instruction("mov", "rax, " + Rbp("qword", 16 + stackIndex++ * 8));
                Instruction("mov", Rbp("qword", displacement) + ", rax");
                Instruction("mov", "rax, " + Rbp("qword", 16 + stackIndex++ * 8));
                Instruction("mov", Rbp("qword", displacement + 8) + ", rax");
            }
        }
        else if (integerIndex < 6) {
            Instruction("mov", std::format("{}, {}", Rbp("qword", displacement), kIntArgRegs[integerIndex++]));
        }
        else {
            Instruction("mov", "rax, " + Rbp("qword", 16 + stackIndex++ * 8));
            StoreStack(parameter.reg, parameter.type);
        }
    }

    // Parameters the allocator gave a callee-saved register are loaded into it.
    for (const auto &parameter : function.params) {
        const auto physical = framePlan.PhysicalRegisters().find(parameter.reg);
        if (physical == framePlan.PhysicalRegisters().end()) {
            continue;
        }
        const int size = IsWin64AddressParameter(parameter.type) ? 8 : SizeOfRuntime(parameter.type);
        const std::int32_t displacement = Disp(parameter.reg);
        if (size == 8 || size == 0) {
            Instruction("mov", "rax, " + Rbp("qword", displacement));
        }
        else if (parameter.type.IsSigned()) {
            Instruction(size == 4 ? "movsxd" : "movsx", "rax, " + Rbp(Width(size), displacement));
        }
        else if (size == 4) {
            Instruction("mov", "eax, " + Rbp("dword", displacement));
        }
        else {
            Instruction("movzx", "rax, " + Rbp(size == 2 ? "word" : "byte", displacement));
        }
        Instruction("mov", std::format("{}, rax", PhysicalRegisterName(physical->second)));
    }
    for (const auto &parameter : function.params) {
        if (const auto found = framePlan.FloatRegisters().find(parameter.reg);
            found != framePlan.FloatRegisters().end()) {
            const bool single = parameter.type->kind == TypeRef::Kind::Float32;
            Instruction(single ? "movss" : "movsd", "xmm0, " + Rbp(single ? "dword" : "qword", Disp(parameter.reg)));
            Instruction("movaps", std::format("xmm{}, xmm0", found->second));
        }
    }
}

int AssemblyInstructionPrinter::ResolveFieldOffset(const LirReg base, const std::string &fieldName) const {
    const auto type = framePlan.RegisterTypes().find(base);
    if (type == framePlan.RegisterTypes().end()) {
        return 0;
    }
    return FieldOffsetOf(type->second, fieldName, layouts, interfaceNames);
}

// ---- wide values (WideIntegerEmitter.cpp, SoftwareFloatEmitter.cpp, EmitWideArithmetic) ---------------------------

void AssemblyInstructionPrinter::LoadWord(const std::uint64_t word) const {
    if (word <= 0x7FFF'FFFF) {
        Instruction("mov", std::format("eax, {}", word));
    }
    else {
        Instruction("mov", "rax, " + Imm64(word));
    }
}

void AssemblyInstructionPrinter::ZeroWide(const std::int32_t destination, const int size) const {
    Instruction("mov", "eax, 0");
    for (int offset = 0; offset < size; offset += 8) {
        Instruction("mov", Rbp("qword", destination + offset) + ", rax");
    }
}

void AssemblyInstructionPrinter::CopyWide(const std::int32_t source, const std::int32_t destination,
                                          const int size) const {
    if (source == destination) {
        return;
    }
    for (int offset = 0; offset < size; offset += 8) {
        Instruction("mov", "rax, " + Rbp("qword", source + offset));
        Instruction("mov", Rbp("qword", destination + offset) + ", rax");
    }
}

void AssemblyInstructionPrinter::NegateWide(const std::int32_t value, const int size) const {
    for (int offset = 0; offset < size; offset += 8) {
        Instruction("mov", "eax, 0"); // mov preserves the borrow from the preceding word
        Instruction(offset == 0 ? "sub" : "sbb", "rax, " + Rbp("qword", value + offset));
        Instruction("mov", Rbp("qword", value + offset) + ", rax");
    }
}

void AssemblyInstructionPrinter::MultiplyWide(const std::int32_t left, const std::int32_t right,
                                              const std::int32_t destination, const int size) const {
    ZeroWide(destination, size);
    const int words = size / 8;
    for (int leftWord = 0; leftWord < words; ++leftWord) {
        Instruction("xor", "r10d, r10d"); // the carry into the next column
        for (int rightWord = 0; rightWord < words - leftWord; ++rightWord) {
            const std::int32_t column = destination + (leftWord + rightWord) * 8;
            Instruction("mov", "rax, " + Rbp("qword", left + leftWord * 8));
            Instruction("mul", Rbp("qword", right + rightWord * 8));
            Instruction("add", "rax, " + Rbp("qword", column));
            Instruction("adc", "rdx, 0");
            Instruction("add", "rax, r10");
            Instruction("adc", "rdx, 0");
            Instruction("mov", Rbp("qword", column) + ", rax");
            Instruction("mov", "r10, rdx");
        }
    }
}

bool AssemblyInstructionPrinter::EmitWideSoftwareFloatNegation(const LirInstr &instruction) {
    const auto &types = framePlan.RegisterTypes();
    const TypeRef operandType = !instruction.srcs.empty() && types.contains(instruction.srcs[0])
                                  ? types.at(instruction.srcs[0])
                                  : instruction.type;
    if (instruction.dst == LirNoReg || !IsSoftwareFloat(operandType) || instruction.op != LirOpcode::Neg) {
        return false;
    }
    // Negation flips the sign bit and nothing else, at whatever width the format stores.
    const PrimitiveInfo *primitive = FindPrimitive(operandType->kind);
    const int size = SizeOf(operandType);
    const int signBit = primitive ? static_cast<int>(primitive->bits - 1) : size * 8 - 1;
    if (size <= 8) {
        LoadA(instruction.srcs.at(0), operandType);
        Instruction("xor", std::format("rax, 0x{:X}", std::uint32_t{1} << signBit));
        StoreA(instruction.dst, instruction.type);
        return true;
    }
    const std::int32_t signWord = Disp(instruction.dst) + (signBit / 64) * 8;
    CopyWide(Disp(instruction.srcs.at(0)), Disp(instruction.dst), size);
    Instruction("mov", "r10, " + Rbp("qword", signWord));
    Instruction("mov", "rax, " + Imm64(std::uint64_t{1} << (signBit % 64)));
    Instruction("xor", "rax, r10");
    Instruction("mov", Rbp("qword", signWord) + ", rax");
    return true;
}

void AssemblyInstructionPrinter::EmitSoftwareFloatConstant(const LirInstr &instruction) {
    // The literal is encoded at compile time and its storage written word by word, as an integer of its width.
    const int size = SizeOf(instruction.type);
    const PrimitiveInfo *primitive = FindPrimitive(instruction.type->kind);
    const FloatFormat *format = primitive ? FindFloatFormat(primitive->bits) : nullptr;
    const auto encoding = format ? ParseFloatEncoding(instruction.strArg, *format) : std::nullopt;
    const WideInteger bits = encoding ? encoding->Bits() : WideInteger::Zero(static_cast<std::uint32_t>(size * 8));
    if (size <= 8) {
        LoadWord(bits.Word64(0));
        StoreA(instruction.dst, instruction.type);
        return;
    }
    for (int offset = 0; offset < size; offset += 8) {
        LoadWord(bits.Word64(static_cast<std::size_t>(offset / 8)));
        Instruction("mov", Rbp("qword", Disp(instruction.dst) + offset) + ", rax");
    }
}

void AssemblyInstructionPrinter::EmitWideDivision(const LirInstr &instruction, const TypeRef &wideType) {
    const int size = SizeOf(wideType);
    const int words = size / 8;
    const std::int32_t destination = Disp(instruction.dst);
    const std::int32_t left = Disp(instruction.srcs.at(0));
    const std::int32_t right = Disp(instruction.srcs.at(1));
    const std::int32_t remainder = -framePlan.WideTemporaryOffset(0);
    const std::int32_t dividend = -framePlan.WideTemporaryOffset(1);
    const std::int32_t divisor = -framePlan.WideTemporaryOffset(2);
    const auto word = [](const std::int32_t base, const int index) { return Rbp("qword", base + index * 8); };

    // A zero divisor traps.
    const std::string nonZero = Label("wide_divisor");
    Instruction("mov", "eax, 0");
    for (int index = 0; index < words; ++index) {
        Instruction("mov", "r10, " + word(right, index));
        Instruction("or", "rax, r10");
    }
    Instruction("test", "rax, rax");
    Instruction("jne", nonZero);
    Instruction("ud2");
    modulePrinter.TextLabel(nonZero);

    if (wideType.IsSigned()) {
        // So does the one quotient that overflows: the most negative value divided by -1.
        const std::string notOverflow = Label("wide_no_overflow");
        Instruction("mov", "rax, " + Imm64(static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::min())));
        Instruction("mov", "r10, " + Rbp("qword", left + size - 8));
        Instruction("cmp", "rax, r10");
        Instruction("jne", notOverflow);
        Instruction("mov", "eax, 0");
        for (int index = 0; index < words - 1; ++index) {
            Instruction("mov", "r10, " + word(left, index));
            Instruction("cmp", "rax, r10");
            Instruction("jne", notOverflow);
        }
        Instruction("mov", "rax, " + Imm64(~std::uint64_t{0}));
        for (int index = 0; index < words; ++index) {
            Instruction("mov", "r10, " + word(right, index));
            Instruction("cmp", "rax, r10");
            Instruction("jne", notOverflow);
        }
        Instruction("ud2");
        modulePrinter.TextLabel(notOverflow);
    }

    // The magnitudes are divided, and the signs applied afterwards.
    CopyWide(left, dividend, size);
    CopyWide(right, divisor, size);
    if (wideType.IsSigned()) {
        for (const std::int32_t operand : {dividend, divisor}) {
            const std::string positive = Label("wide_positive");
            Instruction("mov", "rax, " + Rbp("qword", operand + size - 8));
            Instruction("test", "rax, rax");
            Instruction("jns", positive);
            NegateWide(operand, size);
            modulePrinter.TextLabel(positive);
        }
    }

    // Restoring shift-and-subtract division, one bit of quotient per pass.
    ZeroWide(destination, size);
    ZeroWide(remainder, size);
    Instruction("mov", std::format("rcx, {}", size * 8));
    const std::string loop = Label("wide_divide");
    const std::string less = Label("wide_divide_less");
    const std::string greater = Label("wide_divide_subtract");
    modulePrinter.TextLabel(loop);
    for (int index = 0; index < words; ++index) {
        Instruction(index == 0 ? "shl" : "rcl", word(destination, index) + ", 1");
    }
    for (int index = 0; index < words; ++index) {
        Instruction(index == 0 ? "shl" : "rcl", word(dividend, index) + ", 1");
    }
    for (int index = 0; index < words; ++index) {
        Instruction("rcl", word(remainder, index) + ", 1");
    }
    for (int index = words - 1; index >= 0; --index) {
        Instruction("mov", "rax, " + word(remainder, index));
        Instruction("mov", "r10, " + word(divisor, index));
        Instruction("cmp", "rax, r10");
        Instruction("jb", less);
        Instruction("ja", greater);
    }
    modulePrinter.TextLabel(greater);
    for (int index = 0; index < words; ++index) {
        Instruction("mov", "rax, " + word(remainder, index));
        Instruction("mov", "r10, " + word(divisor, index));
        Instruction(index == 0 ? "sub" : "sbb", "rax, r10");
        Instruction("mov", word(remainder, index) + ", rax");
    }
    Instruction("or", word(destination, 0) + ", 1");
    modulePrinter.TextLabel(less);
    Instruction("dec", "rcx");
    Instruction("jne", loop);

    if (instruction.op == LirOpcode::Mod) {
        // The remainder takes the dividend's sign.
        CopyWide(remainder, destination, size);
        if (wideType.IsSigned()) {
            const std::string positive = Label("wide_positive");
            Instruction("mov", "rax, " + Rbp("qword", left + size - 8));
            Instruction("test", "rax, rax");
            Instruction("jns", positive);
            NegateWide(destination, size);
            modulePrinter.TextLabel(positive);
        }
    }
    else if (wideType.IsSigned()) {
        // The quotient is negative when exactly one operand is.
        const std::string positive = Label("wide_positive");
        Instruction("mov", "rax, " + Rbp("qword", left + size - 8));
        Instruction("mov", "r10, " + Rbp("qword", right + size - 8));
        Instruction("xor", "rax, r10");
        Instruction("test", "rax, rax");
        Instruction("jns", positive);
        NegateWide(destination, size);
        modulePrinter.TextLabel(positive);
    }
}

bool AssemblyInstructionPrinter::EmitWideArithmetic(const LirInstr &instruction) {
    const auto &types = framePlan.RegisterTypes();
    const TypeRef operandType = !instruction.srcs.empty() && types.contains(instruction.srcs[0])
                                  ? types.at(instruction.srcs[0])
                                  : instruction.type;
    if (EmitWideSoftwareFloatNegation(instruction)) {
        return true;
    }
    // A wide bool and a wide character share these word-by-word paths with a wide integer; the cases that would break
    // a wide bool's canonical form (one in word 0 or nothing, zeros above) single it out.
    if (instruction.dst == LirNoReg || (!IsWideScalar(operandType) && !IsWideScalar(instruction.type))) {
        return false;
    }
    const TypeRef wideType = IsWideScalar(operandType) ? operandType : instruction.type;
    const int size = SizeOf(wideType);
    const int words = size / 8;
    const std::int32_t destination = Disp(instruction.dst);
    const auto source = [&](const std::size_t index) { return Disp(instruction.srcs.at(index)); };
    const auto word = [](const std::int32_t base, const int index) { return Rbp("qword", base + index * 8); };
    // Stores a truth value already in rax, 0 or 1, as `resultType`: in word 0 with zeros above for a wide bool.
    const auto storeTruth = [&](const TypeRef &resultType) {
        if (!IsWideScalar(resultType)) {
            StoreA(instruction.dst, resultType);
            return;
        }
        Instruction("mov", word(destination, 0) + ", rax");
        Instruction("mov", "eax, 0");
        for (int offset = 8; offset < SizeOf(resultType); offset += 8) {
            Instruction("mov", Rbp("qword", destination + offset) + ", rax");
        }
    };

    switch (instruction.op) {
    case LirOpcode::Add:
    case LirOpcode::Sub:
    case LirOpcode::And:
    case LirOpcode::Or:
    case LirOpcode::Xor:
        for (int index = 0; index < words; ++index) {
            Instruction("mov", "rax, " + word(source(0), index));
            Instruction("mov", "r10, " + word(source(1), index));
            std::string_view opcode;
            switch (instruction.op) {
            case LirOpcode::Add:
                opcode = index == 0 ? "add" : "adc";
                break;
            case LirOpcode::Sub:
                opcode = index == 0 ? "sub" : "sbb";
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
            Instruction(opcode, "rax, r10");
            Instruction("mov", word(destination, index) + ", rax");
        }
        return true;
    case LirOpcode::Mul:
        MultiplyWide(source(0), source(1), destination, size);
        return true;
    case LirOpcode::Div:
    case LirOpcode::Mod:
        EmitWideDivision(instruction, wideType);
        return true;
    case LirOpcode::Neg:
        CopyWide(source(0), destination, size);
        NegateWide(destination, size);
        return true;
    case LirOpcode::BitNot:
        for (int index = 0; index < words; ++index) {
            Instruction("mov", "rax, " + word(source(0), index));
            if (!wideType.IsBool()) {
                Instruction("not", "rax");
            }
            else if (index == 0) {
                // As for a narrow bool, `~` is logical negation: only the truth bit flips.
                Instruction("xor", "rax, 1");
            }
            Instruction("mov", word(destination, index) + ", rax");
        }
        return true;
    case LirOpcode::Not:
        Instruction("mov", "eax, 0");
        for (int index = 0; index < words; ++index) {
            Instruction("mov", "r10, " + word(source(0), index));
            Instruction("or", "rax, r10");
        }
        Instruction("test", "rax, rax");
        Instruction("sete", "al");
        Instruction("movzx", "rax, al");
        storeTruth(instruction.type.IsBool() ? instruction.type : TypeRef::MakeBool());
        return true;
    case LirOpcode::Shl:
    case LirOpcode::Shr:
    case LirOpcode::Lshr: {
        // Whole words while 64 or more of the count remain, then every word by the rest with shld/shrd, the count
        // reduced modulo the width.
        CopyWide(source(0), destination, size);
        if (const auto physical = framePlan.PhysicalRegisters().find(instruction.srcs[1]);
            physical != framePlan.PhysicalRegisters().end()) {
            Instruction("mov", std::format("r11, {}", PhysicalRegisterName(physical->second)));
            Instruction("mov", "rcx, r11");
        }
        else {
            Instruction("mov", "rcx, " + Rbp("qword", source(1)));
        }
        const bool arithmetic = instruction.op == LirOpcode::Shr && wideType.IsSigned();
        Instruction("and", std::format("rcx, {}", size * 8 - 1));
        const std::string wordLoop = Label("wide_shift_words");
        const std::string bits = Label("wide_shift_bits");
        const std::string done = Label("wide_shift_done");
        modulePrinter.TextLabel(wordLoop);
        Instruction("cmp", "rcx, 64");
        Instruction("jb", bits);
        if (instruction.op == LirOpcode::Shl) {
            for (int index = words - 1; index > 0; --index) {
                Instruction("mov", "rax, " + word(destination, index - 1));
                Instruction("mov", word(destination, index) + ", rax");
            }
            Instruction("mov", "eax, 0");
            Instruction("mov", word(destination, 0) + ", rax");
        }
        else {
            for (int index = 0; index < words - 1; ++index) {
                Instruction("mov", "rax, " + word(destination, index + 1));
                Instruction("mov", word(destination, index) + ", rax");
            }
            Instruction("mov", "rax, " + word(destination, words - 1));
            if (arithmetic) {
                Instruction("sar", "rax, 63");
            }
            else {
                Instruction("mov", "eax, 0");
            }
            Instruction("mov", word(destination, words - 1) + ", rax");
        }
        Instruction("sub", "rcx, 64");
        Instruction("jmp", wordLoop);
        modulePrinter.TextLabel(bits);
        Instruction("test", "rcx, rcx");
        Instruction("je", done);
        if (instruction.op == LirOpcode::Shl) {
            for (int index = words - 1; index > 0; --index) {
                Instruction("mov", "rax, " + word(destination, index));
                Instruction("mov", "r10, " + word(destination, index - 1));
                Instruction("shld", "rax, r10, cl");
                Instruction("mov", word(destination, index) + ", rax");
            }
            Instruction("shl", word(destination, 0) + ", cl");
        }
        else {
            for (int index = 0; index < words - 1; ++index) {
                Instruction("mov", "rax, " + word(destination, index));
                Instruction("mov", "r10, " + word(destination, index + 1));
                Instruction("shrd", "rax, r10, cl");
                Instruction("mov", word(destination, index) + ", rax");
            }
            Instruction(arithmetic ? "sar" : "shr", word(destination, words - 1) + ", cl");
        }
        modulePrinter.TextLabel(done);
        return true;
    }
    case LirOpcode::CmpEq:
    case LirOpcode::CmpNe: {
        const bool equal = instruction.op == LirOpcode::CmpEq;
        const std::string different = Label("wide_different");
        const std::string done = Label("wide_compared");
        Instruction("mov", std::format("eax, {}", equal ? 1 : 0));
        for (int index = 0; index < words; ++index) {
            Instruction("mov", "r10, " + word(source(0), index));
            Instruction("mov", "rax, " + word(source(1), index));
            Instruction("cmp", "rax, r10");
            Instruction("jne", different);
        }
        Instruction("mov", std::format("eax, {}", equal ? 1 : 0));
        Instruction("jmp", done);
        modulePrinter.TextLabel(different);
        Instruction("mov", std::format("eax, {}", equal ? 0 : 1));
        modulePrinter.TextLabel(done);
        StoreA(instruction.dst, TypeRef::MakeBool());
        return true;
    }
    case LirOpcode::CmpLt:
    case LirOpcode::CmpLe:
    case LirOpcode::CmpGt:
    case LirOpcode::CmpGe: {
        // Word by word from the top; the first that differs decides, and only the top word is signed.
        const std::string less = Label("wide_less");
        const std::string greater = Label("wide_greater");
        const std::string done = Label("wide_ordered");
        for (int index = words - 1; index >= 0; --index) {
            const bool signedWord = index == words - 1 && wideType.IsSigned();
            Instruction("mov", "rax, " + word(source(0), index));
            Instruction("mov", "r10, " + word(source(1), index));
            Instruction("cmp", "rax, r10");
            Instruction(signedWord ? "jl" : "jb", less);
            Instruction(signedWord ? "jg" : "ja", greater);
        }
        const bool equalResult = instruction.op == LirOpcode::CmpLe || instruction.op == LirOpcode::CmpGe;
        const bool lessResult = instruction.op == LirOpcode::CmpLt || instruction.op == LirOpcode::CmpLe;
        const bool greaterResult = instruction.op == LirOpcode::CmpGt || instruction.op == LirOpcode::CmpGe;
        Instruction("mov", std::format("eax, {}", equalResult ? 1 : 0));
        Instruction("jmp", done);
        modulePrinter.TextLabel(less);
        Instruction("mov", std::format("eax, {}", lessResult ? 1 : 0));
        Instruction("jmp", done);
        modulePrinter.TextLabel(greater);
        Instruction("mov", std::format("eax, {}", greaterResult ? 1 : 0));
        modulePrinter.TextLabel(done);
        StoreA(instruction.dst, TypeRef::MakeBool());
        return true;
    }
    case LirOpcode::Cast: {
        const TypeRef &destinationType = instruction.type;
        if (destinationType.IsBool()) {
            // The source's truth comes from the words the source has, never from the destination's width.
            if (IsWideScalar(operandType)) {
                Instruction("mov", "eax, 0");
                for (int index = 0; index < SizeOf(operandType) / 8; ++index) {
                    Instruction("mov", "r10, " + word(source(0), index));
                    Instruction("or", "rax, r10");
                }
            }
            else {
                LoadA(instruction.srcs[0], operandType);
            }
            if (!operandType.IsBool()) {
                Instruction("test", "rax, rax");
                Instruction("setne", "al");
                Instruction("movzx", "rax, al");
            }
            storeTruth(destinationType);
            return true;
        }
        if (wideType.IsBool() && IsNativeFloat(destinationType)) {
            // A wide bool's value is word 0, 0 or 1, which converts as the integer it is.
            Instruction("mov", "rax, " + word(source(0), 0));
            Instruction(destinationType->kind == TypeRef::Kind::Float32 ? "cvtsi2ss" : "cvtsi2sd", "xmm0, rax");
            StoreA(instruction.dst, destinationType);
            return true;
        }
        if (IsWideScalar(destinationType)) {
            // Widened by the source's signedness, or truncated; a bool is never signed, so it zero-extends.
            const int destinationSize = SizeOf(destinationType);
            if (!IsWideScalar(operandType)) {
                LoadA(instruction.srcs[0], operandType);
                Instruction("mov", word(destination, 0) + ", rax");
                if (operandType.IsSigned()) {
                    Instruction("sar", "rax, 63");
                }
                else {
                    Instruction("mov", "eax, 0");
                }
                for (int offset = 8; offset < destinationSize; offset += 8) {
                    Instruction("mov", Rbp("qword", destination + offset) + ", rax");
                }
                return true;
            }
            const int copiedSize = std::min(size, destinationSize);
            CopyWide(source(0), destination, copiedSize);
            if (destinationSize > copiedSize) {
                Instruction("mov", "eax, 0");
                if (wideType.IsSigned()) {
                    Instruction("mov", "r10, " + Rbp("qword", source(0) + size - 8));
                    Instruction("mov", "rax, r10");
                    Instruction("sar", "rax, 63");
                }
                for (int offset = copiedSize; offset < destinationSize; offset += 8) {
                    Instruction("mov", Rbp("qword", destination + offset) + ", rax");
                }
            }
            return true;
        }
        Instruction("mov", "rax, " + word(source(0), 0));
        StoreA(instruction.dst, destinationType);
        return true;
    }
    default:
        return false;
    }
}

// ---- scalar arithmetic (FunctionEmitter.cpp, EmitArithmetic) ------------------------------------------------------

bool AssemblyInstructionPrinter::EmitArithmetic(const LirInstr &instruction) {
    if (const auto selected = SelectScalarCastInstruction(instruction, framePlan)) {
        Instruction(ScalarCastMnemonic(*selected), ScalarCastOperands(*selected));
        return true;
    }
    if (framePlan.BranchComparisons().contains(instruction.dst)) {
        return true;
    }
    if (const auto selected = SelectScalarMachineInstruction(instruction, framePlan)) {
        const auto destination = ScalarMachineRegister(*selected, selected->destination);
        const bool immediateMultiply = selected->immediate && selected->opcode == 0x69;
        if (ScalarMachineNeedsCopy(*selected)) {
            Instruction(ScalarMachineIsFloat(selected->registerClass) ? "movaps" : "mov",
                        destination + ", " + ScalarMachineRegister(*selected, selected->left));
        }
        if (ScalarMachineIsZeroShift(*selected))
            return true;
        if (selected->immediate) {
            const auto operands = immediateMultiply
                                    ? destination + ", " + ScalarMachineRegister(*selected, selected->left) + ", "
                                    : destination + ", ";
            Instruction(selected->mnemonic, operands + std::to_string(*selected->immediate));
        }
        else {
            Instruction(selected->mnemonic, destination + ", " + ScalarMachineRegister(*selected, selected->right));
        }
        return true;
    }
    // The x87 forms are checked first: a conversion between float80 and float32 or float64 must not reach the SSE
    // paths below, and neither must a float32 or float64 remainder.
    if (PrintX87(*this, instruction) || EmitWideArithmetic(instruction)) {
        return true;
    }
    const auto &registerTypes = framePlan.RegisterTypes();
    switch (instruction.op) {
    case LirOpcode::Add:
    case LirOpcode::Sub:
    case LirOpcode::And:
    case LirOpcode::Or:
    case LirOpcode::Xor: {
        const TypeRef &type = instruction.type;
        LoadA(instruction.srcs[0], type);
        LoadB(instruction.srcs[1], type);
        if (IsFloat(type)) {
            const bool float32 = type->kind == TypeRef::Kind::Float32;
            // Bitwise operations are not produced for floating values; malformed LIR keeps the emitter's add.
            const bool subtract = instruction.op == LirOpcode::Sub;
            Instruction(subtract ? (float32 ? "subss" : "subsd") : (float32 ? "addss" : "addsd"), "xmm0, xmm1");
        }
        else {
            std::string_view opcode;
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
            Instruction(opcode, "rax, r10");
        }
        StoreA(instruction.dst, type);
        return true;
    }
    case LirOpcode::Mul: {
        const TypeRef &type = instruction.type;
        LoadA(instruction.srcs[0], type);
        LoadB(instruction.srcs[1], type);
        if (IsFloat(type)) {
            Instruction(type->kind == TypeRef::Kind::Float32 ? "mulss" : "mulsd", "xmm0, xmm1");
        }
        else {
            Instruction("imul", "rax, r10");
        }
        StoreA(instruction.dst, type);
        return true;
    }
    case LirOpcode::Div:
    case LirOpcode::Mod: {
        const TypeRef &type = instruction.type;
        LoadA(instruction.srcs[0], type);
        LoadB(instruction.srcs[1], type);
        if (IsFloat(type)) {
            // A float remainder never gets here: `PrintX87` spells it as the `fprem` loop.
            Instruction(type->kind == TypeRef::Kind::Float32 ? "divss" : "divsd", "xmm0, xmm1");
        }
        else {
            if (type.IsSigned()) {
                Instruction("cqo");
                Instruction("idiv", "r10");
            }
            else {
                Instruction("xor", "rdx, rdx");
                Instruction("div", "r10");
            }
            if (instruction.op == LirOpcode::Mod) {
                Instruction("mov", "rax, rdx");
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
        if (const auto immediate = framePlan.ImmediateValues().find(instruction.srcs[1]);
            immediate != framePlan.ImmediateValues().end()) {
            const auto operation = instruction.op == LirOpcode::Shl
                                     ? "shl"
                                     : (type.IsSigned() && instruction.op != LirOpcode::Lshr ? "sar" : "shr");
            Instruction(operation, std::format("rax, {}", immediate->second & 63U));
            StoreA(instruction.dst, type);
            return true;
        }
        if (const auto physical = framePlan.PhysicalRegisters().find(instruction.srcs[1]);
            physical != framePlan.PhysicalRegisters().end()) {
            Instruction("mov", std::format("r11, {}", PhysicalRegisterName(physical->second)));
        }
        else {
            Instruction("mov", "r11, " + Rbp("qword", Disp(instruction.srcs[1])));
        }
        Instruction("mov", "rcx, r11");
        const bool right = instruction.op == LirOpcode::Shr || instruction.op == LirOpcode::Lshr;
        if (right && type.IsSigned() && instruction.op != LirOpcode::Lshr) {
            Instruction("sar", "rax, cl");
        }
        else if (right) {
            Instruction("shr", "rax, cl");
        }
        else {
            Instruction("shl", "rax, cl");
        }
        StoreA(instruction.dst, type);
        return true;
    }
    case LirOpcode::RotateRight:
        LoadA(instruction.srcs[0], instruction.type);
        Instruction("ror", std::format("{}, {}", instruction.type->kind == TypeRef::Kind::UInt64 ? "rax" : "eax",
                                       instruction.strArg));
        StoreA(instruction.dst, instruction.type);
        return true;
    case LirOpcode::Neg: {
        const TypeRef &type = instruction.type;
        LoadA(instruction.srcs[0], type);
        if (IsFloat(type)) {
            if (type->kind == TypeRef::Kind::Float32) {
                Instruction("movss", std::format("xmm1, dword [rel {}]", modulePrinter.InternFloat32("0x80000000")));
                Instruction("xorps", "xmm0, xmm1");
            }
            else {
                Instruction("movsd",
                            std::format("xmm1, qword [rel {}]", modulePrinter.InternFloat64("0x8000000000000000")));
                Instruction("xorpd", "xmm0, xmm1");
            }
        }
        else {
            Instruction("neg", "rax");
        }
        StoreA(instruction.dst, type);
        return true;
    }
    case LirOpcode::Sqrt:
        if (instruction.srcs.size() != 1 || instruction.dst == LirNoReg ||
            (instruction.type->kind != TypeRef::Kind::Float32 && instruction.type->kind != TypeRef::Kind::Float64))
            return false;
        LoadA(instruction.srcs[0], instruction.type);
        Instruction(instruction.type->kind == TypeRef::Kind::Float32 ? "sqrtss" : "sqrtsd", "xmm0, xmm0");
        StoreA(instruction.dst, instruction.type);
        return true;
    case LirOpcode::Not:
        LoadA(instruction.srcs[0], instruction.type);
        Instruction("test", "rax, rax");
        Instruction("sete", "al");
        Instruction("movzx", "rax, al");
        StoreA(instruction.dst, TypeRef::MakeBool());
        return true;
    case LirOpcode::BitNot:
        LoadA(instruction.srcs[0], instruction.type);
        // As in the object emitter, `~` on a bool is logical negation, which keeps it 0 or 1.
        Instruction(instruction.type.IsBool() ? "xor" : "not", instruction.type.IsBool() ? "rax, 1" : "rax");
        StoreA(instruction.dst, instruction.type);
        return true;
    case LirOpcode::CmpEq:
    case LirOpcode::CmpNe:
    case LirOpcode::CmpLt:
    case LirOpcode::CmpLe:
    case LirOpcode::CmpGt:
    case LirOpcode::CmpGe: {
        const TypeRef &lhsType =
            registerTypes.contains(instruction.srcs[0]) ? registerTypes.at(instruction.srcs[0]) : instruction.type;
        LoadA(instruction.srcs[0], lhsType);
        LoadB(instruction.srcs[1], lhsType);
        if (IsFloat(lhsType)) {
            Instruction(lhsType->kind == TypeRef::Kind::Float32 ? "ucomiss" : "ucomisd", "xmm0, xmm1");
            PrintFloatComparison(*this, instruction.op);
        }
        else {
            Instruction("cmp", "rax, r10");
            const bool signedType = lhsType.IsSigned();
            std::string_view setOpcode;
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
            Instruction(setOpcode, "al");
        }
        Instruction("movzx", "rax, al");
        StoreA(instruction.dst, TypeRef::MakeBool());
        return true;
    }
    case LirOpcode::Cast: {
        const TypeRef &destinationType = instruction.type;
        const auto source = registerTypes.find(instruction.srcs[0]);
        const TypeRef sourceType = source == registerTypes.end() ? destinationType : source->second;
        const bool sourceFloat = IsFloat(sourceType);
        const bool destinationFloat = IsFloat(destinationType);
        LoadA(instruction.srcs[0], sourceType);
        if (sourceFloat && !destinationFloat) {
            const bool single = sourceType->kind == TypeRef::Kind::Float32;
            const std::string_view truncate = single ? "cvttss2si" : "cvttsd2si";
            if (destinationType.IsInteger() && !destinationType.IsSigned() && SizeOf(destinationType) == 8) {
                // A value of 2^63 or more is reduced below it and its top bit restored.
                const std::string bound = single ? modulePrinter.InternFloat32("9223372036854775808.0")
                                                 : modulePrinter.InternFloat64("9223372036854775808.0");
                const std::string large = Label("u64_large");
                const std::string done = Label("u64_done");
                Instruction(single ? "movss" : "movsd",
                            std::format("xmm1, {} [rel {}]", single ? "dword" : "qword", bound));
                Instruction(single ? "ucomiss" : "ucomisd", "xmm0, xmm1");
                Instruction("jae", large);
                Instruction(truncate, "rax, xmm0");
                Instruction("jmp", done);
                modulePrinter.TextLabel(large);
                Instruction(single ? "subss" : "subsd", "xmm0, xmm1");
                Instruction(truncate, "rax, xmm0");
                Instruction("btc", "rax, 63");
                modulePrinter.TextLabel(done);
            }
            else {
                Instruction(truncate, "rax, xmm0");
            }
        }
        else if (!sourceFloat && destinationFloat) {
            Instruction(destinationType->kind == TypeRef::Kind::Float32 ? "cvtsi2ss" : "cvtsi2sd", "xmm0, rax");
        }
        else if (sourceFloat && destinationFloat) {
            if (sourceType->kind == TypeRef::Kind::Float32 && destinationType->kind == TypeRef::Kind::Float64) {
                Instruction("cvtss2sd", "xmm0, xmm0");
            }
            else if (sourceType->kind == TypeRef::Kind::Float64 && destinationType->kind == TypeRef::Kind::Float32) {
                Instruction("cvtsd2ss", "xmm0, xmm0");
            }
        }
        StoreA(instruction.dst, destinationType);
        return true;
    }
    default:
        return false;
    }
}

// ---- memory (FunctionEmitter.cpp, EmitMemory) ---------------------------------------------------------------------

bool AssemblyInstructionPrinter::EmitMemory(const LirInstr &instruction) {
    const auto emitAddress = [this](const X86ScalarAddressInstruction &selected) {
        if (selected.baseSlot)
            Instruction("mov", std::format("rax, qword [rbp - {}]", -*selected.baseSlot));
        if (selected.indexSlot)
            Instruction("mov", std::format("r10, qword [rbp - {}]", -*selected.indexSlot));
        Instruction("lea", ScalarAddressOperands(selected));
    };
    if (const auto selected = SelectScalarMemoryInstruction(instruction, framePlan)) {
        const auto address = ScalarMemoryAddress(*selected);
        const auto value = ScalarMemoryValue(*selected);
        Instruction(ScalarMemoryMnemonic(*selected), selected->store ? address + ", " + value : value + ", " + address);
        return true;
    }
    const auto &registerTypes = framePlan.RegisterTypes();
    const auto &physicalRegisters = framePlan.PhysicalRegisters();
    switch (instruction.op) {
    case LirOpcode::Const: {
        if (framePlan.ElidedImmediates().contains(instruction.dst))
            return true;
        if (instruction.dst == LirNoReg) {
            return true;
        }
        const TypeRef &type = instruction.type;
        const int size = SizeOf(type);
        if (type->kind == TypeRef::Kind::Float32) {
            Instruction("movss", std::format("xmm0, dword [rel {}]", modulePrinter.InternFloat32(instruction.strArg)));
            StoreA(instruction.dst, type);
        }
        else if (type->kind == TypeRef::Kind::Float64) {
            Instruction("movsd", std::format("xmm0, qword [rel {}]", modulePrinter.InternFloat64(instruction.strArg)));
            StoreA(instruction.dst, type);
        }
        else if (IsSoftwareFloat(type)) {
            EmitSoftwareFloatConstant(instruction);
        }
        // The wide case comes before the bool one: a wide bool constant fills every word of its slot.
        else if (IsWideScalar(type)) {
            const std::string &literal = instruction.strArg.empty() ? "0" : instruction.strArg;
            const auto width = static_cast<std::uint32_t>(size * 8);
            WideInteger value = WideInteger::Zero(width);
            if (type.IsBool()) {
                value = WideInteger::FromUnsigned(literal == "true" || literal == "1" ? 1 : 0, width);
            }
            else if (const auto parts = SplitIntegerLiteral(literal)) {
                if (const auto magnitude = WideInteger::Parse(parts->digits, parts->base, width)) {
                    value = parts->negative ? magnitude->Negated() : *magnitude;
                }
            }
            for (int offset = 0; offset < size; offset += 8) {
                LoadWord(value.Word64(static_cast<std::size_t>(offset / 8)));
                Instruction("mov", Rbp("qword", Disp(instruction.dst) + offset) + ", rax");
            }
        }
        // Lowering's placeholder 0 of an aggregate wider than a register zeroes every word of its slot.
        else if (IsAggregate(type) && SizeOfRuntime(type) > 8) {
            ZeroWide(Disp(instruction.dst), SizeOfRuntime(type));
        }
        else if (type.IsBool()) {
            Instruction("mov",
                        std::format("eax, {}", instruction.strArg == "true" || instruction.strArg == "1" ? 1 : 0));
            StoreA(instruction.dst, type);
        }
        else {
            LoadWord(ParseIntegerLiteralBits(instruction.strArg.empty() ? "0" : instruction.strArg).value_or(0));
            StoreA(instruction.dst, size > 0 ? type : TypeRef::MakeInt64());
        }
        return true;
    }
    case LirOpcode::Alloca:
        Instruction("lea", "rax, " + Rbp(-framePlan.AllocaDataOffsets().at(instruction.dst)));
        StoreA(instruction.dst, TypeRef::MakePointer(instruction.type));
        return true;
    case LirOpcode::Load: {
        const TypeRef &type = instruction.type;
        const int size = SizeOfRuntime(type);
        // Nothing to read, and reading a word for it could reach past the end of what was allocated.
        if (IsAggregate(type) && size == 0) {
            return true;
        }
        if (!instruction.strArg.empty()) {
            Instruction("mov", std::format("rax, [rel {}]", instruction.strArg));
        }
        else {
            const LirReg pointer = instruction.srcs[0];
            if (const auto physical = physicalRegisters.find(pointer); physical != physicalRegisters.end()) {
                Instruction("mov", std::format("r10, {}", PhysicalRegisterName(physical->second)));
            }
            else {
                Instruction("mov", "r10, " + Rbp("qword", Disp(pointer)));
            }
            if (IsAggregate(type) && (size > 8 || (size > 0 && size != 1 && size != 2 && size != 4 && size != 8))) {
                // Larger than a register, or an odd width no scalar move spells: chunked, because a rounded-up load
                // would read past the object and a rounded-down one would lose its tail.
                CopyAggregateFromR10ToStack(Disp(instruction.dst), size);
                return true;
            }
            if (IsFloat(type)) {
                Instruction(size == 4 ? "movss" : "movsd",
                            std::format("xmm0, {} [r10]", size == 4 ? "dword" : "qword"));
                StoreA(instruction.dst, type);
                return true;
            }
            if (size == 8 || size == 0) {
                Instruction("mov", "rax, qword [r10]");
            }
            else if (type.IsSigned()) {
                Instruction(size == 4 ? "movsxd" : "movsx", std::format("rax, {} [r10]", Width(size)));
            }
            else if (size == 4) {
                Instruction("mov", "eax, dword [r10]");
            }
            else {
                Instruction("movzx", std::format("rax, {} [r10]", size == 2 ? "word" : "byte"));
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
        // A value occupying no bytes has nothing to move; storing a word for it would write over what follows it.
        if (IsAggregate(type) && size == 0) {
            return true;
        }
        if (const auto physical = physicalRegisters.find(pointer); physical != physicalRegisters.end()) {
            Instruction("mov", std::format("r11, {}", PhysicalRegisterName(physical->second)));
        }
        else {
            Instruction("mov", "r11, " + Rbp("qword", Disp(pointer)));
        }
        if (IsAggregate(type) && (size > 8 || (size > 0 && size != 1 && size != 2 && size != 4 && size != 8))) {
            // The same widths on the way out, chunked so nothing beyond the object is touched. The value is either a
            // pointer to it or its own frame slot.
            if (IsRegPointerTo(value, type)) {
                if (const auto physical = physicalRegisters.find(value); physical != physicalRegisters.end()) {
                    Instruction("mov", std::format("r10, {}", PhysicalRegisterName(physical->second)));
                }
                else {
                    Instruction("mov", "r10, " + Rbp("qword", Disp(value)));
                }
            }
            else {
                Instruction("lea", "r10, " + Rbp(Disp(value)));
            }
            CopyAggregateFromR10ToR11(size);
            return true;
        }
        LoadA(value, type);
        if (IsFloat(type)) {
            Instruction(size == 4 ? "movss" : "movsd", std::format("{} [r11], xmm0", size == 4 ? "dword" : "qword"));
        }
        else {
            const int storeSize = size > 0 ? size : 8;
            Instruction("mov", std::format("{} [r11], {}", Width(storeSize), Accumulator(storeSize)));
        }
        return true;
    }
    case LirOpcode::FieldPtr: {
        const LirReg base = instruction.srcs[0];
        const int fieldOffset = ResolveFieldOffset(base, instruction.strArg);
        if (const auto selected = SelectScalarAddressInstruction(instruction, framePlan, 1, fieldOffset)) {
            emitAddress(*selected);
            return true;
        }
        LoadA(base, registerTypes.at(base));
        if (fieldOffset != 0) {
            Instruction("lea", std::format("rax, [rax + {}]", fieldOffset));
        }
        StoreA(instruction.dst, TypeRef::MakePointer(instruction.type));
        return true;
    }
    case LirOpcode::IndexPtr: {
        const LirReg base = instruction.srcs[0];
        const LirReg index = instruction.srcs[1];
        int elementSize = instruction.type->kind == TypeRef::Kind::Pointer && !instruction.type->inner.empty()
                            ? SizeOfRuntime(instruction.type->inner[0])
                            : 8;
        elementSize = std::max(elementSize, 1);
        if (const auto selected = SelectScalarAddressInstruction(instruction, framePlan, elementSize)) {
            emitAddress(*selected);
            return true;
        }
        LoadA(base, registerTypes.at(base));
        LoadB(index, registerTypes.at(index));
        if (elementSize == 1 || elementSize == 2 || elementSize == 4 || elementSize == 8) {
            Instruction("lea", std::format("rax, [rax + r10 * {}]", elementSize));
        }
        else {
            Instruction("imul", std::format("r11, r10, {}", elementSize));
            Instruction("add", "rax, r11");
        }
        StoreA(instruction.dst, TypeRef::MakePointer(instruction.type));
        return true;
    }
    case LirOpcode::Phi:
        // The predecessor's terminator has already copied the selected incoming value into this register's home.
        return true;
    case LirOpcode::GlobalAddr:
        Instruction("lea", std::format("rax, [rel {}]", instruction.strArg));
        StoreA(instruction.dst, TypeRef::MakePointer(instruction.type));
        return true;
    case LirOpcode::StringAddr: {
        const TypeRef elementType = instruction.type->inner.empty() ? TypeRef::MakeChar8() : instruction.type->inner[0];
        const auto unitSize = static_cast<std::uint32_t>(SizeOfRuntime(elementType));
        const std::string label =
            modulePrinter.InternString(EncodeStringLiteral(instruction.strArg, SizeOfRuntime(elementType)), unitSize);
        Instruction("lea", std::format("rax, [rel {}]", label));
        StoreA(instruction.dst, instruction.type);
        return true;
    }
    default:
        return false;
    }
}

// ---- atomics (AtomicEmitter.cpp, EmitAtomic) ----------------------------------------------------------------------

namespace {
/// The register of `bytes` width numbered like RCX, for the and/or/xor loop.
std::string_view Counter(const int bytes) {
    switch (bytes) {
    case 8:
        return "rcx";
    case 4:
        return "ecx";
    case 2:
        return "cx";
    default:
        return "cl";
    }
}

/// R10 at `bytes` width.
std::string_view Scratch(const int bytes) {
    switch (bytes) {
    case 8:
        return "r10";
    case 4:
        return "r10d";
    case 2:
        return "r10w";
    default:
        return "r10b";
    }
}
} // namespace

void AssemblyInstructionPrinter::LoadAtomicAddress(const LirReg pointer) {
    const auto &physicalRegisters = framePlan.PhysicalRegisters();
    if (const auto physical = physicalRegisters.find(pointer); physical != physicalRegisters.end()) {
        Instruction("mov", std::format("r11, {}", PhysicalRegisterName(physical->second)));
    }
    else {
        Instruction("mov", "r11, " + Rbp("qword", Disp(pointer)));
    }
}

void AssemblyInstructionPrinter::ExtendAccumulator(const TypeRef &type, const int size) {
    const bool sign = type.IsSigned();
    if (size == 1 || size == 2) {
        Instruction(sign ? "movsx" : "movzx", std::format("rax, {}", Accumulator(size)));
    }
    else if (size == 4) {
        Instruction(sign ? "movsxd" : "mov", sign ? "rax, eax" : "eax, eax");
    }
}

bool AssemblyInstructionPrinter::EmitAtomic(const LirInstr &instruction) {
    const TypeRef &type = instruction.type;
    const int size = SizeOfRuntime(type);
    const std::string memory = std::format("{} [r11]", Width(size));
    switch (instruction.op) {
    case LirOpcode::Fence:
        if (instruction.memoryOrder == 4) {
            Instruction("mfence");
        }
        return true;
    case LirOpcode::SpinHint:
        Instruction("pause");
        return true;
    case LirOpcode::AtomicLoad:
        LoadAtomicAddress(instruction.srcs[0]);
        if (size == 8 || size == 4) {
            Instruction("mov", std::format("{}, {}", Accumulator(size), memory));
        }
        else {
            Instruction("movzx", std::format("rax, {}", memory));
        }
        ExtendAccumulator(type, size);
        StoreA(instruction.dst, type);
        return true;
    case LirOpcode::AtomicStore:
        LoadA(instruction.srcs[0], type);
        LoadAtomicAddress(instruction.srcs[1]);
        Instruction(instruction.memoryOrder == 4 ? "xchg" : "mov", std::format("{}, {}", memory, Accumulator(size)));
        return true;
    case LirOpcode::AtomicExchange:
        LoadA(instruction.srcs[1], type);
        LoadAtomicAddress(instruction.srcs[0]);
        Instruction("xchg", std::format("{}, {}", memory, Accumulator(size)));
        ExtendAccumulator(type, size);
        StoreA(instruction.dst, type);
        return true;
    case LirOpcode::AtomicFetchAdd:
    case LirOpcode::AtomicFetchSub:
        LoadA(instruction.srcs[1], type);
        if (instruction.op == LirOpcode::AtomicFetchSub) {
            Instruction("neg", "rax");
        }
        LoadAtomicAddress(instruction.srcs[0]);
        Instruction("lock xadd", std::format("{}, {}", memory, Accumulator(size)));
        ExtendAccumulator(type, size);
        StoreA(instruction.dst, type);
        return true;
    case LirOpcode::AtomicCmpXchg:
        LoadA(instruction.srcs[1], type);
        LoadB(instruction.srcs[2], type);
        LoadAtomicAddress(instruction.srcs[0]);
        Instruction("lock cmpxchg", std::format("{}, {}", memory, Scratch(size)));
        ExtendAccumulator(type, size);
        StoreA(instruction.dst, type);
        return true;
    case LirOpcode::AtomicFetchAnd:
    case LirOpcode::AtomicFetchOr:
    case LirOpcode::AtomicFetchXor: {
        LoadB(instruction.srcs[1], type);
        LoadAtomicAddress(instruction.srcs[0]);
        if (size == 8 || size == 4) {
            Instruction("mov", std::format("{}, {}", Accumulator(size), memory));
        }
        else {
            Instruction("movzx", std::format("eax, {}", memory));
        }
        const std::string loop = Label("atomic_retry");
        modulePrinter.TextLabel(loop);
        Instruction("mov", "rcx, rax");
        Instruction(instruction.op == LirOpcode::AtomicFetchAnd  ? "and"
                    : instruction.op == LirOpcode::AtomicFetchOr ? "or"
                                                                 : "xor",
                    "rcx, r10");
        Instruction("lock cmpxchg", std::format("{}, {}", memory, Counter(size)));
        Instruction("jne", loop);
        ExtendAccumulator(type, size);
        StoreA(instruction.dst, type);
        return true;
    }
    default:
        return false;
    }
}

// ---- thread-local addresses (ThreadLocalEmitter.cpp, EmitThreadLocal) ---------------------------------------------

bool AssemblyInstructionPrinter::EmitThreadLocal(const LirInstr &instruction) {
    if (instruction.op != LirOpcode::ThreadLocalAddr) {
        return false;
    }
    switch (targetOs) {
    case Target::OS::Windows:
        Instruction("mov", "r11, qword gs:[0x58]");
        Instruction("mov", "r10d, dword [rel _tls_index]");
        Instruction("mov", "r11, qword [r11 + r10 * 8]");
        Instruction("lea", std::format("rax, [r11 + {} wrt ..secrel]", instruction.strArg));
        break;
    case Target::OS::MacOS:
        Instruction("lea", std::format("rdi, [rel {} wrt ..tlvp]", instruction.strArg));
        Instruction("call", "qword [rdi]");
        break;
    default:
        Instruction("mov", "rax, qword fs:[0]");
        Instruction("lea", std::format("rax, [rax + {} wrt ..tpoff]", instruction.strArg));
        break;
    }
    StoreA(instruction.dst, instruction.type);
    return true;
}
} // namespace Rux
