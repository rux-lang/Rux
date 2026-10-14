// Parameters, calls, returns and runtime failures under the Win64 and System V conventions.
//
// Every placement decision reproduces the established x86-64 emitter (`CallAndTerminatorEmitter.cpp`,
// `RcuEmitter.cpp`), so a function selected here and one emitted there call each other correctly: aggregates of 1, 2,
// 4 or 8 bytes travel in a register under Win64 and anything else by address; System V passes up to 16 bytes in one
// or two integer registers and the rest in memory; narrow scalars are extended to 64 bits before they are passed.

#include "CodeGen/RuntimeFailure.h"
#include "CodeGen/X86_64/MachineSelector.h"
#include "Target/CallingConvention.h"

namespace Rux::X86_64Machine::Detail {
using namespace Layout;

namespace {
[[nodiscard]] Address IncomingArgument(const std::int32_t offset) {
    Address address;
    address.base = Rbp;
    address.displacement = offset;
    return address;
}

[[nodiscard]] Address OutgoingArgument(const std::int32_t offset) {
    Address address;
    address.base = Rsp;
    address.displacement = offset;
    return address;
}

[[nodiscard]] Address Based(const Reg base, const std::int32_t displacement = 0) {
    Address address;
    address.base = base;
    address.displacement = displacement;
    return address;
}
} // namespace

bool Selector::UsesWin64(const CallingConvention convention) const {
    const CallingConvention effective = convention == CallingConvention::Default
                                          ? PlatformDefaultConvention(context.targetOs, Target::Arch::X86_64)
                                          : ResolveCConvention(convention, context.targetOs, Target::Arch::X86_64);
    return effective == CallingConvention::Win64;
}

bool Selector::IsCallClobberingWin64(const CallingConvention convention) const {
    return UsesWin64(convention);
}

void Selector::PassScalar(const LirReg argument, const TypeRef &type, const Reg destination) {
    const int size = ScalarSize(type);
    if (const auto constant = integerConstants.Find(argument); constant != nullptr) {
        std::uint64_t value = (*constant);
        if (size < 8) {
            const int bits = size * 8;
            value &= (std::uint64_t{1} << bits) - 1;
            if (type.IsSigned() && (value >> (bits - 1)) != 0)
                value |= ~((std::uint64_t{1} << bits) - 1);
        }
        Emit(Op::MovImm, 8, {Operand::Def(destination), Operand::Imm(static_cast<std::int64_t>(value))});
        return;
    }
    if (size >= 8 || type.IsBool()) {
        EmitCopy(destination, Value(argument), RegClass::General);
        return;
    }
    const bool isSigned = type.IsSigned();
    auto &extend = Emit(isSigned ? Op::MovSx : Op::MovZx, isSigned ? 8 : 4,
                        {Operand::Def(destination), Operand::Use(Value(argument))});
    extend.sourceSize = static_cast<std::uint8_t>(size);
}

void Selector::SelectParameters() {
    const bool hasResult = !func.returnType.IsOpaque();
    const bool hidden =
        hasResult && (win64 ? IsWin64ByRefAggregate(func.returnType) : IsSysVMemoryAggregate(func.returnType));
    int integerIndex = 0;
    int floatIndex = 0;
    int stackIndex = 0;
    int win64Index = 0;
    if (hidden) {
        hiddenReturnPointer = NewGpr();
        EmitCopy(hiddenReturnPointer, win64 ? Rcx : Rdi, RegClass::General);
        win64Index = 1;
        integerIndex = 1;
    }
    for (const auto &parameter : func.params) {
        const TypeRef &type = TypeOf(parameter.reg);
        const ValueKind kind = KindOf(type);
        const int size = Size(type);
        if (win64) {
            if (win64Index >= 4) {
                const Address slot = IncomingArgument(48 + (win64Index - 4) * 8);
                if (kind == ValueKind::Aggregate) {
                    if (IsWin64ByRefAggregate(type)) {
                        const Reg pointer = NewGpr();
                        LoadInto(pointer, slot, TypeRef::MakeUInt64());
                        CopyMemory(HomeAddress(parameter.reg), Based(pointer), size);
                    }
                    else {
                        CopyMemory(HomeAddress(parameter.reg), slot, size);
                    }
                }
                else {
                    LoadInto(Value(parameter.reg), slot, type);
                }
                ++win64Index;
                continue;
            }
            const Reg argument = Win64IntegerArguments[win64Index];
            if (kind == ValueKind::Xmm) {
                EmitCopy(Value(parameter.reg), Xmm0 + static_cast<Reg>(win64Index), RegClass::Vector);
            }
            else if (kind == ValueKind::Aggregate) {
                if (IsWin64ByRefAggregate(type))
                    CopyMemory(HomeAddress(parameter.reg), Based(argument), size);
                else if (size > 0)
                    Emit(Op::Store, static_cast<std::uint8_t>(size),
                         {Operand::Mem(HomeAddress(parameter.reg)), Operand::Use(argument)});
            }
            else {
                EmitCopy(Value(parameter.reg), argument, RegClass::General);
            }
            ++win64Index;
            continue;
        }
        if (IsSysVMemoryAggregate(type)) {
            const int words = AlignUp(size, 8) / 8;
            CopyMemory(HomeAddress(parameter.reg), IncomingArgument(16 + stackIndex * 8), words * 8);
            stackIndex += words;
            continue;
        }
        if (kind == ValueKind::Xmm) {
            if (floatIndex < 8)
                EmitCopy(Value(parameter.reg), Xmm0 + static_cast<Reg>(floatIndex++), RegClass::Vector);
            else
                LoadInto(Value(parameter.reg), IncomingArgument(16 + stackIndex++ * 8), type);
            continue;
        }
        if (kind == ValueKind::Aggregate && size > 8 && size <= 16) {
            const Address home = HomeAddress(parameter.reg);
            bool fits = true;
            if (integerIndex <= 4) {
                Emit(Op::Store, 8, {Operand::Mem(home), Operand::Use(SysVIntegerArguments[integerIndex++])});
                Emit(Op::Store, 8,
                     {Operand::Mem(Offset(home, 8, fits)), Operand::Use(SysVIntegerArguments[integerIndex++])});
            }
            else {
                CopyMemory(home, IncomingArgument(16 + stackIndex * 8), 16);
                stackIndex += 2;
            }
            continue;
        }
        if (integerIndex < 6) {
            const Reg argument = SysVIntegerArguments[integerIndex++];
            if (kind == ValueKind::Aggregate)
                Emit(Op::Store, 8, {Operand::Mem(HomeAddress(parameter.reg)), Operand::Use(argument)});
            else
                EmitCopy(Value(parameter.reg), argument, RegClass::General);
            continue;
        }
        const Address slot = IncomingArgument(16 + stackIndex++ * 8);
        if (kind == ValueKind::Aggregate)
            CopyMemory(HomeAddress(parameter.reg), slot, SlotSize(type));
        else
            LoadInto(Value(parameter.reg), slot, type);
    }
}

bool Selector::SelectBitCast(const LirInstr &instruction) {
    if (instruction.op != LirOpcode::Call || instruction.srcs.size() != 1 || instruction.dst == LirNoReg)
        return false;
    const std::string &name = instruction.strArg;
    // Lowering routes an unsigned 64-bit conversion to floating point through a software helper, because the hardware
    // conversion is signed. Two instructions and a halving step do it exactly, so the call never happens here.
    if ((name == "__rux_float_f64_from_uint64" || name == "__rux_float_f32_from_uint64") &&
        KindOfReg(instruction.srcs[0]) == ValueKind::Gpr && KindOfReg(instruction.dst) == ValueKind::Xmm) {
        const Reg half = NewGpr();
        const Reg low = NewGpr();
        Emit(Op::UInt64ToFp, name == "__rux_float_f64_from_uint64" ? 8 : 4,
             {Operand::Def(Value(instruction.dst)), Operand::Use(Value(instruction.srcs[0])), Operand::Def(half),
              Operand::Def(low)});
        return true;
    }
    Op op;
    std::uint8_t size;
    if (name == "FloatBits64") {
        op = Op::XmmToGpr;
        size = 8;
    }
    else if (name == "FloatFromBits64") {
        op = Op::GprToXmm;
        size = 8;
    }
    else if (name == "FloatBits32") {
        op = Op::XmmToGpr;
        size = 4;
    }
    else if (name == "FloatFromBits32") {
        op = Op::GprToXmm;
        size = 4;
    }
    else {
        return false;
    }
    Emit(op, size, {Operand::Def(Value(instruction.dst)), Operand::Use(Value(instruction.srcs[0]))});
    return true;
}

void Selector::SelectCall(const LirInstr &instruction) {
    if (SelectBitCast(instruction))
        return;
    const bool callWin64 = UsesWin64(instruction.callConv);
    const bool indirect = instruction.op == LirOpcode::CallIndirect;
    const std::vector<LirReg> arguments(instruction.srcs.begin() + (indirect ? 1 : 0), instruction.srcs.end());
    const TypeRef &type = instruction.type;
    const bool hasResult = instruction.dst != LirNoReg && !type.IsOpaque();
    const bool hidden = hasResult && (callWin64 ? IsWin64ByRefAggregate(type) : IsSysVMemoryAggregate(type));
    const int start = hidden ? 1 : 0;
    std::uint64_t uses = 0;

    struct RegisterArgument {
        LirReg value;
        Reg destination;
        enum class Mode : std::uint8_t {
            Scalar,
            Float,
            Address,
            Load,
            LoadWord,
        } mode;
        std::int32_t offset = 0;
        int size = 8;
    };

    std::vector<RegisterArgument> registerArguments;

    if (callWin64) {
        const std::size_t count = arguments.size() + static_cast<std::size_t>(start);
        outgoingSize = std::max(outgoingSize, AlignUp(static_cast<int>(32 + (count > 4 ? count - 4 : 0) * 8), 16));
        int index = start;
        for (const LirReg argument : arguments) {
            const TypeRef &argumentType = TypeOf(argument);
            const ValueKind kind = KindOf(argumentType);
            const int size = Size(argumentType);
            if (index >= 4) {
                const Address slot = OutgoingArgument(32 + (index - 4) * 8);
                if (kind == ValueKind::Xmm) {
                    Emit(Op::StoreFp, static_cast<std::uint8_t>(size),
                         {Operand::Mem(slot), Operand::Use(Value(argument))});
                }
                else if (kind == ValueKind::Aggregate && IsWin64ByRefAggregate(argumentType)) {
                    const Reg pointer = NewGpr();
                    Emit(Op::Lea, 8, {Operand::Def(pointer), Operand::Mem(HomeAddress(argument))});
                    Emit(Op::Store, 8, {Operand::Mem(slot), Operand::Use(pointer)});
                }
                else if (kind == ValueKind::Aggregate) {
                    const Reg word = NewGpr();
                    LoadInto(word, HomeAddress(argument),
                             size == 8   ? TypeRef::MakeUInt64()
                             : size == 4 ? TypeRef::MakeUInt32()
                             : size == 2 ? TypeRef::MakeUInt16()
                                         : TypeRef::MakeUInt8());
                    Emit(Op::Store, 8, {Operand::Mem(slot), Operand::Use(word)});
                }
                else {
                    const Reg value = NewGpr();
                    PassScalar(argument, argumentType, value);
                    Emit(Op::Store, 8, {Operand::Mem(slot), Operand::Use(value)});
                }
                ++index;
                continue;
            }
            const Reg integerRegister = Win64IntegerArguments[index];
            if (kind == ValueKind::Xmm)
                registerArguments.push_back({argument, Xmm0 + static_cast<Reg>(index), RegisterArgument::Mode::Float});
            else if (kind == ValueKind::Aggregate && IsWin64ByRefAggregate(argumentType))
                registerArguments.push_back({argument, integerRegister, RegisterArgument::Mode::Address});
            else if (kind == ValueKind::Aggregate)
                registerArguments.push_back({argument, integerRegister, RegisterArgument::Mode::Load, 0, size});
            else
                registerArguments.push_back({argument, integerRegister, RegisterArgument::Mode::Scalar});
            ++index;
        }
    }
    else {
        int integerIndex = start;
        int floatIndex = 0;
        int stackIndex = 0;
        for (const LirReg argument : arguments) {
            const TypeRef &argumentType = TypeOf(argument);
            const ValueKind kind = KindOf(argumentType);
            const int size = Size(argumentType);
            if (IsSysVMemoryAggregate(argumentType)) {
                const int bytes = AlignUp(size, 8);
                CopyMemory(OutgoingArgument(stackIndex * 8), HomeAddress(argument), bytes);
                stackIndex += bytes / 8;
                continue;
            }
            if (kind == ValueKind::Aggregate && size > 8 && size <= 16) {
                if (integerIndex <= 4) {
                    registerArguments.push_back(
                        {argument, SysVIntegerArguments[integerIndex++], RegisterArgument::Mode::LoadWord, 0, 8});
                    registerArguments.push_back(
                        {argument, SysVIntegerArguments[integerIndex++], RegisterArgument::Mode::LoadWord, 8, 8});
                }
                else {
                    CopyMemory(OutgoingArgument(stackIndex * 8), HomeAddress(argument), 16);
                    stackIndex += 2;
                }
                continue;
            }
            if (kind == ValueKind::Xmm) {
                if (floatIndex < 8) {
                    registerArguments.push_back(
                        {argument, Xmm0 + static_cast<Reg>(floatIndex++), RegisterArgument::Mode::Float});
                }
                else {
                    Emit(Op::StoreFp, static_cast<std::uint8_t>(size),
                         {Operand::Mem(OutgoingArgument(stackIndex++ * 8)), Operand::Use(Value(argument))});
                }
                continue;
            }
            if (integerIndex < 6) {
                const Reg integerRegister = SysVIntegerArguments[integerIndex++];
                if (kind == ValueKind::Aggregate)
                    registerArguments.push_back({argument, integerRegister, RegisterArgument::Mode::Load, 0, size});
                else
                    registerArguments.push_back({argument, integerRegister, RegisterArgument::Mode::Scalar});
                continue;
            }
            const Address slot = OutgoingArgument(stackIndex++ * 8);
            const Reg value = NewGpr();
            if (kind == ValueKind::Aggregate) {
                LoadInto(value, HomeAddress(argument),
                         size == 1   ? TypeRef::MakeUInt8()
                         : size == 2 ? TypeRef::MakeUInt16()
                         : size == 4 ? TypeRef::MakeUInt32()
                                     : TypeRef::MakeUInt64());
            }
            else {
                PassScalar(argument, argumentType, value);
            }
            Emit(Op::Store, 8, {Operand::Mem(slot), Operand::Use(value)});
        }
        outgoingSize = std::max(outgoingSize, AlignUp(stackIndex * 8, 16));
    }

    // Fixed registers are written last, so nothing computed on the way can need one of them.
    if (hidden) {
        const Reg pointer = callWin64 ? Rcx : Rdi;
        Emit(Op::Lea, 8, {Operand::Def(pointer), Operand::Mem(HomeAddress(instruction.dst))});
        uses |= Machine::RegisterBit(pointer);
    }
    for (const auto &argument : registerArguments) {
        uses |= Machine::RegisterBit(argument.destination);
        switch (argument.mode) {
        case RegisterArgument::Mode::Float:
            EmitCopy(argument.destination, Value(argument.value), RegClass::Vector);
            break;
        case RegisterArgument::Mode::Address:
            Emit(Op::Lea, 8, {Operand::Def(argument.destination), Operand::Mem(HomeAddress(argument.value))});
            break;
        case RegisterArgument::Mode::Load: {
            // An aggregate of an odd width up to eight bytes is padded to a word in its home, so the word is read.
            const int size = argument.size;
            LoadInto(argument.destination, HomeAddress(argument.value),
                     size == 1   ? TypeRef::MakeUInt8()
                     : size == 2 ? TypeRef::MakeUInt16()
                     : size == 4 ? TypeRef::MakeUInt32()
                                 : TypeRef::MakeUInt64());
            break;
        }
        case RegisterArgument::Mode::LoadWord: {
            bool fits = true;
            LoadInto(argument.destination, Offset(HomeAddress(argument.value), argument.offset, fits),
                     TypeRef::MakeUInt64());
            break;
        }
        case RegisterArgument::Mode::Scalar:
            PassScalar(argument.value, TypeOf(argument.value), argument.destination);
            break;
        }
    }
    if (indirect) {
        auto &call = Emit(Op::CallIndirect, 8, {Operand::Use(Value(instruction.srcs[0]))});
        call.implicitUses = uses;
        call.implicitDefs = CallClobbers(callWin64);
    }
    else {
        auto &call =
            Emit(Op::Call, 8, {Operand::SymbolTarget(Symbol({SymbolRef::Kind::Function, instruction.strArg}))});
        call.implicitUses = uses;
        call.implicitDefs = CallClobbers(callWin64);
    }
    if (!hasResult || hidden)
        return;
    const int size = Size(type);
    const ValueKind kind = KindOf(type);
    if (size > 8 && size <= 16) {
        const Address home = HomeAddress(instruction.dst);
        bool fits = true;
        Emit(Op::Store, 8, {Operand::Mem(home), Operand::Use(Rax)});
        Emit(Op::Store, 8, {Operand::Mem(Offset(home, 8, fits)), Operand::Use(Rdx)});
        return;
    }
    if (kind == ValueKind::Xmm) {
        EmitCopy(Value(instruction.dst), Xmm0, RegClass::Vector);
        return;
    }
    if (kind == ValueKind::Aggregate) {
        const int width = size == 1 || size == 2 || size == 4 ? size : 8;
        Emit(Op::Store, static_cast<std::uint8_t>(width),
             {Operand::Mem(HomeAddress(instruction.dst)), Operand::Use(Rax)});
        return;
    }
    EmitCopy(Value(instruction.dst), Rax, RegClass::General);
}

void Selector::SelectReturn(const LirTerminator &terminator) {
    std::uint64_t uses = 0;
    if (terminator.retVal && *terminator.retVal != LirNoReg && !terminator.retType.IsOpaque()) {
        const LirReg value = *terminator.retVal;
        const TypeRef &type = terminator.retType;
        const int size = Size(type);
        const ValueKind kind = KindOfReg(value);
        if (hiddenReturnPointer != NoReg) {
            Address source;
            if (kind == ValueKind::Aggregate)
                source = HomeAddress(value);
            else
                source.base = Value(value);
            CopyMemory(Based(hiddenReturnPointer), source, size);
            EmitCopy(Rax, hiddenReturnPointer, RegClass::General);
            uses = Machine::RegisterBit(Rax);
        }
        else if (KindOf(type) == ValueKind::Aggregate && kind == ValueKind::Gpr) {
            // The value is the address of the aggregate it returns.
            const Reg pointer = Value(value);
            if (size > 8 && size <= 16) {
                const Reg high = NewGpr();
                if (size == 16) {
                    LoadInto(high, Based(pointer, 8), TypeRef::MakeUInt64());
                }
                else {
                    // Only the live tail is read: the pointee may end at the last byte of its allocation.
                    Emit(Op::MovImm, 8, {Operand::Def(high), Operand::Imm(0)});
                    for (int offset = size - 1; offset >= 8; --offset) {
                        const Reg byte = NewGpr();
                        Emit(Op::Shl, 8, {Operand::UseDef(high), Operand::Imm(8)});
                        LoadInto(byte, Based(pointer, offset), TypeRef::MakeUInt8());
                        Emit(Op::Or, 8, {Operand::UseDef(high), Operand::Use(byte)});
                    }
                }
                LoadInto(Rax, Based(pointer), TypeRef::MakeUInt64());
                EmitCopy(Rdx, high, RegClass::General);
                uses = Machine::RegisterBit(Rax) | Machine::RegisterBit(Rdx);
            }
            else {
                LoadInto(Rax, Based(pointer),
                         size == 1   ? TypeRef::MakeUInt8()
                         : size == 2 ? TypeRef::MakeUInt16()
                         : size == 4 ? TypeRef::MakeUInt32()
                                     : TypeRef::MakeUInt64());
                uses = Machine::RegisterBit(Rax);
            }
        }
        else if (size > 8 && size <= 16) {
            const Address home = HomeAddress(value);
            bool fits = true;
            LoadInto(Rax, home, TypeRef::MakeUInt64());
            LoadInto(Rdx, Offset(home, 8, fits), TypeRef::MakeUInt64());
            uses = Machine::RegisterBit(Rax) | Machine::RegisterBit(Rdx);
        }
        else if (kind == ValueKind::Xmm) {
            EmitCopy(Xmm0, Value(value), RegClass::Vector);
            uses = Machine::RegisterBit(Xmm0);
        }
        else if (kind == ValueKind::Aggregate) {
            LoadInto(Rax, HomeAddress(value),
                     size == 1   ? TypeRef::MakeUInt8()
                     : size == 2 ? TypeRef::MakeUInt16()
                     : size == 4 ? TypeRef::MakeUInt32()
                                 : TypeRef::MakeUInt64());
            uses = Machine::RegisterBit(Rax);
        }
        else {
            PassScalar(value, type, Rax);
            uses = Machine::RegisterBit(Rax);
        }
    }
    auto &ret = Emit(Op::Ret, 8, {});
    ret.implicitUses = uses;
}

std::int32_t Selector::PanicSiteFor(const LirInstr &instruction) {
    const RuntimeFailureLayout layout = BuildRuntimeFailureLayout(
        instruction.op == LirOpcode::Assert ? RuntimeFailureKind::Assertion : RuntimeFailureKind::Panic,
        instruction.sourceFunction, instruction.sourceFile, instruction.sourceLine, instruction.sourceColumn);
    PanicSite site;
    site.prefix = Symbol({SymbolRef::Kind::String, layout.prefix});
    site.prefixLength = static_cast<std::uint32_t>(layout.prefix.size());
    site.location = Symbol({SymbolRef::Kind::String, layout.location});
    site.locationLength = static_cast<std::uint32_t>(layout.location.size());
    if (context.targetOs == Target::OS::Windows) {
        site.getStdHandle = Symbol({SymbolRef::Kind::Kernel32, "GetStdHandle"});
        site.writeFile = Symbol({SymbolRef::Kind::Kernel32, "WriteFile"});
    }
    if (context.backtraceEnabled)
        site.backtrace = Symbol({SymbolRef::Kind::Function, std::string(kBacktraceFunctionName)});
    out.panics.push_back(site);
    hasPanic = true;
    return static_cast<std::int32_t>(out.panics.size() - 1);
}

void Selector::SelectPanic(const LirInstr &instruction) {
    const LirReg message = instruction.srcs[0];
    Reg pointer;
    if (KindOfReg(message) == ValueKind::Aggregate) {
        pointer = NewGpr();
        Emit(Op::Lea, 8, {Operand::Def(pointer), Operand::Mem(HomeAddress(message))});
    }
    else {
        pointer = Value(message);
    }
    const std::int32_t site = PanicSiteFor(instruction);
    Emit(Op::Panic, 8, {Operand::Use(pointer), Operand::Imm(site)});
}

void Selector::SelectAssert(const LirInstr &instruction) {
    const auto &block = out.function.blocks[current];
    const std::uint32_t depth = block.loopDepth;
    const bool cold = block.cold;
    const std::uint32_t continuation = NewBlock(depth, cold);
    const std::uint32_t failure = NewBlock(depth, true);
    const Reg condition = Value(instruction.srcs[0]);
    Emit(Op::Test, 4, {Operand::Use(condition), Operand::Use(condition)});
    Emit(Op::Jcc, 8, {Operand::BlockTarget(continuation)}).condition = static_cast<std::uint8_t>(Cond::NotEqual);
    Emit(Op::Jmp, 8, {Operand::BlockTarget(failure)});
    current = failure;
    const LirReg message = instruction.srcs[1];
    Reg pointer;
    if (KindOfReg(message) == ValueKind::Aggregate) {
        pointer = NewGpr();
        Emit(Op::Lea, 8, {Operand::Def(pointer), Operand::Mem(HomeAddress(message))});
    }
    else {
        pointer = Value(message);
    }
    const std::int32_t site = PanicSiteFor(instruction);
    Emit(Op::Panic, 8, {Operand::Use(pointer), Operand::Imm(site)});
    current = continuation;
}
} // namespace Rux::X86_64Machine::Detail
