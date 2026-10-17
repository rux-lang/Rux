// Rewrites operations on software floats into calls to synthesized helpers, and synthesizes the helpers.

#include "Lowering/HirToLir/HirToLirSoftFloat.h"

#include "Lowering/HirToLir/HirToLirContext.h"
#include "Numeric/FloatParsing.h"
#include "Types/PrimitiveCatalog.h"

#include <algorithm>
#include <array>
#include <format>
#include <string_view>
#include <utility>

namespace Rux::HirToLirDetail {
namespace {
using Target::FloatLowering;
using Target::FloatOperation;

/// Whether a lowering leaves at least some operations on its width to helpers.
[[nodiscard]] bool IsComputedByHelpers(const FloatLowering lowering) noexcept {
    return lowering == FloatLowering::Software || lowering == FloatLowering::X87;
}

[[nodiscard]] const FloatFormat *FormatOf(const TypeRef::Kind kind) noexcept {
    const PrimitiveInfo *primitive = FindPrimitive(kind);
    return primitive && primitive->category == PrimitiveCategory::Float ? FindFloatFormat(primitive->bits) : nullptr;
}

[[nodiscard]] TypeRef::Kind UnsignedOfWidth(const std::uint32_t bits) noexcept {
    const auto primitives = PrimitiveCatalog();
    const auto found = std::ranges::find_if(primitives, [&](const PrimitiveInfo &primitive) {
        return primitive.category == PrimitiveCategory::UnsignedInt && primitive.bits == bits;
    });
    return found == primitives.end() ? TypeRef::Kind::Unknown : found->kind;
}

/// The helper a conversion from `from` to `to` needs, if any.
[[nodiscard]] std::optional<SoftFloatHelperKey> ConversionHelperFor(const TypeRef::Kind from, const TypeRef::Kind to,
                                                                    const SoftFloatPolicy &policy) {
    const PrimitiveInfo *source = FindPrimitive(from);
    const PrimitiveInfo *destination = FindPrimitive(to);
    if (from == to || !source || !destination) {
        return std::nullopt;
    }
    const FloatLowering sourceLowering = policy.For(from);
    const FloatLowering destinationLowering = policy.For(to);
    if (!IsComputedByHelpers(sourceLowering) && !IsComputedByHelpers(destinationLowering)) {
        if (!ConvertsThroughSoftFloatHelper(from, to, policy.arch)) {
            return std::nullopt;
        }
        return SoftFloatHelperKey{SoftFloatOperation::ConvertFrom, to, SoftFloatHelperOperandKind(from)};
    }
    if (IsComputedByHelpers(sourceLowering)) {
        // A bool asks whether the value is non-zero, which is a comparison rather than a conversion.
        if (destination->category == PrimitiveCategory::Bool) {
            if (sourceLowering == FloatLowering::X87 && Target::IsX87NativeOperation(FloatOperation::Compare)) {
                return std::nullopt;
            }
            return SoftFloatHelperKey{SoftFloatOperation::Compare, from, TypeRef::Kind::Unknown};
        }
        const FloatOperation operation = destinationLowering == FloatLowering::Native ? FloatOperation::ToNativeFloat
                                       : destination->category == PrimitiveCategory::Float ? FloatOperation::OtherFloat
                                                                                           : FloatOperation::ToInteger;
        if (sourceLowering == FloatLowering::X87 && Target::IsX87NativeOperation(operation)) {
            return std::nullopt;
        }
        return SoftFloatHelperKey{SoftFloatOperation::ConvertTo, from, SoftFloatHelperOperandKind(to)};
    }
    const TypeRef::Kind operand = SoftFloatHelperOperandKind(from);
    const PrimitiveInfo *normalized = FindPrimitive(operand);
    const FloatOperation operation =
        sourceLowering == FloatLowering::Native ? FloatOperation::FromNativeFloat
        : normalized && normalized->category == PrimitiveCategory::SignedInt && normalized->bits <= 64
            ? FloatOperation::FromSigned64
            : FloatOperation::FromOtherInteger;
    if (destinationLowering == FloatLowering::X87 && Target::IsX87NativeOperation(operation)) {
        return std::nullopt;
    }
    return SoftFloatHelperKey{SoftFloatOperation::ConvertFrom, to, operand};
}

/// What a register defined by `instruction` holds.
[[nodiscard]] TypeRef::Kind ResultKind(const LirInstr &instruction) noexcept {
    return instruction.op == LirOpcode::Alloca ? TypeRef::Kind::Pointer : instruction.type->kind;
}

/// The kind a comparison or a conversion is about: a cast names its source type, and a comparison's operands are
/// whatever defined them.
[[nodiscard]] TypeRef::Kind OperandKind(const LirInstr &instruction, const std::vector<TypeRef::Kind> &registerKinds) {
    if (instruction.op == LirOpcode::Cast) {
        if (const PrimitiveInfo *source = FindPrimitive(instruction.strArg)) {
            return source->kind;
        }
    }
    if (instruction.srcs.empty() || instruction.srcs.front() >= registerKinds.size()) {
        return TypeRef::Kind::Unknown;
    }
    return registerKinds[instruction.srcs.front()];
}

[[nodiscard]] LirInstr HelperCall(const LirReg dst, TypeRef type, const std::string &symbol, LirRegs arguments) {
    LirInstr call;
    call.dst = dst;
    call.op = LirOpcode::Call;
    call.type = type;
    call.strArg = symbol;
    call.srcs = std::move(arguments);
    return call;
}

void Synthesize(SoftFloatHelperSynthesis &synthesis) {
    switch (synthesis.Key().operation) {
    case SoftFloatOperation::Add:
    case SoftFloatOperation::Subtract:
        SynthesizeSoftFloatAddition(synthesis);
        return;
    case SoftFloatOperation::Multiply:
        SynthesizeSoftFloatMultiplication(synthesis);
        return;
    case SoftFloatOperation::Divide:
    case SoftFloatOperation::Remainder:
        SynthesizeSoftFloatDivision(synthesis);
        return;
    case SoftFloatOperation::Compare:
        SynthesizeSoftFloatComparison(synthesis);
        return;
    case SoftFloatOperation::ConvertTo:
    case SoftFloatOperation::ConvertFrom:
        SynthesizeSoftFloatConversion(synthesis);
        return;
    }
}

[[nodiscard]] Diagnostic InternalError(std::string message, std::string note) {
    return ErrorDiagnostic(std::move(message), {std::move(note)},
                           "please report this compiler bug with a minimal source example");
}
} // namespace

std::string SoftFloatHelperName(const SoftFloatHelperKey &key) {
    const PrimitiveInfo *format = FindPrimitive(key.format);
    const PrimitiveInfo *other = FindPrimitive(key.other);
    // In `SoftFloatOperation` order; a conversion appends the other side's spelling.
    constexpr std::array<std::string_view, 8> operations{"add", "sub", "mul", "div", "rem", "compare", "to_", "from_"};
    return std::format("__rux_float_f{}_{}{}", format ? format->bits : 0,
                       operations[static_cast<std::size_t>(key.operation)], other ? other->name : "");
}

SoftFloatHelperSignature SoftFloatHelperSignatureOf(const SoftFloatHelperKey &key) {
    const TypeRef value = TypeRef::MakePrimitive(key.format);
    switch (key.operation) {
    case SoftFloatOperation::Compare:
        return {{value, value}, TypeRef::MakeUInt8()};
    case SoftFloatOperation::ConvertTo:
        return {{value}, TypeRef::MakePrimitive(key.other)};
    case SoftFloatOperation::ConvertFrom:
        return {{TypeRef::MakePrimitive(key.other)}, value};
    default:
        return {{value, value}, value};
    }
}

/// Every target this compiler supports has 64-bit pointers, so `int` and `uint` have one fixed width everywhere.
TypeRef::Kind SoftFloatHelperOperandKind(const TypeRef::Kind kind) noexcept {
    if (kind == TypeRef::Kind::Int) {
        return TypeRef::Kind::Int64;
    }
    if (kind == TypeRef::Kind::UInt) {
        return TypeRef::Kind::UInt64;
    }
    const PrimitiveInfo *primitive = FindPrimitive(kind);
    if (primitive &&
        (primitive->category == PrimitiveCategory::Bool || primitive->category == PrimitiveCategory::Char)) {
        return UnsignedOfWidth(primitive->bits);
    }
    return kind;
}

std::optional<SoftFloatHelperKey> SoftFloatHelperFor(const LirInstr &instruction, const TypeRef::Kind operandKind,
                                                     const SoftFloatPolicy &policy) {
    const auto computedByHelper = [&](const TypeRef::Kind kind, const FloatOperation operation) {
        const FloatLowering lowering = policy.For(kind);
        return lowering == FloatLowering::Software ||
               (lowering == FloatLowering::X87 && !Target::IsX87NativeOperation(operation));
    };
    const auto arithmetic = [&](const SoftFloatOperation operation) -> std::optional<SoftFloatHelperKey> {
        if (!computedByHelper(instruction.type->kind, FloatOperation::Arithmetic)) {
            return std::nullopt;
        }
        return SoftFloatHelperKey{operation, instruction.type->kind, TypeRef::Kind::Unknown};
    };
    switch (instruction.op) {
    case LirOpcode::Add:
        return arithmetic(SoftFloatOperation::Add);
    case LirOpcode::Sub:
        return arithmetic(SoftFloatOperation::Subtract);
    case LirOpcode::Mul:
        return arithmetic(SoftFloatOperation::Multiply);
    case LirOpcode::Div:
        return arithmetic(SoftFloatOperation::Divide);
    case LirOpcode::Mod:
        if (policy.For(instruction.type->kind) == FloatLowering::Native &&
            !Target::HasNativeFloatRemainder(policy.arch)) {
            return SoftFloatHelperKey{SoftFloatOperation::Remainder, instruction.type->kind, TypeRef::Kind::Unknown};
        }
        return arithmetic(SoftFloatOperation::Remainder);
    case LirOpcode::CmpEq:
    case LirOpcode::CmpNe:
    case LirOpcode::CmpLt:
    case LirOpcode::CmpLe:
    case LirOpcode::CmpGt:
    case LirOpcode::CmpGe:
        if (!computedByHelper(operandKind, FloatOperation::Compare)) {
            return std::nullopt;
        }
        return SoftFloatHelperKey{SoftFloatOperation::Compare, operandKind, TypeRef::Kind::Unknown};
    case LirOpcode::Cast:
        return ConversionHelperFor(operandKind, instruction.type->kind, policy);
    default:
        return std::nullopt;
    }
}

// Helper synthesis

SoftFloatHelperSynthesis::SoftFloatHelperSynthesis(SoftFloatHelperRegistry &inputRegistry,
                                                   const SoftFloatHelperKey &inputKey, LirFunc &inputFunction)
    : registry(&inputRegistry)
    , key(inputKey)
    , format(FormatOf(inputKey.format))
    , function(&inputFunction)
    , builder(inputFunction) {
    const SoftFloatHelperSignature signature = SoftFloatHelperSignatureOf(key);
    function->name = SoftFloatHelperName(key);
    function->isPublic = false;
    function->returnType = signature.result;
    if (!format) {
        Fail("the helper's type is not a float");
    }
    if (!builder.SelectBlock(builder.CreateBlock("entry"))) {
        Fail(std::string(builder.FailureReason()));
    }
    const bool unary = signature.parameters.size() == 1;
    for (std::size_t index = 0; index < signature.parameters.size(); ++index) {
        const LirReg reg = builder.DefineParameter();
        function->params.push_back({reg, signature.parameters[index], unary ? "value" : index == 0 ? "left" : "right"});
    }
}

const SoftFloatHelperKey &SoftFloatHelperSynthesis::Key() const noexcept {
    return key;
}

const FloatFormat &SoftFloatHelperSynthesis::Format() const noexcept {
    return *format;
}

LirFunc &SoftFloatHelperSynthesis::Function() const noexcept {
    return *function;
}

CheckedLirBuilder &SoftFloatHelperSynthesis::Builder() noexcept {
    return builder;
}

LirReg SoftFloatHelperSynthesis::Parameter(const std::size_t index) const {
    return function->params.at(index).reg;
}

const std::string &SoftFloatHelperSynthesis::Require(const SoftFloatHelperKey &helper) {
    return registry->Request(helper);
}

LirReg SoftFloatHelperSynthesis::NewReg() {
    return builder.AllocateRegister();
}

void SoftFloatHelperSynthesis::Emit(LirInstr &&instruction) {
    if (!builder.Insert(std::move(instruction))) {
        Fail(std::string(builder.FailureReason()));
    }
}

void SoftFloatHelperSynthesis::Terminate(LirTerminator terminator) {
    if (!builder.Terminate(std::move(terminator))) {
        Fail(std::string(builder.FailureReason()));
    }
}

void SoftFloatHelperSynthesis::Fail(std::string detail) {
    if (failure.empty()) {
        failure = std::move(detail);
    }
}

const std::string &SoftFloatHelperSynthesis::Failure() const noexcept {
    return failure;
}

const std::string &SoftFloatHelperRegistry::Request(const SoftFloatHelperKey &key) {
    const auto [entry, inserted] = symbols.try_emplace(key);
    if (inserted) {
        entry->second = SoftFloatHelperName(key);
        pending.push_back(key);
    }
    return entry->second;
}

void SoftFloatHelperRegistry::Absorb(const SoftFloatHelperRegistry &other) {
    for (const auto &key : other.pending)
        static_cast<void>(Request(key));
}

std::vector<LirFunc> SoftFloatHelperRegistry::SynthesizePending(std::vector<Diagnostic> &diagnostics) {
    std::vector<LirFunc> helpers;
    // A helper may ask for others while it is built, which lengthens the queue this walks.
    for (std::size_t next = 0; next < pending.size(); ++next) {
        const SoftFloatHelperKey key = pending[next];
        LirFunc function;
        std::string failure;
        {
            SoftFloatHelperSynthesis synthesis(*this, key, function);
            if (synthesis.Failure().empty()) {
                Synthesize(synthesis);
            }
            for (const LirBlock &block : function.blocks) {
                if (!block.term) {
                    synthesis.Fail(std::format("block '{}' has no terminator", block.label));
                }
            }
            failure = synthesis.Failure();
        }
        if (!failure.empty()) {
            diagnostics.push_back(
                InternalError(std::format("invalid internal LIR while synthesizing '{}': {}", function.name, failure),
                              "a software-float helper was built from instructions the checked builder rejected"));
        }
        helpers.push_back(std::move(function));
    }
    pending.clear();
    return helpers;
}

void EmitSoftFloatPlaceholder(SoftFloatHelperSynthesis &synthesis) {
    // The same message slice and `Panic` that `EmitRuntimeTrap` builds, so both back ends report it the usual way.
    const std::string message = std::format("software-float helper '{}' is not implemented", synthesis.Function().name);
    const TypeRef element = TypeRef::MakeChar8();
    const TypeRef pointer = TypeRef::MakePointer(element);
    const TypeRef length = TypeRef::MakeUInt64();
    const auto emit = [&](const LirOpcode op, TypeRef type, std::vector<LirReg> sources, std::string argument,
                          const bool defines) {
        LirInstr instruction;
        instruction.dst = defines ? synthesis.NewReg() : LirNoReg;
        instruction.op = op;
        instruction.type = type;
        instruction.srcs = std::move(sources);
        instruction.strArg = std::move(argument);
        const LirReg reg = instruction.dst;
        synthesis.Emit(std::move(instruction));
        return reg;
    };
    const LirReg slot = emit(LirOpcode::Alloca, TypeRef::MakeSlice(element), {}, {}, true);
    const LirReg text = emit(LirOpcode::StringAddr, pointer, {}, message, true);
    const LirReg data = emit(LirOpcode::FieldPtr, TypeRef::MakePointer(pointer), {slot}, "data", true);
    emit(LirOpcode::Store, pointer, {text, data}, {}, false);
    const LirReg size = emit(LirOpcode::Const, length, {}, std::to_string(message.size()), true);
    const LirReg sizeField = emit(LirOpcode::FieldPtr, TypeRef::MakePointer(length), {slot}, "length", true);
    emit(LirOpcode::Store, length, {size, sizeField}, {}, false);
    emit(LirOpcode::Panic, TypeRef::MakeOpaque(), {slot}, message, false);
    LirTerminator unreachable;
    unreachable.kind = LirTermKind::Unreachable;
    synthesis.Terminate(std::move(unreachable));
}

void VerifySoftFloatLowering(const LirPackage &package, const SoftFloatPolicy &policy,
                             std::vector<Diagnostic> &diagnostics) {
    for (const LirModule &module : package.modules) {
        for (const LirFunc &function : module.funcs) {
            if (function.isExtern || function.isAsm) {
                continue;
            }
            std::vector<TypeRef::Kind> registerKinds;
            const auto record = [&](const LirReg reg, const TypeRef::Kind kind) {
                if (reg == LirNoReg) {
                    return;
                }
                if (registerKinds.size() <= reg) {
                    registerKinds.resize(reg + 1, TypeRef::Kind::Unknown);
                }
                registerKinds[reg] = kind;
            };
            for (const LirParam &param : function.params) {
                record(param.reg, param.type->kind);
            }
            for (const LirBlock &block : function.blocks) {
                for (const LirInstr &instruction : block.instrs) {
                    record(instruction.dst, ResultKind(instruction));
                }
            }
            for (const LirBlock &block : function.blocks) {
                for (const LirInstr &instruction : block.instrs) {
                    const TypeRef::Kind operand = OperandKind(instruction, registerKinds);
                    if (!SoftFloatHelperFor(instruction, operand, policy)) {
                        continue;
                    }
                    const TypeRef subject = instruction.op == LirOpcode::Add || instruction.op == LirOpcode::Sub ||
                                                    instruction.op == LirOpcode::Mul ||
                                                    instruction.op == LirOpcode::Div || instruction.op == LirOpcode::Mod
                                              ? instruction.type
                                              : TypeRef::MakePrimitive(operand);
                    diagnostics.push_back(InternalError(
                        std::format("invalid internal LIR in function '{}': '{}' on {} reached code generation",
                                    function.name, LirOpcodeName(instruction.op), subject.ToString()),
                        "an operation on a float the target computes in software must become a helper call"));
                }
            }
        }
    }
}

// The rewrite

bool HirToLirContext::EmitSoftFloatReplacement(LirInstr &instruction) {
    // A constant is spelled once, exactly, so that each back end parses the same text to the same bits.
    if (instruction.op == LirOpcode::Const) {
        if (IsComputedByHelpers(softFloatPolicy.For(instruction.type->kind))) {
            if (const auto encoding = ParseFloatEncoding(instruction.strArg, *FormatOf(instruction.type->kind))) {
                instruction.strArg = FormatFloatEncoding(*encoding);
            }
        }
        return false;
    }
    const TypeRef::Kind operandKind = OperandKind(instruction, registerKinds);
    const auto key = SoftFloatHelperFor(instruction, operandKind, softFloatPolicy);
    if (!key) {
        return false;
    }
    const std::string &symbol = softFloatHelpers.Request(*key);
    switch (key->operation) {
    case SoftFloatOperation::Compare:
        EmitSoftFloatComparison(instruction, symbol, operandKind);
        break;
    case SoftFloatOperation::ConvertTo:
    case SoftFloatOperation::ConvertFrom:
        EmitSoftFloatConversion(instruction, symbol, SoftFloatHelperSignatureOf(*key), operandKind);
        break;
    default:
        Emit(HelperCall(instruction.dst, instruction.type, symbol, instruction.srcs));
        break;
    }
    return true;
}

/// A comparison becomes the helper's ordering code and a test of it, the test defining the comparison's register. A
/// cast to a bool is the comparison `!= 0`, so -0.0 is false and a NaN true.
void HirToLirContext::EmitSoftFloatComparison(const LirInstr &instruction, const std::string &symbol,
                                              const TypeRef::Kind operandKind) {
    const TypeRef codeType = TypeRef::MakeUInt8();
    const TypeRef boolType = TypeRef::MakeBool();
    const bool castsToBool = instruction.op == LirOpcode::Cast;
    const LirReg right = castsToBool ? EmitConst("0", TypeRef::MakePrimitive(operandKind)) : instruction.srcs.at(1);
    const LirReg code = NewReg();
    Emit(HelperCall(code, codeType, symbol, {instruction.srcs.at(0), right}));
    const auto ordering = [&](const SoftFloatOrdering value) {
        return EmitConst(std::to_string(static_cast<unsigned>(value)), codeType);
    };
    const auto answer = [&](const LirOpcode op, const LirReg left, const LirReg rightOperand) {
        LirInstr test;
        test.dst = instruction.dst;
        test.op = op;
        test.type = castsToBool ? boolType : instruction.type;
        test.srcs = {left, rightOperand};
        Emit(std::move(test));
    };
    if (castsToBool && instruction.type != boolType) {
        const LirReg truth = EmitBinary(LirOpcode::CmpNe, code, ordering(SoftFloatOrdering::Equal), boolType);
        LirInstr widen;
        widen.dst = instruction.dst;
        widen.op = LirOpcode::Cast;
        widen.type = instruction.type;
        widen.srcs = {truth};
        widen.strArg = boolType.ToString();
        Emit(std::move(widen));
        return;
    }
    switch (castsToBool ? LirOpcode::CmpNe : instruction.op) {
    case LirOpcode::CmpEq:
        answer(LirOpcode::CmpEq, code, ordering(SoftFloatOrdering::Equal));
        return;
    case LirOpcode::CmpNe:
        answer(LirOpcode::CmpNe, code, ordering(SoftFloatOrdering::Equal));
        return;
    case LirOpcode::CmpLt:
        answer(LirOpcode::CmpEq, code, ordering(SoftFloatOrdering::Less));
        return;
    case LirOpcode::CmpGt:
        answer(LirOpcode::CmpEq, code, ordering(SoftFloatOrdering::Greater));
        return;
    case LirOpcode::CmpLe:
        // Less and Equal are the two codes below Greater, whichever signedness the back end reads them with.
        answer(LirOpcode::CmpLe, code, ordering(SoftFloatOrdering::Equal));
        return;
    default: {
        const LirReg equal = EmitBinary(LirOpcode::CmpEq, code, ordering(SoftFloatOrdering::Equal), boolType);
        const LirReg greater = EmitBinary(LirOpcode::CmpEq, code, ordering(SoftFloatOrdering::Greater), boolType);
        answer(LirOpcode::Or, equal, greater);
        return;
    }
    }
}

/// A conversion becomes a helper call, with the operand or result restated at the fixed width the helper takes when
/// the source names `int`, `uint`, a char or a bool.
void HirToLirContext::EmitSoftFloatConversion(const LirInstr &instruction, const std::string &symbol,
                                              const SoftFloatHelperSignature &signature,
                                              const TypeRef::Kind operandKind) {
    const TypeRef &parameter = signature.parameters.front();
    LirReg source = instruction.srcs.at(0);
    if (operandKind != parameter->kind) {
        source = EmitConversion(source, TypeRef::MakePrimitive(operandKind), parameter);
    }
    if (signature.result->kind == instruction.type->kind) {
        Emit(HelperCall(instruction.dst, instruction.type, symbol, {source}));
        return;
    }
    const LirReg result = NewReg();
    Emit(HelperCall(result, signature.result, symbol, {source}));
    LirInstr restate;
    restate.dst = instruction.dst;
    restate.op = LirOpcode::Cast;
    restate.type = instruction.type;
    restate.srcs = {result};
    restate.strArg = signature.result.ToString();
    Emit(std::move(restate));
}

void HirToLirContext::RecordRegisterKind(const LirInstr &instruction) {
    if (instruction.dst == LirNoReg) {
        return;
    }
    if (registerKinds.size() <= instruction.dst) {
        registerKinds.resize(instruction.dst + 1, TypeRef::Kind::Unknown);
    }
    registerKinds[instruction.dst] = ResultKind(instruction);
}
} // namespace Rux::HirToLirDetail
