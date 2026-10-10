#pragma once

#include "CodeGen/X86_64/FramePlan.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace Rux {
enum class X86ScalarRegisterClass {
    Integer64,
    Float32,
    Float64
};

/// Selected two-address machine operation. Registers are architectural numbers. The destination is tied to `left`;
/// emission inserts its copy when needed. Integer operations define flags; floating operations preserve them.
struct X86ScalarMachineInstruction {
    X86ScalarRegisterClass registerClass;
    int destination;
    int left;
    int right;
    std::uint8_t opcode;
    bool twoByteOpcode;
    bool destinationInRegField;
    std::string_view mnemonic;
};

/// Select only fully allocated, same-width scalar arithmetic. Narrow/wide values, spills, fixed-register operations
/// and destructive right-operand overlaps continue through the established scratch-register emitter.
[[nodiscard]] inline std::optional<X86ScalarMachineInstruction>
SelectScalarMachineInstruction(const LirInstr &instruction, const X86_64FramePlan &plan) {
    if (instruction.srcs.size() != 2 || instruction.dst == LirNoReg || instruction.isVolatile)
        return std::nullopt;
    const bool single = instruction.type.kind == TypeRef::Kind::Float32;
    const bool floating = single || instruction.type.kind == TypeRef::Kind::Float64;
    if (!floating && instruction.type.kind != TypeRef::Kind::Int64 && instruction.type.kind != TypeRef::Kind::UInt64 &&
        instruction.type.kind != TypeRef::Kind::Int && instruction.type.kind != TypeRef::Kind::UInt)
        return std::nullopt;
    const auto &homes = floating ? plan.FloatRegisters() : plan.PhysicalRegisters();
    const auto &types = plan.RegisterTypes();
    const auto destination = homes.find(instruction.dst);
    const auto left = homes.find(instruction.srcs[0]);
    const auto right = homes.find(instruction.srcs[1]);
    if (destination == homes.end() || left == homes.end() || right == homes.end() ||
        !types.contains(instruction.srcs[0]) || !types.contains(instruction.srcs[1]) ||
        types.at(instruction.srcs[0]) != instruction.type || types.at(instruction.srcs[1]) != instruction.type ||
        (destination->second == right->second && destination->second != left->second))
        return std::nullopt;
    constexpr std::array<int, 5> generalRegisters = {3, 12, 13, 14, 15};
    const auto reg = [&](const int home) {
        return floating ? home : generalRegisters.at(static_cast<std::size_t>(home));
    };
    X86ScalarMachineInstruction selected{
        floating ? (single ? X86ScalarRegisterClass::Float32 : X86ScalarRegisterClass::Float64)
                 : X86ScalarRegisterClass::Integer64,
        reg(destination->second),
        reg(left->second),
        reg(right->second),
        0,
        floating,
        floating,
        {}};
    switch (instruction.op) {
    case LirOpcode::Add:
        selected.opcode = floating ? 0x58 : 0x01;
        selected.mnemonic = floating ? (single ? "addss" : "addsd") : "add";
        break;
    case LirOpcode::Sub:
        selected.opcode = floating ? 0x5C : 0x29;
        selected.mnemonic = floating ? (single ? "subss" : "subsd") : "sub";
        break;
    case LirOpcode::Mul:
        selected.opcode = floating ? 0x59 : 0xAF;
        selected.mnemonic = floating ? (single ? "mulss" : "mulsd") : "imul";
        selected.twoByteOpcode = true;
        selected.destinationInRegField = true;
        break;
    case LirOpcode::Div:
        if (!floating)
            return std::nullopt;
        selected.opcode = 0x5E;
        selected.mnemonic = single ? "divss" : "divsd";
        break;
    case LirOpcode::And:
    case LirOpcode::Or:
    case LirOpcode::Xor:
        if (floating)
            return std::nullopt;
        selected.opcode = instruction.op == LirOpcode::And ? 0x21 : instruction.op == LirOpcode::Or ? 0x09 : 0x31;
        selected.mnemonic = instruction.op == LirOpcode::And ? "and" : instruction.op == LirOpcode::Or ? "or" : "xor";
        break;
    default:
        return std::nullopt;
    }
    return selected;
}

[[nodiscard]] inline std::string ScalarMachineRegister(const X86ScalarMachineInstruction &instruction, const int reg) {
    if (instruction.registerClass != X86ScalarRegisterClass::Integer64)
        return "xmm" + std::to_string(reg);
    return reg == 3 ? "rbx" : "r" + std::to_string(reg);
}
} // namespace Rux
