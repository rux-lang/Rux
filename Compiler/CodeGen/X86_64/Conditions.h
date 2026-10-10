#pragma once

#include "Ir/Lir/Lir.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace Rux {
/// Architectural condition nibble, shared by binary branches and their textual representation.
enum class X86Condition : std::uint8_t {
    Below = 2,
    AboveEqual = 3,
    Equal = 4,
    NotEqual = 5,
    BelowEqual = 6,
    Above = 7,
    Less = 12,
    GreaterEqual = 13,
    LessEqual = 14,
    Greater = 15
};

[[nodiscard]] constexpr X86Condition Inverse(const X86Condition condition) {
    return static_cast<X86Condition>(static_cast<std::uint8_t>(condition) ^ 1U);
}

[[nodiscard]] constexpr std::string_view ConditionMnemonic(const X86Condition condition) {
    switch (condition) {
    case X86Condition::Below:
        return "jb";
    case X86Condition::AboveEqual:
        return "jae";
    case X86Condition::Equal:
        return "je";
    case X86Condition::NotEqual:
        return "jne";
    case X86Condition::BelowEqual:
        return "jbe";
    case X86Condition::Above:
        return "ja";
    case X86Condition::Less:
        return "jl";
    case X86Condition::GreaterEqual:
        return "jge";
    case X86Condition::LessEqual:
        return "jle";
    case X86Condition::Greater:
        return "jg";
    }
    return {};
}

[[nodiscard]] constexpr std::optional<X86Condition> IntegerComparisonCondition(const LirOpcode operation,
                                                                               const bool isSigned) {
    switch (operation) {
    case LirOpcode::CmpEq:
        return X86Condition::Equal;
    case LirOpcode::CmpNe:
        return X86Condition::NotEqual;
    case LirOpcode::CmpLt:
        return isSigned ? X86Condition::Less : X86Condition::Below;
    case LirOpcode::CmpLe:
        return isSigned ? X86Condition::LessEqual : X86Condition::BelowEqual;
    case LirOpcode::CmpGt:
        return isSigned ? X86Condition::Greater : X86Condition::Above;
    case LirOpcode::CmpGe:
        return isSigned ? X86Condition::GreaterEqual : X86Condition::AboveEqual;
    default:
        return std::nullopt;
    }
}
} // namespace Rux
