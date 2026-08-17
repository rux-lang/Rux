#include "Optimization/LirDeclarationPruner.h"

#include "Optimization/LirReachability.h"

#include <cstddef>
#include <utility>
#include <vector>

namespace Rux::Optimization {
namespace {
/// A rough size for one declaration, counted in IR nodes. Only ever compared against itself to report how much pruning
/// removed, so the exact weighting matters less than staying consistent between runs.
std::size_t EstimateNodes(const LirFunc &function) {
    std::size_t nodes = 1 + function.params.size() + function.asmBody.size();
    for (const auto &block : function.blocks) {
        nodes += 1 + block.instrs.size() + (block.term.has_value() ? 1U : 0U);
    }
    return nodes;
}

std::size_t EstimateNodes(const LirConstDecl &constant) {
    return 1 + constant.elements.size();
}

std::size_t EstimateNodes(const LirVtable &vtable) {
    return 1 + vtable.methods.size();
}

template <typename Declaration, typename IsReachable, typename OnPruned>
/// Keep the reachable declarations of one kind, reporting each removal so the caller can total what it cost. Rebuilding
/// the vector rather than erasing in place keeps the surviving declarations in their original order, which their
/// recorded indices depend on.
bool Prune(std::vector<Declaration> &declarations, IsReachable isReachable, OnPruned onPruned) {
    std::vector<Declaration> retained;
    retained.reserve(declarations.size());
    bool changed = false;
    for (std::size_t index = 0; index < declarations.size(); ++index) {
        if (isReachable(index)) {
            retained.push_back(std::move(declarations[index]));
        }
        else {
            onPruned(declarations[index]);
            changed = true;
        }
    }
    declarations = std::move(retained);
    return changed;
}
} // namespace

LirDeclarationPruner::LirDeclarationPruner(const ArtifactKind inputArtifactKind)
    : artifactKind(inputArtifactKind) {
}

PassChange LirDeclarationPruner::Run(LirPackage &package, const PassContext &context) {
    if (context.profile != BuildProfile::Release) {
        return PassChange::None;
    }

    const auto reachability = LirReachabilityAnalysis::Run(package, artifactKind);
    LirPruningStats ignoredStats;
    LirPruningStats &stats = context.lirPruning != nullptr ? *context.lirPruning : ignoredStats;
    bool changed = false;

    for (std::size_t moduleIndex = 0; moduleIndex < package.modules.size(); ++moduleIndex) {
        auto &module = package.modules[moduleIndex];
        changed = Prune(
                      module.funcs,
                      [&](const std::size_t index) {
                          return reachability.IsReachable({LirDeclarationKind::Function, moduleIndex, index});
                      },
                      [&](const LirFunc &function) {
                          if (function.isExtern) {
                              ++stats.externDeclarations;
                          }
                          else {
                              ++stats.functionDefinitions;
                          }
                          stats.estimatedIrNodes += EstimateNodes(function);
                      }) ||
                  changed;
        changed = Prune(
                      module.consts,
                      [&](const std::size_t index) {
                          return reachability.IsReachable({LirDeclarationKind::Constant, moduleIndex, index});
                      },
                      [&](const LirConstDecl &constant) {
                          ++stats.constants;
                          stats.estimatedIrNodes += EstimateNodes(constant);
                      }) ||
                  changed;
        changed = Prune(
                      module.vtables,
                      [&](const std::size_t index) {
                          return reachability.IsReachable({LirDeclarationKind::Vtable, moduleIndex, index});
                      },
                      [&](const LirVtable &vtable) {
                          ++stats.vtables;
                          stats.estimatedIrNodes += EstimateNodes(vtable);
                      }) ||
                  changed;
        changed = Prune(
                      module.externVars,
                      [&](const std::size_t index) {
                          return reachability.IsReachable({LirDeclarationKind::ExternVariable, moduleIndex, index});
                      },
                      [&](const LirExternVar &) {
                          ++stats.externDeclarations;
                          ++stats.estimatedIrNodes;
                      }) ||
                  changed;
    }
    return changed ? PassChange::Changed : PassChange::None;
}
} // namespace Rux::Optimization
