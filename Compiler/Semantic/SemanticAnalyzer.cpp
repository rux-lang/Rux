#include "Semantic/SemanticAnalyzer.h"

#include "Semantic/Analysis/AnalysisContext.h"
#include "Semantic/Conditional/ConditionalCompilation.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <utility>

namespace Rux {
std::string ResolvePackageImport(const PackageImportBindings &bindings, const std::string &owner,
                                 const std::string_view alias) {
    const auto package = bindings.find(owner);
    if (package == bindings.end())
        return std::string(alias);
    const auto found = package->second.find(std::string(alias));
    return found == package->second.end() ? std::string{} : found->second;
}

const std::string &PackageOwningSource(const std::vector<DepPackage> &deps, const std::string &rootId,
                                       const std::string_view source) {
    for (const auto &dependency : deps) {
        for (const auto &entry : dependency.modules) {
            if (entry.module->name == source)
                return dependency.name;
        }
    }
    return rootId;
}

// Sema public API
SemanticAnalyzer::SemanticAnalyzer(std::vector<Module *> userModules, std::vector<DepPackage> inputDeps,
                                   std::string inputPackageName, CompileTimeContext inputContext,
                                   PackageImportBindings inputImports)
    : modules(std::move(userModules))
    , deps(std::move(inputDeps))
    , packageName(std::move(inputPackageName))
    , compileTimeContext(std::move(inputContext))
    , imports(std::move(inputImports)) {
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

namespace {
/// A rough measure of the work checking a module's bodies takes: its declarations and the statements of every body.
std::size_t ModuleWeight(const Module &module) {
    std::size_t weight = 0;
    const auto declaration = [&](this const auto &self, const Decl &item) -> void {
        ++weight;
        if (const auto *function = AstCast<const FuncDecl>(&item)) {
            if (function->body)
                weight += function->body->stmts.size();
        }
        else if (const auto *extension = AstCast<const ImplDecl>(&item)) {
            for (const auto &method : extension->methods)
                if (method && method->body)
                    weight += method->body->stmts.size();
        }
        else if (const auto *nested = AstCast<const ModuleDecl>(&item)) {
            for (const auto &child : nested->items)
                if (child)
                    self(*child);
        }
    };
    for (const auto &item : module.items)
        if (item)
            declaration(*item);
    return weight;
}
} // namespace

bool SemanticAnalyzer::AnalyzeInParallel(std::vector<const Module *> &constModules, SemanticFacts &facts) {
    if (pool == nullptr || pool->Jobs() < 2)
        return false;
    // Modules are checked in one order: every dependency's, then the package's own.
    std::vector<const Module *> ordered;
    for (const auto &dependency : deps)
        for (const auto &entry : dependency.modules)
            ordered.push_back(entry.module);
    const std::size_t dependencyCount = ordered.size();
    ordered.insert(ordered.end(), constModules.begin(), constModules.end());
    // A declaration inside a body joins the package's indexes when it is checked, which later modules would see.
    if (std::ranges::any_of(ordered, [](const Module *module) { return module->bodiesDeclareItems; }))
        return false;

    // Contiguous ranges of about equal work, one per context.
    std::vector<std::size_t> weights;
    std::size_t total = 0;
    std::size_t reusedWeight = 0;
    for (const Module *module : ordered) {
        const std::size_t weight = ModuleWeight(*module) + 1;
        const bool reused = declarationOnlyModules.contains(module);
        reusedWeight += reused ? weight : 0;
        weights.push_back(reused ? 1 : weight);
        total += weights.back();
    }
    // Every context repeats the Begin phases, which cost about as much as checking a few modules' bodies: when most
    // modules are reused and only their declarations are checked, one context is faster than several.
    if (total * 4 < reusedWeight)
        return false;
    // One context per thread: the finisher, which checks no body, and one per range of modules. Every context repeats
    // the Begin phases, so past a few ranges that repeated work costs more than splitting bodies further saves.
    constexpr std::size_t maxRanges = 6;
    const std::size_t contexts = std::min({maxRanges, pool->Jobs() - 1, ordered.size()});
    if (contexts < 2)
        return false;
    std::vector<std::size_t> bounds{0};
    std::size_t accumulated = 0;
    for (std::size_t index = 0; index + 1 < ordered.size() && bounds.size() < contexts; ++index) {
        accumulated += weights[index];
        if (accumulated * contexts >= total * bounds.size())
            bounds.push_back(index + 1);
    }
    bounds.push_back(ordered.size());
    const std::size_t ranges = bounds.size() - 1;

    struct Replica {
        std::vector<SemanticDiagnostic> diagnostics;
        std::vector<SemanticSymbol> symbols;
        SemanticFacts facts;
        std::unique_ptr<SemanticDetail::AnalysisContext> context;
    };

    // The first context checks no body: once its Begin phases are done, the others take their own records out of
    // themselves against it, and it merges them and finishes. It is claimed first, so a worker waiting on it never
    // waits on a task no thread will run.
    std::vector<Replica> replicas(ranges + 1);
    std::vector<SemanticDetail::AnalysisContext::BodyRecords> records(ranges);
    std::mutex mutex;
    std::condition_variable condition;
    bool finisherReady = false;
    std::atomic<bool> failed = false;
    pool->ForEach(replicas.size(), [&](const std::size_t index) {
        auto &replica = replicas[index];
        replica.context = std::make_unique<SemanticDetail::AnalysisContext>(
            SemanticDetail::AnalysisInputs{constModules, deps, packageName, replica.diagnostics, replica.symbols,
                                           compileTimeContext, imports, &declarationOnlyModules},
            replica.facts);
        auto &context = *replica.context;
        const std::size_t begin = index == 0 ? 0 : bounds[index - 1];
        const std::size_t end = index == 0 ? 0 : bounds[index];
        context.BeginDependencies();
        context.CheckDependencyModules(std::min(begin, dependencyCount), std::min(end, dependencyCount));
        context.BeginUserModules();
        if (index == 0) {
            {
                const std::scoped_lock lock(mutex);
                finisherReady = true;
            }
            condition.notify_all();
            if (!replica.diagnostics.empty())
                failed = true;
            return;
        }
        context.CheckUserModules(std::max(begin, dependencyCount) - dependencyCount,
                                 std::max(end, dependencyCount) - dependencyCount);
        if (!replica.diagnostics.empty())
            failed = true;
        {
            std::unique_lock lock(mutex);
            condition.wait(lock, [&] { return finisherReady; });
        }
        if (!failed)
            records[index - 1] = context.ExtractBodyRecords(*replicas.front().context);
    });
    // The records may point into the contexts that made them, which are released together once nothing reads them.
    const auto release = [&] {
        pool->ForEach(replicas.size(), [&](const std::size_t index) {
            replicas[index].context.reset();
            replicas[index].facts = SemanticFacts{};
        });
    };
    if (failed) {
        release();
        return false;
    }

    auto &finisher = replicas.front();
    finisher.context->SetWorkerPool(pool);
    finisher.context->AbsorbBodyRecords(records, pool);
    records.clear();
    finisher.context->Finish();
    const bool succeeded = finisher.diagnostics.empty();
    if (succeeded) {
        finisher.facts.effectiveVisibilities = finisher.context->EffectiveVisibilities();
        finisher.facts.exportedDeclarations = finisher.context->ExportedDeclarations();
        facts = std::move(finisher.facts);
        symbols = std::move(finisher.symbols);
    }
    release();
    return succeeded;
}

SemanticModel SemanticAnalyzer::Analyze() {
    // Fold `when` first: the branches that were not taken are dropped here, so
    // nothing below ever sees — or type-checks — them. Each package resolves its
    // own conditionals against its own constants.
    const auto importedModules = [this](const std::string_view alias, const std::string_view source) {
        const auto name = ResolvePackageImport(imports, PackageOwningSource(deps, packageName, source), alias);
        if (name == packageName)
            return modules;
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
    if (!conditionalsResolved) {
        for (auto &dep : deps) {
            std::vector<Module *> depModules;
            depModules.reserve(dep.modules.size());
            for (const auto &entry : dep.modules) {
                depModules.push_back(entry.module);
            }
            ResolveConditionalCompilation(depModules, compileTimeContext, diags, importedModules);
        }
        ResolveConditionalCompilation(modules, compileTimeContext, diags, importedModules);
    }

    std::vector<const Module *> constModules(modules.begin(), modules.end());
    SemanticFacts facts;
    // Folding conditionals may already have reported something, which a fallback below keeps.
    const std::size_t foldingDiagnostics = diags.size();
    if (!AnalyzeInParallel(constModules, facts)) {
        diags.erase(diags.begin() + static_cast<std::ptrdiff_t>(foldingDiagnostics), diags.end());
        symbols.clear();
        facts = SemanticFacts{};
        SemanticDetail::AnalysisContext analyzer(
            {constModules, deps, packageName, diags, symbols, compileTimeContext, imports, &declarationOnlyModules},
            facts);
        analyzer.SetWorkerPool(pool);
        analyzer.Run();
        facts.effectiveVisibilities = analyzer.EffectiveVisibilities();
        facts.exportedDeclarations = analyzer.ExportedDeclarations();
    }
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
