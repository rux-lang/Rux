// Parameters, calls, returns and runtime failures under AAPCS64.
//
// Every placement comes from `AArch64CallPlanner`, the planner the established emitter uses
// (`CallAndTerminatorEmitter.cpp`), so a function selected here and one emitted there call each other correctly: a
// scalar travels in X0-X7 or V0-V7, an aggregate of up to sixteen bytes in one or two general registers or as a
// homogeneous float aggregate in vector registers, a larger one as the address of a copy, the rest on the stack, and
// a result too large for registers through the address the caller passes in X8.

#include "CodeGen/AArch64/MachineSelector.h"
#include "CodeGen/RuntimeFailure.h"

namespace Rux::AArch64Machine::Detail {
using namespace Layout;

namespace {
using Location = AArch64ArgumentLocation;

[[nodiscard]] Address Based(const Reg base, const std::int32_t displacement = 0) {
    Address address;
    address.base = base;
    address.displacement = displacement;
    return address;
}

/// A stack argument as the callee sees it: the frame record sits sixteen bytes below the stack pointer the caller
/// left, and the frame pointer addresses the record.
[[nodiscard]] Address IncomingArgument(const std::int32_t offset) {
    return Based(Fp, 16 + offset);
}

/// A stack argument as the caller writes it, at the bottom of its frame.
[[nodiscard]] Address OutgoingArgument(const std::int32_t offset) {
    return Based(Sp, offset);
}

[[nodiscard]] Reg General(const unsigned index) {
    return static_cast<Reg>(index);
}

[[nodiscard]] Reg Vector(const unsigned index) {
    return V0 + static_cast<Reg>(index);
}
} // namespace

void Selector::PassScalar(const LirReg argument, const TypeRef &type, const Reg destination) {
    const int size = ScalarSize(type);
    if (const auto constant = integerConstants.find(argument); constant != integerConstants.end()) {
        std::uint64_t value = constant->second;
        if (size < 8) {
            const int bits = size * 8;
            value &= (std::uint64_t{1} << bits) - 1;
            if (type.IsSigned() && (value >> (bits - 1)) != 0)
                value |= ~((std::uint64_t{1} << bits) - 1);
        }
        Emit(Op::MovImm, 8, {Operand::Def(destination), Operand::Imm(static_cast<std::int64_t>(value))});
        return;
    }
    // A narrow integer leaves extended to a whole register, which every platform's callee accepts and Apple's
    // requires.
    if (size >= 8 || type.IsBool() || AlreadyExtended(argument, size, type.IsSigned())) {
        EmitCopy(destination, Value(argument), RegClass::General);
        return;
    }
    Emit(type.IsSigned() ? Op::SignExtend : Op::ZeroExtend, 8,
         {Operand::Def(destination), Operand::Use(Value(argument))})
        .sourceSize = static_cast<std::uint8_t>(size);
}

void Selector::SelectParameters() {
    if (!func.returnType.IsOpaque() && context.callPlanner.ReturnsInMemory(func.returnType)) {
        indirectResult = NewGpr();
        EmitCopy(indirectResult, X8, RegClass::General);
    }
    std::vector<TypeRef> parameterTypes;
    for (const auto &parameter : func.params)
        parameterTypes.push_back(parameter.type);
    const auto layout = context.callPlanner.PlanArguments(parameterTypes);
    for (std::size_t index = 0; index < func.params.size(); ++index) {
        const LirReg reg = func.params[index].reg;
        const TypeRef &type = TypeOf(reg);
        const Location &location = layout.args[index];
        const int size = Size(type);
        if (location.byReference) {
            Reg pointer = NewGpr();
            if (location.kind == Location::Kind::Stack)
                LoadWidth(pointer, IncomingArgument(location.offset), 8, false);
            else
                EmitCopy(pointer, General(location.first), RegClass::General);
            CopyMemory(HomeAddress(reg), Based(pointer), size);
            continue;
        }
        switch (location.kind) {
        case Location::Kind::Stack:
            if (KindOf(type) == ValueKind::Aggregate)
                CopyMemory(HomeAddress(reg), IncomingArgument(location.offset), size);
            else
                LoadInto(Value(reg), IncomingArgument(location.offset), type);
            break;
        case Location::Kind::Vector:
            if (location.count == 1 && KindOf(type) == ValueKind::Fpr) {
                EmitCopy(Value(reg), Vector(location.first), RegClass::Vector);
                break;
            }
            for (unsigned member = 0; member < location.count; ++member) {
                bool fits = true;
                Emit(Op::StoreFp, static_cast<std::uint8_t>(location.memberBytes),
                     {Operand::Mem(
                          Offset(HomeAddress(reg), static_cast<std::int64_t>(member) * location.memberBytes, fits)),
                      Operand::Use(Vector(location.first + member))});
            }
            break;
        default:
            if (KindOf(type) == ValueKind::Aggregate) {
                for (unsigned word = 0; word < location.count; ++word) {
                    bool fits = true;
                    Emit(Op::Store, 8,
                         {Operand::Mem(Offset(HomeAddress(reg), static_cast<std::int64_t>(word) * 8, fits)),
                          Operand::Use(General(location.first + word))});
                }
            }
            else {
                EmitCopy(Value(reg), General(location.first), RegClass::General);
            }
            break;
        }
    }
}

bool Selector::SelectBitCast(const LirInstr &instruction) {
    if (instruction.op != LirOpcode::Call || instruction.srcs.size() != 1 || instruction.dst == LirNoReg)
        return false;
    const std::string &name = instruction.strArg;
    // Lowering routes an unsigned 64-bit conversion to floating point through a software helper for x86-64's sake;
    // AArch64 converts unsigned values natively.
    if ((name == "__rux_float_f64_from_uint64" || name == "__rux_float_f32_from_uint64") &&
        KindOfReg(instruction.srcs[0]) == ValueKind::Gpr && KindOfReg(instruction.dst) == ValueKind::Fpr) {
        Emit(Op::UIntToFp, name == "__rux_float_f64_from_uint64" ? 8 : 4,
             {Operand::Def(Value(instruction.dst)), Operand::Use(Value(instruction.srcs[0]))})
            .sourceSize = 8;
        return true;
    }
    Op op;
    std::uint8_t size;
    if (name == "FloatBits64") {
        op = Op::FpToGpr;
        size = 8;
    }
    else if (name == "FloatFromBits64") {
        op = Op::GprToFp;
        size = 8;
    }
    else if (name == "FloatBits32") {
        op = Op::FpToGpr;
        size = 4;
    }
    else if (name == "FloatFromBits32") {
        op = Op::GprToFp;
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
    const bool indirect = instruction.op == LirOpcode::CallIndirect;
    const std::vector<LirReg> arguments(instruction.srcs.begin() + (indirect ? 1 : 0), instruction.srcs.end());
    std::vector<TypeRef> argumentTypes;
    for (const LirReg argument : arguments)
        argumentTypes.push_back(TypeOf(argument));
    const auto layout = context.callPlanner.PlanArguments(argumentTypes);
    const TypeRef &type = instruction.type;
    const bool hasResult = instruction.dst != LirNoReg && !type.IsOpaque();
    const bool resultInMemory = hasResult && context.callPlanner.ReturnsInMemory(type);
    outgoingSize = std::max(outgoingSize, layout.areaBytes);

    // Memory first: stack arguments and the copies large aggregates travel as.
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const LirReg argument = arguments[index];
        const TypeRef &argumentType = argumentTypes[index];
        const Location &location = layout.args[index];
        const int size = Size(argumentType);
        if (location.byReference) {
            CopyMemory(OutgoingArgument(location.copyOffset), HomeAddress(argument), location.copyBytes);
            if (location.kind == Location::Kind::Stack) {
                const Reg pointer = AddressRegister(OutgoingArgument(location.copyOffset));
                Emit(Op::Store, 8, {Operand::Mem(OutgoingArgument(location.offset)), Operand::Use(pointer)});
            }
            continue;
        }
        if (location.kind != Location::Kind::Stack)
            continue;
        if (KindOf(argumentType) == ValueKind::Aggregate) {
            CopyMemory(OutgoingArgument(location.offset), HomeAddress(argument), size);
        }
        else if (KindOf(argumentType) == ValueKind::Fpr) {
            Emit(Op::StoreFp, static_cast<std::uint8_t>(size),
                 {Operand::Mem(OutgoingArgument(location.offset)), Operand::Use(Value(argument))});
        }
        else {
            const Reg value = NewGpr();
            PassScalar(argument, argumentType, value);
            const int width = context.callPlanner.Policy().compactStackArguments ? ScalarSize(argumentType) : 8;
            Emit(Op::Store, static_cast<std::uint8_t>(width),
                 {Operand::Mem(OutgoingArgument(location.offset)), Operand::Use(value)});
        }
    }

    // Fixed registers are written last, so nothing computed on the way can need one of them.
    std::uint64_t uses = 0;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const LirReg argument = arguments[index];
        const TypeRef &argumentType = argumentTypes[index];
        const Location &location = layout.args[index];
        if (location.kind == Location::Kind::Stack)
            continue;
        if (location.byReference) {
            Emit(Op::AddressOf, 8,
                 {Operand::Def(General(location.first)), Operand::Mem(OutgoingArgument(location.copyOffset))});
            uses |= Machine::RegisterBit(General(location.first));
            continue;
        }
        if (location.kind == Location::Kind::Vector) {
            if (location.count == 1 && KindOf(argumentType) == ValueKind::Fpr) {
                EmitCopy(Vector(location.first), Value(argument), RegClass::Vector);
                uses |= Machine::RegisterBit(Vector(location.first));
                continue;
            }
            for (unsigned member = 0; member < location.count; ++member) {
                bool fits = true;
                Emit(Op::LoadFp, static_cast<std::uint8_t>(location.memberBytes),
                     {Operand::Def(Vector(location.first + member)),
                      Operand::Mem(Offset(HomeAddress(argument),
                                          static_cast<std::int64_t>(member) * location.memberBytes, fits))});
                uses |= Machine::RegisterBit(Vector(location.first + member));
            }
            continue;
        }
        if (KindOf(argumentType) == ValueKind::Aggregate) {
            for (unsigned word = 0; word < location.count; ++word) {
                bool fits = true;
                LoadWidth(General(location.first + word),
                          Offset(HomeAddress(argument), static_cast<std::int64_t>(word) * 8, fits), 8, false);
                uses |= Machine::RegisterBit(General(location.first + word));
            }
            continue;
        }
        PassScalar(argument, argumentType, General(location.first));
        uses |= Machine::RegisterBit(General(location.first));
    }
    if (resultInMemory) {
        Emit(Op::AddressOf, 8, {Operand::Def(X8), Operand::Mem(HomeAddress(instruction.dst))});
        uses |= Machine::RegisterBit(X8);
    }
    auto &call =
        indirect ? Emit(Op::CallIndirect, 8, {Operand::Use(Value(instruction.srcs[0]))})
                 : Emit(Op::Call, 8, {Operand::SymbolTarget(Symbol({SymbolRef::Kind::Function, instruction.strArg}))});
    call.implicitUses = uses;
    call.implicitDefs = CallClobbers();
    if (!hasResult || resultInMemory)
        return;
    const Location result = context.callPlanner.PlanResult(type);
    if (result.kind == Location::Kind::Vector) {
        if (result.count == 1 && KindOf(type) == ValueKind::Fpr) {
            EmitCopy(Value(instruction.dst), Vector(result.first), RegClass::Vector);
            return;
        }
        for (unsigned member = 0; member < result.count; ++member) {
            bool fits = true;
            Emit(Op::StoreFp, static_cast<std::uint8_t>(result.memberBytes),
                 {Operand::Mem(Offset(HomeAddress(instruction.dst),
                                      static_cast<std::int64_t>(member) * result.memberBytes, fits)),
                  Operand::Use(Vector(result.first + member))});
        }
        return;
    }
    if (KindOf(type) == ValueKind::Aggregate) {
        for (unsigned word = 0; word < result.count; ++word) {
            bool fits = true;
            Emit(Op::Store, 8,
                 {Operand::Mem(Offset(HomeAddress(instruction.dst), static_cast<std::int64_t>(word) * 8, fits)),
                  Operand::Use(General(result.first + word))});
        }
        return;
    }
    EmitCopy(Value(instruction.dst), General(result.first), RegClass::General);
}

void Selector::SelectReturn(const LirTerminator &terminator) {
    std::uint64_t uses = 0;
    if (terminator.retVal && *terminator.retVal != LirNoReg && !terminator.retType.IsOpaque()) {
        const LirReg value = *terminator.retVal;
        const TypeRef &type = terminator.retType;
        const int size = Size(type);
        const ValueKind kind = KindOfReg(value);
        // An aggregate result named by its address is read from there.
        Address source;
        if (kind == ValueKind::Aggregate)
            source = HomeAddress(value);
        else if (KindOf(type) == ValueKind::Aggregate)
            source = Based(Value(value));
        if (indirectResult != NoReg) {
            CopyMemory(Based(indirectResult), source, size);
        }
        else if (KindOf(type) == ValueKind::Aggregate) {
            const Location result = context.callPlanner.PlanResult(type);
            if (kind != ValueKind::Aggregate) {
                // Words are read whole below, so the pointee is first copied where reading past its end is safe.
                Address staged;
                staged.frameObject = out.function.NewFrameObject(AlignUp(size, 8), 8);
                CopyMemory(staged, source, size);
                source = staged;
            }
            if (result.kind == Location::Kind::Vector) {
                for (unsigned member = 0; member < result.count; ++member) {
                    bool fits = true;
                    Emit(Op::LoadFp, static_cast<std::uint8_t>(result.memberBytes),
                         {Operand::Def(Vector(result.first + member)),
                          Operand::Mem(Offset(source, static_cast<std::int64_t>(member) * result.memberBytes, fits))});
                    uses |= Machine::RegisterBit(Vector(result.first + member));
                }
            }
            else {
                for (unsigned word = 0; word < result.count; ++word) {
                    bool fits = true;
                    LoadWidth(General(result.first + word), Offset(source, static_cast<std::int64_t>(word) * 8, fits),
                              8, false);
                    uses |= Machine::RegisterBit(General(result.first + word));
                }
            }
        }
        else if (kind == ValueKind::Fpr) {
            EmitCopy(Vector(0), Value(value), RegClass::Vector);
            uses = Machine::RegisterBit(Vector(0));
        }
        else {
            PassScalar(value, type, X0);
            uses = Machine::RegisterBit(X0);
        }
    }
    Emit(Op::Ret, 8, {}).implicitUses = uses;
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
    if (context.backtraceEnabled)
        site.backtrace = Symbol({SymbolRef::Kind::Function, std::string(kBacktraceFunctionName)});
    out.panics.push_back(site);
    return static_cast<std::int32_t>(out.panics.size() - 1);
}

void Selector::SelectPanic(const LirInstr &instruction) {
    const LirReg message = instruction.srcs[0];
    const Reg pointer =
        KindOfReg(message) == ValueKind::Aggregate ? AddressRegister(HomeAddress(message)) : Value(message);
    const std::int32_t site = PanicSiteFor(instruction);
    Emit(Op::Panic, 8, {Operand::Use(pointer), Operand::Imm(site)});
}

void Selector::SelectAssert(const LirInstr &instruction) {
    const auto &block = out.function.blocks[current];
    const std::uint32_t depth = block.loopDepth;
    const bool cold = block.cold;
    const std::uint32_t continuation = NewBlock(depth, cold);
    const std::uint32_t failure = NewBlock(depth, true);
    Emit(Op::Cmp, 4, {Operand::Use(Value(instruction.srcs[0])), Operand::Imm(0)});
    Emit(Op::Jcc, 8, {Operand::BlockTarget(continuation)}).condition = static_cast<std::uint8_t>(A64Condition::Ne);
    Emit(Op::Jmp, 8, {Operand::BlockTarget(failure)});
    current = failure;
    const LirReg message = instruction.srcs[1];
    const Reg pointer =
        KindOfReg(message) == ValueKind::Aggregate ? AddressRegister(HomeAddress(message)) : Value(message);
    const std::int32_t site = PanicSiteFor(instruction);
    Emit(Op::Panic, 8, {Operand::Use(pointer), Operand::Imm(site)});
    current = continuation;
}
} // namespace Rux::AArch64Machine::Detail
