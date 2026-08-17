#pragma once

#include "BuildInfo/ArtifactKind.h"
#include "Ir/Lir/Lir.h"

#include <compare>
#include <cstddef>
#include <vector>

namespace Rux::Optimization {
enum class LirDeclarationKind {
    Function,
    Constant,
    Vtable,
    ExternVariable,
};

struct LirDeclarationId {
    LirDeclarationKind kind = LirDeclarationKind::Function;
    std::size_t moduleIndex = 0;
    std::size_t declarationIndex = 0;

    auto operator<=>(const LirDeclarationId &) const = default;
};

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
    [[nodiscard]] static LirReachabilityResult Run(const LirPackage &package, ArtifactKind artifactKind);
};
} // namespace Rux::Optimization
