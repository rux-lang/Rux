#pragma once

#include "Semantic/Model/SemanticModel.h"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace Rux {
/// Import aliases indexed by declaring package identity, then alias. An absent owner supports standalone semantic
/// callers whose dependency names already serve as identities; a present owner exposes only its declared bindings.
using PackageImportBindings = std::map<std::string, std::map<std::string, std::string>>;
[[nodiscard]] std::string ResolvePackageImport(const PackageImportBindings &bindings, const std::string &owner,
                                               std::string_view alias);

/// A dependency package: its resolved identity and parsed source modules. Symbols remain isolated until imported.
struct DepPackage {
    std::string name; ///< opaque package identity supplied by the dependency resolver

    struct ModuleEntry {
        std::string moduleName; ///< source identifier for diagnostics/bookkeeping
        Module *module;
    };

    std::vector<ModuleEntry> modules;
};

/// Locate a declaration source's owner, including when a conditional evaluator follows an imported alias or constant.
[[nodiscard]] const std::string &PackageOwningSource(const std::vector<DepPackage> &deps, const std::string &rootId,
                                                     std::string_view source);

/**
 * @brief Runs semantic analysis over a set of parsed modules.
 *
 * Modules should be passed in dependency order when possible, but the analyzer performs a global first pass so forward
 * references within a package work.
 *
 * The modules are taken by non-const pointer because analysis begins by folding their `#if` chains (see
 * Semantic/Conditional/ConditionalCompilation.h), which rewrites the AST in place.
 */
class SemanticAnalyzer {
public:
    explicit SemanticAnalyzer(std::vector<Module *> userModules, std::vector<DepPackage> inputDeps = {},
                              std::string inputPackageName = {}, CompileTimeContext inputContext = {},
                              PackageImportBindings inputImports = {});

    /// Overload for callers that know only the target system name, leaving the rest of the compile-time context at its
    /// defaults.
    SemanticAnalyzer(std::vector<Module *> userModules, std::vector<DepPackage> inputDeps, std::string inputPackageName,
                     std::string inputTargetSystem);

    /// Analyze every module and return the resolved model. The model borrows from the AST, so those modules must
    /// outlive it.
    [[nodiscard]] SemanticModel Analyze();

private:
    std::vector<Module *> modules;
    std::vector<DepPackage> deps;
    std::string packageName;
    CompileTimeContext compileTimeContext;
    PackageImportBindings imports;
    std::vector<SemanticDiagnostic> diags;
    std::vector<SemanticSymbol> symbols;
};
} // namespace Rux
