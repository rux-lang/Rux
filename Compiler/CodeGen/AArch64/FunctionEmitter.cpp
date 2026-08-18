#include "CodeGen/AArch64/FunctionEmitter.h"

#include "CodeGen/BackendDiagnostics.h"
#include "CodeGen/IntegerLiteral.h"
#include "CodeGen/Layout.h"

#include <algorithm>
#include <bit>
#include <format>
#include <optional>
#include <utility>

namespace Rux {
using namespace Layout;

namespace {
constexpr unsigned kTemp = 9;
constexpr unsigned kAddr = 10;
constexpr unsigned kSrcAddr = 11;
constexpr unsigned kTemp2 = 12;
constexpr unsigned kReturn = 0;
constexpr unsigned kFpTemp = 16;
constexpr unsigned kFpTemp2 = 17;
constexpr unsigned kFpTemp3 = 18;
constexpr std::int32_t kFrameRecordSize = 16;

[[nodiscard]] unsigned AccessWidth(const int size) {
    if (size <= 0) {
        return 8;
    }
    if (size <= 1) {
        return 1;
    }
    if (size <= 2) {
        return 2;
    }
    if (size <= 4) {
        return 4;
    }
    return 8;
}

[[nodiscard]] A64Condition IntegerCondition(const LirOpcode op, const bool isSigned) {
    switch (op) {
    case LirOpcode::CmpEq:
        return A64Condition::Eq;
    case LirOpcode::CmpNe:
        return A64Condition::Ne;
    case LirOpcode::CmpLt:
        return isSigned ? A64Condition::Lt : A64::Lo;
    case LirOpcode::CmpLe:
        return isSigned ? A64Condition::Le : A64Condition::Ls;
    case LirOpcode::CmpGt:
        return isSigned ? A64Condition::Gt : A64Condition::Hi;
    default:
        return isSigned ? A64Condition::Ge : A64::Hs;
    }
}

[[nodiscard]] A64Condition FloatCondition(const LirOpcode op) {
    switch (op) {
    case LirOpcode::CmpEq:
        return A64Condition::Eq;
    case LirOpcode::CmpNe:
        return A64Condition::Ne;
    case LirOpcode::CmpLt:
        return A64Condition::Mi;
    case LirOpcode::CmpLe:
        return A64Condition::Ls;
    case LirOpcode::CmpGt:
        return A64Condition::Gt;
    default:
        return A64Condition::Ge;
    }
}

[[nodiscard]] bool IsFloatBitsBuiltin(const std::string &name) {
    return name == "FloatBits32" || name == "FloatBits64" || name == "FloatFromBits32" || name == "FloatFromBits64";
}
} // namespace

AArch64FunctionEmitter::AArch64FunctionEmitter(A64Enc &encoder, const AArch64FramePlan &framePlan,
                                               AArch64RuntimeHelperEmitter &runtimeHelpers, const LayoutMap &layouts,
                                               const std::unordered_set<std::string> &interfaceNames,
                                               std::string functionName, const Target::OS targetOs,
                                               AArch64FunctionEmitterHooks &hooks)
    : encoder(encoder)
    , framePlan(framePlan)
    , runtimeHelpers(runtimeHelpers)
    , layouts(layouts)
    , interfaceNames(interfaceNames)
    , functionName(std::move(functionName))
    , targetOs(targetOs)
    , hooks(hooks) {
}

TypeRef AArch64FunctionEmitter::TypeOfReg(const LirReg reg) const {
    const auto &registerTypes = framePlan.RegisterTypes();
    const auto it = registerTypes.find(reg);
    return it == registerTypes.end() ? TypeRef::MakeInt64() : it->second;
}

std::int32_t AArch64FunctionEmitter::Disp(const LirReg reg) {
    if (const auto it = framePlan.SlotOffsets().find(reg); it != framePlan.SlotOffsets().end()) {
        return it->second;
    }
    Report(std::format("AArch64 code generation reached register %{} with no stack slot in '{}'", reg, functionName));
    return kFrameRecordSize;
}

int AArch64FunctionEmitter::RuntimeSize(const TypeRef &type) const {
    return RuntimeSizeOf(type, layouts, interfaceNames);
}

int AArch64FunctionEmitter::RuntimeAlign(const TypeRef &type) const {
    if (!type.IsRange() && type.kind == TypeRef::Kind::Named) {
        const std::string base = BaseTypeName(type.name);
        if (interfaceNames.contains(base)) {
            return 8;
        }
        if (const auto it = layouts.find(base); it != layouts.end()) {
            return it->second.alignment;
        }
    }
    return AlignOf(type);
}

bool AArch64FunctionEmitter::IsAggregate(const TypeRef &type) const {
    if (IsWideInteger(type) || (IsSoftwareFloat(type) && RuntimeSize(type) > 8)) {
        return true;
    }
    if (type.IsRange()) {
        return true;
    }
    switch (type.kind) {
    case TypeRef::Kind::Tuple:
    case TypeRef::Kind::Array:
        return true;
    case TypeRef::Kind::Named: {
        const std::string base = BaseTypeName(type.name);
        return base == "Slice" || interfaceNames.contains(base) || layouts.contains(base) ||
               (!type.inner.empty() && SizeOf(type) > 8);
    }
    default:
        return false;
    }
}

bool AArch64FunctionEmitter::IsRegisterValue(const TypeRef &type) const {
    return !IsFloat(type) && !IsAggregate(type) && type.kind != TypeRef::Kind::Str;
}

bool AArch64FunctionEmitter::IsRegPointerTo(const LirReg reg, const TypeRef &pointee) const {
    const auto &registerTypes = framePlan.RegisterTypes();
    const auto it = registerTypes.find(reg);
    return it != registerTypes.end() && it->second.kind == TypeRef::Kind::Pointer && !it->second.inner.empty() &&
           it->second.inner[0] == pointee;
}

A64Reg AArch64FunctionEmitter::FpReg(const TypeRef &type, const unsigned index) {
    return type.kind == TypeRef::Kind::Float32 ? A64::Sn(index) : A64::Dn(index);
}

std::uint64_t AArch64FunctionEmitter::ConstantBits(const LirInstr &instruction) {
    if (instruction.type.IsBool()) {
        return instruction.strArg == "true" || instruction.strArg == "1" ? 1 : 0;
    }
    return ParseIntegerLiteralBits(instruction.strArg.empty() ? "0" : instruction.strArg).value_or(0);
}

std::optional<AArch64FunctionEmitter::BinaryOperands>
AArch64FunctionEmitter::LoadBinaryOperands(const LirInstr &instruction, const TypeRef &lhsType,
                                           const TypeRef &rhsType) {
    if (!IsRegisterValue(lhsType)) {
        NotImplemented(std::format("the '{}' opcode on '{}'", LirOpcodeName(instruction.op), lhsType.ToString()));
        return std::nullopt;
    }
    if (instruction.srcs.size() < 2) {
        Report(std::format("AArch64 code generation reached a '{}' with one operand in '{}'",
                           LirOpcodeName(instruction.op), functionName));
        return std::nullopt;
    }
    const A64Reg lhs = hooks.ReadOperand(instruction.srcs[0], lhsType, A64::Xn(kTemp));
    const A64Reg rhs = hooks.ReadOperand(instruction.srcs[1], rhsType, A64::Xn(kTemp2));
    return BinaryOperands{lhs, rhs};
}

std::optional<A64Reg> AArch64FunctionEmitter::LoadUnaryOperand(const LirInstr &instruction, const TypeRef &type) {
    if (!IsRegisterValue(type)) {
        NotImplemented(std::format("the '{}' opcode on '{}'", LirOpcodeName(instruction.op), type.ToString()));
        return std::nullopt;
    }
    if (instruction.srcs.empty()) {
        Report(std::format("AArch64 code generation reached a '{}' with no operand in '{}'",
                           LirOpcodeName(instruction.op), functionName));
        return std::nullopt;
    }
    return hooks.ReadOperand(instruction.srcs[0], type, A64::Xn(kTemp));
}

std::optional<AArch64FunctionEmitter::BinaryOperands>
AArch64FunctionEmitter::LoadFloatOperands(const LirInstr &instruction, const TypeRef &type) {
    if (instruction.srcs.size() < 2) {
        Report(std::format("AArch64 code generation reached a '{}' with one operand in '{}'",
                           LirOpcodeName(instruction.op), functionName));
        return std::nullopt;
    }
    const A64Reg lhs = hooks.ReadFloatOperand(instruction.srcs[0], FpReg(type, kFpTemp));
    const A64Reg rhs = hooks.ReadFloatOperand(instruction.srcs[1], FpReg(type, kFpTemp2));
    return BinaryOperands{lhs, rhs};
}

void AArch64FunctionEmitter::Report(std::string message) {
    hooks.ReportFunctionDiagnostic(std::move(message));
}

void AArch64FunctionEmitter::NotImplemented(std::string what) {
    hooks.ReportFunctionDiagnostic(
        UnsupportedBackendConstructDiagnostic(what, targetOs, Target::Arch::AArch64, functionName));
}

void AArch64FunctionEmitter::Must(const A64Status status, const std::string_view what) {
    if (status != A64Status::Ok) {
        Report(std::format("AArch64 code generation could not encode {} in '{}': {}", what, functionName,
                           A64StatusName(status)));
    }
}

void AArch64FunctionEmitter::EmitFloatBits(const LirInstr &instruction) {
    const bool single = instruction.strArg.ends_with("32");
    const bool toBits = instruction.strArg.starts_with("FloatBits");
    const TypeRef floatType = single ? TypeRef::MakeFloat32() : TypeRef::MakeFloat64();
    const unsigned width = single ? 4 : 8;
    const bool keepsResult = instruction.dst != LirNoReg && !instruction.type.IsOpaque();
    if (toBits) {
        const A64Reg vector = hooks.ReadFloatOperand(instruction.srcs[0], FpReg(floatType, kFpTemp));
        const A64Reg result = hooks.ResultRegister(instruction.dst, A64::Xn(kTemp));
        Must(encoder.Fmov(A64::Gpr(result.code, single ? 32 : 64), vector), "a reinterpretation of a float");
        if (keepsResult) {
            hooks.StoreWidthToSlot(result, instruction.dst, width);
        }
        return;
    }
    const A64Reg general = hooks.ReadRawOperand(instruction.srcs[0], width, A64::Xn(kTemp));
    const A64Reg vector = hooks.FloatResultRegister(instruction.dst, FpReg(floatType, kFpTemp));
    Must(encoder.Fmov(vector, A64::Gpr(general.code, single ? 32 : 64)), "a reinterpretation of an integer");
    if (keepsResult) {
        hooks.StoreFpToSlot(vector, instruction.dst);
    }
}

bool AArch64FunctionEmitter::EmitArithmetic(const LirInstr &instruction) {
    if (EmitWideArithmetic(instruction)) {
        return true;
    }
    switch (instruction.op) {
    case LirOpcode::Add:
    case LirOpcode::Sub:
    case LirOpcode::Mul:
    case LirOpcode::And:
    case LirOpcode::Or:
    case LirOpcode::Xor: {
        const TypeRef &type = instruction.type;
        if (IsFloat(type)) {
            if (instruction.op != LirOpcode::Add && instruction.op != LirOpcode::Sub &&
                instruction.op != LirOpcode::Mul) {
                NotImplemented(std::format("the '{}' opcode on '{}'", LirOpcodeName(instruction.op), type.ToString()));
                return true;
            }
            const auto operands = LoadFloatOperands(instruction, type);
            if (!operands) {
                return true;
            }
            const A64Reg out = hooks.FloatResultRegister(instruction.dst, FpReg(type, kFpTemp));
            A64Status status = A64Status::Ok;
            if (instruction.op == LirOpcode::Add) {
                status = encoder.Fadd(out, operands->lhs, operands->rhs);
            }
            else if (instruction.op == LirOpcode::Sub) {
                status = encoder.Fsub(out, operands->lhs, operands->rhs);
            }
            else {
                status = encoder.Fmul(out, operands->lhs, operands->rhs);
            }
            Must(status, LirOpcodeName(instruction.op));
            hooks.StoreFpToSlot(out, instruction.dst);
            return true;
        }
        const auto operands = LoadBinaryOperands(instruction, type, type);
        if (!operands) {
            return true;
        }
        const A64Reg out = hooks.ResultRegister(instruction.dst, A64::Xn(kTemp));
        A64Status status = A64Status::Ok;
        switch (instruction.op) {
        case LirOpcode::Add:
            status = encoder.Add(out, operands->lhs, operands->rhs);
            break;
        case LirOpcode::Sub:
            status = encoder.Sub(out, operands->lhs, operands->rhs);
            break;
        case LirOpcode::Mul:
            status = encoder.Mul(out, operands->lhs, operands->rhs);
            break;
        case LirOpcode::And:
            status = encoder.And(out, operands->lhs, operands->rhs);
            break;
        case LirOpcode::Or:
            status = encoder.Orr(out, operands->lhs, operands->rhs);
            break;
        default:
            status = encoder.Eor(out, operands->lhs, operands->rhs);
            break;
        }
        Must(status, LirOpcodeName(instruction.op));
        hooks.StoreToSlot(out, instruction.dst, type);
        return true;
    }
    case LirOpcode::Div:
    case LirOpcode::Mod: {
        const TypeRef &type = instruction.type;
        if (IsFloat(type)) {
            const auto operands = LoadFloatOperands(instruction, type);
            if (!operands) {
                return true;
            }
            const A64Reg out = hooks.FloatResultRegister(instruction.dst, FpReg(type, kFpTemp));
            if (instruction.op == LirOpcode::Div) {
                Must(encoder.Fdiv(out, operands->lhs, operands->rhs), LirOpcodeName(instruction.op));
                hooks.StoreFpToSlot(out, instruction.dst);
                return true;
            }
            const A64Reg quotient = FpReg(type, kFpTemp3);
            Must(encoder.Fdiv(quotient, operands->lhs, operands->rhs), LirOpcodeName(instruction.op));
            Must(encoder.Frintz(quotient, quotient), LirOpcodeName(instruction.op));
            Must(encoder.Fmsub(out, quotient, operands->rhs, operands->lhs), LirOpcodeName(instruction.op));
            hooks.StoreFpToSlot(out, instruction.dst);
            return true;
        }
        const auto operands = LoadBinaryOperands(instruction, type, type);
        if (!operands) {
            return true;
        }
        const bool divide = instruction.op == LirOpcode::Div;
        const A64Reg quotient = divide ? hooks.ResultRegister(instruction.dst, A64::Xn(kAddr)) : A64::Xn(kAddr);
        Must(type.IsSigned() ? encoder.Sdiv(quotient, operands->lhs, operands->rhs)
                             : encoder.Udiv(quotient, operands->lhs, operands->rhs),
             LirOpcodeName(instruction.op));
        if (divide) {
            hooks.StoreToSlot(quotient, instruction.dst, type);
            return true;
        }
        const A64Reg remainder = hooks.ResultRegister(instruction.dst, A64::Xn(kTemp));
        Must(encoder.Msub(remainder, quotient, operands->rhs, operands->lhs), LirOpcodeName(instruction.op));
        hooks.StoreToSlot(remainder, instruction.dst, type);
        return true;
    }
    case LirOpcode::Pow: {
        const TypeRef &type = instruction.type;
        if (instruction.srcs.size() < 2) {
            Report(std::format("AArch64 code generation reached a 'pow' with one operand in '{}'", functionName));
            return true;
        }
        if (IsFloat(type)) {
            const bool single = type.kind == TypeRef::Kind::Float32;
            hooks.LoadFpFromSlot(FpReg(type, 0), instruction.srcs[0]);
            hooks.LoadFpFromSlot(FpReg(type, 1), instruction.srcs[1]);
            const std::uint32_t site = encoder.Size();
            Must(encoder.Bl(0), "a call to the exponentiation helper");
            runtimeHelpers.AddCallRelocation(site, single ? AArch64RuntimeHelper::FloatPower32
                                                          : AArch64RuntimeHelper::FloatPower64);
            hooks.StoreFpToSlot(FpReg(type, 0), instruction.dst);
            return true;
        }
        if (!IsRegisterValue(type)) {
            NotImplemented(std::format("the '{}' opcode on '{}'", LirOpcodeName(instruction.op), type.ToString()));
            return true;
        }
        hooks.LoadFromSlot(A64::Xn(kReturn), instruction.srcs[0], type);
        hooks.LoadFromSlot(A64::Xn(kReturn + 1), instruction.srcs[1], TypeOfReg(instruction.srcs[1]));
        const std::uint32_t callSite = encoder.Size();
        Must(encoder.Bl(0), "a call to the exponentiation helper");
        runtimeHelpers.AddCallRelocation(callSite, AArch64RuntimeHelper::IntegerPower);
        hooks.StoreToSlot(A64::Xn(kReturn), instruction.dst, type);
        return true;
    }
    case LirOpcode::Shl:
    case LirOpcode::Shr:
    case LirOpcode::Lshr: {
        const TypeRef &type = instruction.type;
        if (instruction.srcs.size() < 2) {
            Report(std::format("AArch64 code generation reached a '{}' with no amount in '{}'",
                               LirOpcodeName(instruction.op), functionName));
            return true;
        }
        const bool logical = instruction.op == LirOpcode::Lshr;
        const auto operands =
            LoadBinaryOperands(instruction, logical ? UnsignedIntegerType(type) : type, TypeOfReg(instruction.srcs[1]));
        if (!operands) {
            return true;
        }
        const A64Reg out = hooks.ResultRegister(instruction.dst, A64::Xn(kTemp));
        A64Status status = A64Status::Ok;
        if (instruction.op == LirOpcode::Shl) {
            status = encoder.Lslv(out, operands->lhs, operands->rhs);
        }
        else if (logical || !type.IsSigned()) {
            status = encoder.Lsrv(out, operands->lhs, operands->rhs);
        }
        else {
            status = encoder.Asrv(out, operands->lhs, operands->rhs);
        }
        Must(status, LirOpcodeName(instruction.op));
        hooks.StoreToSlot(out, instruction.dst, type);
        return true;
    }
    case LirOpcode::Neg: {
        const TypeRef &type = instruction.type;
        if (IsFloat(type)) {
            if (instruction.srcs.empty()) {
                Report(std::format("AArch64 code generation reached a '{}' with no operand in '{}'",
                                   LirOpcodeName(instruction.op), functionName));
                return true;
            }
            const A64Reg value = hooks.ReadFloatOperand(instruction.srcs[0], FpReg(type, kFpTemp));
            const A64Reg out = hooks.FloatResultRegister(instruction.dst, FpReg(type, kFpTemp));
            Must(encoder.Fneg(out, value), LirOpcodeName(instruction.op));
            hooks.StoreFpToSlot(out, instruction.dst);
            return true;
        }
        const auto value = LoadUnaryOperand(instruction, type);
        if (!value) {
            return true;
        }
        const A64Reg out = hooks.ResultRegister(instruction.dst, A64::Xn(kTemp));
        Must(encoder.Neg(out, *value), LirOpcodeName(instruction.op));
        hooks.StoreToSlot(out, instruction.dst, type);
        return true;
    }
    case LirOpcode::Not: {
        const auto value = LoadUnaryOperand(instruction, instruction.type);
        if (!value) {
            return true;
        }
        const A64Reg out = hooks.ResultRegister(instruction.dst, A64::Xn(kTemp));
        Must(encoder.SubsImm(A64::Xzr, *value, 0), LirOpcodeName(instruction.op));
        Must(encoder.Cset(out, A64Condition::Eq), LirOpcodeName(instruction.op));
        hooks.StoreToSlot(out, instruction.dst, TypeRef::MakeBool());
        return true;
    }
    case LirOpcode::BitNot: {
        const TypeRef &type = instruction.type;
        const auto value = LoadUnaryOperand(instruction, type);
        if (!value) {
            return true;
        }
        const A64Reg out = hooks.ResultRegister(instruction.dst, A64::Xn(kTemp));
        Must(type.IsBool() ? encoder.EorImm(out, *value, 1) : encoder.Mvn(out, *value), LirOpcodeName(instruction.op));
        hooks.StoreToSlot(out, instruction.dst, type);
        return true;
    }
    case LirOpcode::CmpEq:
    case LirOpcode::CmpNe:
    case LirOpcode::CmpLt:
    case LirOpcode::CmpLe:
    case LirOpcode::CmpGt:
    case LirOpcode::CmpGe: {
        if (instruction.srcs.size() < 2) {
            Report(std::format("AArch64 code generation reached a '{}' with one operand in '{}'",
                               LirOpcodeName(instruction.op), functionName));
            return true;
        }
        const auto &registerTypes = framePlan.RegisterTypes();
        const auto operand = registerTypes.find(instruction.srcs[0]);
        const TypeRef operandType = operand != registerTypes.end() ? operand->second : instruction.type;
        A64Condition condition = A64Condition::Eq;
        if (IsFloat(operandType)) {
            const auto operands = LoadFloatOperands(instruction, operandType);
            if (!operands) {
                return true;
            }
            Must(encoder.Fcmp(operands->lhs, operands->rhs), LirOpcodeName(instruction.op));
            condition = FloatCondition(instruction.op);
        }
        else {
            const auto operands = LoadBinaryOperands(instruction, operandType, operandType);
            if (!operands) {
                return true;
            }
            Must(encoder.Cmp(operands->lhs, operands->rhs), LirOpcodeName(instruction.op));
            condition = IntegerCondition(instruction.op, operandType.IsSigned());
        }
        const A64Reg result = hooks.ResultRegister(instruction.dst, A64::Xn(kTemp));
        Must(encoder.Cset(result, condition), LirOpcodeName(instruction.op));
        hooks.StoreToSlot(result, instruction.dst, TypeRef::MakeBool());
        return true;
    }
    case LirOpcode::Cast: {
        if (instruction.srcs.empty()) {
            Report(std::format("AArch64 code generation reached a cast with no operand in '{}'", functionName));
            return true;
        }
        const TypeRef &dstType = instruction.type;
        const auto &registerTypes = framePlan.RegisterTypes();
        const auto source = registerTypes.find(instruction.srcs[0]);
        const TypeRef srcType = source != registerTypes.end() ? source->second : dstType;
        const bool srcFloat = IsFloat(srcType);
        const bool dstFloat = IsFloat(dstType);
        if (!srcFloat && !dstFloat) {
            if (!IsRegisterValue(srcType) || !IsRegisterValue(dstType)) {
                NotImplemented(std::format("a cast from '{}' to '{}'", srcType.ToString(), dstType.ToString()));
                return true;
            }
            const A64Reg value = hooks.ReadOperand(instruction.srcs[0], srcType, A64::Xn(kTemp));
            hooks.StoreToSlot(value, instruction.dst, dstType);
            return true;
        }
        if (srcFloat && dstFloat) {
            const A64Reg value = hooks.ReadFloatOperand(instruction.srcs[0], FpReg(srcType, kFpTemp));
            if (srcType.kind == dstType.kind) {
                hooks.StoreFpToSlot(value, instruction.dst);
                return true;
            }
            const A64Reg result = hooks.FloatResultRegister(instruction.dst, FpReg(dstType, kFpTemp2));
            Must(encoder.Fcvt(result, value), "a conversion between precisions");
            hooks.StoreFpToSlot(result, instruction.dst);
            return true;
        }
        if (srcFloat) {
            const A64Reg value = hooks.ReadFloatOperand(instruction.srcs[0], FpReg(srcType, kFpTemp));
            const A64Reg result = hooks.ResultRegister(instruction.dst, A64::Xn(kTemp));
            Must(dstType.IsSigned() ? encoder.Fcvtzs(result, value) : encoder.Fcvtzu(result, value),
                 "a conversion to an integer");
            hooks.StoreToSlot(result, instruction.dst, dstType);
            return true;
        }
        if (!IsRegisterValue(srcType)) {
            NotImplemented(std::format("a cast from '{}' to '{}'", srcType.ToString(), dstType.ToString()));
            return true;
        }
        const A64Reg value = hooks.ReadOperand(instruction.srcs[0], srcType, A64::Xn(kTemp));
        const A64Reg result = hooks.FloatResultRegister(instruction.dst, FpReg(dstType, kFpTemp));
        Must(srcType.IsSigned() ? encoder.Scvtf(result, value) : encoder.Ucvtf(result, value),
             "a conversion from an integer");
        hooks.StoreFpToSlot(result, instruction.dst);
        return true;
    }
    case LirOpcode::Call:
        if (IsFloatBitsBuiltin(instruction.strArg) && instruction.srcs.size() == 1) {
            EmitFloatBits(instruction);
            return true;
        }
        return false;
    default:
        return false;
    }
}

bool AArch64FunctionEmitter::EmitMemory(const LirInstr &instruction) {
    switch (instruction.op) {
    case LirOpcode::Const: {
        if (instruction.dst == LirNoReg) {
            return true;
        }
        if (instruction.type.kind == TypeRef::Kind::Str) {
            const A64Reg text = hooks.ResultRegister(instruction.dst, A64::Xn(kTemp));
            hooks.LoadSymbolAddress(text, hooks.InternStringLiteral(instruction.strArg));
            hooks.StoreToSlot(text, instruction.dst, instruction.type);
            return true;
        }
        if (IsFloat(instruction.type)) {
            const A64Reg value = hooks.FloatResultRegister(instruction.dst, FpReg(instruction.type, kFpTemp));
            hooks.LoadFloatConstant(value, instruction.type, instruction.strArg);
            hooks.StoreFpToSlot(value, instruction.dst);
            return true;
        }
        if (EmitSoftwareFloatConstant(instruction)) {
            return true;
        }
        if (EmitWideConstant(instruction)) {
            return true;
        }
        if (!IsRegisterValue(instruction.type)) {
            NotImplemented(std::format("a constant of type '{}'", instruction.type.ToString()));
            return true;
        }
        const A64Reg value = hooks.ResultRegister(instruction.dst, A64::Xn(kTemp));
        Must(encoder.LoadImm64(value, ConstantBits(instruction)), "a constant");
        hooks.StoreToSlot(value, instruction.dst, instruction.type);
        return true;
    }
    case LirOpcode::Alloca: {
        const auto &allocaData = framePlan.AllocaDataOffsets();
        const auto it = allocaData.find(instruction.dst);
        if (it == allocaData.end()) {
            Report(std::format("AArch64 code generation reached an alloca with no storage in '{}'", functionName));
            return true;
        }
        const A64Reg address = hooks.ResultRegister(instruction.dst, A64::Xn(kTemp));
        Must(encoder.AddSubLargeImm(address, A64::Fp, it->second), "the address of a local");
        hooks.StoreToSlot(address, instruction.dst, TypeRef::MakePointer(instruction.type));
        return true;
    }
    case LirOpcode::GlobalAddr: {
        const std::uint32_t symbol = hooks.ResolveGlobalSymbol(instruction.strArg);
        const A64Reg address = hooks.ResultRegister(instruction.dst, A64::Xn(kTemp));
        hooks.LoadSymbolAddress(address, symbol);
        hooks.StoreToSlot(address, instruction.dst, TypeRef::MakePointer(instruction.type));
        return true;
    }
    case LirOpcode::StringAddr: {
        const TypeRef elementType = instruction.type.inner.empty() ? TypeRef::MakeChar8() : instruction.type.inner[0];
        const std::uint32_t symbol =
            hooks.InternStringLiteral(EncodeStringLiteral(instruction.strArg, RuntimeSize(elementType)));
        const A64Reg address = hooks.ResultRegister(instruction.dst, A64::Xn(kTemp));
        hooks.LoadSymbolAddress(address, symbol);
        hooks.StoreToSlot(address, instruction.dst, instruction.type);
        return true;
    }
    case LirOpcode::Load: {
        if (instruction.dst == LirNoReg) {
            return true;
        }
        const TypeRef &type = instruction.type;
        const int size = RuntimeSize(type);
        if (!instruction.strArg.empty()) {
            const A64Reg value = hooks.ResultRegister(instruction.dst, A64::Xn(kTemp));
            hooks.LoadNamedDataSymbol(value, instruction.strArg);
            hooks.StoreToSlot(value, instruction.dst, type);
            return true;
        }
        const A64Reg address = hooks.ReadPointerOperand(instruction.srcs[0], A64::Xn(kAddr));
        if (IsAggregate(type) && size > 8) {
            hooks.CopyBlock(A64::Fp, Disp(instruction.dst), address, 0, size, RuntimeAlign(type) >= 8);
            return true;
        }
        if (IsFloat(type)) {
            const A64Reg value = hooks.FloatResultRegister(instruction.dst, FpReg(type, kFpTemp));
            hooks.LoadScalar(value, address, 0, value.bits / 8U, false);
            hooks.StoreFpToSlot(value, instruction.dst);
            return true;
        }
        const A64Reg value = hooks.ResultRegister(instruction.dst, A64::Xn(kTemp));
        hooks.LoadScalar(value, address, 0, AccessWidth(size), type.IsSigned());
        hooks.StoreToSlot(value, instruction.dst, type);
        return true;
    }
    case LirOpcode::Store: {
        if (instruction.srcs.size() < 2) {
            Report(std::format("AArch64 code generation reached a store with no pointer in '{}'", functionName));
            return true;
        }
        const LirReg valueReg = instruction.srcs[0];
        const TypeRef &type = instruction.type;
        const int size = RuntimeSize(type);
        const A64Reg address = hooks.ReadPointerOperand(instruction.srcs[1], A64::Xn(kAddr));
        if (IsAggregate(type) && size > 8) {
            A64Reg source = A64::Xn(kSrcAddr);
            if (IsRegPointerTo(valueReg, type)) {
                source = hooks.ReadPointerOperand(valueReg, source);
            }
            else {
                hooks.SlotAddress(source, valueReg);
            }
            hooks.CopyBlock(address, 0, source, 0, size, RuntimeAlign(type) >= 8);
            return true;
        }
        if (IsFloat(type)) {
            const A64Reg value = hooks.ReadFloatOperand(valueReg, FpReg(type, kFpTemp));
            hooks.StoreScalar(value, address, 0, value.bits / 8U);
            return true;
        }
        const A64Reg value = hooks.ReadRawOperand(valueReg, AccessWidth(size), A64::Xn(kTemp));
        hooks.StoreScalar(value, address, 0, AccessWidth(size));
        return true;
    }
    case LirOpcode::FieldPtr: {
        const LirReg base = instruction.srcs[0];
        const auto &registerTypes = framePlan.RegisterTypes();
        const auto baseType = registerTypes.find(base);
        const int offset = baseType == registerTypes.end()
                             ? 0
                             : FieldOffsetOf(baseType->second, instruction.strArg, layouts, interfaceNames);
        const A64Reg source = hooks.ReadPointerOperand(base, A64::Xn(kTemp));
        const A64Reg address = hooks.ResultRegister(instruction.dst, A64::Xn(kTemp));
        if (offset != 0) {
            Must(encoder.AddSubLargeImm(address, source, offset), "the address of a field");
        }
        else if (address.code != source.code) {
            Must(encoder.Mov(address, source), "the address of a field");
        }
        hooks.StoreToSlot(address, instruction.dst, TypeRef::MakePointer(instruction.type));
        return true;
    }
    case LirOpcode::IndexPtr: {
        if (instruction.srcs.size() < 2) {
            Report(std::format("AArch64 code generation reached an index with no subscript in '{}'", functionName));
            return true;
        }
        const bool known = instruction.type.kind == TypeRef::Kind::Pointer && !instruction.type.inner.empty();
        const int elementSize = std::max(known ? RuntimeSize(instruction.type.inner[0]) : 8, 1);
        const A64Reg source = hooks.ReadPointerOperand(instruction.srcs[0], A64::Xn(kTemp));
        const A64Reg index = hooks.ReadOperand(instruction.srcs[1], TypeOfReg(instruction.srcs[1]), A64::Xn(kAddr));
        const A64Reg address = hooks.ResultRegister(instruction.dst, A64::Xn(kTemp));
        const auto shift = static_cast<unsigned>(std::countr_zero(static_cast<unsigned>(elementSize)));
        if (std::has_single_bit(static_cast<unsigned>(elementSize)) && shift < 64) {
            Must(encoder.Add(address, source, index, A64ShiftKind::Lsl, shift), "an element address");
        }
        else {
            const A64Reg width = A64::Xn(kTemp2);
            Must(encoder.LoadImm64(width, static_cast<std::uint64_t>(elementSize)), "an element width");
            Must(encoder.Madd(address, index, width, source), "an element address");
        }
        hooks.StoreToSlot(address, instruction.dst, TypeRef::MakePointer(instruction.type));
        return true;
    }
    default:
        return false;
    }
}
} // namespace Rux
