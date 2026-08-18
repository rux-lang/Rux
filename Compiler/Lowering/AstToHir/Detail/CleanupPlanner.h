#pragma once

#include "Ir/Hir/Hir.h"
#include "Semantic/SemanticModel.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Rux::AstToHirDetail {
/// Owns the lexical cleanup stack while one AST package is lowered. Scope frames mirror HIR name scopes, while a
/// separate function boundary keeps recursively lowered generic functions from capturing their caller's bindings.
class CleanupPlanner {
public:
    struct FunctionToken {
        std::optional<std::size_t> previousBase;
        std::optional<std::size_t> previousLoopBase;
    };

    struct LoopToken {
        std::size_t index = 0;
    };

    explicit CleanupPlanner(const SemanticModel &semanticModel);

    void PushScope();
    void PopScope();

    /// Begin a function after its name scope has been pushed. The returned token restores a surrounding function when
    /// monomorphization recursively lowers a new body while the caller's body is still active.
    [[nodiscard]] FunctionToken BeginFunction();
    void EndFunction(FunctionToken token);
    [[nodiscard]] LoopToken BeginLoop(std::string label);
    void EndLoop(LoopToken token);

    /// Register an initialized or potentially initialized named binding. Copy and unresolved values return zero and
    /// never enter the cleanup stack because the semantic model has no destruction recipe for them.
    [[nodiscard]] std::uint64_t Register(const std::string &name, const TypeRef &type, SourceLocation origin);

    /// Return only the innermost frame in reverse declaration order for ordinary lexical fallthrough.
    [[nodiscard]] std::vector<HirDropAction> CurrentScopeActions() const;

    /// Return every frame owned by the active function, innermost first and each in reverse declaration order.
    [[nodiscard]] std::vector<HirDropAction> FunctionExitActions() const;
    [[nodiscard]] std::vector<HirDropAction> LoopExitActions(const std::string &label) const;
    [[nodiscard]] std::optional<HirDropAction> ActionFor(std::uint64_t bindingId) const;

    /// Introspection used by lowering invariants and focused ownership tests without exposing mutable planner state.
    [[nodiscard]] bool HasActiveFunction() const noexcept;
    [[nodiscard]] std::size_t ScopeDepth() const noexcept;

private:
    const SemanticModel &model;
    /// Frames preserve declaration order; action queries reverse them without mutating the canonical schedule.
    std::vector<std::vector<HirDropAction>> scopes;
    /// Index of the active function's parameter frame, excluding every surrounding recursively lowered function.
    std::optional<std::size_t> functionBase;

    struct LoopBoundary {
        std::string label;
        std::size_t scopeDepth = 0;
    };

    std::vector<LoopBoundary> loops;
    std::optional<std::size_t> loopBase;
    /// Zero stays reserved for Copy values and consumed temporaries that do not own named storage.
    std::uint64_t nextBindingId = 1;

    [[nodiscard]] std::vector<HirDropAction> ReverseFrame(std::size_t index) const;
    void AppendReverseFrame(std::vector<HirDropAction> &actions, std::size_t index) const;
    [[nodiscard]] bool InvariantsHold() const;
};
} // namespace Rux::AstToHirDetail
