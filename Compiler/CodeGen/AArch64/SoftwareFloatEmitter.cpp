#include "CodeGen/AArch64/FunctionEmitter.h"
#include "CodeGen/Layout.h"
#include "Numeric/FloatParsing.h"
#include "Types/PrimitiveCatalog.h"

namespace Rux {
using namespace Layout;

namespace {
constexpr unsigned kValue = 13;
constexpr unsigned kMask = 14;
} // namespace

bool AArch64FunctionEmitter::EmitSoftwareFloatNegation(const LirInstr &instruction) {
    const TypeRef operandType = instruction.srcs.empty() ? instruction.type : TypeOfReg(instruction.srcs[0]);
    if (instruction.dst == LirNoReg || instruction.srcs.empty() || !IsSoftwareFloat(operandType) ||
        instruction.op != LirOpcode::Neg) {
        return false;
    }

    const PrimitiveInfo *primitive = FindPrimitive(operandType.kind);
    const int size = RuntimeSize(operandType);
    const int signBit = primitive ? static_cast<int>(primitive->bits - 1) : size * 8 - 1;
    Must(encoder.LoadImm64(A64::Xn(kMask), std::uint64_t{1} << (signBit % 64)), "a software-float sign mask");

    if (size <= 8) {
        const A64Reg value = hooks.ReadOperand(instruction.srcs[0], operandType, A64::Xn(kValue));
        const A64Reg result = hooks.ResultRegister(instruction.dst, A64::Xn(kValue));
        Must(encoder.Eor(result, value, A64::Xn(kMask)), "a software-float negation");
        hooks.StoreToSlot(result, instruction.dst, instruction.type);
        return true;
    }

    CopyWide(Disp(instruction.srcs[0]), Disp(instruction.dst), size);
    const int signWord = signBit / 64;
    LoadWideWord(A64::Xn(kValue), instruction.dst, signWord);
    Must(encoder.Eor(A64::Xn(kValue), A64::Xn(kValue), A64::Xn(kMask)), "a software-float negation");
    StoreWideWord(A64::Xn(kValue), instruction.dst, signWord);
    return true;
}

bool AArch64FunctionEmitter::EmitSoftwareFloatConstant(const LirInstr &instruction) {
    if (!IsSoftwareFloat(instruction.type)) {
        return false;
    }

    const int size = RuntimeSize(instruction.type);
    const PrimitiveInfo *primitive = FindPrimitive(instruction.type.kind);
    const FloatFormat *format = primitive ? FindFloatFormat(primitive->bits) : nullptr;
    const auto encoding = format ? ParseFloatEncoding(instruction.strArg, *format) : std::nullopt;
    const WideInteger bits = encoding ? encoding->Bits() : WideInteger::Zero(static_cast<std::uint32_t>(size * 8));

    if (size <= 8) {
        const A64Reg value = hooks.ResultRegister(instruction.dst, A64::Xn(kValue));
        Must(encoder.LoadImm64(value, bits.Word64(0)), "a software-float constant");
        hooks.StoreToSlot(value, instruction.dst, instruction.type);
        return true;
    }

    for (int word = 0; word < size / 8; ++word) {
        Must(encoder.LoadImm64(A64::Xn(kValue), bits.Word64(static_cast<std::size_t>(word))),
             "a software-float constant");
        StoreWideWord(A64::Xn(kValue), instruction.dst, word);
    }
    return true;
}
} // namespace Rux
