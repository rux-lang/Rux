#include "CodeGen/X86_64/MachineSelection.h"

#include "CodeGen/IntegerLiteral.h"
#include "CodeGen/X86_64/MachineSelector.h"
#include "Ir/Lir/ControlFlow.h"
#include "Ir/Lir/NaturalLoops.h"
#include "Target/CallingConvention.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <limits>
#include <string_view>

namespace Rux::X86_64Machine {
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

bool Selector::IsInterface(const TypeRef &type) const {
    return type.kind == TypeRef::Kind::Named && context.interfaceNames.contains(BaseTypeName(type.name));
}

bool Selector::IsAggregate(const TypeRef &type) const {
    if (IsWideValue(type) || type.IsRange() || type.IsSlice())
        return true;
    switch (type.kind) {
    case TypeRef::Kind::Tuple:
    case TypeRef::Kind::Array:
    case TypeRef::Kind::Sum:
    case TypeRef::Kind::Optional:
    case TypeRef::Kind::Fallible:
        return true;
    case TypeRef::Kind::Named: {
        const std::string base = BaseTypeName(type.name);
        return context.interfaceNames.contains(base) || context.layouts.contains(base) ||
               (!type.inner.empty() && SizeOf(type) > 8);
    }
    default:
        return false;
    }
}

ValueKind Selector::KindOf(const TypeRef &type) const {
    if (IsWideValue(type) || IsSoftwareFloat(type))
        return ValueKind::Unsupported;
    switch (type.kind) {
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
        return ValueKind::Xmm;
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
    const TypeRef &elementType = instruction.type.inner.empty() ? instruction.type : instruction.type.inner[0];
    const int elementSize = Size(elementType);
    return count * (elementSize > 0 ? elementSize : 8);
}

bool Selector::IsWin64ByRefAggregate(const TypeRef &type) const {
    if (!IsAggregate(type))
        return false;
    const int size = Size(type);
    return size > 0 && size != 1 && size != 2 && size != 4 && size != 8;
}

bool Selector::IsSysVMemoryAggregate(const TypeRef &type) const {
    return IsAggregate(type) && Size(type) > 16;
}

bool Selector::IsPointerTo(const LirReg reg, const TypeRef &pointee) const {
    const TypeRef &type = TypeOf(reg);
    return type.kind == TypeRef::Kind::Pointer && !type.inner.empty() && type.inner[0] == pointee;
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
    for (const auto &parameter : func.params) {
        const TypeRef registerType = parameter.type.IsSlice() || IsInterface(parameter.type)
                                       ? TypeRef::MakePointer(parameter.type)
                                       : parameter.type;
        if (!types.emplace(parameter.reg, registerType).second)
            return false;
    }
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

bool Selector::EligibleInstruction(const LirInstr &instruction) const {
    const auto kind = [&](const std::size_t index) { return KindOfReg(instruction.srcs.at(index)); };
    // A result of no type is no value at all: a call to a function returning nothing, or a placeholder.
    const bool opaque = instruction.dst != LirNoReg && TypeOf(instruction.dst).IsOpaque();
    if (opaque && instruction.op != LirOpcode::Call && instruction.op != LirOpcode::CallIndirect &&
        instruction.op != LirOpcode::Const)
        return false;
    const ValueKind result = instruction.dst != LirNoReg && !opaque ? KindOf(TypeOf(instruction.dst)) : ValueKind::Gpr;
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
        if (instruction.srcs.size() != 2 || (result != ValueKind::Gpr && result != ValueKind::Xmm) ||
            kind(0) != result || kind(1) != result)
            return false;
        if (result == ValueKind::Xmm)
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
               (instruction.type.kind == TypeRef::Kind::UInt32 || instruction.type.kind == TypeRef::Kind::UInt64) &&
               ParseIntegerLiteralBits(instruction.strArg).has_value();
    case LirOpcode::Neg:
        return instruction.srcs.size() == 1 && (result == ValueKind::Gpr || result == ValueKind::Xmm) &&
               kind(0) == result;
    case LirOpcode::Not:
    case LirOpcode::BitNot:
        return instruction.srcs.size() == 1 && result == ValueKind::Gpr && kind(0) == ValueKind::Gpr;
    case LirOpcode::Sqrt:
        return instruction.srcs.size() == 1 && result == ValueKind::Xmm && kind(0) == ValueKind::Xmm;
    case LirOpcode::CmpEq:
    case LirOpcode::CmpNe:
    case LirOpcode::CmpLt:
    case LirOpcode::CmpLe:
    case LirOpcode::CmpGt:
    case LirOpcode::CmpGe:
        return instruction.srcs.size() == 2 && result == ValueKind::Gpr &&
               (kind(0) == ValueKind::Gpr || kind(0) == ValueKind::Xmm) && kind(1) == kind(0);
    case LirOpcode::Cast:
        return instruction.srcs.size() == 1 && (result == ValueKind::Gpr || result == ValueKind::Xmm) &&
               (kind(0) == ValueKind::Gpr || kind(0) == ValueKind::Xmm);
    case LirOpcode::Call:
    case LirOpcode::CallIndirect:
        if (instruction.isCVariadic)
            return false;
        if (instruction.op == LirOpcode::CallIndirect && (instruction.srcs.empty() || kind(0) != ValueKind::Gpr))
            return false;
        return true;
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
    for (const auto &[reg, type] : types)
        if (!type.IsOpaque() && KindOf(type) == ValueKind::Unsupported)
            return false;
    if (!func.returnType.IsOpaque() && KindOf(func.returnType) == ValueKind::Unsupported)
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
    for (std::size_t block = 0; block < func.blocks.size(); ++block) {
        std::unordered_set<LirReg> live = liveness.liveOut[block];
        const auto &term = func.blocks[block].term;
        if (term) {
            live.insert(term->cond);
            if (term->retVal)
                live.insert(*term->retVal);
        }
        const auto &instructions = func.blocks[block].instrs;
        for (std::size_t reverse = instructions.size(); reverse > 0; --reverse) {
            const auto &instruction = instructions[reverse - 1];
            if (instruction.op == LirOpcode::Phi)
                continue;
            for (const auto source : instruction.srcs) {
                if (!live.contains(source)) {
                    lastUses[&instruction].push_back(source);
                    live.insert(source);
                }
            }
            if (instruction.dst != LirNoReg)
                live.erase(instruction.dst);
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

Reg Selector::NewXmm() {
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
    const Reg created = KindOfReg(reg) == ValueKind::Xmm ? NewXmm() : NewGpr();
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

Reg Selector::ExtendRegister(const Reg value, const int size, const bool isSigned) {
    if (size >= 8)
        return value;
    const Reg extended = NewGpr();
    auto &instruction =
        Emit(isSigned ? Op::MovSx : Op::MovZx, isSigned ? 8 : 4, {Operand::Def(extended), Operand::Use(value)});
    instruction.sourceSize = static_cast<std::uint8_t>(size);
    return extended;
}

Reg Selector::Extended(const LirReg reg, const bool forceSigned, const bool forceUnsigned) {
    const TypeRef &type = TypeOf(reg);
    const int size = ScalarSize(type);
    if (size >= 8 || type.IsBool())
        return Materialize(reg);
    if (const auto constant = integerConstants.find(reg); constant != integerConstants.end()) {
        const int bits = size * 8;
        std::uint64_t value = constant->second & ((std::uint64_t{1} << bits) - 1);
        const bool isSigned = forceSigned || (!forceUnsigned && type.IsSigned());
        if (isSigned && (value >> (bits - 1)) != 0)
            value |= ~((std::uint64_t{1} << bits) - 1);
        const Reg materialized = NewGpr();
        auto &move = Emit(Op::MovImm, 8, {Operand::Def(materialized), Operand::Imm(static_cast<std::int64_t>(value))});
        move.rematerializable = true;
        return materialized;
    }
    const bool isSigned = forceSigned || (!forceUnsigned && type.IsSigned());
    if (AlreadyExtended(reg, size, isSigned))
        return Value(reg);
    return ExtendRegister(Value(reg), size, isSigned);
}

Reg Selector::Materialize(const LirReg reg) {
    return Value(reg);
}

std::optional<std::int64_t> Selector::Immediate(const LirReg reg, const int width) const {
    const auto found = integerConstants.find(reg);
    if (found == integerConstants.end())
        return std::nullopt;
    const std::uint64_t bits = found->second;
    if (width >= 8) {
        const auto value = static_cast<std::int64_t>(bits);
        if (value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max())
            return std::nullopt;
        return value;
    }
    // A narrower operation reads only the low bits of its immediate, so any value encodes once truncated.
    return static_cast<std::int64_t>(static_cast<std::int32_t>(static_cast<std::uint32_t>(bits)));
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
    case LirOpcode::Alloca: {
        Address address;
        address.frameObject = allocas.contains(pointer) ? allocas.at(pointer) : -1;
        return address.frameObject >= 0 ? address : direct;
    }
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
        const int elementSize = instruction.type.kind == TypeRef::Kind::Pointer && !instruction.type.inner.empty()
                                  ? std::max(1, Size(instruction.type.inner[0]))
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
        if (elementSize != 1 && elementSize != 2 && elementSize != 4 && elementSize != 8)
            return direct;
        // A RIP-relative or already indexed base cannot take another index: the base pointer itself becomes the
        // base register, which is rematerialized or hoisted like any other address.
        Address result = base;
        if (base.index != NoReg || base.symbol != Machine::NoSymbol) {
            result = Address{};
            result.base = Value(instruction.srcs[0]);
        }
        result.index = Extended(index);
        result.scale = static_cast<std::uint8_t>(elementSize);
        return result;
    }
    default:
        return direct;
    }
}

void Selector::CopyMemory(const Address &destination, const Address &source, const int size) {
    if (size <= 0)
        return;
    if (size > 256) {
        const Reg sourceAddress = NewGpr();
        const Reg destinationAddress = NewGpr();
        const Reg count = NewGpr();
        Emit(Op::Lea, 8, {Operand::Def(sourceAddress), Operand::Mem(source)});
        Emit(Op::Lea, 8, {Operand::Def(destinationAddress), Operand::Mem(destination)});
        Emit(Op::MovImm, 8, {Operand::Def(count), Operand::Imm(size)}).rematerializable = true;
        EmitCopy(Rsi, sourceAddress, RegClass::General);
        EmitCopy(Rdi, destinationAddress, RegClass::General);
        EmitCopy(Rcx, count, RegClass::General);
        auto &copy = Emit(Op::RepMovsb, 1, {});
        copy.implicitUses = Machine::RegisterBit(Rsi) | Machine::RegisterBit(Rdi) | Machine::RegisterBit(Rcx);
        copy.implicitDefs = copy.implicitUses;
        return;
    }
    int offset = 0;
    for (const int chunk : {8, 4, 2, 1}) {
        while (offset + chunk <= size) {
            bool fits = true;
            const Address from = Offset(source, offset, fits);
            const Address to = Offset(destination, offset, fits);
            const Reg temporary = NewGpr();
            auto &load = Emit(Op::Load, chunk == 8 ? 8 : 4, {Operand::Def(temporary), Operand::Mem(from)});
            load.sourceSize = static_cast<std::uint8_t>(chunk);
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

void Selector::LoadInto(const Reg destination, const Address &address, const TypeRef &type) {
    if (IsFloat(type)) {
        Emit(Op::LoadFp, static_cast<std::uint8_t>(Size(type)), {Operand::Def(destination), Operand::Mem(address)});
        return;
    }
    const int size = ScalarSize(type);
    const bool isSigned = type.IsSigned() && size < 8;
    auto &load = Emit(isSigned ? Op::LoadSigned : Op::Load, size == 8 || isSigned ? 8 : 4,
                      {Operand::Def(destination), Operand::Mem(address)});
    load.sourceSize = static_cast<std::uint8_t>(size);
}

// ---- memory and addresses ----------------------------------------------------------------------------------------

void Selector::SelectConstant(const LirInstr &instruction) {
    switch (KindOf(instruction.type)) {
    case ValueKind::Gpr: {
        const Reg destination = Value(instruction.dst);
        const std::uint64_t bits = integerConstants.at(instruction.dst);
        const int size = ScalarSize(instruction.type);
        std::uint64_t value = bits;
        if (size < 8)
            value &= (std::uint64_t{1} << (size * 8)) - 1;
        auto &move = Emit(Op::MovImm, 8, {Operand::Def(destination), Operand::Imm(static_cast<std::int64_t>(value))});
        move.rematerializable = true;
        return;
    }
    case ValueKind::Xmm: {
        const auto symbol = Symbol(
            {instruction.type.kind == TypeRef::Kind::Float32 ? SymbolRef::Kind::Float32 : SymbolRef::Kind::Float64,
             instruction.strArg});
        Address address;
        address.symbol = symbol;
        auto &load = Emit(Op::LoadFp, static_cast<std::uint8_t>(Size(instruction.type)),
                          {Operand::Def(Value(instruction.dst)), Operand::Mem(address)});
        load.rematerializable = true;
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
        const Reg temporary = NewGpr();
        Emit(Op::MovImm, 8, {Operand::Def(temporary), Operand::Imm(static_cast<std::int64_t>(bits))});
        Emit(Op::Store, static_cast<std::uint8_t>(width),
             {Operand::Mem(HomeAddress(instruction.dst)), Operand::Use(temporary)});
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
    case ValueKind::Xmm:
        Emit(Op::StoreFp, static_cast<std::uint8_t>(Size(type)), {Operand::Mem(address), Operand::Use(Value(value))});
        return;
    default: {
        const int size = ScalarSize(type);
        if (const auto immediate = Immediate(value, size)) {
            Emit(Op::Store, static_cast<std::uint8_t>(size), {Operand::Mem(address), Operand::Imm(*immediate)});
            return;
        }
        Emit(Op::Store, static_cast<std::uint8_t>(size), {Operand::Mem(address), Operand::Use(Value(value))});
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
        auto &lea = Emit(Op::Lea, 8, {Operand::Def(destination), Operand::Mem(address)});
        lea.rematerializable = true;
        return;
    }
    case LirOpcode::GlobalAddr: {
        Address address;
        address.symbol = Symbol({SymbolRef::Kind::Global, instruction.strArg});
        auto &lea = Emit(Op::Lea, 8, {Operand::Def(destination), Operand::Mem(address)});
        lea.rematerializable = true;
        return;
    }
    case LirOpcode::StringAddr: {
        const TypeRef elementType = instruction.type.inner.empty() ? TypeRef::MakeChar8() : instruction.type.inner[0];
        Address address;
        address.symbol = Symbol({SymbolRef::Kind::String, EncodeStringLiteral(instruction.strArg, Size(elementType))});
        auto &lea = Emit(Op::Lea, 8, {Operand::Def(destination), Operand::Mem(address)});
        lea.rematerializable = true;
        return;
    }
    case LirOpcode::FramePointer:
        Emit(Op::Mov, 8, {Operand::Def(destination), Operand::Use(Rbp)});
        return;
    case LirOpcode::IndexPtr: {
        const int elementSize = instruction.type.kind == TypeRef::Kind::Pointer && !instruction.type.inner.empty()
                                  ? std::max(1, Size(instruction.type.inner[0]))
                                  : 8;
        if (elementSize != 1 && elementSize != 2 && elementSize != 4 && elementSize != 8 &&
            !integerConstants.contains(instruction.srcs[1])) {
            const Reg scaled = NewGpr();
            Emit(Op::ImulImm, 8,
                 {Operand::Def(scaled), Operand::Use(Extended(instruction.srcs[1])), Operand::Imm(elementSize)});
            Address address;
            address.base = Value(instruction.srcs[0]);
            address.index = scaled;
            Emit(Op::Lea, 8, {Operand::Def(destination), Operand::Mem(address)});
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
                fallback.scale = static_cast<std::uint8_t>(instruction.type.kind == TypeRef::Kind::Pointer &&
                                                                   !instruction.type.inner.empty()
                                                               ? std::max(1, Size(instruction.type.inner[0]))
                                                               : 8);
            }
            Emit(Op::Lea, 8, {Operand::Def(destination), Operand::Mem(fallback)});
            return;
        }
        if (address.frameObject < 0 && address.symbol == Machine::NoSymbol && address.index == NoReg &&
            address.displacement == 0) {
            EmitCopy(destination, address.base, RegClass::General);
            return;
        }
        auto &lea = Emit(Op::Lea, 8, {Operand::Def(destination), Operand::Mem(address)});
        lea.rematerializable = address.base == NoReg && address.index == NoReg;
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
        if (KindOf(instruction.type) == ValueKind::Xmm)
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
    // A block is cold when every path from it ends in a failure report: it panics, or it only reaches cold blocks.
    std::vector<bool> cold(func.blocks.size(), false);
    for (std::size_t block = 0; block < func.blocks.size(); ++block) {
        const auto &instructions = func.blocks[block].instrs;
        cold[block] = std::ranges::any_of(
            instructions, [](const LirInstr &instruction) { return instruction.op == LirOpcode::Panic; });
    }
    bool changed = true;
    while (changed) {
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
        case ValueKind::Xmm: {
            auto found = phiTemporaries.find(instruction.dst);
            if (found == phiTemporaries.end())
                found = phiTemporaries.emplace(instruction.dst, NewXmm()).first;
            EmitCopy(found->second, Value(source), RegClass::Vector);
            break;
        }
        default: {
            auto found = phiTemporaries.find(instruction.dst);
            if (found == phiTemporaries.end())
                found = phiTemporaries.emplace(instruction.dst, NewGpr()).first;
            if (const auto constant = integerConstants.find(source); constant != integerConstants.end()) {
                auto &move =
                    Emit(Op::MovImm, 8,
                         {Operand::Def(found->second), Operand::Imm(static_cast<std::int64_t>(constant->second))});
                static_cast<void>(move);
            }
            else {
                EmitCopy(found->second, Value(source), RegClass::General);
            }
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
        case ValueKind::Xmm: {
            auto found = phiTemporaries.find(instruction.dst);
            if (found == phiTemporaries.end())
                found = phiTemporaries.emplace(instruction.dst, NewXmm()).first;
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
    const auto jump = [&](const Cond condition, const std::uint32_t target) {
        auto &branch = Emit(Op::Jcc, 8, {Operand::BlockTarget(target)});
        branch.condition = static_cast<std::uint8_t>(condition);
    };
    if (Fusable(lirBlock, terminator)) {
        const LirInstr &comparison = *definitions.at(terminator.cond);
        const bool floating = KindOfReg(comparison.srcs[0]) == ValueKind::Xmm;
        const Cond condition = EmitCompare(comparison);
        if (floating && comparison.op == LirOpcode::CmpEq) {
            jump(Cond::Parity, whenFalse);
            jump(Cond::Equal, whenTrue);
        }
        else if (floating && comparison.op == LirOpcode::CmpNe) {
            jump(Cond::Parity, whenTrue);
            jump(Cond::NotEqual, whenTrue);
        }
        else {
            jump(condition, whenTrue);
        }
    }
    else if (const auto definition = definitions.find(terminator.cond); definition != definitions.end() &&
                                                                        fusedComparisons.contains(definition->second) &&
                                                                        definition->second->op == LirOpcode::Not) {
        // A branch on a negation tests the operand and takes the opposite edge.
        const LirInstr &negation = *definition->second;
        const Reg operand = Value(negation.srcs[0]);
        Emit(Op::Test, static_cast<std::uint8_t>(ScalarSize(negation.type)),
             {Operand::Use(operand), Operand::Use(operand)});
        jump(Cond::Equal, whenTrue);
    }
    else {
        const Reg condition = Value(terminator.cond);
        Emit(Op::Test, 4, {Operand::Use(condition), Operand::Use(condition)});
        jump(Cond::NotEqual, whenTrue);
    }
    Emit(Op::Jmp, 8, {Operand::BlockTarget(whenFalse)});
}

void Selector::EmitSwitch(const LirTerminator &terminator, const std::uint32_t lirBlock) {
    const TypeRef &type = TypeOf(terminator.cond);
    const int size = ScalarSize(type);
    const Reg value = Value(terminator.cond);
    for (const auto &entry : terminator.cases) {
        const std::uint64_t bits = ParseIntegerLiteralBits(entry.value).value_or(0);
        const std::uint32_t target = EdgeTarget(lirBlock, entry.target, true);
        const auto signedBits = static_cast<std::int64_t>(bits);
        if (size < 8 || (signedBits >= std::numeric_limits<std::int32_t>::min() &&
                         signedBits <= std::numeric_limits<std::int32_t>::max())) {
            Emit(Op::Cmp, static_cast<std::uint8_t>(size),
                 {Operand::Use(value), Operand::Imm(static_cast<std::int32_t>(static_cast<std::uint32_t>(bits)))});
        }
        else {
            const Reg wide = NewGpr();
            Emit(Op::MovImm, 8, {Operand::Def(wide), Operand::Imm(signedBits)}).rematerializable = true;
            Emit(Op::Cmp, 8, {Operand::Use(value), Operand::Use(wide)});
        }
        auto &branch = Emit(Op::Jcc, 8, {Operand::BlockTarget(target)});
        branch.condition = static_cast<std::uint8_t>(Cond::Equal);
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
        Emit(Op::Ud2, 8, {});
        return;
    }
}

void Selector::SelectBlock(const std::uint32_t lirBlock) {
    current = blockEntry[lirBlock];
    EmitPhiDestinations(lirBlock);
    const auto &block = func.blocks[lirBlock];
    if (block.term && Fusable(lirBlock, *block.term)) {
        const LirInstr *comparison = definitions.at(block.term->cond);
        // Materialized as well only when something besides the branch reads the result.
        if (useCounts[comparison->dst] <= 1)
            fusedComparisons.insert(comparison);
    }
    else if (block.term && block.term->kind == LirTermKind::Branch) {
        // A negation only the branch reads is folded into the branch.
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
    win64 = UsesWin64(func.callConv);
    out.win64 = win64;
    if (!CollectFacts() || !Eligible())
        return std::nullopt;
    ComputeLastUses();
    const std::uint32_t entry = NewBlock(0, false);
    out.blockLabels.back() = "entry";
    MarkBlocks();
    // Every allocation's storage exists before any block is selected, so an address folds into its users wherever
    // the allocation sits in block order.
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
    if (hasPanic && win64)
        outgoingSize = std::max(outgoingSize, 48);
    out.outgoingArgumentSize = AlignUp(outgoingSize, 16);
    return std::move(out);
}
} // namespace Detail

std::optional<SelectedFunction> SelectFunction(const LirFunc &function, const SelectionContext &context) {
    return Detail::Selector(function, context).Run();
}
} // namespace Rux::X86_64Machine
