#include "Lowering/AstToHir/Detail/CleanupPlanner.h"

#include <cassert>
#include <limits>
#include <unordered_set>
#include <utility>

namespace Rux::AstToHirDetail {
CleanupPlanner::CleanupPlanner(const SemanticModel &semanticModel)
    : model(semanticModel) {
}

void CleanupPlanner::PushScope() {
    scopes.emplace_back();
    assert(InvariantsHold());
}

void CleanupPlanner::PopScope() {
    assert(!scopes.empty() && "cannot pop an absent cleanup scope");
    assert((!functionBase || scopes.size() - 1 != *functionBase) &&
           "end the cleanup function before popping its parameter scope");
    scopes.pop_back();
    assert(InvariantsHold());
}

CleanupPlanner::FunctionToken CleanupPlanner::BeginFunction() {
    assert(!scopes.empty() && "a cleanup function requires a parameter scope");
    FunctionToken token{functionBase};
    functionBase = scopes.size() - 1;
    assert(InvariantsHold());
    return token;
}

void CleanupPlanner::EndFunction(const FunctionToken token) {
    assert(functionBase && *functionBase < scopes.size() && "cleanup function boundary is not active");
    functionBase = token.previousBase;
    assert(InvariantsHold());
}

std::uint64_t CleanupPlanner::Register(const std::string &name, const TypeRef &type, SourceLocation origin) {
    if (!functionBase || scopes.empty()) {
        return 0;
    }
    const DropGluePlan *glue = model.TryGetDropGlue(type);
    if (!glue) {
        return 0;
    }
    assert(nextBindingId != std::numeric_limits<std::uint64_t>::max() && "cleanup binding identity overflow");
    const std::uint64_t bindingId = nextBindingId++;
    scopes.back().push_back(HirDropAction{bindingId, name, type, glue->symbol, std::move(origin)});
    assert(InvariantsHold());
    return bindingId;
}

void CleanupPlanner::AppendReverseFrame(std::vector<HirDropAction> &actions, const std::size_t index) const {
    assert(index < scopes.size());
    const auto &frame = scopes[index];
    actions.insert(actions.end(), frame.rbegin(), frame.rend());
}

std::vector<HirDropAction> CleanupPlanner::ReverseFrame(const std::size_t index) const {
    assert(index < scopes.size());
    std::vector<HirDropAction> actions;
    actions.reserve(scopes[index].size());
    AppendReverseFrame(actions, index);
    return actions;
}

std::vector<HirDropAction> CleanupPlanner::CurrentScopeActions() const {
    if (!functionBase || scopes.empty()) {
        return {};
    }
    return ReverseFrame(scopes.size() - 1);
}

std::vector<HirDropAction> CleanupPlanner::FunctionExitActions() const {
    std::vector<HirDropAction> actions;
    if (!functionBase) {
        return actions;
    }

    std::size_t actionCount = 0;
    for (std::size_t index = *functionBase; index < scopes.size(); ++index) {
        actionCount += scopes[index].size();
    }
    actions.reserve(actionCount);

    for (std::size_t offset = 0; offset < scopes.size() - *functionBase; ++offset) {
        AppendReverseFrame(actions, scopes.size() - offset - 1);
    }
    return actions;
}

bool CleanupPlanner::HasActiveFunction() const noexcept {
    return functionBase.has_value();
}

std::size_t CleanupPlanner::ScopeDepth() const noexcept {
    return scopes.size();
}

bool CleanupPlanner::InvariantsHold() const {
    if (functionBase && *functionBase >= scopes.size()) {
        return false;
    }
    std::unordered_set<std::uint64_t> identities;
    for (const auto &scope : scopes) {
        for (const HirDropAction &action : scope) {
            if (action.bindingId == 0 || action.bindingId >= nextBindingId || action.name.empty() ||
                action.glueSymbol.empty() || !identities.insert(action.bindingId).second) {
                return false;
            }
        }
    }
    return true;
}
} // namespace Rux::AstToHirDetail
