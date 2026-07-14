#pragma once

#include "Semantic/SemanticModel.h"

#include <string>
#include <vector>

namespace Rux {
// A dependency package: its name and parsed source modules.
// Symbols from dep packages are isolated until explicitly imported.
struct DepPackage {
    std::string name;

    struct ModuleEntry {
        std::string moduleName; // source identifier for diagnostics/bookkeeping
        Module *module;
    };

    std::vector<ModuleEntry> modules;
};

// Runs semantic analysis over a set of parsed modules.
// Modules should be passed in dependency order when possible, but the
// analyzer performs a global first pass so forward references within a
// package work.
// The modules are taken by non-const pointer because analysis begins by folding
// their `#if` chains (see Semantic/ConditionalCompilation.h), which rewrites the
// AST in place.
class SemanticAnalyzer {
public:
    explicit SemanticAnalyzer(std::vector<Module *> userModules, std::vector<DepPackage> inputDeps = {},
                              std::string inputPackageName = {}, CompileTimeContext inputContext = {});
    SemanticAnalyzer(std::vector<Module *> userModules, std::vector<DepPackage> inputDeps, std::string inputPackageName,
                     std::string inputTargetSystem);
    [[nodiscard]] SemanticModel Analyze();

private:
    std::vector<Module *> modules;
    std::vector<DepPackage> deps;
    std::string packageName;
    CompileTimeContext compileTimeContext;
    std::vector<SemanticDiagnostic> diags;
    std::vector<SemanticSymbol> symbols;
};
} // namespace Rux
