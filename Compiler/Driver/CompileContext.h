#pragma once

#include "Driver/CompilerDriver.h"
#include "Driver/DependencyGraph.h"
#include "Driver/ModuleCache.h"
#include "Parallel/WorkerPool.h"
#include "Semantic/Model/SemanticModel.h"
#include "SourceModel/SourceFile.h"
#include "SourceModel/SourceText.h"

#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace Rux::Driver {

class CompilerDriver::Impl {
public:
    explicit Impl(CompileOptions options);
    [[nodiscard]] CompileResult Compile();

private:
    void Emit(const Diagnostic &diag);
    void BeginPhase(CompilePhase phase, std::string_view subject, const std::filesystem::path &path = {});
    void EmitProgress(CompilePhase phase, std::string_view subject, const std::filesystem::path &path = {}) const;
    /// Emit every diagnostic; returns true if any is an error.
    bool EmitAll(std::span<const Diagnostic> diags);

    struct SourceView {
        std::filesystem::path path;
        const SourceText &source;
    };

    [[nodiscard]] std::vector<SourceView> RememberSources(std::span<SourceFile> sources);
    bool WriteInspectionOutput(InspectionKind kind, const std::filesystem::path &path,
                               const std::function<bool()> &write);
    [[nodiscard]] std::optional<std::string_view> LookupSourceLine(std::string_view sourceName,
                                                                   std::size_t lineNumber) const;

    void InitializeCompileTimeContext();
    /// The compilation's worker threads, started on first use.
    [[nodiscard]] Parallel::WorkerPool &Workers();

    /// Build records (Driver/BuildCache.h): whether this compilation may use one, where it lives, the request it
    /// describes, reusing an up-to-date one, and recording a finished build, or with no artifact a clean check.
    [[nodiscard]] bool BuildCacheEligible() const;
    [[nodiscard]] std::filesystem::path BuildRecordPath() const;
    [[nodiscard]] std::string BuildKey() const;
    [[nodiscard]] bool ReuseRecordedBuild(CompileResult &result);
    void RecordBuild(const std::filesystem::path &artifactPath,
                     const std::vector<std::filesystem::path> &secondaryArtifactPaths);

    /// Reuse of compiled modules (Driver/ModuleCache.h): whether this compilation may reuse them, where its pack
    /// lives, keying every parsed module, and choosing the modules whose recorded objects this build links.
    [[nodiscard]] bool ModuleCacheEligible() const;
    [[nodiscard]] std::filesystem::path ModulePackPath() const;
    void PrepareModuleReuse();
    void SelectReusedModules();
    /// Precompiled dependency packages (Driver/PackageCache.h): keying each dependency package and reading its newest
    /// kept variant, checking once the program is analyzed that the variants reused were compiled under the names the
    /// program gives, and keeping each package compiled or confirmed by this build.
    [[nodiscard]] std::string PackageContextKey() const;
    void PreparePackageReuse();
    [[nodiscard]] bool RejectStalePackages();
    /// Analyze, and again with the packages whose names differ compiled, until every reused package checks out.
    bool AnalyzeReusing();
    /// Whether `RUX_VERIFY_MODULE_CACHE` asks every build that reuses modules to be checked against a fresh one.
    [[nodiscard]] static bool VerifyingModuleReuse();
    /// Compile the package again reusing nothing and report an internal error unless the artifact is the same.
    bool VerifyReusedArtifact(const std::filesystem::path &artifactPath);

    /// Pipeline phases. Each returns false when the pipeline cannot continue.
    bool LexAndParseSources();
    /// In a test build, adds a generated `Main` that runs the package's `#Test()` functions. Defined in
    /// TestEntry.cpp.
    bool SynthesizeTestEntry();
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

    std::optional<DependencyGraph> dependencyGraph;
    std::vector<ParseResult> parseResults;      // user modules
    std::vector<ParseResult> depParseResults;   // dependency modules
    std::vector<std::string> loadedPackages;    // parallel: package identity per dep entry
    std::vector<std::string> loadedModuleNames; // parallel: source name per dep entry
    std::unordered_map<std::string, SourceText> loadedSourceTexts;
    /// Size and modification time of every loaded source as it was read, for the build record.
    std::unordered_map<std::string, std::pair<std::uintmax_t, std::int64_t>> loadedSourceStamps;
    std::optional<SemanticModel> semanticModel;
    std::unique_ptr<Parallel::WorkerPool> workers;

    struct ModuleReuse {
        bool enabled = false;
        ModulePack pack;
        /// The key of every module read from a source file; a generated module has none and is always compiled.
        std::unordered_map<const Module *, std::string> keys;

        /// What the keys were made of, recorded with the module so the next build can take it over.
        struct SourceDigests {
            std::uintmax_t size = 0;
            std::int64_t modified = 0;
            std::string source;
            std::string interface;
        };

        std::unordered_map<const Module *, SourceDigests> digests;
        /// Modules this build reuses the objects of, and their entries in `pack`.
        std::unordered_set<const Module *> declarationOnly;
        std::unordered_map<const Module *, const CachedModule *> cached;
        /// Modules an attempt found needed differently than when they were recorded, which are compiled again.
        std::set<std::string> fresh;

        /// A dependency package, which may come from the shared cache of precompiled packages.
        struct Package {
            std::string id;
            std::vector<const Module *> modules;
            /// The packages it imports from, which come before it.
            std::vector<std::string> dependencies;
            /// Its key, empty when it cannot be kept, and the directory its variants are kept in.
            std::string key;
            std::filesystem::path directory;
            /// The newest variant kept of it, whose modules this build reuses when nothing else does.
            std::optional<ModulePack> variant;
            /// The names the program gave its declarations and those of its dependencies, once analyzed.
            std::string names;
        };

        /// The dependency packages, each after those it imports from.
        std::vector<Package> packages;
    };

    ModuleReuse moduleReuse;
    /// Set by `GenerateArtifact` when a reused module has to be compiled after all, which analyzes the program again.
    bool retryWithFreshModules = false;
    /// While the program is analyzed again: diagnostics the first analysis already reported are not repeated.
    bool repeatingAnalysis = false;
};

} // namespace Rux::Driver
