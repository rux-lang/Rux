#pragma once

#include "CodeGen/IntegerLiteral.h"
#include "CodeGen/X86_64/FramePlan.h"
#include "CodeGen/X86_64/Registers.h"

#include <array>
#include <bit>
#include <optional>
#include <string>
#include <string_view>

namespace Rux {
enum class X86ScalarRegisterClass {
    Integer32,
    Integer64,
    Float32,
    Float64
};

[[nodiscard]] inline bool ScalarMachineIsFloat(const X86ScalarRegisterClass registerClass) {
    return registerClass == X86ScalarRegisterClass::Float32 || registerClass == X86ScalarRegisterClass::Float64;
}

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
    std::optional<std::int32_t> immediate = std::nullopt;
};

[[nodiscard]] inline bool ScalarMachineIsZeroShift(const X86ScalarMachineInstruction &instruction) {
    return instruction.opcode == 0xC1 && instruction.immediate == 0;
}

[[nodiscard]] inline bool ScalarMachineNeedsCopy(const X86ScalarMachineInstruction &instruction) {
    // A zero-count operation still produces a normalized UInt32 value even if its physical home has stale upper bits.
    return (instruction.destination != instruction.left && !(instruction.immediate && instruction.opcode == 0x69)) ||
           (instruction.registerClass == X86ScalarRegisterClass::Integer32 && ScalarMachineIsZeroShift(instruction));
}

/// x86 sign-extends an imm32 in a 64-bit operation; a 32-bit operation consumes the low 32 bits.
[[nodiscard]] inline std::optional<std::int32_t> ScalarMachineImmediate(const LirInstr &instruction,
                                                                        const std::uint64_t value) {
    if (instruction.isVolatile || instruction.dst == LirNoReg || instruction.srcs.size() != 2)
        return std::nullopt;
    const auto kind = instruction.type.kind;
    if (kind != TypeRef::Kind::UInt32 && kind != TypeRef::Kind::Int64 && kind != TypeRef::Kind::UInt64 &&
        kind != TypeRef::Kind::Int && kind != TypeRef::Kind::UInt)
        return std::nullopt;
    switch (instruction.op) {
    case LirOpcode::Shl:
    case LirOpcode::Shr:
    case LirOpcode::Lshr:
        // Keep oversized-count behavior on the established path rather than introducing hardware width masking here.
        return value < (kind == TypeRef::Kind::UInt32 ? 32U : 64U) ? std::optional{static_cast<std::int32_t>(value)}
                                                                   : std::nullopt;
    case LirOpcode::Add:
    case LirOpcode::Sub:
    case LirOpcode::Mul:
    case LirOpcode::And:
    case LirOpcode::Or:
    case LirOpcode::Xor:
        break;
    default:
        return std::nullopt;
    }
    const auto immediate = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value));
    if (kind != TypeRef::Kind::UInt32 && value != static_cast<std::uint64_t>(static_cast<std::int64_t>(immediate)))
        return std::nullopt;
    return immediate;
}

/// Select only fully allocated, same-width scalar arithmetic. UInt32 operations naturally zero-extend in a 32-bit
/// destination; signed/narrow values requiring normalization, wide values, spills, fixed-register operations and
/// destructive right-operand overlaps continue through the established scratch-register emitter.
[[nodiscard]] inline std::optional<X86ScalarMachineInstruction>
SelectScalarMachineInstruction(const LirInstr &instruction, const X86_64FramePlan &plan) {
    if (instruction.op == LirOpcode::RotateRight && instruction.srcs.size() == 1 && !instruction.isVolatile &&
        (instruction.type.kind == TypeRef::Kind::UInt32 || instruction.type.kind == TypeRef::Kind::UInt64)) {
        const auto &homes = plan.PhysicalRegisters();
        const auto &types = plan.RegisterTypes();
        const bool word32 = instruction.type.kind == TypeRef::Kind::UInt32;
        const auto count = ParseIntegerLiteralBits(instruction.strArg);
        if (count && *count < (word32 ? 32U : 64U) && homes.contains(instruction.dst) &&
            homes.contains(instruction.srcs[0]) && types.contains(instruction.srcs[0]) &&
            types.at(instruction.srcs[0]) == instruction.type) {
            return X86ScalarMachineInstruction{
                word32 ? X86ScalarRegisterClass::Integer32 : X86ScalarRegisterClass::Integer64,
                X86AllocatedGprs.at(static_cast<std::size_t>(homes.at(instruction.dst))),
                X86AllocatedGprs.at(static_cast<std::size_t>(homes.at(instruction.srcs[0]))),
                1,
                0xC1,
                false,
                false,
                "ror",
                static_cast<std::int32_t>(*count)};
        }
    }
    if (instruction.op == LirOpcode::Sqrt && instruction.srcs.size() == 1 && !instruction.isVolatile &&
        (instruction.type.kind == TypeRef::Kind::Float32 || instruction.type.kind == TypeRef::Kind::Float64)) {
        const auto &homes = plan.FloatRegisters();
        const auto &types = plan.RegisterTypes();
        if (homes.contains(instruction.dst) && homes.contains(instruction.srcs[0]) &&
            types.contains(instruction.srcs[0]) && types.at(instruction.srcs[0]) == instruction.type) {
            const bool single = instruction.type.kind == TypeRef::Kind::Float32;
            const auto destination = homes.at(instruction.dst);
            return X86ScalarMachineInstruction{single ? X86ScalarRegisterClass::Float32
                                                      : X86ScalarRegisterClass::Float64,
                                               destination,
                                               destination,
                                               homes.at(instruction.srcs[0]),
                                               0x51,
                                               true,
                                               true,
                                               single ? "sqrtss" : "sqrtsd"};
        }
    }
    if (instruction.srcs.size() != 2 || instruction.dst == LirNoReg || instruction.isVolatile)
        return std::nullopt;
    const bool single = instruction.type.kind == TypeRef::Kind::Float32;
    const bool floating = single || instruction.type.kind == TypeRef::Kind::Float64;
    const bool word32 = instruction.type.kind == TypeRef::Kind::UInt32;
    if (!floating && !word32 && instruction.type.kind != TypeRef::Kind::Int64 &&
        instruction.type.kind != TypeRef::Kind::UInt64 && instruction.type.kind != TypeRef::Kind::Int &&
        instruction.type.kind != TypeRef::Kind::UInt)
        return std::nullopt;
    const auto &homes = floating ? plan.FloatRegisters() : plan.PhysicalRegisters();
    const auto &types = plan.RegisterTypes();
    const auto destination = homes.find(instruction.dst);
    const auto left = homes.find(instruction.srcs[0]);
    const auto right = homes.find(instruction.srcs[1]);
    std::optional<std::int32_t> immediate;
    if (const auto found = plan.ImmediateValues().find(instruction.srcs[1]); found != plan.ImmediateValues().end())
        immediate = ScalarMachineImmediate(instruction, found->second);
    const bool immediateShift = immediate && (instruction.op == LirOpcode::Shl || instruction.op == LirOpcode::Shr ||
                                              instruction.op == LirOpcode::Lshr);
    if (destination == homes.end() || left == homes.end() || (!immediate && right == homes.end()) ||
        !types.contains(instruction.srcs[0]) || !types.contains(instruction.srcs[1]) ||
        types.at(instruction.srcs[0]) != instruction.type ||
        (immediateShift
             ? !types.at(instruction.srcs[1]).IsInteger() || Layout::SizeOf(types.at(instruction.srcs[1])) > 8
             : types.at(instruction.srcs[1]) != instruction.type) ||
        (!immediate && destination->second == right->second && destination->second != left->second))
        return std::nullopt;
    const auto reg = [&](const int home) {
        return floating ? home : X86AllocatedGprs.at(static_cast<std::size_t>(home));
    };
    X86ScalarMachineInstruction selected{
        floating ? (single ? X86ScalarRegisterClass::Float32 : X86ScalarRegisterClass::Float64)
        : word32 ? X86ScalarRegisterClass::Integer32
                 : X86ScalarRegisterClass::Integer64,
        reg(destination->second),
        reg(left->second),
        immediate ? 0 : reg(right->second),
        0,
        floating,
        floating,
        {}};
    switch (instruction.op) {
    case LirOpcode::Shl:
    case LirOpcode::Shr:
    case LirOpcode::Lshr:
        if (floating || !immediate)
            return std::nullopt;
        selected.opcode = 0xC1;
        selected.immediate = immediate;
        selected.right = instruction.op == LirOpcode::Shl                                ? 4
                       : instruction.op == LirOpcode::Shr && instruction.type.IsSigned() ? 7
                                                                                         : 5;
        selected.mnemonic = selected.right == 4 ? "shl" : selected.right == 7 ? "sar" : "shr";
        return selected;
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
    if (immediate) {
        selected.immediate = immediate;
        selected.opcode = instruction.op == LirOpcode::Mul ? 0x69 : 0x81;
        selected.twoByteOpcode = false;
        selected.destinationInRegField = instruction.op == LirOpcode::Mul;
        // For group-1 immediates, right holds the opcode extension rather than a physical register.
        switch (instruction.op) {
        case LirOpcode::Or:
            selected.right = 1;
            break;
        case LirOpcode::And:
            selected.right = 4;
            break;
        case LirOpcode::Sub:
            selected.right = 5;
            break;
        case LirOpcode::Xor:
            selected.right = 6;
            break;
        default:
            selected.right = 0;
            break;
        }
    }
    return selected;
}

[[nodiscard]] inline std::string ScalarMachineRegister(const X86ScalarMachineInstruction &instruction, const int reg) {
    if (ScalarMachineIsFloat(instruction.registerClass))
        return "xmm" + std::to_string(reg);
    return std::string(X86GeneralRegisterName(reg, instruction.registerClass == X86ScalarRegisterClass::Integer32));
}
} // namespace Rux
