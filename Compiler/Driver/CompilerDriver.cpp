#include "BuildInfo/CompilerMetadata.h"
#include "CodeGen/AArch64/RcuEmitter.h"
#include "CodeGen/X86_64/AssemblyPrinter.h"
#include "CodeGen/X86_64/RcuEmitter.h"
#include "Driver/BuildTarget.h"
#include "Driver/CompileContext.h"
#include "Ir/Hir/Hir.h"
#include "Ir/Hir/HirPrinter.h"
#include "Ir/Lir/Lir.h"
#include "Ir/Lir/LirPrinter.h"
#include "Lexer/Lexer.h"
#include "Linker/Linker.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Object/Rcu/Rcu.h"
#include "Object/Rcu/RcuDumper.h"
#include "Object/Rcu/RcuWriter.h"
#include "Optimization/Pipeline.h"
#include "Package/Cache.h"
#include "Semantic/Conditional/ConditionalCompilation.h"
#include "Semantic/Model/SemanticPrinter.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Source/SourceLoader.h"
#include "Syntax/Ast/Ast.h"
#include "Syntax/Parser/Parser.h"
#include "System/Os.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <format>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

using namespace Rux::Packages;

namespace Rux::Driver {
using namespace Target;
using namespace System;

CompilerDriver::CompilerDriver(CompileOptions options)
    : impl(std::make_unique<Impl>(std::move(options))) {
}

CompilerDriver::~CompilerDriver() = default;
CompilerDriver::CompilerDriver(CompilerDriver &&) noexcept = default;
CompilerDriver &CompilerDriver::operator=(CompilerDriver &&) noexcept = default;

CompileResult CompilerDriver::Compile() {
    return impl->Compile();
}

CompilerDriver::Impl::Impl(CompileOptions options)
    : opts(std::move(options)) {
    root = opts.manifestPath.parent_path();
    InitializeCompileTimeContext();
}

void CompilerDriver::Impl::InitializeCompileTimeContext() {
    compileTimeContext.target = TargetContextForTriple(opts.target);
    compileTimeContext.targetTriple = opts.target.CanonicalName();
    compileTimeContext.profile = opts.profile;
    compileTimeContext.isTest = opts.isTest;
    compileTimeContext.sourceRoot = root.lexically_normal();
    compileTimeContext.config = opts.manifest.build.ConfigValues();
    for (const auto &[name, value] : opts.defines) {
        compileTimeContext.config[name] = value;
    }

    if (opts.manifest.package.type == ManifestPackageType::SharedLibrary) {
        compileTimeContext.outputKind = OutputKind::SharedLibrary;
    }
    else if (opts.manifest.package.type == ManifestPackageType::StaticLibrary) {
        compileTimeContext.outputKind = OutputKind::StaticLibrary;
    }
    else if (opts.manifest.package.type == ManifestPackageType::SourceLibrary) {
        compileTimeContext.outputKind = OutputKind::SourceLibrary;
    }

    std::int64_t buildTimestamp =
        static_cast<std::int64_t>(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    if (const auto epoch = System::GetEnv("SOURCE_DATE_EPOCH")) {
        std::int64_t parsed = 0;
        const auto result = std::from_chars(epoch->data(), epoch->data() + epoch->size(), parsed);
        if (result.ec != std::errc{} || result.ptr != epoch->data() + epoch->size() || parsed < 0) {
            invalidSourceDateEpoch = true;
        }
        else {
            buildTimestamp = parsed;
        }
    }
    compileTimeContext.buildInfo = BuildInfo(std::string(CompilerBuild::compilerVersion), buildTimestamp);
}

void CompilerDriver::Impl::Emit(const Diagnostic &diag) {
    Diagnostic contextual = diag;
    if (currentPhase) {
        contextual.notes.push_back("compiler phase: " + std::string(CompilePhaseName(*currentPhase)));
    }
    diagnostics.push_back(contextual);
    const SourceLineLookup sourceLineLookup = [this](const std::string_view sourceName, const std::size_t lineNumber) {
        return LookupSourceLine(sourceName, lineNumber);
    };
    if (opts.emitDiagnostic) {
        opts.emitDiagnostic(contextual, sourceLineLookup);
    }
}

void CompilerDriver::Impl::BeginPhase(const CompilePhase phase, const std::string_view subject,
                                      const std::filesystem::path &path) {
    currentPhase = phase;
    EmitProgress(phase, subject, path);
}

void CompilerDriver::Impl::EmitProgress(const CompilePhase phase, const std::string_view subject,
                                        const std::filesystem::path &path) const {
    if (opts.emitProgress) {
        opts.emitProgress({.phase = phase, .subject = subject, .path = path});
    }
}

bool CompilerDriver::Impl::EmitAll(std::span<const Diagnostic> diags) {
    bool hasErrors = false;
    for (const auto &diag : diags) {
        Emit(diag);
        hasErrors |= diag.IsError();
    }
    return hasErrors;
}

std::vector<CompilerDriver::Impl::SourceView>
CompilerDriver::Impl::RememberSources(const std::span<SourceFile> sources) {
    std::vector<SourceView> views;
    views.reserve(sources.size());
    for (auto &file : sources) {
        // Dependencies may name an already loaded path. Keep the first immutable copy and its diagnostic index.
        const auto [stored, inserted] = loadedSourceTexts.try_emplace(file.path.string(), std::move(file.source));
        if (!inserted) {
            file.source.clear();
        }
        views.push_back({std::move(file.path), stored->second});
    }
    return views;
}

bool CompilerDriver::Impl::WriteInspectionOutput(const InspectionKind kind, const std::filesystem::path &path,
                                                 const std::function<bool()> &write) {
    std::error_code directoryError;
    std::filesystem::create_directories(path.parent_path(), directoryError);
    if (directoryError) {
        Emit(ErrorDiagnostic(std::format("could not write {} to '{}'", InspectionHeading(kind), path.string()),
                             {std::format("filesystem error {}: {}", directoryError.value(), directoryError.message())},
                             "check that the destination directory is writable and is not an existing file"));
        return false;
    }

    errno = 0;
    if (!write()) {
        std::error_code writeError(errno, std::generic_category());
        if (!writeError) {
            writeError = std::make_error_code(std::errc::io_error);
        }
        Emit(ErrorDiagnostic(std::format("could not write {} to '{}'", InspectionHeading(kind), path.string()),
                             {std::format("filesystem error {}: {}", writeError.value(), writeError.message())},
                             "check that the destination path is writable and has enough free space"));
        return false;
    }
    inspectionOutputs.push_back({kind, path});
    return true;
}

std::optional<std::string_view> CompilerDriver::Impl::LookupSourceLine(const std::string_view sourceName,
                                                                       const std::size_t lineNumber) const {
    const auto found = loadedSourceTexts.find(std::string(sourceName));
    if (found == loadedSourceTexts.end()) {
        return std::nullopt;
    }
    return found->second.Line(lineNumber);
}

std::string CompilerDriver::Impl::TargetSystemName() const {
    return std::string(Target::ToString(opts.target.Os()));
}

CompileResult CompilerDriver::Impl::Compile() {
    CompileResult result;
    const auto t0 = std::chrono::steady_clock::now();
    auto finish = [&]() {
        const auto buildEnd = std::chrono::steady_clock::now();
        stats.total = ElapsedMs(t0, buildEnd);
        stats.totalSeconds = ElapsedSeconds(t0, buildEnd);
        stats.peakMemoryBytes = PeakMemoryBytes();
        result.stats = stats;
        result.diagnostics = diagnostics;
        result.inspectionOutputs = inspectionOutputs;
    };
    BeginPhase(CompilePhase::Configuring, opts.manifest.package.name.Text(), opts.manifestPath);
    if (invalidSourceDateEpoch) {
        const auto epoch = System::GetEnv("SOURCE_DATE_EPOCH");
        Emit(ErrorDiagnostic(
            std::format("invalid SOURCE_DATE_EPOCH value '{}': expected a non-negative integer number of seconds",
                        epoch.value_or("")),
            {"SOURCE_DATE_EPOCH controls the reproducible build timestamp embedded in generated objects"},
            "unset SOURCE_DATE_EPOCH or set it to a Unix timestamp"));
        finish();
        return result;
    }
    // A SourceLibrary package has no artifact of its own: it is compiled into whichever
    // package depends on it. Checking it in place is still useful, so the kind is
    // only rejected once a build is actually requested.
    if (!opts.checkOnly && opts.manifest.package.type == ManifestPackageType::SourceLibrary) {
        Emit(ErrorDiagnostic(
            "package '" + opts.manifest.package.name.Text() +
            "' has Type = \"SourceLibrary\" and is compiled into dependent packages; it cannot be built or "
            "run as a top-level target"));
        finish();
        return result;
    }

    if (!LexAndParseSources()) {
        finish();
        return result;
    }
    if (!LoadDependencies()) {
        finish();
        return result;
    }
    // In checkOnly mode frontend errors are accumulated instead of aborting;
    // semantic analysis only runs on a package whose frontend is clean.
    if (hadErrors) {
        finish();
        return result;
    }
    if (!Analyze()) {
        finish();
        return result;
    }
    if (opts.checkOnly) {
        if (opts.captureFrontend) {
            result.modules = std::move(parseResults);
        }
        result.ok = true;
        finish();
        return result;
    }

    std::filesystem::path artifactPath;
    if (!GenerateArtifact(artifactPath, result.secondaryArtifactPaths)) {
        finish();
        return result;
    }

    std::error_code sizeError;
    stats.executableSize = std::filesystem::file_size(artifactPath, sizeError);
    if (sizeError) {
        stats.executableSize = 0;
    }
    result.primaryArtifactPath = std::move(artifactPath);
    result.ok = true;
    finish();
    return result;
}

bool CompilerDriver::Impl::LexAndParseSources() {
    BeginPhase(CompilePhase::LoadingSources, opts.manifest.package.name.Text(), root / "Src");
    auto loadResult = SourceLoader::Load(root);
    const auto sourceFiles = RememberSources(loadResult.files);
    stats.localFiles = sourceFiles.size();
    for (const auto &file : sourceFiles) {
        stats.localLines += CountLines(file.source.Text());
        stats.localSourceSize += file.source.Text().size();
    }
    if (EmitAll(loadResult.diagnostics)) {
        hadErrors = true;
        if (!opts.checkOnly) {
            return false;
        }
    }

    // Lex
    bool lexErrors = false;
    bool inspectionErrors = false;
    std::vector<LexerResult> lexResults;
    lexResults.reserve(sourceFiles.size());
    const auto lexingStart = std::chrono::steady_clock::now();
    for (const auto &file : sourceFiles) {
        BeginPhase(CompilePhase::Lexing, file.path.string(), file.path);
        auto lexResult = Lexer::TokenizeSource(file.source.Text(), file.path.string());
        stats.localTokens += CountTokens(lexResult.tokens);
        if (EmitAll(lexResult.diagnostics)) {
            lexErrors = true;
        }
        if (opts.dumpTokens) {
            auto rel = std::filesystem::relative(file.path, root / "Src");
            auto tokPath = root / "Temp" / "Tokens" / rel;
            tokPath.replace_extension(".tokens");
            inspectionErrors |= !WriteInspectionOutput(InspectionKind::Tokens, tokPath,
                                                       [&] { return Lexer::DumpTokens(lexResult, tokPath); });
        }
        lexResults.push_back(std::move(lexResult));
    }
    stats.lexing += ElapsedMs(lexingStart);
    if (lexErrors || inspectionErrors) {
        hadErrors = true;
        if (!opts.checkOnly) {
            return false;
        }
    }

    // Parse
    bool parseErrors = false;
    parseResults.reserve(sourceFiles.size());
    const auto parsingStart = std::chrono::steady_clock::now();
    for (std::size_t fileIndex = 0; fileIndex < sourceFiles.size(); ++fileIndex) {
        const auto &file = sourceFiles[fileIndex];
        BeginPhase(CompilePhase::Parsing, file.path.string(), file.path);
        auto &lexResult = lexResults[fileIndex];
        if (lexResult.HasErrors()) {
            continue;
        }
        Parser parser(std::move(lexResult.tokens), file.path.string(), compileTimeContext.target.arch);
        auto parseResult = parser.Parse();
        if (EmitAll(parseResult.diagnostics)) {
            parseErrors = true;
        }
        if (parseResult.HasErrors()) {
            continue;
        }
        parseResults.push_back(std::move(parseResult));
    }
    stats.parsing += ElapsedMs(parsingStart);
    if (parseErrors || inspectionErrors) {
        hadErrors = true;
        if (!opts.checkOnly) {
            return false;
        }
    }
    return true;
}

bool CompilerDriver::Impl::LoadDependencies() {
    BeginPhase(CompilePhase::LoadingDependency, opts.manifest.package.name.Text(), root);

    struct PendingPackage {
        std::string name;
        std::filesystem::path root;
        Manifest manifest;
    };

    std::vector<PendingPackage> pendingPackages;
    std::unordered_set<std::string> queuedPackageNames;
    auto enqueueDependency = [&](const std::string &pkgName, const Manifest &ownerManifest,
                                 const std::filesystem::path &ownerRoot) -> bool {
        if (queuedPackageNames.count(pkgName)) {
            return true;
        }
        const ManifestDependency *dep = nullptr;
        for (const auto &d : ownerManifest.dependencies) {
            if (d.importName.Text() == pkgName) {
                dep = &d;
                break;
            }
        }
        if (!dep) {
            Emit(ErrorDiagnostic("package '" + pkgName + "' is not listed in [Dependencies] of '" +
                                     (ownerRoot / "Rux.toml").string() + "'",
                                 {"the import requires a package dependency with the same import name"},
                                 "add the package under [Dependencies] or correct the import path"));
            return false;
        }
        if (!dep->MatchesTarget(opts.target.Os())) {
            Emit(ErrorDiagnostic(
                "dependency '" + pkgName + "' is not available for target '" +
                    std::string(opts.target.CanonicalName()) + "'",
                {"TargetOS in '" + (ownerRoot / "Rux.toml").string() + "' excludes " + TargetSystemName()},
                "select an available target or include the target OS in the dependency's TargetOS list"));
            return false;
        }
        std::filesystem::path depRoot;
        if (dep->IsPath()) {
            depRoot = (ownerRoot / dep->Path()).lexically_normal();
        }
        else if (const auto local = opts.localPackageRoots.find(dep->package.Normalized());
                 local != opts.localPackageRoots.end()) {
            depRoot = local->second;
        }
        else {
            if (opts.localDependenciesOnly) {
                Emit(
                    ErrorDiagnostic("package '" + DependencyPackageName(*dep) +
                                        "' is not a local workspace member; registry dependencies are disabled",
                                    {"workspace checks resolve registry declarations only from matching local members"},
                                    "add the package to [Workspace].Packages or use a local Path dependency"));
                return false;
            }
            // The cache holds every installed version side by side, so the
            // requirement in the manifest decides which one this build sees.
            // Resolution is local: a build never contacts the registry.
            const RegistryDependencySource *registry = dep->Registry();
            const std::string identity = registry->ns.Text() + "/" + DependencyPackageName(*dep);
            const auto installed = FindInstalledPackage(registry->ns, dep->package, registry->version);
            if (!installed) {
                const auto present = InstalledVersions(registry->ns, dep->package);
                std::string listed;
                for (const auto &candidate : present) {
                    listed += (listed.empty() ? "" : ", ") + candidate.version.Text();
                }
                Emit(ErrorDiagnostic("no installed version of '" + identity + "' satisfies '" +
                                         registry->version.Text() + "'",
                                     listed.empty() ? std::vector<std::string>{"no versions are installed"}
                                                    : std::vector<std::string>{"installed versions: " + listed},
                                     "run 'rux install' to resolve and cache the dependency"));
                return false;
            }
            depRoot = installed->root;
        }
        auto depManifest = Manifest::Load(depRoot / "Rux.toml");
        if (!depManifest.Ok()) {
            for (const auto &diagnostic : depManifest.diagnostics) {
                Emit({Diagnostic::Severity::Error,
                      diagnostic.path.string(),
                      {.line = diagnostic.line, .column = diagnostic.column, .offset = 0},
                      diagnostic.message,
                      diagnostic.notes,
                      diagnostic.help,
                      diagnostic.documentationUrl});
            }
            Emit(ErrorDiagnostic("cannot load dependency package '" + pkgName + "' from '" + depRoot.string() + "'",
                                 {"the dependency manifest is missing or invalid"},
                                 "check the dependency path and its Rux.toml manifest"));
            return false;
        }
        queuedPackageNames.insert(pkgName);
        // Keep the import name as the package namespace loaded into Sema,
        // even when the files came from another package name.
        pendingPackages.push_back({dep->importName.Text(), depRoot, std::move(*depManifest.manifest)});
        return true;
    };

    // Packages are parsed into stable storage before their conditionals ask for imported declarations.
    // The resolver loads only imports in surviving branches, including imports needed by a condition itself.
    std::unordered_map<std::string, std::vector<ParseResult>> parsedPackages;
    std::unordered_set<std::string> loadingPackages;
    std::vector<std::string> packageOrder;
    bool failed = false;
    const auto modulesOf = [](std::vector<ParseResult> &parsed) {
        std::vector<Module *> modules;
        for (auto &result : parsed) {
            modules.push_back(&result.module);
        }
        return modules;
    };
    const auto loadPackage = [&](this auto &&self, const std::string &name, const Manifest &owner,
                                 const std::filesystem::path &ownerRoot) -> std::vector<Module *> {
        if (failed) {
            return {};
        }
        if (loadingPackages.contains(name)) {
            Emit(ErrorDiagnostic("cyclic dependency while resolving declarations from package '" + name + "'"));
            failed = true;
            return {};
        }
        if (const auto loaded = parsedPackages.find(name); loaded != parsedPackages.end()) {
            return modulesOf(loaded->second);
        }
        if (!enqueueDependency(name, owner, ownerRoot)) {
            failed = true;
            return {};
        }
        const auto found = std::ranges::find(pendingPackages, name, &PendingPackage::name);
        if (found == pendingPackages.end()) {
            failed = true;
            return {};
        }
        // Recursive loading may reallocate pendingPackages, so retain an owning copy.
        const PendingPackage pending = *found; // NOLINT(performance-unnecessary-copy-initialization)
        loadingPackages.insert(name);
        BeginPhase(CompilePhase::LoadingDependency, name, pending.root);
        auto loaded = SourceLoader::Load(pending.root);
        const auto files = RememberSources(loaded.files);
        stats.dependencyFiles += files.size();
        if (EmitAll(loaded.diagnostics)) {
            failed = true;
            return {};
        }
        auto &parsed = parsedPackages[name];
        parsed.reserve(files.size());
        for (const auto &file : files) {
            stats.dependencyLines += CountLines(file.source.Text());
            stats.dependencySourceSize += file.source.Text().size();
            const auto lexingStart = std::chrono::steady_clock::now();
            auto lexed = Lexer::TokenizeSource(file.source.Text(), file.path.string());
            stats.lexing += ElapsedMs(lexingStart);
            stats.dependencyTokens += CountTokens(lexed.tokens);
            if (EmitAll(lexed.diagnostics)) {
                failed = true;
                return {};
            }
            const auto parsingStart = std::chrono::steady_clock::now();
            Parser parser(std::move(lexed.tokens), file.path.string(), compileTimeContext.target.arch);
            auto result = parser.Parse();
            stats.parsing += ElapsedMs(parsingStart);
            if (EmitAll(result.diagnostics)) {
                failed = true;
                return {};
            }
            parsed.push_back(std::move(result));
        }
        const auto modules = modulesOf(parsed);
        std::vector<Diagnostic> foldDiagnostics;
        ResolveConditionalCompilation(modules, compileTimeContext, foldDiagnostics,
                                      [&](const std::string_view imported) {
                                          if (imported == name || imported == pending.manifest.package.name.Text()) {
                                              return modules;
                                          }
                                          return self(std::string(imported), pending.manifest, pending.root);
                                      });
        failed = EmitAll(foldDiagnostics) || failed;
        loadingPackages.erase(name);
        packageOrder.push_back(name);
        return modules;
    };

    const auto modules = modulesOf(parseResults);
    std::vector<Diagnostic> foldDiagnostics;
    ResolveConditionalCompilation(modules, compileTimeContext, foldDiagnostics, [&](const std::string_view imported) {
        if (imported == opts.manifest.package.name.Text()) {
            return modules;
        }
        return loadPackage(std::string(imported), opts.manifest, root);
    });
    failed = EmitAll(foldDiagnostics) || failed;
    if (failed) {
        return false;
    }
    for (const std::string &name : packageOrder) {
        for (auto &parsed : parsedPackages.at(name)) {
            loadedModuleNames.push_back(parsed.module.name);
            depParseResults.push_back(std::move(parsed));
            loadedPackages.push_back(name);
        }
    }
    if (opts.dumpAst) {
        for (const auto &parsed : parseResults) {
            const auto relative = std::filesystem::relative(parsed.module.name, root / "Src");
            const auto path = (root / "Temp" / "Ast" / relative).replace_extension(".ast");
            if (!WriteInspectionOutput(InspectionKind::Ast, path, [&] { return Parser::DumpAst(parsed, path); })) {
                return false;
            }
        }
    }
    return true;
}

bool CompilerDriver::Impl::Analyze() {
    const auto semanticStart = std::chrono::steady_clock::now();
    BeginPhase(CompilePhase::Analyzing, opts.manifest.package.name.Text());
    std::vector<Module *> userModules;
    userModules.reserve(parseResults.size());
    for (auto &pr : parseResults) {
        userModules.push_back(&pr.module);
    }
    // Build per-package dep info so Sema can isolate imported package symbols.
    std::vector<DepPackage> depPackages;
    {
        std::unordered_map<std::string, std::size_t> pkgIdx;
        for (std::size_t i = 0; i < depParseResults.size(); ++i) {
            const std::string &pkgName = loadedPackages[i];
            auto [it, inserted] = pkgIdx.emplace(pkgName, depPackages.size());
            if (inserted) {
                depPackages.push_back({pkgName, {}});
            }
            depPackages[it->second].modules.push_back({loadedModuleNames[i], &depParseResults[i].module});
        }
    }
    SemanticAnalyzer analyzer(std::move(userModules), std::move(depPackages), opts.manifest.package.name.Text(),
                              compileTimeContext);
    semanticModel = analyzer.Analyze();
    EmitAll(semanticModel->diagnostics);
    if (opts.dumpSema) {
        const auto semaPath = root / "Temp" / "Sema" / "sema.txt";
        if (!WriteInspectionOutput(InspectionKind::Semantic, semaPath,
                                   [&] { return SemanticPrinter::Dump(*semanticModel, semaPath); })) {
            return false;
        }
    }
    if (semanticModel->HasErrors()) {
        hadErrors = true;
        return false;
    }
    stats.semantic = ElapsedMs(semanticStart);
    return true;
}

bool CompilerDriver::Impl::GenerateArtifact(std::filesystem::path &artifactPath,
                                            std::vector<std::filesystem::path> &secondaryArtifactPaths) {
    // HIR
    const auto hirStart = std::chrono::steady_clock::now();
    BeginPhase(CompilePhase::LoweringToHir, opts.manifest.package.name.Text());
    AstToHirLowering hirLowering(*semanticModel);
    auto hirPackage = hirLowering.Generate();
    if (EmitAll(hirLowering.Diagnostics())) {
        return false;
    }
    if (opts.dumpHir) {
        const auto hirPath = root / "Temp" / "Hir" / "hir.txt";
        if (!WriteInspectionOutput(InspectionKind::Hir, hirPath,
                                   [&] { return HirPrinter::Dump(hirPackage, hirPath); })) {
            return false;
        }
    }
    const ArtifactKind artifactKind = PackageArtifactKind(opts.manifest.package.type);
    auto optimizationPipeline =
        Optimization::OptimizationPipeline::ForProfile(compileTimeContext.profile, artifactKind);
    BeginPhase(CompilePhase::OptimizingHir, opts.manifest.package.name.Text());
    const auto hirOptimization = optimizationPipeline.RunHir(hirPackage);
    if (EmitAll(hirOptimization.diagnostics) || !hirOptimization.reachedFixedPoint) {
        return false;
    }
    stats.hir = ElapsedMs(hirStart);

    // LIR
    const auto lirStart = std::chrono::steady_clock::now();
    BeginPhase(CompilePhase::LoweringToLir, opts.manifest.package.name.Text());
    HirToLirLowering lirLowering(std::move(hirPackage), compileTimeContext.target);
    auto lirPackage = lirLowering.Generate();
    if (EmitAll(lirLowering.Diagnostics())) {
        return false;
    }
    BeginPhase(CompilePhase::OptimizingLir, opts.manifest.package.name.Text());
    const auto lirOptimization = optimizationPipeline.RunLir(lirPackage);
    if (EmitAll(lirOptimization.diagnostics)) {
        return false;
    }
    if (!lirOptimization.reachedFixedPoint) {
        return false;
    }
    stats.prunedFunctionDefinitions = lirOptimization.lirPruning.functionDefinitions;
    stats.prunedConstants = lirOptimization.lirPruning.constants;
    stats.prunedVtables = lirOptimization.lirPruning.vtables;
    stats.prunedExternDeclarations = lirOptimization.lirPruning.externDeclarations;
    stats.estimatedLirNodesEliminated = lirOptimization.lirPruning.estimatedIrNodes;
    if (opts.dumpLir) {
        const auto lirPath = root / "Temp" / "Lir" / "lir.txt";
        if (!WriteInspectionOutput(InspectionKind::Lir, lirPath,
                                   [&] { return LirPrinter::Dump(lirPackage, lirPath); })) {
            return false;
        }
    }
    stats.lir = ElapsedMs(lirStart);

    const auto codegenStart = std::chrono::steady_clock::now();
    const bool isAArch64 = compileTimeContext.target.arch == Target::Arch::AArch64;

    // Assembly dump (optional). AssemblyPrinter prints x86-64; the AArch64 back
    // end has no printer of its own yet.
    if (opts.dumpAsm && !isAArch64) {
        BeginPhase(CompilePhase::EmittingAssembly, opts.manifest.package.name.Text());
        const auto asmPath = root / "Temp" / "Asm" / "out.asm";
        if (!WriteInspectionOutput(InspectionKind::Assembly, asmPath, [&] {
                return AssemblyPrinter::Emit(lirPackage, asmPath, compileTimeContext.target.os);
            })) {
            return false;
        }
    }
    else if (opts.dumpAsm) {
        Diagnostic unavailable;
        unavailable.severity = Diagnostic::Severity::Warning;
        unavailable.message =
            std::format("assembly inspection output is unavailable for target '{}'", opts.target.CanonicalName());
        unavailable.notes.push_back("textual assembly inspection is currently supported only for x86-64 targets");
        Emit(unavailable);
    }

    // RCU object generation
    BeginPhase(CompilePhase::EmittingObjects, opts.manifest.package.name.Text());
    std::vector<RcuFile> rcuFiles;
    std::vector<Diagnostic> codegenDiagnostics;
    if (isAArch64) {
        AArch64RcuEmitter aarch64Emitter(lirPackage, std::string(opts.manifest.package.name.Text()),
                                         compileTimeContext.target.os, compileTimeContext.buildInfo);
        rcuFiles = aarch64Emitter.Generate();
        codegenDiagnostics = aarch64Emitter.Diagnostics();
    }
    else {
        RcuEmitter rcuEmitter(lirPackage, std::string(opts.manifest.package.name.Text()), compileTimeContext.target.os,
                              compileTimeContext.buildInfo);
        rcuFiles = rcuEmitter.Generate();
        codegenDiagnostics = rcuEmitter.Diagnostics();
    }
    {
        bool hasError = false;
        for (const auto &diag : codegenDiagnostics) {
            Emit(diag);
            hasError = hasError || diag.IsError();
        }
        if (hasError) {
            return false;
        }
    }
    if (opts.dumpRcu) {
        bool inspectionErrors = false;
        for (const auto &rcuFile : rcuFiles) {
            std::filesystem::path stem = rcuFile.sourcePath.empty() ? std::filesystem::path("out")
                                                                    : std::filesystem::path(rcuFile.sourcePath).stem();
            const auto objectPath = root / "Temp" / "Obj" / (stem.string() + ".rcu");
            const auto dumpPath = root / "Temp" / "Rcu" / (stem.string() + ".rcu.txt");
            inspectionErrors |= !WriteInspectionOutput(InspectionKind::RcuObject, objectPath,
                                                       [&] { return RcuWriter::Write(rcuFile, objectPath); });
            inspectionErrors |= !WriteInspectionOutput(InspectionKind::Rcu, dumpPath,
                                                       [&] { return RcuDumper::Dump(rcuFile, dumpPath); });
        }
        if (inspectionErrors) {
            return false;
        }
    }
    stats.codegen = ElapsedMs(codegenStart);

    // Link
    const auto linkingStart = std::chrono::steady_clock::now();
    BeginPhase(CompilePhase::Linking, opts.manifest.package.name.Text());
    const auto binDir = opts.isTest ? ResolveTestOutputDir(root, opts.manifest, opts.target)
                                    : ResolveArtifactOutputDir(root, opts.manifest, opts.profile, opts.target);
    const OS targetOs = opts.target.Os();
    const Arch targetArch = opts.target.Architecture();
    artifactPath = binDir / OutputFileName(opts.manifest.package.name.Text(), artifactKind, targetOs);
    std::error_code outputDirectoryError;
    std::filesystem::create_directories(binDir, outputDirectoryError);
    if (outputDirectoryError) {
        Emit(ErrorDiagnostic(
            std::format("cannot prepare artifact output directory '{}'", binDir.string()),
            {std::format("system error {}: {}", outputDirectoryError.value(), outputDirectoryError.message())},
            "check that the output path is writable and is not an existing file"));
        return false;
    }
    Linker linker(std::move(rcuFiles), std::string(opts.manifest.package.name.Text()), {root}, artifactKind, targetOs,
                  targetArch);
    if (!linker.Link(artifactPath)) {
        for (const auto &err : linker.Errors()) {
            Emit(ErrorDiagnostic(err.message, err.notes));
        }
        return false;
    }
    if (artifactKind == ArtifactKind::SharedLibrary && targetOs == OS::Windows) {
        secondaryArtifactPaths.push_back(binDir / StaticLibraryFileName(opts.manifest.package.name.Text(), targetOs));
    }
    stats.linking = ElapsedMs(linkingStart);
    return true;
}
} // namespace Rux::Driver
