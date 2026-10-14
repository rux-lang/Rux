#pragma once

#include "CodeGen/X86_64/FramePlan.h"
#include "CodeGen/X86_64/Registers.h"

#include <optional>
#include <string>
#include <string_view>

namespace Rux {
/// One exact-width scalar memory access. Architectural register numbers are shared by text and binary emission.
struct X86ScalarMemoryInstruction {
    int address;
    int value;
    int bytes;
    bool store;
    bool floating;
    bool signedLoad;
};

[[nodiscard]] inline std::optional<X86ScalarMemoryInstruction>
SelectScalarMemoryInstruction(const LirInstr &instruction, const X86_64FramePlan &plan) {
    const bool store = instruction.op == LirOpcode::Store;
    if ((!store && instruction.op != LirOpcode::Load) || instruction.isVolatile || !instruction.strArg.empty() ||
        instruction.srcs.size() != (store ? 2U : 1U))
        return std::nullopt;
    const auto &type = instruction.type;
    const bool floating = type->kind == TypeRef::Kind::Float32 || type->kind == TypeRef::Kind::Float64;
    // No aggregate, interface, Boolean normalization or software-floating representation is guessed here.
    if (!floating && !type.IsInteger() && !type.IsChar() && type->kind != TypeRef::Kind::Pointer)
        return std::nullopt;
    const auto bytes = Layout::SizeOf(type);
    if (bytes != 1 && bytes != 2 && bytes != 4 && bytes != 8)
        return std::nullopt;
    const auto pointer = instruction.srcs[store ? 1 : 0];
    const auto value = store ? instruction.srcs[0] : instruction.dst;
    const auto &general = plan.PhysicalRegisters();
    const auto &values = floating ? plan.FloatRegisters() : general;
    const auto &types = plan.RegisterTypes();
    if (!general.contains(pointer) || !values.contains(value) || !types.contains(value) || types.at(value) != type ||
        !types.contains(pointer) ||
        (types.at(pointer)->kind != TypeRef::Kind::Pointer && types.at(pointer)->kind != TypeRef::Kind::Reference))
        return std::nullopt;
    return X86ScalarMemoryInstruction{X86AllocatedGprs.at(static_cast<std::size_t>(general.at(pointer))),
                                      floating ? values.at(value)
                                               : X86AllocatedGprs.at(static_cast<std::size_t>(values.at(value))),
                                      bytes,
                                      store,
                                      floating,
                                      !store && type.IsSigned()};
}

[[nodiscard]] inline std::string_view ScalarMemoryMnemonic(const X86ScalarMemoryInstruction &instruction) {
    if (instruction.floating)
        return instruction.bytes == 4 ? "movss" : "movsd";
    if (instruction.store || instruction.bytes == 8 || (instruction.bytes == 4 && !instruction.signedLoad))
        return "mov";
    if (instruction.bytes == 4)
        return "movsxd";
    return instruction.signedLoad ? "movsx" : "movzx";
}

[[nodiscard]] inline std::string ScalarMemoryValue(const X86ScalarMemoryInstruction &instruction) {
    if (instruction.floating)
        return "xmm" + std::to_string(instruction.value);
    const auto width = instruction.store                                 ? instruction.bytes
                     : instruction.bytes == 4 && !instruction.signedLoad ? 4
                                                                         : 8;
    return std::string(X86SizedRegisterName(instruction.value, width));
}

[[nodiscard]] inline std::string ScalarMemoryAddress(const X86ScalarMemoryInstruction &instruction) {
    const std::string_view size = instruction.bytes == 1 ? "byte"
                                : instruction.bytes == 2 ? "word"
                                : instruction.bytes == 4 ? "dword"
                                                         : "qword";
    return std::string(size) + " [" + std::string(X86GeneralRegisterName(instruction.address)) + "]";
}
} // namespace Rux
