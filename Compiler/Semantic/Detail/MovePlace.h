#pragma once

#include "Syntax/Ast/Ast.h"

#include <string>
#include <vector>

namespace Rux::SemanticDetail {
/// Structural description of storage read by a potentially consuming expression. It is deliberately narrower than a
/// borrow-checker place model: L08 needs only named roots, pointer dereferences, and field/index projections.
struct MovePlace {
    enum class RootKind {
        Named,
        Self,
        Dereference,
        Temporary,
    };

    struct Projection {
        enum class Kind {
            Field,
            Index,
        };

        Kind kind = Kind::Field;
        std::string value;
        SourceLocation location;

        [[nodiscard]] std::string Display() const;
        [[nodiscard]] std::string Description() const;
    };

    RootKind rootKind = RootKind::Temporary;
    std::string rootName;
    const Expr *rootExpression = nullptr;
    std::vector<Projection> projections;

    [[nodiscard]] bool IsNamedStorage() const noexcept;
    [[nodiscard]] bool IsBorrowedStorage() const noexcept;
    [[nodiscard]] bool IsPartial() const noexcept;
    [[nodiscard]] bool IsComplete() const noexcept;
    [[nodiscard]] bool HasKnownIdentity() const noexcept;
    [[nodiscard]] std::string RootDisplay() const;
    [[nodiscard]] std::string Display() const;
    [[nodiscard]] std::string ContainerDisplay() const;
    [[nodiscard]] std::string LastProjectionDescription() const;
};

/// Decompose an expression without mutating semantic state. Expressions that compute a fresh value remain Temporary.
[[nodiscard]] MovePlace AnalyzeMovePlace(const Expr &expression);

/// True only when both expressions name the same complete storage path. Unknown computed indices are intentionally
/// unequal, avoiding a false self-move diagnostic when aliasing cannot be proven without data-flow analysis.
[[nodiscard]] bool SameStoragePlace(const Expr &left, const Expr &right);
} // namespace Rux::SemanticDetail
