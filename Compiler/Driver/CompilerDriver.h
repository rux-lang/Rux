#pragma once

// The compile pipeline for one package: load → lex → parse → resolve
// dependencies → sema, and unless checkOnly: HIR → LIR → RCU objects → link.
// The CLI parses arguments, loads the manifest, validates the target, and
// decides what to print; the Driver owns everything in between.

#include "BuildInfo/BuildProfile.h"
#include "Diagnostics/Diagnostics.h"
#include "Driver/BuildReport.h"
#include "Package/Manifest.h"
#include "Semantic/SemanticModel.h"
#include "SourceModel/SourceFile.h"
#include "Syntax/Parser/Parser.h"
#include "Target/TargetTriple.h"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Rux::Driver {
enum class CompilePhase {
    Configuring,
    LoadingSources,
    Lexing,
    Parsing,
    LoadingDependency,
    Analyzing,
    LoweringToHir,
    OptimizingHir,
    LoweringToLir,
    OptimizingLir,
    EmittingAssembly,
    EmittingObjects,
    Linking,
};

enum class InspectionKind {
    Tokens,
    Ast,
    Semantic,
    Hir,
    Lir,
    Assembly,
    RcuObject,
    Rcu,
};

struct InspectionOutput {
    InspectionKind kind;
    std::filesystem::path path;
};

[[nodiscard]] std::string_view InspectionHeading(InspectionKind kind) noexcept;
[[nodiscard]] std::string_view InspectionDescription(InspectionKind kind) noexcept;

struct CompileProgress {
    CompilePhase phase;
    std::string_view subject;
    std::filesystem::path path;
};

[[nodiscard]] std::string_view CompilePhaseName(CompilePhase phase) noexcept;

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
    [[nodiscard]] CompileResult Compile();

private:
    void Emit(const Diagnostic &diag);
    void BeginPhase(CompilePhase phase, std::string_view subject, const std::filesystem::path &path = {});
    void EmitProgress(CompilePhase phase, std::string_view subject, const std::filesystem::path &path = {}) const;
    /// Emit every diagnostic; returns true if any is an error.
    bool EmitAll(std::span<const Diagnostic> diags);
    void RememberSources(std::span<const SourceFile> sources);
    bool WriteInspectionOutput(InspectionKind kind, const std::filesystem::path &path,
                               const std::function<bool()> &write);
    [[nodiscard]] std::optional<std::string_view> LookupSourceLine(std::string_view sourceName,
                                                                   std::size_t lineNumber) const;

    /// The operating system of the build target, named exactly ("FreeBSD", not the "BSD" family). This is what
    /// `#target.os` reports.
    [[nodiscard]] std::string TargetSystemName() const;
    void InitializeCompileTimeContext();

    /// Pipeline phases. Each returns false when the pipeline cannot continue.
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
    std::optional<CompilePhase> currentPhase;
    std::vector<Diagnostic> diagnostics;
    std::vector<InspectionOutput> inspectionOutputs;
    CompileTimeContext compileTimeContext;

    std::vector<ParseResult> parseResults;      // user modules
    std::vector<ParseResult> depParseResults;   // dependency modules
    std::vector<std::string> loadedPackages;    // parallel: package name per dep entry
    std::vector<std::string> loadedModuleNames; // parallel: source name per dep entry
    std::unordered_map<std::string, std::string> loadedSourceTexts;
    std::optional<SemanticModel> semanticModel;
};
} // namespace Rux::Driver
