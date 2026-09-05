#pragma once

// The compile pipeline for one package: load → lex → parse → resolve
// dependencies → sema, and unless checkOnly: HIR → LIR → RCU objects → link.
// The CLI parses arguments, loads the manifest, validates the target, and
// decides what to print; the Driver owns everything in between.

#include "BuildInfo/BuildProfile.h"
#include "Diagnostics/Diagnostics.h"
#include "Driver/BuildStats.h"
#include "Driver/CompileEvents.h"
#include "Package/Manifest.h"
#include "Syntax/ParseResult.h"
#include "Target/TargetTriple.h"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Rux::Driver {
struct CompileOptions {
    std::filesystem::path manifestPath; // resolved path to Rux.toml
    Manifest manifest;                  // parsed manifest for manifestPath
    Target::TargetTriple target = Target::TargetTriple::Host();
    BuildProfile profile = BuildProfile::Debug;
    std::map<std::string, std::string> defines; // --define overrides

    /// Workspace members that override registry dependencies by normalized package name. Repository checks and tests
    /// use this map to compile against the local source tree while publishable package manifests retain version-based
    /// dependencies.
    std::map<std::string, std::filesystem::path> localPackageRoots;
    bool localDependenciesOnly = false;

    bool isTest = false;
    bool checkOnly = false; // stop after semantic analysis; keep going past
    // frontend errors so all diagnostics are reported
    bool captureFrontend = false; // retain folded user ASTs in CompileResult

    /// Debug dumps written under <package root>/Temp (build --dump-*).
    bool dumpTokens = false;
    bool dumpAst = false;
    bool dumpSema = false;
    bool dumpHir = false;
    bool dumpLir = false;
    bool dumpAsm = false;
    bool dumpRcu = false;

    /// Optional presentation hook. The driver always retains diagnostics in CompileResult and supplies already-loaded
    /// source text to a callback so a CLI can render context without rereading files.
    std::function<void(const Diagnostic &, const SourceLineLookup &)> emitDiagnostic;
    /// Optional semantic progress events. The CLI owns visibility, styling, and stream selection; the reusable driver
    /// never writes progress itself.
    std::function<void(const CompileProgress &)> emitProgress;
};

struct CompileResult {
    bool ok = false;
    std::filesystem::path primaryArtifactPath; // empty in checkOnly mode or on failure
    std::vector<std::filesystem::path> secondaryArtifactPaths;
    BuildStats stats;
    std::vector<Diagnostic> diagnostics;
    std::vector<ParseResult> modules; // populated when captureFrontend succeeds
    std::vector<InspectionOutput> inspectionOutputs;
};

class CompilerDriver {
public:
    explicit CompilerDriver(CompileOptions options);
    ~CompilerDriver();
    CompilerDriver(CompilerDriver &&) noexcept;
    CompilerDriver &operator=(CompilerDriver &&) noexcept;
    CompilerDriver(const CompilerDriver &) = delete;
    CompilerDriver &operator=(const CompilerDriver &) = delete;
    [[nodiscard]] CompileResult Compile();

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace Rux::Driver
