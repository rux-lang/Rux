#pragma once

// The compile pipeline for one package: load → lex → parse → resolve
// dependencies → sema, and unless checkOnly: HIR → LIR → RCU objects → link.
// The CLI parses arguments, loads the manifest, validates the target, and
// decides what to print; the Driver owns everything in between.

#include "Driver/BuildReport.h"
#include "Frontend/Parser/Parser.h"
#include "Package/Manifest.h"
#include "Support/Diagnostics.h"

#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Rux {

struct CompileOptions {
    std::filesystem::path manifestPath; // resolved path to Rux.toml
    Manifest manifest;                  // parsed manifest for manifestPath
    std::string targetName;             // validated "os-arch" triple
    std::string profileName;            // "Release", "Debug", or a custom profile

    bool quiet = false;
    bool verbose = false;   // print per-phase progress lines to stdout
    bool checkOnly = false; // stop after semantic analysis; keep going past
                            // frontend errors so all diagnostics are reported

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
    std::filesystem::path executablePath; // empty in checkOnly mode or on failure
    Misc::BuildStats stats;
};

class Driver {
public:
    explicit Driver(CompileOptions options);
    [[nodiscard]] CompileResult Compile();

private:
    void Emit(const Diagnostic &diag) const;
    void EmitErrorLine(std::string_view line) const;
    // Emit every diagnostic; returns true if any is an error.
    bool EmitAll(std::span<const Diagnostic> diags) const;

    // Pipeline phases. Each returns false when the pipeline cannot continue.
    bool LexAndParseSources();
    bool LoadDependencies();
    bool Analyze();
    bool GenerateExecutable(std::filesystem::path &exePath);

    CompileOptions opts;
    std::filesystem::path root; // package root (manifest directory)
    Misc::BuildStats stats;
    bool hadErrors = false; // frontend errors accumulated in checkOnly mode

    std::vector<ParseResult> parseResults;      // user modules
    std::vector<ParseResult> depParseResults;   // dependency modules
    std::vector<std::string> loadedPackages;    // parallel: package name per dep entry
    std::vector<std::string> loadedModuleNames; // parallel: source name per dep entry
};

} // namespace Rux
