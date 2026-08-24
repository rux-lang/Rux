#pragma once

#include <string_view>

namespace Rux {
/// The by-value context that receives ownership of a move-only expression.
enum class ValueConsumptionKind {
    Initialization,
    Argument,
    Receiver,
    Return,
    Assignment,
    Aggregate,
    ConditionalArm,
    ExplicitMove,
};

/// Returns the stable diagnostic spelling for a by-value ownership-transfer context.
[[nodiscard]] std::string_view ValueConsumptionKindName(ValueConsumptionKind kind) noexcept;

/// Describes whether values of a resolved type may be copied and whether leaving one live requires drop glue.
struct TypeProperties {
    enum class Mobility {
        Copy,
        MoveOnly,
        Unresolved,
    };

    Mobility mobility = Mobility::Unresolved;
    bool droppable = false;

    [[nodiscard]] static constexpr TypeProperties Copy() noexcept {
        return {Mobility::Copy, false};
    }

    [[nodiscard]] static constexpr TypeProperties MoveOnly(const bool needsDrop = false) noexcept {
        return {Mobility::MoveOnly, needsDrop};
    }

    [[nodiscard]] static constexpr TypeProperties Unresolved() noexcept {
        return {};
    }

    [[nodiscard]] constexpr bool IsCopy() const noexcept {
        return mobility == Mobility::Copy;
    }

    [[nodiscard]] constexpr bool IsMoveOnly() const noexcept {
        return mobility == Mobility::MoveOnly;
    }

    [[nodiscard]] constexpr bool IsResolved() const noexcept {
        return mobility != Mobility::Unresolved;
    }

    [[nodiscard]] constexpr bool IsDroppable() const noexcept {
        return droppable;
    }
};
} // namespace Rux
