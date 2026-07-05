#pragma once

#include <string>
#include <vector>

#include "Semantic/SemanticModel.h"

namespace Rux {
// A dependency package: its name and parsed source modules.
// Symbols from dep packages are isolated until explicitly imported.
struct DepPackage {
    std::string name;

    struct ModuleEntry {
        std::string moduleName; // source identifier for diagnostics/bookkeeping
        const Module *module;
    };

    std::vector<ModuleEntry> modules;
};

// Runs semantic analysis over a set of parsed modules.
// Modules should be passed in dependency order when possible, but the
// analyzer performs a global first pass so forward references within a
// package work.
class SemanticAnalyzer {
public:
    explicit SemanticAnalyzer(std::vector<const Module *> userModules, std::vector<DepPackage> inputDeps = {},
                              std::string inputPackageName = {}, std::string inputTargetOs = {});
    [[nodiscard]] SemanticModel Analyze();

private:
    std::vector<const Module *> modules;
    std::vector<DepPackage> deps;
    std::string packageName;
    std::string targetOs;
    std::vector<SemanticDiagnostic> diags;
    std::vector<SemanticSymbol> symbols;
};
} // namespace Rux
