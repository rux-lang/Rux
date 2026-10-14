#pragma once

#include "BuildInfo/ArtifactKind.h"
#include "Ir/Lir/Lir.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Rux::Optimization {
/// The kinds of declaration reachability tracks. Anything a symbol reference can name and pruning can therefore remove.
enum class LirDeclarationKind {
    Function,
    Constant,
    Vtable,
    ExternVariable,
    Global,
};

inline constexpr std::size_t kLirDeclarationKinds = 5;

/// Where one declaration lives, as a pair of indices rather than a pointer, so an identifier stays valid while the pass
/// rewrites the package around it. The defaulted ordering is what lets a result set be sorted and searched.
struct LirDeclarationId {
    LirDeclarationKind kind = LirDeclarationKind::Function;
    std::size_t moduleIndex = 0;
    std::size_t declarationIndex = 0;

    auto operator<=>(const LirDeclarationId &) const = default;
};

/// A symbol a declaration names, and which declarations the name may stand for.
struct LirSymbolReference {
    enum class Scope : std::uint8_t {
        Function,
        Data,
        Any,
    };

    Scope scope = Scope::Any;
    std::string symbol;

    auto operator<=>(const LirSymbolReference &) const = default;
};

/// What reachability reads of one declaration: its name, whether a library artifact roots it, and what it references.
struct LirDeclarationReferences {
    std::string name;
    bool libraryRoot = false;
    std::vector<LirSymbolReference> references;

    auto operator<=>(const LirDeclarationReferences &) const = default;
};

/// Everything reachability reads of one module, by declaration kind and in declaration order. It stands in for a module
/// whose code a build does not lower again, so it is also what the build cache keeps of a module.
struct LirModuleReferences {
    std::array<std::vector<LirDeclarationReferences>, kLirDeclarationKinds> declarations;

    auto operator<=>(const LirModuleReferences &) const = default;
};

/// The references of `module` as it stands.
[[nodiscard]] LirModuleReferences CollectModuleReferences(const LirModule &module);

/// A declaration of a module that something outside the module needs: the artifact's roots, or a reference from a
/// declaration of another module that is itself needed.
struct LirModuleRoot {
    LirDeclarationKind kind = LirDeclarationKind::Function;
    std::string name;

    auto operator<=>(const LirModuleRoot &) const = default;
};

/// For every module, sorted, the declarations reachability from the artifact's roots enters it through. Pruning a
/// module from these alone, following only its own references, keeps exactly what pruning the whole program keeps of
/// it, so a module pruned this way depends on the rest of the program only through its roots.
[[nodiscard]] std::vector<std::vector<LirModuleRoot>> ModuleRoots(std::span<const LirModuleReferences> modules,
                                                                  ArtifactKind artifactKind);
[[nodiscard]] std::vector<std::vector<LirModuleRoot>> ModuleRoots(std::span<const LirModuleReferences *const> modules,
                                                                  ArtifactKind artifactKind);

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

    /// Every declaration of module `moduleIndex` reachable from `roots` (one sorted list per module, as `ModuleRoots`
    /// returns) through that module's own references. Declarations of other modules are never reachable.
    [[nodiscard]] static LirReachabilityResult RunModule(const LirPackage &package, std::size_t moduleIndex,
                                                         std::span<const LirModuleRoot> roots);
};
} // namespace Rux::Optimization
