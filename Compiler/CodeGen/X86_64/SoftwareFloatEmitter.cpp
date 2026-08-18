#include "CodeGen/Layout.h"
#include "CodeGen/X86_64/Encoder.h"
#include "CodeGen/X86_64/FunctionEmitter.h"
#include "Numeric/FloatParsing.h"
#include "Semantic/PrimitiveCatalog.h"

namespace Rux {
using namespace Layout;

bool X86_64FunctionEmitter::EmitWideSoftwareFloatNegation(const LirInstr &instruction) {
    const auto &types = framePlan.RegisterTypes();
    const TypeRef operandType = !instruction.srcs.empty() && types.contains(instruction.srcs[0])
                                  ? types.at(instruction.srcs[0])
                                  : instruction.type;
    if (instruction.dst == LirNoReg || !IsSoftwareFloat(operandType) || instruction.op != LirOpcode::Neg) {
        return false;
    }
    const PrimitiveInfo *primitive = FindPrimitive(operandType.kind);
    const int size = SizeOf(operandType);
    const int signBit = primitive ? static_cast<int>(primitive->bits - 1) : size * 8 - 1;
    if (size <= 8) {
        hooks.LoadA(instruction.srcs.at(0), operandType);
        encoder.XorRaxImmediate(static_cast<std::int32_t>(std::uint32_t{1} << signBit));
        hooks.StoreA(instruction.dst, instruction.type);
        return true;
    }
    const int signOffset = (signBit / 64) * 8;
    CopyWide(Disp(instruction.srcs.at(0)), Disp(instruction.dst), size);
    encoder.MovR10Load(Disp(instruction.dst) + signOffset);
    encoder.MovRaxImm64(static_cast<std::int64_t>(std::uint64_t{1} << (signBit % 64)));
    encoder.XorRaxR10();
    encoder.MovRaxStore(Disp(instruction.dst) + signOffset);
    return true;
}

void X86_64FunctionEmitter::EmitSoftwareFloatConstant(const LirInstr &instruction) {
    const int size = SizeOf(instruction.type);
    const PrimitiveInfo *primitive = FindPrimitive(instruction.type.kind);
    const FloatFormat *format = primitive ? FindFloatFormat(primitive->bits) : nullptr;
    const auto encoding = format ? ParseFloatEncoding(instruction.strArg, *format) : std::nullopt;
    const WideInteger bits = encoding ? encoding->Bits() : WideInteger::Zero(static_cast<std::uint32_t>(size * 8));
    const auto loadWord = [&](const std::uint64_t word) {
        if (word <= 0x7FFF'FFFF) {
            encoder.MovEaxImm32(static_cast<std::int32_t>(word));
        }
        else {
            encoder.MovRaxImm64(static_cast<std::int64_t>(word));
        }
    };
    if (size <= 8) {
        loadWord(bits.Word64(0));
        hooks.StoreA(instruction.dst, instruction.type);
        return;
    }
    for (int offset = 0; offset < size; offset += 8) {
        loadWord(bits.Word64(static_cast<std::size_t>(offset / 8)));
        encoder.MovRaxStore(Disp(instruction.dst) + offset);
    }
}
} // namespace Rux
