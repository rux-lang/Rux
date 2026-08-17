#pragma once

#include "BuildInfo/ArtifactKind.h"
#include "Ir/Lir/Lir.h"

#include <compare>
#include <cstddef>
#include <vector>

namespace Rux::Optimization {
/// The kinds of declaration reachability tracks. Anything a symbol reference can name and pruning can therefore remove.
enum class LirDeclarationKind {
    Function,
    Constant,
    Vtable,
    ExternVariable,
};

/// Where one declaration lives, as a pair of indices rather than a pointer, so an identifier stays valid while the pass
/// rewrites the package around it. The defaulted ordering is what lets a result set be sorted and searched.
struct LirDeclarationId {
    LirDeclarationKind kind = LirDeclarationKind::Function;
    std::size_t moduleIndex = 0;
    std::size_t declarationIndex = 0;

    auto operator<=>(const LirDeclarationId &) const = default;
};

/// The declarations the analysis proved reachable, held sorted so membership is a binary search: pruning asks this
/// question once per declaration in the package.
class LirReachabilityResult {
public:
    [[nodiscard]] bool IsReachable(LirDeclarationId declaration) const;
    [[nodiscard]] const std::vector<LirDeclarationId> &ReachableDeclarations() const noexcept;

private:
    friend class LirReachabilityAnalysis;
    std::vector<LirDeclarationId> reachableDeclarations;
};

class LirReachabilityAnalysis {
public:
    /**
     * @brief Find every declaration reachable from the artifact's roots.
     *
     * A transitive walk from the root set, following calls, global addresses, named loads, and the symbols named in
     * inline assembly operands — that last one matters, because a body the compiler does not otherwise read can still
     * be the only reference keeping a symbol alive.
     *
     * An unresolved external declaration stays reachable with no body to walk, so the reference that needs it survives
     * to the linker.
     *
     * @param artifactKind Decides the roots: an executable starts at `Main`, a library at every public declaration
     */
    [[nodiscard]] static LirReachabilityResult Run(const LirPackage &package, ArtifactKind artifactKind);
};
} // namespace Rux::Optimization
