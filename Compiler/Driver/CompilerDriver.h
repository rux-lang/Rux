#pragma once

// The compile pipeline for one package: load → lex → parse → resolve
// dependencies → sema, and unless checkOnly: HIR → LIR → RCU objects → link.
// The CLI parses arguments, loads the manifest, validates the target, and
// decides what to print; the Driver owns everything in between.

#include "Diagnostics/Diagnostics.h"
#include "Driver/BuildReport.h"
#include "Package/Manifest.h"
#include "Semantic/SemanticModel.h"
#include "Syntax/Parser/Parser.h"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Rux::Driver {
struct CompileOptions {
    std::filesystem::path manifestPath;         // resolved path to Rux.toml
    Manifest manifest;                          // parsed manifest for manifestPath
    std::string targetName;                     // validated "os-arch" triple
    std::string profileName;                    // "Release", "Debug", or a custom profile
    std::map<std::string, std::string> defines; // --define overrides

    // Workspace members that override registry dependencies by package name.
    // Repository checks and tests use this map to compile against the local
    // source tree while publishable package manifests retain version-based
    // dependencies.
    std::map<std::string, std::filesystem::path> localPackageRoots;
    bool localDependenciesOnly = false;

    bool quiet = false;
    bool verbose = false; // print per-phase progress lines to stdout
    bool isTest = false;
    bool checkOnly = false; // stop after semantic analysis; keep going past
    // frontend errors so all diagnostics are reported
    bool captureFrontend = false; // retain folded user ASTs in CompileResult

    // Lower AArch64 with the native back end (CodeGen/AArch64/RcuEmitter.cpp)
    // rather than with the Clang emitter. That back end is being written a
    // group of opcodes at a time (BACKLOG.md Phases 3-5), so it stays opt-in —
    // by this flag or by the RUX_AARCH64_RCU environment variable — until it
    // can build every program the Clang path can, at which point task 34
    // removes the Clang path and this choice with it.
    bool nativeAArch64Backend = false;

    // Debug dumps written under <package root>/Temp (build --dump-*).
    bool dumpTokens = false;
    bool dumpAst = false;
    bool dumpSema = false;
    bool dumpHir = false;
    bool dumpLir = false;
    bool dumpAsm = false;
    bool dumpRcu = false;

    // Where diagnostics go. Defaults to PrintDiagnostic (text on stderr).
    std::function<void(const Diagnostic &)> emitDiagnostic;
    // Where pre-formatted error lines (e.g. source-loader failures, which
    // carry their own "error: " prefix and newline) go. Defaults to printing
    // the line to stderr as-is.
    std::function<void(std::string_view)> emitError;
};

struct CompileResult {
    bool ok = false;
    std::filesystem::path primaryArtifactPath; // empty in checkOnly mode or on failure
    std::vector<std::filesystem::path> secondaryArtifactPaths;
    BuildStats stats;
    std::vector<ParseResult> modules; // populated when captureFrontend succeeds
};

class CompilerDriver {
public:
    explicit CompilerDriver(CompileOptions options);
    [[nodiscard]] CompileResult Compile();

private:
    void Emit(const Diagnostic &diag) const;
    void EmitErrorLine(std::string_view line) const;
    // Emit every diagnostic; returns true if any is an error.
    bool EmitAll(std::span<const Diagnostic> diags) const;

    // The operating system of the build target, named exactly ("FreeBSD", not
    // the "BSD" family). This is what `#target.os` reports.
    [[nodiscard]] std::string TargetSystemName() const;
    void InitializeCompileTimeContext();

    // Whether this build asked for the native AArch64 back end rather than the
    // Clang emitter, by CompileOptions or by RUX_AARCH64_RCU.
    [[nodiscard]] bool UseNativeAArch64Backend() const;

    // Pipeline phases. Each returns false when the pipeline cannot continue.
    bool LexAndParseSources();
    bool LoadDependencies();
    bool Analyze();
    bool GenerateArtifact(std::filesystem::path &artifactPath,
                          std::vector<std::filesystem::path> &secondaryArtifactPaths);

    CompileOptions opts;
    std::filesystem::path root; // package root (manifest directory)
    BuildStats stats;
    bool hadErrors = false; // frontend errors accumulated in checkOnly mode
    bool invalidSourceDateEpoch = false;
    CompileTimeContext compileTimeContext;

    std::vector<ParseResult> parseResults;      // user modules
    std::vector<ParseResult> depParseResults;   // dependency modules
    std::vector<std::string> loadedPackages;    // parallel: package name per dep entry
    std::vector<std::string> loadedModuleNames; // parallel: source name per dep entry
    std::optional<SemanticModel> semanticModel;
};
} // namespace Rux::Driver
