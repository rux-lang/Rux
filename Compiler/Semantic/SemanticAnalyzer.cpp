#include "Semantic/SemanticAnalyzer.h"

#include "Semantic/Analysis/AnalysisContext.h"
#include "Semantic/Conditional/ConditionalCompilation.h"

#include <utility>

namespace Rux {
// Sema public API
SemanticAnalyzer::SemanticAnalyzer(std::vector<Module *> userModules, std::vector<DepPackage> inputDeps,
                                   std::string inputPackageName, CompileTimeContext inputContext)
    : modules(std::move(userModules))
    , deps(std::move(inputDeps))
    , packageName(std::move(inputPackageName))
    , compileTimeContext(std::move(inputContext)) {
}

SemanticAnalyzer::SemanticAnalyzer(std::vector<Module *> userModules, std::vector<DepPackage> inputDeps,
                                   std::string inputPackageName, std::string inputTargetSystem)
    : SemanticAnalyzer(std::move(userModules), std::move(inputDeps), std::move(inputPackageName),
                       CompileTimeContext{}) {
    if (inputTargetSystem == "FreeBSD")
        compileTimeContext.target.os = Target::OS::FreeBSD;
    else if (inputTargetSystem == "Linux")
        compileTimeContext.target.os = Target::OS::Linux;
    else if (inputTargetSystem == "macOS" || inputTargetSystem == "MacOS")
        compileTimeContext.target.os = Target::OS::MacOS;
    else if (inputTargetSystem == "Windows")
        compileTimeContext.target.os = Target::OS::Windows;
    compileTimeContext.target.object_format = Target::GetObjectFormat(compileTimeContext.target.os);
}

SemanticModel SemanticAnalyzer::Analyze() {
    // Fold `when` first: the branches that were not taken are dropped here, so
    // nothing below ever sees — or type-checks — them. Each package resolves its
    // own conditionals against its own constants.
    const auto importedModules = [this](const std::string_view name) {
        std::vector<Module *> result;
        for (auto &dependency : deps) {
            if (dependency.name == name) {
                for (const auto &entry : dependency.modules) {
                    result.push_back(entry.module);
                }
            }
        }
        return result;
    };
    for (auto &dep : deps) {
        std::vector<Module *> depModules;
        depModules.reserve(dep.modules.size());
        for (const auto &entry : dep.modules) {
            depModules.push_back(entry.module);
        }
        ResolveConditionalCompilation(depModules, compileTimeContext, diags, importedModules);
    }
    ResolveConditionalCompilation(modules, compileTimeContext, diags, importedModules);

    std::vector<const Module *> constModules(modules.begin(), modules.end());
    SemanticFacts facts;
    SemanticDetail::AnalysisContext analyzer({constModules, deps, packageName, diags, symbols, compileTimeContext},
                                             facts);
    analyzer.Run();
    facts.effectiveVisibilities = analyzer.EffectiveVisibilities();
    std::vector<const Module *> orderedModules;
    for (const auto &dep : deps) {
        for (const auto &entry : dep.modules) {
            orderedModules.push_back(entry.module);
        }
    }
    orderedModules.insert(orderedModules.end(), modules.begin(), modules.end());
    return SemanticModel{std::move(diags), std::move(symbols), std::move(orderedModules), std::move(compileTimeContext),
                         std::move(facts)};
}
} // namespace Rux
