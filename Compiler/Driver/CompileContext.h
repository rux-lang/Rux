#pragma once

#include "Driver/CompilerDriver.h"
#include "Semantic/Model/SemanticModel.h"
#include "SourceModel/SourceFile.h"

#include <unordered_map>

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
