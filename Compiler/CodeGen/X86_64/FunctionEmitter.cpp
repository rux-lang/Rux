#include "CodeGen/X86_64/FunctionEmitter.h"

#include "CodeGen/Layout.h"
#include "CodeGen/X86_64/Encoder.h"

namespace Rux {
using namespace Layout;

X86_64FunctionEmitter::X86_64FunctionEmitter(X64Enc &encoder, const X86_64FramePlan &framePlan,
                                             X86_64RuntimeHelperEmitter &runtimeHelpers,
                                             const CallingConvention defaultConvention,
                                             X86_64FunctionEmitterHooks &hooks)
    : encoder(encoder)
    , framePlan(framePlan)
    , runtimeHelpers(runtimeHelpers)
    , defaultConvention(defaultConvention)
    , hooks(hooks) {
}

std::int32_t X86_64FunctionEmitter::Disp(const LirReg reg) const {
    return -framePlan.SlotOffsets().at(reg);
}

bool X86_64FunctionEmitter::EmitArithmetic(const LirInstr &instruction) {
    const auto &registerTypes = framePlan.RegisterTypes();
    const auto &physicalRegisters = framePlan.PhysicalRegisters();
    switch (instruction.op) {
    case LirOpcode::Add:
    case LirOpcode::Sub:
    case LirOpcode::And:
    case LirOpcode::Or:
    case LirOpcode::Xor: {
        const TypeRef &type = instruction.type;
        hooks.LoadA(instruction.srcs[0], type);
        hooks.LoadB(instruction.srcs[1], type);
        if (IsFloat(type)) {
            const bool float32 = type.kind == TypeRef::Kind::Float32;
            if (instruction.op == LirOpcode::Add) {
                float32 ? encoder.AddssXmm01() : encoder.AddsdXmm01();
            }
            else if (instruction.op == LirOpcode::Sub) {
                float32 ? encoder.SubssXmm01() : encoder.SubsdXmm01();
            }
            else {
                // Bitwise operations are not produced for floating values;
                // preserve the historical fallback if malformed LIR reaches
                // instruction selection.
                float32 ? encoder.AddssXmm01() : encoder.AddsdXmm01();
            }
        }
        else if (instruction.op == LirOpcode::Add) {
            encoder.AddRaxR10();
        }
        else if (instruction.op == LirOpcode::Sub) {
            encoder.SubRaxR10();
        }
        else if (instruction.op == LirOpcode::And) {
            encoder.AndRaxR10();
        }
        else if (instruction.op == LirOpcode::Or) {
            encoder.OrRaxR10();
        }
        else {
            encoder.XorRaxR10();
        }
        hooks.StoreA(instruction.dst, type);
        return true;
    }
    case LirOpcode::Mul: {
        const TypeRef &type = instruction.type;
        hooks.LoadA(instruction.srcs[0], type);
        hooks.LoadB(instruction.srcs[1], type);
        if (IsFloat(type)) {
            type.kind == TypeRef::Kind::Float32 ? encoder.MulssXmm01() : encoder.MulsdXmm01();
        }
        else {
            encoder.ImulRaxR10();
        }
        hooks.StoreA(instruction.dst, type);
        return true;
    }
    case LirOpcode::Div:
    case LirOpcode::Mod: {
        const TypeRef &type = instruction.type;
        hooks.LoadA(instruction.srcs[0], type);
        hooks.LoadB(instruction.srcs[1], type);
        if (IsFloat(type)) {
            if (instruction.op == LirOpcode::Div) {
                type.kind == TypeRef::Kind::Float32 ? encoder.DivssXmm01() : encoder.DivsdXmm01();
            }
            else {
                type.kind == TypeRef::Kind::Float32 ? encoder.FmodssXmm01() : encoder.FmodsdXmm01();
            }
        }
        else {
            if (type.IsSigned()) {
                encoder.Cqo();
                encoder.IdivR10();
            }
            else {
                encoder.XorRdxRdx();
                encoder.DivR10();
            }
            if (instruction.op == LirOpcode::Mod) {
                encoder.MovRaxRdx();
            }
        }
        hooks.StoreA(instruction.dst, type);
        return true;
    }
    case LirOpcode::Pow: {
        const TypeRef &type = instruction.type;
        const bool win64Call = defaultConvention == CallingConvention::Win64;
        const std::size_t stackArguments = instruction.srcs.size() > 4 ? instruction.srcs.size() - 4 : 0;
        const int callFrameSize = win64Call ? AlignUp(static_cast<int>(32 + stackArguments * 8), 16) : 0;
        if (win64Call) {
            encoder.SubRspImm32(callFrameSize);
        }
        hooks.EmitCallArguments(instruction.srcs);
        std::uint32_t relocationOffset;
        encoder.Call(relocationOffset);
        if (IsFloat(type)) {
            runtimeHelpers.AddCallRelocation(relocationOffset, type.kind == TypeRef::Kind::Float32
                                                                   ? X86_64RuntimeHelper::FloatPower32
                                                                   : X86_64RuntimeHelper::FloatPower64);
        }
        else {
            runtimeHelpers.AddCallRelocation(relocationOffset, X86_64RuntimeHelper::IntegerPower);
        }
        if (win64Call) {
            encoder.AddRspImm32(callFrameSize);
        }
        hooks.StoreA(instruction.dst, type);
        return true;
    }
    case LirOpcode::Shl:
    case LirOpcode::Shr:
    case LirOpcode::Lshr: {
        const TypeRef &type = instruction.type;
        hooks.LoadA(instruction.srcs[0], instruction.op == LirOpcode::Lshr ? UnsignedIntegerType(type) : type);
        if (const auto physical = physicalRegisters.find(instruction.srcs[1]); physical != physicalRegisters.end()) {
            encoder.MovR11PhysReg(physical->second);
        }
        else {
            encoder.MovR11Load(Disp(instruction.srcs[1]));
        }
        encoder.MovRcxR11();
        const bool shiftRight = instruction.op == LirOpcode::Shr || instruction.op == LirOpcode::Lshr;
        if (shiftRight && type.IsSigned() && instruction.op != LirOpcode::Lshr) {
            encoder.SarRaxCl();
        }
        else if (shiftRight) {
            encoder.ShrRaxCl();
        }
        else {
            encoder.ShlRaxCl();
        }
        hooks.StoreA(instruction.dst, type);
        return true;
    }
    case LirOpcode::Neg: {
        const TypeRef &type = instruction.type;
        hooks.LoadA(instruction.srcs[0], type);
        if (IsFloat(type)) {
            const bool float32 = type.kind == TypeRef::Kind::Float32;
            const std::uint32_t maskSymbol = hooks.InternFloatSignMask(float32);
            std::uint32_t relocationOffset;
            float32 ? encoder.MovssXmm1Rip(relocationOffset) : encoder.MovsdXmm1Rip(relocationOffset);
            hooks.AddTextRelocation(relocationOffset, maskSymbol);
            float32 ? encoder.XorpsXmm01() : encoder.XorpdXmm01();
        }
        else {
            encoder.NegRax();
        }
        hooks.StoreA(instruction.dst, type);
        return true;
    }
    case LirOpcode::Not:
        hooks.LoadA(instruction.srcs[0], instruction.type);
        encoder.TestRaxRax();
        encoder.SeteAl();
        encoder.MovzxRaxAl();
        hooks.StoreA(instruction.dst, TypeRef::MakeBool());
        return true;
    case LirOpcode::BitNot:
        hooks.LoadA(instruction.srcs[0], instruction.type);
        if (instruction.type.IsBool()) {
            // Bool bitwise NOT is logical NOT so its stored value remains
            // canonical (zero or one).
            encoder.XorRaxImmediate(1);
        }
        else {
            encoder.NotRax();
        }
        hooks.StoreA(instruction.dst, instruction.type);
        return true;
    case LirOpcode::CmpEq:
    case LirOpcode::CmpNe:
    case LirOpcode::CmpLt:
    case LirOpcode::CmpLe:
    case LirOpcode::CmpGt:
    case LirOpcode::CmpGe: {
        const TypeRef &lhsType =
            registerTypes.contains(instruction.srcs[0]) ? registerTypes.at(instruction.srcs[0]) : instruction.type;
        hooks.LoadA(instruction.srcs[0], lhsType);
        hooks.LoadB(instruction.srcs[1], lhsType);
        if (IsFloat(lhsType)) {
            lhsType.kind == TypeRef::Kind::Float32 ? encoder.UcomissXmm01() : encoder.UcomisdXmm01();
            switch (instruction.op) {
            case LirOpcode::CmpEq:
                encoder.SeteAl();
                encoder.SetnpDl();
                encoder.AndAlDl();
                break;
            case LirOpcode::CmpNe:
                encoder.SetneAl();
                encoder.SetpDl();
                encoder.OrAlDl();
                break;
            case LirOpcode::CmpLt:
                encoder.SetbAl();
                encoder.SetnpDl();
                encoder.AndAlDl();
                break;
            case LirOpcode::CmpLe:
                encoder.SetbeAl();
                encoder.SetnpDl();
                encoder.AndAlDl();
                break;
            case LirOpcode::CmpGt:
                encoder.SetaAl();
                encoder.SetnpDl();
                encoder.AndAlDl();
                break;
            case LirOpcode::CmpGe:
                encoder.SetaeAl();
                encoder.SetnpDl();
                encoder.AndAlDl();
                break;
            default:
                break;
            }
        }
        else {
            encoder.CmpRaxR10();
            const bool signedComparison = lhsType.IsSigned();
            switch (instruction.op) {
            case LirOpcode::CmpEq:
                encoder.SeteAl();
                break;
            case LirOpcode::CmpNe:
                encoder.SetneAl();
                break;
            case LirOpcode::CmpLt:
                signedComparison ? encoder.SetlAl() : encoder.SetbAl();
                break;
            case LirOpcode::CmpLe:
                signedComparison ? encoder.SetleAl() : encoder.SetbeAl();
                break;
            case LirOpcode::CmpGt:
                signedComparison ? encoder.SetgAl() : encoder.SetaAl();
                break;
            default:
                signedComparison ? encoder.SetgeAl() : encoder.SetaeAl();
                break;
            }
        }
        encoder.MovzxRaxAl();
        hooks.StoreA(instruction.dst, TypeRef::MakeBool());
        return true;
    }
    case LirOpcode::Cast: {
        const TypeRef &destinationType = instruction.type;
        const TypeRef sourceType =
            registerTypes.contains(instruction.srcs[0]) ? registerTypes.at(instruction.srcs[0]) : destinationType;
        hooks.LoadA(instruction.srcs[0], sourceType);
        const bool sourceFloat = IsFloat(sourceType);
        const bool destinationFloat = IsFloat(destinationType);
        if (sourceFloat && !destinationFloat) {
            sourceType.kind == TypeRef::Kind::Float32 ? encoder.CvttsssiRaxXmm0() : encoder.CvttsdsiRaxXmm0();
        }
        else if (!sourceFloat && destinationFloat) {
            destinationType.kind == TypeRef::Kind::Float32 ? encoder.Cvtsi2ssXmm0Rax() : encoder.Cvtsi2sdXmm0Rax();
        }
        else if (sourceFloat && destinationFloat) {
            if (sourceType.kind == TypeRef::Kind::Float32 && destinationType.kind == TypeRef::Kind::Float64) {
                encoder.CvtsssdXmm0();
            }
            else if (sourceType.kind == TypeRef::Kind::Float64 && destinationType.kind == TypeRef::Kind::Float32) {
                encoder.CvtsdssXmm0();
            }
        }
        hooks.StoreA(instruction.dst, destinationType);
        return true;
    }
    default:
        return false;
    }
}
} // namespace Rux
