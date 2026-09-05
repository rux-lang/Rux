#pragma once

#include "Syntax/Ast/Ast.h"

#include <functional>
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

/// Whether one index expression calls a declared `[]` instead of projecting into its object. Decomposition is purely
/// structural and cannot tell the two apart, so analysis -- the stage that resolved the operator -- supplies the
/// answer. An empty function decomposes every index as a projection, which is what a caller with no resolved
/// operators in hand wants.
using IndexIsOperatorCall = std::function<bool(const IndexExpr &)>;

/// Decompose an expression without mutating semantic state. Expressions that compute a fresh value remain Temporary.
[[nodiscard]] MovePlace AnalyzeMovePlace(const Expr &expression, const IndexIsOperatorCall &isOperatorCall = {});

/// True only when both expressions name the same complete storage path. Unknown computed indices are intentionally
/// unequal, avoiding a false self-move diagnostic when aliasing cannot be proven without data-flow analysis.
[[nodiscard]] bool SameStoragePlace(const Expr &left, const Expr &right,
                                    const IndexIsOperatorCall &isOperatorCall = {});
} // namespace Rux::SemanticDetail
