#pragma once

#include "CodeGen/X86_64/FramePlan.h"
#include "CodeGen/X86_64/Registers.h"

#include <optional>
#include <string>
#include <string_view>

namespace Rux {
struct X86ScalarCastInstruction {
    int destination;
    int source;
    int sourceBytes;
    bool signedSource;
};

[[nodiscard]] inline bool ScalarCastType(const TypeRef &type) {
    const auto bytes = Layout::SizeOf(type);
    return (type.IsInteger() || type.IsChar() || type.kind == TypeRef::Kind::Pointer) &&
           (bytes == 1 || bytes == 2 || bytes == 4 || bytes == 8);
}

[[nodiscard]] inline std::optional<X86ScalarCastInstruction> SelectScalarCastInstruction(const LirInstr &instruction,
                                                                                         const X86_64FramePlan &plan) {
    if (instruction.op != LirOpcode::Cast || instruction.isVolatile || instruction.srcs.size() != 1 ||
        instruction.dst == LirNoReg || !ScalarCastType(instruction.type))
        return std::nullopt;
    const auto &types = plan.RegisterTypes();
    const auto &homes = plan.PhysicalRegisters();
    const auto source = instruction.srcs[0];
    if (!types.contains(source) || !ScalarCastType(types.at(source)) || !types.contains(instruction.dst) ||
        types.at(instruction.dst) != instruction.type || !homes.contains(source) || !homes.contains(instruction.dst))
        return std::nullopt;
    const auto &sourceType = types.at(source);
    // Register homes carry the source-normalized word, just like LoadA followed by StoreA. Narrow destinations
    // retain their low bits; consumers already read/normalize at the destination type's width.
    return X86ScalarCastInstruction{X86AllocatedGprs.at(static_cast<std::size_t>(homes.at(instruction.dst))),
                                    X86AllocatedGprs.at(static_cast<std::size_t>(homes.at(source))),
                                    Layout::SizeOf(sourceType), sourceType.IsSigned()};
}

[[nodiscard]] inline std::string_view ScalarCastMnemonic(const X86ScalarCastInstruction &instruction) {
    if (instruction.sourceBytes == 8 || (instruction.sourceBytes == 4 && !instruction.signedSource))
        return "mov";
    if (instruction.sourceBytes == 4)
        return "movsxd";
    return instruction.signedSource ? "movsx" : "movzx";
}

[[nodiscard]] inline std::string ScalarCastOperands(const X86ScalarCastInstruction &instruction) {
    const int destinationBytes = instruction.sourceBytes == 4 && !instruction.signedSource ? 4 : 8;
    return std::string(X86SizedRegisterName(instruction.destination, destinationBytes)) + ", " +
           std::string(X86SizedRegisterName(instruction.source, instruction.sourceBytes));
}
} // namespace Rux
