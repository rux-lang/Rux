#pragma once

#include "CodeGen/X86_64/FramePlan.h"
#include "CodeGen/X86_64/Registers.h"

#include <bit>
#include <format>
#include <optional>
#include <string>

namespace Rux {
struct X86ScalarAddressInstruction {
    int destination;
    int base;
    int index = -1;
    int scale = 1;
    std::int32_t displacement = 0;
    std::optional<std::int32_t> baseSlot;
    std::optional<std::int32_t> indexSlot;
};

[[nodiscard]] inline std::optional<std::int32_t> ScalarAddressImmediate(const LirInstr &instruction,
                                                                        const std::uint64_t index, const int scale) {
    if (instruction.op != LirOpcode::IndexPtr || instruction.isVolatile || instruction.dst == LirNoReg ||
        instruction.srcs.size() != 2 || instruction.type->kind != TypeRef::Kind::Pointer ||
        (scale != 1 && scale != 2 && scale != 4 && scale != 8))
        return std::nullopt;
    // Address arithmetic wraps at the target's 64-bit pointer width, just like the indexed LEA form.
    const auto bytes = index * static_cast<std::uint64_t>(scale);
    const auto immediate = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(bytes));
    return bytes == static_cast<std::uint64_t>(static_cast<std::int64_t>(immediate)) ? std::optional{immediate}
                                                                                     : std::nullopt;
}

/// Preserve existing layout decisions while removing address-generation scratch copies. Narrow indices still need
/// the ordinary operand loader's normalization; spilled references may have a multiword representation.
[[nodiscard]] inline std::optional<X86ScalarAddressInstruction>
SelectScalarAddressInstruction(const LirInstr &instruction, const X86_64FramePlan &plan, const int scale,
                               const std::int32_t displacement = 0) {
    const bool indexed = instruction.op == LirOpcode::IndexPtr;
    if ((!indexed && instruction.op != LirOpcode::FieldPtr) || instruction.isVolatile ||
        instruction.srcs.size() != (indexed ? 2U : 1U) ||
        (indexed && (scale != 1 && scale != 2 && scale != 4 && scale != 8)))
        return std::nullopt;
    const auto &homes = plan.PhysicalRegisters();
    const auto &types = plan.RegisterTypes();
    const auto &slots = plan.SlotOffsets();
    const auto base = instruction.srcs[0];
    if (!homes.contains(instruction.dst) || !types.contains(base) || !types.contains(instruction.dst) ||
        types.at(instruction.dst)->kind != TypeRef::Kind::Pointer ||
        (types.at(base)->kind != TypeRef::Kind::Pointer && types.at(base)->kind != TypeRef::Kind::Reference))
        return std::nullopt;
    X86ScalarAddressInstruction result;
    result.destination = X86AllocatedGprs.at(static_cast<std::size_t>(homes.at(instruction.dst)));
    result.base = 0;
    result.displacement = displacement;
    if (homes.contains(base))
        result.base = X86AllocatedGprs.at(static_cast<std::size_t>(homes.at(base)));
    else {
        if (types.at(base)->kind != TypeRef::Kind::Pointer || !slots.contains(base))
            return std::nullopt;
        result.baseSlot = -slots.at(base);
    }
    if (indexed) {
        const auto index = instruction.srcs[1];
        if (!types.contains(index) || !types.at(index).IsInteger() || Layout::SizeOf(types.at(index)) != 8)
            return std::nullopt;
        if (displacement == 0 && plan.ImmediateValues().contains(index)) {
            if (const auto immediate = ScalarAddressImmediate(instruction, plan.ImmediateValues().at(index), scale)) {
                result.displacement = *immediate;
                return result;
            }
        }
        result.scale = scale;
        result.index = 10;
        if (homes.contains(index))
            result.index = X86AllocatedGprs.at(static_cast<std::size_t>(homes.at(index)));
        else {
            if (!slots.contains(index))
                return std::nullopt;
            result.indexSlot = -slots.at(index);
        }
    }
    return result;
}

[[nodiscard]] inline std::string ScalarAddressOperands(const X86ScalarAddressInstruction &instruction) {
    auto address = std::string(X86GeneralRegisterName(instruction.base));
    if (instruction.index >= 0)
        address += std::format(" + {} * {}", X86GeneralRegisterName(instruction.index), instruction.scale);
    if (instruction.displacement > 0)
        address += std::format(" + {}", instruction.displacement);
    else if (instruction.displacement < 0)
        address += std::format(" - {}", -static_cast<std::int64_t>(instruction.displacement));
    return std::format("{}, [{}]", X86GeneralRegisterName(instruction.destination), address);
}
} // namespace Rux
