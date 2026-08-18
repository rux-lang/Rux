#pragma once

namespace Rux {
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
