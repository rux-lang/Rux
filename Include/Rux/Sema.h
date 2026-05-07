/*
    Rux Compiler
    Copyright © 2026 Ivan Muzyka
    Licensed under the MIT License
*/

#pragma once

#include "Rux/Ast.h"

#include <filesystem>
#include <string>
#include <vector>

namespace Rux {
    struct SemaDiagnostic {
        enum class Severity { Warning, Error };

        Severity severity = Severity::Error;
        std::string sourceName; // source file path for multi-file diagnostics
        SourceLocation location;
        std::string message;
    };

    // A globally-scoped symbol collected during the first analysis pass.
    struct SemaSymbol {
        enum class Kind { Var, Func, Type, Const, Module, Interface };

        Kind kind = Kind::Var;
        std::string name;
        std::string sourceName;
        SourceLocation location;
        std::string resolvedType; // TypeRef::ToString(), empty for opaque types
        bool isMut = false; // meaningful for Var
    };

    struct SemaResult {
        std::vector<SemaDiagnostic> diagnostics;
        std::vector<SemaSymbol> symbols; // global symbols collected in the first pass
        [[nodiscard]] bool HasErrors() const noexcept;
    };

    // A dependency package: its name and parsed source modules.
    // Symbols from dep packages are isolated until explicitly imported.
    struct DepPackage {
        std::string name;

        struct ModuleEntry {
            std::string moduleName; // source identifier for diagnostics/bookkeeping
            const Module* module;
        };

        std::vector<ModuleEntry> modules;
    };

    // Runs semantic analysis over a set of parsed modules.
    // Modules should be passed in dependency order when possible, but the analyzer
    // performs a global first pass so forward references within a package work.
    class Sema {
    public:
        explicit Sema(std::vector<const Module*> userModules,
                      std::vector<DepPackage> deps = {});
        [[nodiscard]] SemaResult Analyze();

        // Write a human-readable dump of the sema result to `path`.
        static bool DumpResult(const SemaResult& result, const std::filesystem::path& path);

    private:
        std::vector<const Module*> modules;
        std::vector<DepPackage> deps;
        std::vector<SemaDiagnostic> diags;
        std::vector<SemaSymbol> symbols;
    };
}
