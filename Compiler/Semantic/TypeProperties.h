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
    ArrayRepeat,
    ConditionalArm,
    ExplicitMove,
};

/// Returns the stable diagnostic spelling for a by-value ownership-transfer context.
[[nodiscard]] std::string_view ValueConsumptionKindName(ValueConsumptionKind kind) noexcept;

/// Describes whether values of a resolved type may be copied and whether leaving one live requires drop glue.
struct TypeProperties {
    enum class SpecialOperationState {
        Generated,
        Custom,
        Prohibited,
        Unresolved,
    };

    enum class Mobility {
        Copy,
        MoveOnly,
        Unresolved,
    };

    Mobility mobility = Mobility::Unresolved;
    bool droppable = false;
    SpecialOperationState copyOperation = SpecialOperationState::Unresolved;
    SpecialOperationState moveOperation = SpecialOperationState::Unresolved;

    [[nodiscard]] static constexpr TypeProperties Copy() noexcept {
        return {Mobility::Copy, false, SpecialOperationState::Generated, SpecialOperationState::Generated};
    }

    [[nodiscard]] static constexpr TypeProperties MoveOnly(const bool needsDrop = false) noexcept {
        return {Mobility::MoveOnly, needsDrop, SpecialOperationState::Prohibited, SpecialOperationState::Generated};
    }

    [[nodiscard]] static constexpr TypeProperties FromOperations(const SpecialOperationState copy,
                                                                 const SpecialOperationState move,
                                                                 const bool needsDrop = false) noexcept {
        const Mobility valueMobility =
            copy == SpecialOperationState::Unresolved || move == SpecialOperationState::Unresolved
                ? Mobility::Unresolved
            : copy == SpecialOperationState::Prohibited ? Mobility::MoveOnly
                                                        : Mobility::Copy;
        return {valueMobility, needsDrop, copy, move};
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

    [[nodiscard]] constexpr bool IsMovable() const noexcept {
        return moveOperation == SpecialOperationState::Generated || moveOperation == SpecialOperationState::Custom;
    }

    [[nodiscard]] constexpr bool IsMoveProhibited() const noexcept {
        return moveOperation == SpecialOperationState::Prohibited;
    }

    [[nodiscard]] constexpr bool IsDroppable() const noexcept {
        return droppable;
    }
};
} // namespace Rux
