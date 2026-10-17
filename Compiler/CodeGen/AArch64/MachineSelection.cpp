#include "CodeGen/AArch64/MachineSelection.h"

#include "CodeGen/AArch64/Encoder.h"
#include "CodeGen/AArch64/MachineSelector.h"
#include "CodeGen/IntegerLiteral.h"
#include "Ir/Lir/ControlFlow.h"
#include "Ir/Lir/NaturalLoops.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <format>
#include <limits>

namespace Rux::AArch64Machine {
namespace Detail {
using namespace Layout;

Selector::Selector(const LirFunc &inputFunction, const SelectionContext &inputContext)
    : func(inputFunction)
    , context(inputContext) {
}

// ---- types -------------------------------------------------------------------------------------------------------

int Selector::Size(const TypeRef &type) const {
    return RuntimeSizeOf(type, context.layouts, context.interfaceNames);
}

int Selector::ScalarSize(const TypeRef &type) const {
    const int size = Size(type);
    return size == 1 || size == 2 || size == 4 ? size : 8;
}

bool Selector::IsAggregate(const TypeRef &type) const {
    if (IsWideValue(type) || type.IsRange() || type.IsSlice())
        return true;
    switch (type->kind) {
    case TypeRef::Kind::Tuple:
    case TypeRef::Kind::Array:
    case TypeRef::Kind::Sum:
    case TypeRef::Kind::Optional:
    case TypeRef::Kind::Fallible:
        return true;
    case TypeRef::Kind::Named: {
        const std::string base = BaseTypeName(type->name);
        return context.interfaceNames.contains(base) || context.layouts.contains(base) ||
               (!type->inner.empty() && SizeOf(type) > 8);
    }
    default:
        return false;
    }
}

ValueKind Selector::KindOf(const TypeRef &type) const {
    if (IsWideValue(type) || IsSoftwareFloat(type))
        return ValueKind::Unsupported;
    switch (type->kind) {
    case TypeRef::Kind::Unknown:
    case TypeRef::Kind::Opaque:
    case TypeRef::Kind::TypeParam:
        return ValueKind::Unsupported;
    default:
        break;
    }
    if (IsAggregate(type))
        return ValueKind::Aggregate;
    if (IsFloat(type))
        return ValueKind::Fpr;
    return Size(type) <= 8 ? ValueKind::Gpr : ValueKind::Unsupported;
}

const TypeRef &Selector::TypeOf(const LirReg reg) const {
    static const TypeRef fallback = TypeRef::MakeInt64();
    const auto found = types.find(reg);
    return found != types.end() ? found->second : fallback;
}

ValueKind Selector::KindOfReg(const LirReg reg) const {
    return KindOf(TypeOf(reg));
}

int Selector::SlotSize(const TypeRef &type) const {
    const int size = Size(type);
    if (size <= 0)
        return 8;
    if (IsAggregate(type))
        return AlignUp(size, 8);
    return size == 3 || size == 5 || size == 6 || size == 7 ? 8 : size;
}

int Selector::StorageAlign(const TypeRef &type) const {
    return std::clamp(RuntimeAlignmentOf(type, context.layouts, context.interfaceNames), 1, 16);
}

int Selector::AllocaSize(const LirInstr &instruction) const {
    if (instruction.strArg.empty())
        return Size(instruction.type);
    int count = 0;
    const std::string_view text = instruction.strArg;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), count);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        count = 0;
    const TypeRef &elementType = instruction.type->inner.empty() ? instruction.type : instruction.type->inner[0];
    const int elementSize = Size(elementType);
    return count * (elementSize > 0 ? elementSize : 8);
}

bool Selector::IsPointerTo(const LirReg reg, const TypeRef &pointee) const {
    const TypeRef &type = TypeOf(reg);
    return type->kind == TypeRef::Kind::Pointer && !type->inner.empty() && type->inner[0] == pointee;
}

bool Selector::IsKilledAt(const LirInstr &instruction, const LirReg reg) const {
    const auto found = lastUses.find(&instruction);
    return found != lastUses.end() && std::ranges::find(found->second, reg) != found->second.end();
}

int Selector::KnownZeroAbove(const LirReg reg) const {
    if (const auto found = zeroAbove.find(reg); found != zeroAbove.end())
        return found->second;
    if (const auto constant = integerConstants.find(reg); constant != integerConstants.end()) {
        const int size = ScalarSize(TypeOf(reg));
        std::uint64_t value = constant->second;
        if (size < 8)
            value &= (std::uint64_t{1} << (size * 8)) - 1;
        return static_cast<int>(std::bit_width(value));
    }
    return 64;
}

void Selector::NoteZeroAbove(const LirReg reg, const int bits) {
    if (bits < 64)
        zeroAbove[reg] = std::max(bits, 0);
}

bool Selector::AlreadyExtended(const LirReg reg, const int size, const bool isSigned) const {
    const int known = KnownZeroAbove(reg);
    return isSigned ? known < size * 8 : known <= size * 8;
}

// ---- facts and eligibility ---------------------------------------------------------------------------------------

bool Selector::CollectFacts() {
    for (const auto &parameter : func.params)
        if (!types.emplace(parameter.reg, parameter.type).second)
            return false;
    for (const auto &block : func.blocks) {
        for (const auto &instruction : block.instrs) {
            for (const auto source : instruction.srcs)
                ++useCounts[source];
            for (const auto &[source, predecessor] : instruction.phiPreds) {
                static_cast<void>(predecessor);
                ++useCounts[source];
            }
            if (instruction.dst == LirNoReg)
                continue;
            types[instruction.dst] =
                instruction.op == LirOpcode::Alloca ? TypeRef::MakePointer(instruction.type) : instruction.type;
            definitions[instruction.dst] = &instruction;
            if (instruction.op == LirOpcode::Load && KindOf(instruction.type) == ValueKind::Gpr &&
                !instruction.type.IsSigned())
                NoteZeroAbove(instruction.dst, ScalarSize(instruction.type) * 8);
            if (instruction.op >= LirOpcode::CmpEq && instruction.op <= LirOpcode::CmpGe)
                NoteZeroAbove(instruction.dst, 1);
            if (instruction.op != LirOpcode::Const)
                continue;
            if (IsFloat(instruction.type)) {
                floatConstants[instruction.dst] = instruction.strArg;
            }
            else if (KindOf(instruction.type) == ValueKind::Gpr) {
                const std::uint64_t bits =
                    instruction.type.IsBool()
                        ? (instruction.strArg == "true" || instruction.strArg == "1" ? 1U : 0U)
                        : ParseIntegerLiteralBits(instruction.strArg.empty() ? "0" : instruction.strArg).value_or(0);
                integerConstants[instruction.dst] = bits;
            }
        }
        if (block.term) {
            ++useCounts[block.term->cond];
            if (block.term->retVal)
                ++useCounts[*block.term->retVal];
        }
    }
    return true;
}

bool Selector::EligibleLocation(const AArch64ArgumentLocation &location) const {
    // The Windows imaginary-stack slots of a C-variadic call never reach here; everything else is reproduced.
    return location.kind != AArch64ArgumentLocation::Kind::Slots;
}

bool Selector::EligibleCall(const LirInstr &instruction) const {
    if (instruction.isCVariadic || instruction.cVariadicFixedParamCount)
        return false;
    const bool indirect = instruction.op == LirOpcode::CallIndirect;
    if (indirect && (instruction.srcs.empty() || KindOfReg(instruction.srcs[0]) != ValueKind::Gpr))
        return false;
    std::vector<TypeRef> argumentTypes;
    for (std::size_t index = indirect ? 1 : 0; index < instruction.srcs.size(); ++index) {
        const TypeRef &type = TypeOf(instruction.srcs[index]);
        // An aggregate argument travels from its home; a pointer standing in for one has no home to copy.
        if (KindOf(type) == ValueKind::Unsupported)
            return false;
        argumentTypes.push_back(type);
    }
    const auto layout = context.callPlanner.PlanArguments(argumentTypes);
    if (layout.windowsVariadic ||
        !std::ranges::all_of(layout.args, [&](const auto &location) { return EligibleLocation(location); }))
        return false;
    return true;
}

bool Selector::EligibleInstruction(const LirInstr &instruction) const {
    const auto kind = [&](const std::size_t index) { return KindOfReg(instruction.srcs.at(index)); };
    const bool opaque = instruction.dst != LirNoReg && TypeOf(instruction.dst).IsOpaque();
    if (opaque && instruction.op != LirOpcode::Call && instruction.op != LirOpcode::CallIndirect &&
        instruction.op != LirOpcode::Const)
        return false;
    const ValueKind result = instruction.dst != LirNoReg && !opaque ? KindOfReg(instruction.dst) : ValueKind::Gpr;
    if (result == ValueKind::Unsupported)
        return false;
    if (instruction.op == LirOpcode::Store && instruction.type.IsOpaque())
        return instruction.srcs.size() == 2;
    for (const auto source : instruction.srcs)
        if (KindOfReg(source) == ValueKind::Unsupported)
            return false;
    switch (instruction.op) {
    case LirOpcode::Const:
    case LirOpcode::Alloca:
    case LirOpcode::GlobalAddr:
    case LirOpcode::StringAddr:
    case LirOpcode::FramePointer:
        return true;
    case LirOpcode::Load:
        if (!instruction.strArg.empty())
            return result != ValueKind::Aggregate;
        return instruction.srcs.size() == 1 && kind(0) == ValueKind::Gpr;
    case LirOpcode::Store: {
        if (instruction.srcs.size() != 2 || kind(1) != ValueKind::Gpr)
            return false;
        const ValueKind stored = KindOf(instruction.type);
        if (stored == ValueKind::Unsupported)
            return false;
        if (stored == ValueKind::Aggregate)
            return kind(0) == ValueKind::Aggregate || IsPointerTo(instruction.srcs[0], instruction.type);
        return kind(0) == stored;
    }
    case LirOpcode::Add:
    case LirOpcode::Sub:
    case LirOpcode::Mul:
    case LirOpcode::Div:
    case LirOpcode::Mod:
    case LirOpcode::And:
    case LirOpcode::Or:
    case LirOpcode::Xor:
        if (instruction.srcs.size() != 2 || (result != ValueKind::Gpr && result != ValueKind::Fpr) ||
            kind(0) != result || kind(1) != result)
            return false;
        if (result == ValueKind::Fpr)
            return instruction.op == LirOpcode::Add || instruction.op == LirOpcode::Sub ||
                   instruction.op == LirOpcode::Mul || instruction.op == LirOpcode::Div;
        return true;
    case LirOpcode::Shl:
    case LirOpcode::Shr:
    case LirOpcode::Lshr:
        return instruction.srcs.size() == 2 && result == ValueKind::Gpr && kind(0) == ValueKind::Gpr &&
               kind(1) == ValueKind::Gpr;
    case LirOpcode::RotateRight:
        return instruction.srcs.size() == 1 && result == ValueKind::Gpr && kind(0) == ValueKind::Gpr &&
               (instruction.type->kind == TypeRef::Kind::UInt32 || instruction.type->kind == TypeRef::Kind::UInt64) &&
               ParseIntegerLiteralBits(instruction.strArg).has_value();
    case LirOpcode::Neg:
        return instruction.srcs.size() == 1 && (result == ValueKind::Gpr || result == ValueKind::Fpr) &&
               kind(0) == result;
    case LirOpcode::Not:
    case LirOpcode::BitNot:
        return instruction.srcs.size() == 1 && result == ValueKind::Gpr && kind(0) == ValueKind::Gpr;
    case LirOpcode::Sqrt:
        return instruction.srcs.size() == 1 && result == ValueKind::Fpr && kind(0) == ValueKind::Fpr;
    case LirOpcode::CmpEq:
    case LirOpcode::CmpNe:
    case LirOpcode::CmpLt:
    case LirOpcode::CmpLe:
    case LirOpcode::CmpGt:
    case LirOpcode::CmpGe:
        return instruction.srcs.size() == 2 && result == ValueKind::Gpr &&
               (kind(0) == ValueKind::Gpr || kind(0) == ValueKind::Fpr) && kind(1) == kind(0);
    case LirOpcode::Cast:
        return instruction.srcs.size() == 1 && (result == ValueKind::Gpr || result == ValueKind::Fpr) &&
               (kind(0) == ValueKind::Gpr || kind(0) == ValueKind::Fpr);
    case LirOpcode::Call:
    case LirOpcode::CallIndirect:
        return EligibleCall(instruction);
    case LirOpcode::Panic:
        return !instruction.srcs.empty() && (kind(0) == ValueKind::Gpr || kind(0) == ValueKind::Aggregate);
    case LirOpcode::Assert:
        return instruction.srcs.size() >= 2 && kind(0) == ValueKind::Gpr &&
               (kind(1) == ValueKind::Gpr || kind(1) == ValueKind::Aggregate);
    case LirOpcode::FieldPtr:
        return instruction.srcs.size() == 1 && kind(0) == ValueKind::Gpr;
    case LirOpcode::IndexPtr:
        return instruction.srcs.size() == 2 && kind(0) == ValueKind::Gpr && kind(1) == ValueKind::Gpr;
    case LirOpcode::Phi:
        return std::ranges::all_of(instruction.phiPreds,
                                   [&](const auto &incoming) { return KindOfReg(incoming.first) == result; });
    default:
        return false;
    }
}

bool Selector::Eligible() const {
    if (func.isExtern || func.isAsm || func.isVariadic || func.blocks.empty())
        return false;
    // Windows and macOS are reproduced through the same planner, but only Linux and FreeBSD have been run.
    if (context.targetOs != Target::OS::Linux && context.targetOs != Target::OS::FreeBSD)
        return false;
    for (const auto &[reg, type] : types)
        if (!type.IsOpaque() && KindOf(type) == ValueKind::Unsupported)
            return false;
    if (!func.returnType.IsOpaque() && KindOf(func.returnType) == ValueKind::Unsupported)
        return false;
    std::vector<TypeRef> parameterTypes;
    for (const auto &parameter : func.params)
        parameterTypes.push_back(parameter.type);
    const auto layout = context.callPlanner.PlanArguments(parameterTypes);
    if (layout.windowsVariadic ||
        !std::ranges::all_of(layout.args, [&](const auto &location) { return EligibleLocation(location); }))
        return false;
    for (const auto &block : func.blocks) {
        if (!block.term)
            return false;
        for (const auto &instruction : block.instrs)
            if (!EligibleInstruction(instruction))
                return false;
        const auto &terminator = *block.term;
        switch (terminator.kind) {
        case LirTermKind::Branch:
        case LirTermKind::Switch:
            if (KindOfReg(terminator.cond) != ValueKind::Gpr)
                return false;
            break;
        case LirTermKind::Return:
            if (terminator.retVal && *terminator.retVal != LirNoReg && !terminator.retType.IsOpaque()) {
                const ValueKind value = KindOfReg(*terminator.retVal);
                if (value == ValueKind::Unsupported)
                    return false;
                const ValueKind returned = KindOf(terminator.retType);
                if (returned == ValueKind::Aggregate) {
                    if (value != ValueKind::Aggregate && !IsPointerTo(*terminator.retVal, terminator.retType))
                        return false;
                }
                else if (returned != value) {
                    return false;
                }
            }
            break;
        default:
            break;
        }
    }
    return true;
}

void Selector::ComputeLastUses() {
    const LirControlFlow cfg(func);
    const LirLiveness liveness(func, cfg);
    // A register is live at the current point of the walk when it is marked with the block's number plus one.
    LirRegisterTable<std::uint32_t> live(RegisterCountHint(func));
    for (std::size_t block = 0; block < func.blocks.size(); ++block) {
        const auto mark = static_cast<std::uint32_t>(block + 1);
        for (const auto reg : liveness.liveOut[block])
            live.Assign(reg, mark);
        const auto &term = func.blocks[block].term;
        if (term) {
            live.Assign(term->cond, mark);
            if (term->retVal)
                live.Assign(*term->retVal, mark);
        }
        const auto &instructions = func.blocks[block].instrs;
        for (std::size_t reverse = instructions.size(); reverse > 0; --reverse) {
            const auto &instruction = instructions[reverse - 1];
            if (instruction.op == LirOpcode::Phi)
                continue;
            for (const auto source : instruction.srcs) {
                if (const std::uint32_t *marked = live.Find(source); marked == nullptr || *marked != mark) {
                    lastUses[&instruction].push_back(source);
                    live.Assign(source, mark);
                }
            }
            if (instruction.dst != LirNoReg)
                live.Assign(instruction.dst, 0);
        }
    }
}

// ---- emission helpers --------------------------------------------------------------------------------------------

Instruction &Selector::Emit(const Op op, const std::uint8_t size, std::vector<Operand> operands) {
    Instruction instruction;
    instruction.opcode = static_cast<std::uint16_t>(op);
    instruction.size = size;
    instruction.operands = std::move(operands);
    auto &instructions = out.function.blocks[current].instructions;
    instructions.push_back(std::move(instruction));
    return instructions.back();
}

void Selector::EmitCopy(const Reg destination, const Reg source, const RegClass regClass) {
    if (destination == source)
        return;
    auto &copy =
        Emit(regClass == RegClass::Vector ? Op::MovFp : Op::Mov, 8, {Operand::Def(destination), Operand::Use(source)});
    copy.isCopy = true;
}

Reg Selector::NewGpr() {
    return out.function.NewVirtual(RegClass::General);
}

Reg Selector::NewFpr() {
    return out.function.NewVirtual(RegClass::Vector);
}

std::uint32_t Selector::NewBlock(const std::uint32_t loopDepth, const bool cold) {
    out.function.blocks.emplace_back();
    out.function.blocks.back().loopDepth = loopDepth;
    out.function.blocks.back().cold = cold;
    out.blockLabels.push_back(std::format("bb{}", out.function.blocks.size() - 1));
    return static_cast<std::uint32_t>(out.function.blocks.size() - 1);
}

std::uint32_t Selector::Symbol(SymbolRef symbol) {
    const auto found = std::ranges::find(out.symbols, symbol);
    if (found != out.symbols.end())
        return static_cast<std::uint32_t>(found - out.symbols.begin());
    out.symbols.push_back(std::move(symbol));
    return static_cast<std::uint32_t>(out.symbols.size() - 1);
}

Reg Selector::Value(const LirReg reg) {
    if (const auto found = registers.find(reg); found != registers.end())
        return found->second;
    const Reg created = KindOfReg(reg) == ValueKind::Fpr ? NewFpr() : NewGpr();
    registers.emplace(reg, created);
    return created;
}

std::int32_t Selector::Home(const LirReg reg) {
    if (const auto found = homes.find(reg); found != homes.end())
        return found->second;
    const TypeRef &type = TypeOf(reg);
    const std::int32_t object = out.function.NewFrameObject(SlotSize(type), std::max(8, StorageAlign(type)));
    homes.emplace(reg, object);
    return object;
}

Address Selector::HomeAddress(const LirReg reg) {
    Address address;
    address.frameObject = Home(reg);
    return address;
}

Reg Selector::Constant(const std::uint64_t value) {
    const Reg reg = NewGpr();
    Emit(Op::MovImm, 8, {Operand::Def(reg), Operand::Imm(static_cast<std::int64_t>(value))}).rematerializable = true;
    return reg;
}

Reg Selector::ExtendRegister(const Reg value, const int size, const bool isSigned) {
    if (size >= 8)
        return value;
    const Reg extended = NewGpr();
    Emit(isSigned ? Op::SignExtend : Op::ZeroExtend, 8, {Operand::Def(extended), Operand::Use(value)}).sourceSize =
        static_cast<std::uint8_t>(size);
    return extended;
}

Reg Selector::Extended(const LirReg reg, const bool forceSigned, const bool forceUnsigned) {
    const TypeRef &type = TypeOf(reg);
    const int size = ScalarSize(type);
    if (size >= 8 || type.IsBool())
        return Value(reg);
    const bool isSigned = forceSigned || (!forceUnsigned && type.IsSigned());
    if (const auto constant = integerConstants.find(reg); constant != integerConstants.end()) {
        const int bits = size * 8;
        std::uint64_t value = constant->second & ((std::uint64_t{1} << bits) - 1);
        if (isSigned && (value >> (bits - 1)) != 0)
            value |= ~((std::uint64_t{1} << bits) - 1);
        return Constant(value);
    }
    if (AlreadyExtended(reg, size, isSigned))
        return Value(reg);
    return ExtendRegister(Value(reg), size, isSigned);
}

Address Selector::Offset(Address address, const std::int64_t displacement, bool &fits) {
    const std::int64_t total = static_cast<std::int64_t>(address.displacement) + displacement;
    if (total < std::numeric_limits<std::int32_t>::min() || total > std::numeric_limits<std::int32_t>::max()) {
        fits = false;
        return address;
    }
    address.displacement = static_cast<std::int32_t>(total);
    return address;
}

Address Selector::AddressOf(const LirReg pointer, const int depth) {
    Address direct;
    direct.base = Value(pointer);
    if (depth > 6)
        return direct;
    if (const auto object = allocas.find(pointer); object != allocas.end()) {
        Address address;
        address.frameObject = object->second;
        return address;
    }
    const auto definition = definitions.find(pointer);
    if (definition == definitions.end())
        return direct;
    const LirInstr &instruction = *definition->second;
    switch (instruction.op) {
    case LirOpcode::GlobalAddr: {
        Address address;
        address.symbol = Symbol({SymbolRef::Kind::Global, instruction.strArg});
        return address;
    }
    case LirOpcode::FieldPtr: {
        const int offset =
            FieldOffsetOf(TypeOf(instruction.srcs[0]), instruction.strArg, context.layouts, context.interfaceNames);
        bool fits = true;
        const Address base = AddressOf(instruction.srcs[0], depth + 1);
        const Address result = Offset(base, offset, fits);
        return fits ? result : direct;
    }
    case LirOpcode::IndexPtr: {
        const int elementSize = instruction.type->kind == TypeRef::Kind::Pointer && !instruction.type->inner.empty()
                                  ? std::max(1, Size(instruction.type->inner[0]))
                                  : 8;
        const LirReg index = instruction.srcs[1];
        const Address base = AddressOf(instruction.srcs[0], depth + 1);
        if (const auto constant = integerConstants.find(index); constant != integerConstants.end()) {
            const auto value = static_cast<std::int64_t>(constant->second);
            if (value > -(std::int64_t{1} << 31) && value < (std::int64_t{1} << 31)) {
                bool fits = true;
                const Address result = Offset(base, value * elementSize, fits);
                if (fits)
                    return result;
            }
            return direct;
        }
        if (elementSize != 1 && elementSize != 2 && elementSize != 4 && elementSize != 8 && elementSize != 16)
            return direct;
        // Only a plain base register takes an index: anything else is computed into one first.
        Address result;
        result.base =
            base.index == NoReg && base.symbol == Machine::NoSymbol && base.frameObject < 0 && base.displacement == 0
                ? base.base
                : Value(instruction.srcs[0]);
        // An index that adds a constant to another 64-bit value moves the constant into the displacement.
        LirReg folded = index;
        std::int64_t elements = 0;
        for (int step = 0; step < 4; ++step) {
            const auto arithmetic = definitions.find(folded);
            if (arithmetic == definitions.end())
                break;
            const LirInstr &sum = *arithmetic->second;
            if (sum.op == LirOpcode::Cast && sum.srcs.size() == 1 && ScalarSize(sum.type) == 8 && !sum.type.IsBool() &&
                ScalarSize(TypeOf(sum.srcs[0])) == 8 && !TypeOf(sum.srcs[0]).IsBool() &&
                TypeOf(sum.srcs[0]).IsInteger() && sum.type.IsInteger()) {
                folded = sum.srcs[0];
                continue;
            }
            if ((sum.op != LirOpcode::Add && sum.op != LirOpcode::Sub) || sum.srcs.size() != 2 ||
                ScalarSize(sum.type) != 8 || !sum.type.IsInteger())
                break;
            const auto constant = integerConstants.find(sum.srcs[1]);
            if (constant == integerConstants.end())
                break;
            const auto amount = static_cast<std::int64_t>(constant->second);
            if (amount <= -(std::int64_t{1} << 20) || amount >= (std::int64_t{1} << 20))
                break;
            elements += sum.op == LirOpcode::Add ? amount : -amount;
            folded = sum.srcs[0];
        }
        if (folded != index) {
            bool fits = true;
            Address shifted = result;
            shifted.index = Extended(folded);
            shifted.scale = static_cast<std::uint8_t>(elementSize);
            shifted = Offset(shifted, elements * elementSize, fits);
            if (fits)
                return shifted;
        }
        result.index = Extended(index);
        result.scale = static_cast<std::uint8_t>(elementSize);
        return result;
    }
    default:
        return direct;
    }
}

Reg Selector::AddressRegister(const Address &address) {
    if (address.base != NoReg && address.index == NoReg && address.displacement == 0 && address.frameObject < 0 &&
        address.symbol == Machine::NoSymbol)
        return address.base;
    const Reg reg = NewGpr();
    Emit(Op::AddressOf, 8, {Operand::Def(reg), Operand::Mem(address)});
    return reg;
}

void Selector::CopyMemory(const Address &destination, const Address &source, const int size) {
    if (size <= 0)
        return;
    if (size > 128) {
        const Reg to = NewGpr();
        const Reg from = NewGpr();
        Emit(Op::AddressOf, 8, {Operand::Def(to), Operand::Mem(destination)});
        Emit(Op::AddressOf, 8, {Operand::Def(from), Operand::Mem(source)});
        Emit(Op::CopyBytes, 8, {Operand::UseDef(to), Operand::UseDef(from), Operand::Imm(size)});
        return;
    }
    int offset = 0;
    for (const int chunk : {8, 4, 2, 1}) {
        while (offset + chunk <= size) {
            bool fits = true;
            const Address from = Offset(source, offset, fits);
            const Address to = Offset(destination, offset, fits);
            const Reg temporary = NewGpr();
            Emit(Op::Load, chunk == 8 ? 8 : 4, {Operand::Def(temporary), Operand::Mem(from)}).sourceSize =
                static_cast<std::uint8_t>(chunk);
            Emit(Op::Store, static_cast<std::uint8_t>(chunk), {Operand::Mem(to), Operand::Use(temporary)});
            offset += chunk;
        }
    }
}

void Selector::ZeroMemory(const Address &destination, const int size) {
    int offset = 0;
    for (const int chunk : {8, 4, 2, 1}) {
        while (offset + chunk <= size) {
            bool fits = true;
            Emit(Op::Store, static_cast<std::uint8_t>(chunk),
                 {Operand::Mem(Offset(destination, offset, fits)), Operand::Imm(0)});
            offset += chunk;
        }
    }
}

void Selector::LoadWidth(const Reg destination, const Address &address, const int width, const bool isSigned) {
    auto &load =
        Emit(isSigned && width < 8 ? Op::LoadSigned : Op::Load, 8, {Operand::Def(destination), Operand::Mem(address)});
    load.sourceSize = static_cast<std::uint8_t>(width);
}

void Selector::LoadInto(const Reg destination, const Address &address, const TypeRef &type) {
    if (IsFloat(type)) {
        Emit(Op::LoadFp, static_cast<std::uint8_t>(Size(type)), {Operand::Def(destination), Operand::Mem(address)});
        return;
    }
    const int size = ScalarSize(type);
    LoadWidth(destination, address, size, type.IsSigned());
}

// ---- memory and addresses ----------------------------------------------------------------------------------------

void Selector::SelectConstant(const LirInstr &instruction) {
    switch (KindOf(instruction.type)) {
    case ValueKind::Gpr: {
        const Reg destination = Value(instruction.dst);
        std::uint64_t value = integerConstants.at(instruction.dst);
        const int size = ScalarSize(instruction.type);
        if (size < 8)
            value &= (std::uint64_t{1} << (size * 8)) - 1;
        Emit(Op::MovImm, 8, {Operand::Def(destination), Operand::Imm(static_cast<std::int64_t>(value))})
            .rematerializable = true;
        return;
    }
    case ValueKind::Fpr: {
        Address address;
        address.symbol = Symbol(
            {instruction.type->kind == TypeRef::Kind::Float32 ? SymbolRef::Kind::Float32 : SymbolRef::Kind::Float64,
             instruction.strArg});
        Emit(Op::LoadFp, static_cast<std::uint8_t>(Size(instruction.type)),
             {Operand::Def(Value(instruction.dst)), Operand::Mem(address)})
            .rematerializable = true;
        return;
    }
    case ValueKind::Aggregate: {
        const int size = Size(instruction.type);
        if (size > 8) {
            ZeroMemory(HomeAddress(instruction.dst), SlotSize(instruction.type));
            return;
        }
        const std::uint64_t bits =
            ParseIntegerLiteralBits(instruction.strArg.empty() ? "0" : instruction.strArg).value_or(0);
        const int width = size == 1 || size == 2 || size == 4 ? size : 8;
        Emit(Op::Store, static_cast<std::uint8_t>(width),
             {Operand::Mem(HomeAddress(instruction.dst)), Operand::Use(Constant(bits))});
        return;
    }
    default:
        return;
    }
}

void Selector::SelectLoad(const LirInstr &instruction) {
    Address address;
    if (!instruction.strArg.empty())
        address.symbol = Symbol({SymbolRef::Kind::NamedData, instruction.strArg});
    else
        address = AddressOf(instruction.srcs[0]);
    if (KindOf(instruction.type) == ValueKind::Aggregate) {
        CopyMemory(HomeAddress(instruction.dst), address, Size(instruction.type));
        return;
    }
    LoadInto(Value(instruction.dst), address, instruction.type);
    if (KindOf(instruction.type) == ValueKind::Gpr && !instruction.type.IsSigned())
        NoteZeroAbove(instruction.dst, ScalarSize(instruction.type) * 8);
}

void Selector::SelectStore(const LirInstr &instruction) {
    const LirReg value = instruction.srcs[0];
    const Address address = AddressOf(instruction.srcs[1]);
    const TypeRef &type = instruction.type;
    switch (KindOf(type)) {
    case ValueKind::Aggregate: {
        const int size = Size(type);
        if (size == 0)
            return;
        Address source;
        if (KindOfReg(value) == ValueKind::Aggregate)
            source = HomeAddress(value);
        else
            source.base = Value(value);
        CopyMemory(address, source, size);
        return;
    }
    case ValueKind::Fpr:
        Emit(Op::StoreFp, static_cast<std::uint8_t>(Size(type)), {Operand::Mem(address), Operand::Use(Value(value))});
        return;
    default: {
        const int size = ScalarSize(type);
        const auto constant = integerConstants.find(value);
        const bool zero =
            constant != integerConstants.end() &&
            (size == 8 ? constant->second : constant->second & ((std::uint64_t{1} << (size * 8)) - 1)) == 0;
        Emit(Op::Store, static_cast<std::uint8_t>(size),
             {Operand::Mem(address), zero ? Operand::Imm(0) : Operand::Use(Value(value))});
        return;
    }
    }
}

void Selector::SelectAddress(const LirInstr &instruction) {
    const Reg destination = Value(instruction.dst);
    switch (instruction.op) {
    case LirOpcode::Alloca: {
        Address address;
        address.frameObject = allocas.at(instruction.dst);
        Emit(Op::AddressOf, 8, {Operand::Def(destination), Operand::Mem(address)}).rematerializable = true;
        return;
    }
    case LirOpcode::GlobalAddr: {
        Address address;
        address.symbol = Symbol({SymbolRef::Kind::Global, instruction.strArg});
        Emit(Op::AddressOf, 8, {Operand::Def(destination), Operand::Mem(address)}).rematerializable = true;
        return;
    }
    case LirOpcode::StringAddr: {
        const TypeRef elementType = instruction.type->inner.empty() ? TypeRef::MakeChar8() : instruction.type->inner[0];
        Address address;
        const auto unitSize = static_cast<std::uint32_t>(Size(elementType));
        address.symbol =
            Symbol({SymbolRef::Kind::String, EncodeStringLiteral(instruction.strArg, Size(elementType)), unitSize});
        Emit(Op::AddressOf, 8, {Operand::Def(destination), Operand::Mem(address)}).rematerializable = true;
        return;
    }
    case LirOpcode::FramePointer:
        Emit(Op::Mov, 8, {Operand::Def(destination), Operand::Use(Fp)});
        return;
    case LirOpcode::IndexPtr: {
        const int elementSize = instruction.type->kind == TypeRef::Kind::Pointer && !instruction.type->inner.empty()
                                  ? std::max(1, Size(instruction.type->inner[0]))
                                  : 8;
        if (elementSize != 1 && elementSize != 2 && elementSize != 4 && elementSize != 8 && elementSize != 16 &&
            !integerConstants.contains(instruction.srcs[1])) {
            const Reg scaled = NewGpr();
            Emit(Op::Mul, 8,
                 {Operand::Def(scaled), Operand::Use(Extended(instruction.srcs[1])),
                  Operand::Use(Constant(static_cast<std::uint64_t>(elementSize)))});
            Emit(Op::Add, 8,
                 {Operand::Def(destination), Operand::Use(Value(instruction.srcs[0])), Operand::Use(scaled)});
            return;
        }
        [[fallthrough]];
    }
    case LirOpcode::FieldPtr: {
        const Address address = AddressOf(instruction.dst);
        if (address.base == destination) {
            // The address did not fold, so compute it from the base register directly.
            Address fallback;
            fallback.base = Value(instruction.srcs[0]);
            if (instruction.op == LirOpcode::FieldPtr) {
                fallback.displacement = FieldOffsetOf(TypeOf(instruction.srcs[0]), instruction.strArg, context.layouts,
                                                      context.interfaceNames);
            }
            else {
                fallback.index = Extended(instruction.srcs[1]);
                fallback.scale = static_cast<std::uint8_t>(instruction.type->kind == TypeRef::Kind::Pointer &&
                                                                   !instruction.type->inner.empty()
                                                               ? std::max(1, Size(instruction.type->inner[0]))
                                                               : 8);
            }
            Emit(Op::AddressOf, 8, {Operand::Def(destination), Operand::Mem(fallback)});
            return;
        }
        if (address.frameObject < 0 && address.symbol == Machine::NoSymbol && address.index == NoReg &&
            address.displacement == 0) {
            EmitCopy(destination, address.base, RegClass::General);
            return;
        }
        Emit(Op::AddressOf, 8, {Operand::Def(destination), Operand::Mem(address)}).rematerializable =
            address.base == NoReg && address.index == NoReg;
        return;
    }
    default:
        return;
    }
}

void Selector::SelectInstruction(const LirInstr &instruction) {
    if (instruction.dst != LirNoReg && TypeOf(instruction.dst).IsOpaque() && instruction.op != LirOpcode::Call &&
        instruction.op != LirOpcode::CallIndirect)
        return;
    if (instruction.op == LirOpcode::Store && instruction.type.IsOpaque())
        return;
    switch (instruction.op) {
    case LirOpcode::Const:
        SelectConstant(instruction);
        return;
    case LirOpcode::Alloca:
    case LirOpcode::GlobalAddr:
    case LirOpcode::StringAddr:
    case LirOpcode::FramePointer:
    case LirOpcode::FieldPtr:
    case LirOpcode::IndexPtr:
        SelectAddress(instruction);
        return;
    case LirOpcode::Load:
        SelectLoad(instruction);
        return;
    case LirOpcode::Store:
        SelectStore(instruction);
        return;
    case LirOpcode::Add:
    case LirOpcode::Sub:
    case LirOpcode::Mul:
    case LirOpcode::And:
    case LirOpcode::Or:
    case LirOpcode::Xor:
        SelectBinary(instruction);
        return;
    case LirOpcode::Div:
    case LirOpcode::Mod:
        if (KindOf(instruction.type) == ValueKind::Fpr)
            SelectBinary(instruction);
        else
            SelectDivision(instruction);
        return;
    case LirOpcode::Shl:
    case LirOpcode::Shr:
    case LirOpcode::Lshr:
    case LirOpcode::RotateRight:
        SelectShift(instruction);
        return;
    case LirOpcode::Neg:
    case LirOpcode::Not:
    case LirOpcode::BitNot:
    case LirOpcode::Sqrt:
        if (!fusedComparisons.contains(&instruction))
            SelectUnary(instruction);
        return;
    case LirOpcode::CmpEq:
    case LirOpcode::CmpNe:
    case LirOpcode::CmpLt:
    case LirOpcode::CmpLe:
    case LirOpcode::CmpGt:
    case LirOpcode::CmpGe:
        if (!fusedComparisons.contains(&instruction))
            SelectComparison(instruction);
        return;
    case LirOpcode::Cast:
        SelectCast(instruction);
        return;
    case LirOpcode::Call:
    case LirOpcode::CallIndirect:
        SelectCall(instruction);
        return;
    case LirOpcode::Panic:
        SelectPanic(instruction);
        return;
    case LirOpcode::Assert:
        SelectAssert(instruction);
        return;
    default:
        return;
    }
}

// ---- control flow ------------------------------------------------------------------------------------------------

void Selector::MarkBlocks() {
    const LirControlFlow cfg(func);
    std::vector<std::uint32_t> depth(func.blocks.size(), 0);
    if (func.blocks.size() <= 4096) {
        const LirDominance dominance(cfg);
        const LirNaturalLoops loops(cfg, dominance);
        if (loops.complete)
            for (const auto &[header, members] : loops.members) {
                static_cast<void>(header);
                for (const auto block : members)
                    ++depth[block];
            }
    }
    // A block is cold when every path from it ends in a failure report.
    std::vector<bool> cold(func.blocks.size(), false);
    for (std::size_t block = 0; block < func.blocks.size(); ++block)
        cold[block] = std::ranges::any_of(
            func.blocks[block].instrs, [](const LirInstr &instruction) { return instruction.op == LirOpcode::Panic; });
    for (bool changed = true; changed;) {
        changed = false;
        for (std::size_t block = 0; block < func.blocks.size(); ++block) {
            if (cold[block] || cfg.successors[block].empty())
                continue;
            if (std::ranges::all_of(cfg.successors[block], [&](const std::uint32_t next) { return cold[next]; })) {
                cold[block] = true;
                changed = true;
            }
        }
    }
    blockEntry.resize(func.blocks.size());
    for (std::size_t block = 0; block < func.blocks.size(); ++block) {
        blockEntry[block] = NewBlock(depth[block], cold[block] && block != 0);
        out.blockLabels.back() = func.blocks[block].label.empty()
                                   ? std::format("bb{}", block)
                                   : std::format("{}.{}", func.blocks[block].label, block);
    }
}

bool Selector::Fusable(const std::uint32_t lirBlock, const LirTerminator &terminator) const {
    if (terminator.kind != LirTermKind::Branch)
        return false;
    const auto definition = definitions.find(terminator.cond);
    if (definition == definitions.end())
        return false;
    const LirInstr &comparison = *definition->second;
    switch (comparison.op) {
    case LirOpcode::CmpEq:
    case LirOpcode::CmpNe:
    case LirOpcode::CmpLt:
    case LirOpcode::CmpLe:
    case LirOpcode::CmpGt:
    case LirOpcode::CmpGe:
        break;
    default:
        return false;
    }
    const auto &instructions = func.blocks[lirBlock].instrs;
    return std::ranges::any_of(instructions, [&](const LirInstr &instruction) { return &instruction == &comparison; });
}

std::uint32_t Selector::EdgeTarget(const std::uint32_t from, const std::uint32_t to, const bool shared) {
    const bool hasPhis = std::ranges::any_of(
        func.blocks[to].instrs, [](const LirInstr &instruction) { return instruction.op == LirOpcode::Phi; });
    if (!hasPhis)
        return blockEntry[to];
    if (!shared) {
        EmitPhiSources(from, to);
        return blockEntry[to];
    }
    // A conditional edge into a block with phis gets its own block for the copies, which only that edge executes.
    const std::uint32_t saved = current;
    const auto &target = out.function.blocks[blockEntry[to]];
    const std::uint32_t split = NewBlock(target.loopDepth, target.cold);
    current = split;
    EmitPhiSources(from, to);
    Emit(Op::Jmp, 8, {Operand::BlockTarget(blockEntry[to])});
    current = saved;
    return split;
}

void Selector::EmitPhiSources(const std::uint32_t from, const std::uint32_t to) {
    for (const auto &instruction : func.blocks[to].instrs) {
        if (instruction.op != LirOpcode::Phi)
            continue;
        const auto incoming =
            std::ranges::find_if(instruction.phiPreds, [&](const auto &entry) { return entry.second == from; });
        if (incoming == instruction.phiPreds.end())
            continue;
        const LirReg source = incoming->first;
        switch (KindOf(TypeOf(instruction.dst))) {
        case ValueKind::Aggregate: {
            auto found = phiTemporaryHomes.find(instruction.dst);
            if (found == phiTemporaryHomes.end()) {
                const TypeRef &type = TypeOf(instruction.dst);
                found = phiTemporaryHomes
                            .emplace(instruction.dst,
                                     out.function.NewFrameObject(SlotSize(type), std::max(8, StorageAlign(type))))
                            .first;
            }
            Address temporary;
            temporary.frameObject = found->second;
            CopyMemory(temporary, HomeAddress(source), SlotSize(TypeOf(instruction.dst)));
            break;
        }
        case ValueKind::Fpr: {
            auto found = phiTemporaries.find(instruction.dst);
            if (found == phiTemporaries.end())
                found = phiTemporaries.emplace(instruction.dst, NewFpr()).first;
            EmitCopy(found->second, Value(source), RegClass::Vector);
            break;
        }
        default: {
            auto found = phiTemporaries.find(instruction.dst);
            if (found == phiTemporaries.end())
                found = phiTemporaries.emplace(instruction.dst, NewGpr()).first;
            if (const auto constant = integerConstants.find(source); constant != integerConstants.end())
                Emit(Op::MovImm, 8,
                     {Operand::Def(found->second), Operand::Imm(static_cast<std::int64_t>(constant->second))});
            else
                EmitCopy(found->second, Value(source), RegClass::General);
            break;
        }
        }
    }
}

void Selector::EmitPhiDestinations(const std::uint32_t lirBlock) {
    for (const auto &instruction : func.blocks[lirBlock].instrs) {
        if (instruction.op != LirOpcode::Phi)
            continue;
        switch (KindOf(TypeOf(instruction.dst))) {
        case ValueKind::Aggregate: {
            auto found = phiTemporaryHomes.find(instruction.dst);
            if (found == phiTemporaryHomes.end()) {
                const TypeRef &type = TypeOf(instruction.dst);
                found = phiTemporaryHomes
                            .emplace(instruction.dst,
                                     out.function.NewFrameObject(SlotSize(type), std::max(8, StorageAlign(type))))
                            .first;
            }
            Address temporary;
            temporary.frameObject = found->second;
            CopyMemory(HomeAddress(instruction.dst), temporary, SlotSize(TypeOf(instruction.dst)));
            break;
        }
        case ValueKind::Fpr: {
            auto found = phiTemporaries.find(instruction.dst);
            if (found == phiTemporaries.end())
                found = phiTemporaries.emplace(instruction.dst, NewFpr()).first;
            EmitCopy(Value(instruction.dst), found->second, RegClass::Vector);
            break;
        }
        default: {
            auto found = phiTemporaries.find(instruction.dst);
            if (found == phiTemporaries.end())
                found = phiTemporaries.emplace(instruction.dst, NewGpr()).first;
            EmitCopy(Value(instruction.dst), found->second, RegClass::General);
            break;
        }
        }
    }
}

void Selector::EmitBranch(const LirTerminator &terminator, const std::uint32_t lirBlock) {
    const std::uint32_t whenTrue = EdgeTarget(lirBlock, terminator.trueTarget, true);
    const std::uint32_t whenFalse = EdgeTarget(lirBlock, terminator.falseTarget, true);
    const auto jump = [&](const A64Condition condition, const std::uint32_t target) {
        Emit(Op::Jcc, 8, {Operand::BlockTarget(target)}).condition = static_cast<std::uint8_t>(condition);
    };
    if (Fusable(lirBlock, terminator)) {
        jump(EmitCompare(*definitions.at(terminator.cond)), whenTrue);
    }
    else if (const auto definition = definitions.find(terminator.cond); definition != definitions.end() &&
                                                                        fusedComparisons.contains(definition->second) &&
                                                                        definition->second->op == LirOpcode::Not) {
        // A branch on a negation tests the operand and takes the opposite edge.
        const LirInstr &negation = *definition->second;
        Emit(Op::Cmp, ScalarSize(negation.type) == 8 ? 8 : 4,
             {Operand::Use(Extended(negation.srcs[0], false, true)), Operand::Imm(0)});
        jump(A64Condition::Eq, whenTrue);
    }
    else {
        Emit(Op::Cmp, 4, {Operand::Use(Value(terminator.cond)), Operand::Imm(0)});
        jump(A64Condition::Ne, whenTrue);
    }
    Emit(Op::Jmp, 8, {Operand::BlockTarget(whenFalse)});
}

void Selector::EmitSwitch(const LirTerminator &terminator, const std::uint32_t lirBlock) {
    const TypeRef &type = TypeOf(terminator.cond);
    const int size = ScalarSize(type);
    const Reg value = Extended(terminator.cond, false, true);
    for (const auto &entry : terminator.cases) {
        std::uint64_t bits = ParseIntegerLiteralBits(entry.value).value_or(0);
        if (size < 8)
            bits &= (std::uint64_t{1} << (size * 8)) - 1;
        const std::uint32_t target = EdgeTarget(lirBlock, entry.target, true);
        if (TryEncodeArithImm12(bits))
            Emit(Op::Cmp, 8, {Operand::Use(value), Operand::Imm(static_cast<std::int64_t>(bits))});
        else
            Emit(Op::Cmp, 8, {Operand::Use(value), Operand::Use(Constant(bits))});
        Emit(Op::Jcc, 8, {Operand::BlockTarget(target)}).condition = static_cast<std::uint8_t>(A64Condition::Eq);
    }
    Emit(Op::Jmp, 8, {Operand::BlockTarget(EdgeTarget(lirBlock, terminator.defaultTarget, true))});
}

void Selector::SelectTerminator(const std::uint32_t lirBlock, const LirTerminator &terminator) {
    switch (terminator.kind) {
    case LirTermKind::Jump:
        Emit(Op::Jmp, 8, {Operand::BlockTarget(EdgeTarget(lirBlock, terminator.trueTarget, false))});
        return;
    case LirTermKind::Branch:
        if (terminator.trueTarget == terminator.falseTarget) {
            Emit(Op::Jmp, 8, {Operand::BlockTarget(EdgeTarget(lirBlock, terminator.trueTarget, false))});
            return;
        }
        EmitBranch(terminator, lirBlock);
        return;
    case LirTermKind::Switch:
        EmitSwitch(terminator, lirBlock);
        return;
    case LirTermKind::Return:
        SelectReturn(terminator);
        return;
    case LirTermKind::Unreachable:
        Emit(Op::Trap, 8, {});
        return;
    }
}

void Selector::SelectBlock(const std::uint32_t lirBlock) {
    current = blockEntry[lirBlock];
    EmitPhiDestinations(lirBlock);
    const auto &block = func.blocks[lirBlock];
    if (block.term && Fusable(lirBlock, *block.term)) {
        const LirInstr *comparison = definitions.at(block.term->cond);
        if (useCounts[comparison->dst] <= 1)
            fusedComparisons.insert(comparison);
    }
    else if (block.term && block.term->kind == LirTermKind::Branch) {
        if (const auto definition = definitions.find(block.term->cond);
            definition != definitions.end() && definition->second->op == LirOpcode::Not &&
            useCounts[block.term->cond] <= 1 && std::ranges::any_of(block.instrs, [&](const LirInstr &instruction) {
                return &instruction == definition->second;
            }))
            fusedComparisons.insert(definition->second);
    }
    for (const auto &instruction : block.instrs) {
        if (instruction.op == LirOpcode::Phi)
            continue;
        SelectInstruction(instruction);
        if (instruction.op == LirOpcode::Panic)
            return;
    }
    SelectTerminator(lirBlock, *block.term);
}

void Selector::ComputeSuccessors() {
    for (auto &block : out.function.blocks) {
        block.successors.clear();
        for (const auto &instruction : block.instructions) {
            const auto op = static_cast<Op>(instruction.opcode);
            if (op != Op::Jmp && op != Op::Jcc)
                continue;
            const std::uint32_t target = instruction.operands.at(0).target;
            if (std::ranges::find(block.successors, target) == block.successors.end())
                block.successors.push_back(target);
        }
    }
}

std::optional<SelectedFunction> Selector::Run() {
    if (!CollectFacts() || !Eligible())
        return std::nullopt;
    ComputeLastUses();
    const std::uint32_t entry = NewBlock(0, false);
    out.blockLabels.back() = "entry";
    MarkBlocks();
    for (const auto &block : func.blocks) {
        for (const auto &instruction : block.instrs) {
            if (instruction.op != LirOpcode::Alloca)
                continue;
            const int size = AllocaSize(instruction);
            allocas.emplace(instruction.dst, out.function.NewFrameObject(size > 0 ? size : 8,
                                                                         std::max(8, StorageAlign(instruction.type))));
        }
    }
    current = entry;
    SelectParameters();
    Emit(Op::Jmp, 8, {Operand::BlockTarget(blockEntry[0])});
    for (std::uint32_t block = 0; block < func.blocks.size(); ++block)
        SelectBlock(block);
    ComputeSuccessors();
    out.outgoingArgumentSize = AlignUp(outgoingSize, 16);
    return std::move(out);
}
} // namespace Detail

std::optional<SelectedFunction> SelectFunction(const LirFunc &function, const SelectionContext &context) {
    return Detail::Selector(function, context).Run();
}
} // namespace Rux::AArch64Machine
