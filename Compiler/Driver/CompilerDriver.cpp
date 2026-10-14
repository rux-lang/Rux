#include "BuildInfo/CompilerMetadata.h"
#include "CodeGen/AArch64/RcuEmitter.h"
#include "CodeGen/X86_64/AssemblyPrinter.h"
#include "CodeGen/X86_64/RcuEmitter.h"
#include "Driver/BuildCache.h"
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
#include "Package/Registry.h"
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

    // A test build of any package is an executable that runs its tests.
    if (opts.isTest) {
        compileTimeContext.outputKind = OutputKind::Executable;
    }
    else if (opts.manifest.package.type == ManifestPackageType::SharedLibrary) {
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

bool CompilerDriver::Impl::BuildCacheEligible() const {
    return opts.useBuildCache && !opts.captureFrontend && !opts.collectOptimizationStatistics && !opts.dumpTokens &&
           !opts.dumpAst && !opts.dumpSema && !opts.dumpHir && !opts.dumpLir && !opts.dumpOptimizationPasses &&
           !opts.dumpAsm && !opts.dumpRcu;
}

std::filesystem::path CompilerDriver::Impl::BuildRecordPath() const {
    return root / "Temp" / "Cache" /
           std::format("{}-{}{}{}.record", ToString(opts.profile), opts.target.CanonicalName(),
                       opts.isTest ? "-test" : "", opts.checkOnly ? "-check" : "");
}

std::string CompilerDriver::Impl::BuildKey() const {
    // Everything outside the sources that changes what a build writes. The compiler is identified by its own file, so
    // rebuilding it without changing its version still invalidates every record.
    std::string key = std::format("compiler {}\n", CompilerBuild::compilerVersion);
    const auto compiler = System::CurrentExecutablePath();
    std::error_code error;
    const auto compilerSize = std::filesystem::file_size(compiler, error);
    const auto compilerTime =
        error ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(compiler, error);
    key += std::format("executable {} {} {}\n", compiler.string(), error ? 0 : compilerSize,
                       compilerTime.time_since_epoch().count());
    key +=
        std::format("manifest {}\ntarget {}\nprofile {}\ntest {}\ncheck {}\nsymbols {}\n", opts.manifestPath.string(),
                    opts.target.CanonicalName(), ToString(opts.profile), opts.isTest, opts.checkOnly, opts.emitSymbols);
    for (const auto &[name, value] : opts.defines)
        key += std::format("define {}={}\n", name, value);
    for (const auto &pass : opts.disabledOptimizationPasses)
        key += std::format("disable {}\n", pass);
    for (const auto &localRoot : opts.localPackageRoots)
        key += std::format("local {}\n", localRoot.string());
    for (const auto &ns : opts.localNamespaces)
        key += std::format("namespace {}\n", ns);
    key += std::format("epoch {}\n", System::GetEnv("SOURCE_DATE_EPOCH").value_or("-"));
    return HashText(key);
}

bool CompilerDriver::Impl::ReuseRecordedBuild(CompileResult &result) {
    const auto record = ReadBuildRecord(BuildRecordPath());
    // A check writes nothing, so its record names no artifact; a build's always names one.
    if (!record || record->artifacts.empty() != opts.checkOnly || record->key != BuildKey() || !IsUpToDate(*record))
        return false;
    stats = record->stats;
    stats.upToDate = true;
    if (!record->artifacts.empty())
        result.primaryArtifactPath = record->artifacts.front().path;
    for (std::size_t index = 1; index < record->artifacts.size(); ++index)
        result.secondaryArtifactPaths.push_back(record->artifacts[index].path);
    result.ok = true;
    return true;
}

void CompilerDriver::Impl::RecordBuild(const std::filesystem::path &artifactPath,
                                       const std::vector<std::filesystem::path> &secondaryArtifactPaths) {
    BuildRecord record;
    record.key = BuildKey();
    record.stats = stats;
    for (const auto &[path, text] : loadedSourceTexts) {
        const auto &[size, modified] = loadedSourceStamps.at(path);
        record.inputs.push_back({path, size, modified, HashText(text.Text())});
    }
    auto manifests = dependencyGraph->ManifestPaths();
    manifests.push_back(opts.manifestPath);
    std::ranges::sort(manifests);
    manifests.erase(std::ranges::unique(manifests).begin(), manifests.end());
    for (const auto &manifest : manifests) {
        auto file = RecordFile(manifest);
        if (!file)
            return;
        record.inputs.push_back(std::move(*file));
    }
    std::ranges::sort(record.inputs, {}, &BuildRecord::File::path);
    // Sources are discovered by listing each package's Src tree, and a registry package is chosen among the versions
    // beside it, so a file or version that appears changes what the build would read.
    record.directories.push_back({root / "Src", DirectoryListing(root / "Src", true), true});
    for (const auto &[id, package] : dependencyGraph->Packages()) {
        if (package.Root() == root)
            continue;
        record.directories.push_back({package.Root() / "Src", DirectoryListing(package.Root() / "Src", true), true});
        record.directories.push_back(
            {package.Root().parent_path(), DirectoryListing(package.Root().parent_path(), false), false});
    }
    std::vector<std::filesystem::path> artifacts;
    if (!artifactPath.empty())
        artifacts.push_back(artifactPath);
    artifacts.insert(artifacts.end(), secondaryArtifactPaths.begin(), secondaryArtifactPaths.end());
    for (const auto &artifact : artifacts) {
        std::error_code error;
        const auto size = std::filesystem::file_size(artifact, error);
        const auto modified =
            error ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(artifact, error);
        if (error)
            return;
        record.artifacts.push_back(
            {artifact, size, static_cast<std::int64_t>(modified.time_since_epoch().count()), std::string{}});
    }
    static_cast<void>(WriteBuildRecord(BuildRecordPath(), record));
}

Parallel::WorkerPool &CompilerDriver::Impl::Workers() {
    if (!workers)
        workers = std::make_unique<Parallel::WorkerPool>(Parallel::ResolveJobs(opts.jobs));
    return *workers;
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
        else {
            // Stamped as soon as it is read, so an edit made while the build runs is not mistaken for what was built.
            std::error_code error;
            const auto size = std::filesystem::file_size(file.path, error);
            const auto modified =
                error ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(file.path, error);
            loadedSourceStamps.emplace(stored->first,
                                       std::pair{error ? ~std::uintmax_t{0} : size,
                                                 static_cast<std::int64_t>(modified.time_since_epoch().count())});
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
    // `MinRux` names the oldest compiler that can build the package, so an older one stops before reading a source.
    if (auto tooOld = Packages::CompilerTooOldMessage(opts.manifest.package.name.Text(), opts.manifest.header.minRux)) {
        auto diagnostic = ErrorDiagnostic(std::move(*tooOld),
                                          {"[Manifest].MinRux names the oldest compiler that can build the package"},
                                          "install a newer Rux release to build this package");
        diagnostic.sourceName = opts.manifestPath.string();
        Emit(diagnostic);
        finish();
        return result;
    }
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
    if (!opts.checkOnly && !opts.isTest && opts.manifest.package.type == ManifestPackageType::SourceLibrary) {
        Emit(ErrorDiagnostic(
            "package '" + opts.manifest.package.name.Text() +
            "' has Type = \"SourceLibrary\" and is compiled into dependent packages; it cannot be built or "
            "run as a top-level target"));
        finish();
        return result;
    }
    if (BuildCacheEligible() && ReuseRecordedBuild(result)) {
        finish();
        return result;
    }

    if (!LexAndParseSources()) {
        finish();
        return result;
    }
    if (opts.isTest && !SynthesizeTestEntry()) {
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
        // Only a check that reported nothing is recorded, so a warning is reported again on every check.
        if (BuildCacheEligible() && diagnostics.empty()) {
            RecordBuild({}, {});
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
    if (BuildCacheEligible()) {
        RecordBuild(artifactPath, result.secondaryArtifactPaths);
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

    // Lex. Files are independent, so they are lexed on the worker threads and reported in file order.
    bool lexErrors = false;
    bool inspectionErrors = false;
    std::vector<LexerResult> lexResults(sourceFiles.size());
    const auto lexingStart = std::chrono::steady_clock::now();
    Workers().ForEach(sourceFiles.size(), [&](const std::size_t index) {
        lexResults[index] = Lexer::TokenizeSource(sourceFiles[index].source.Text(), sourceFiles[index].path.string());
    });
    for (std::size_t fileIndex = 0; fileIndex < sourceFiles.size(); ++fileIndex) {
        const auto &file = sourceFiles[fileIndex];
        auto &lexResult = lexResults[fileIndex];
        BeginPhase(CompilePhase::Lexing, file.path.string(), file.path);
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
    std::vector<std::optional<ParseResult>> parsedFiles(sourceFiles.size());
    Workers().ForEach(sourceFiles.size(), [&](const std::size_t index) {
        if (lexResults[index].HasErrors())
            return;
        Parser parser(std::move(lexResults[index].tokens), sourceFiles[index].path.string(),
                      compileTimeContext.target.arch);
        parsedFiles[index] = parser.Parse();
    });
    for (std::size_t fileIndex = 0; fileIndex < sourceFiles.size(); ++fileIndex) {
        const auto &file = sourceFiles[fileIndex];
        BeginPhase(CompilePhase::Parsing, file.path.string(), file.path);
        if (!parsedFiles[fileIndex]) {
            continue;
        }
        auto &parseResult = *parsedFiles[fileIndex];
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

    dependencyGraph.emplace(opts.manifest, opts.manifestPath, opts.target, opts.localPackageRoots,
                            opts.localNamespaces);
    auto &graph = *dependencyGraph;

    // Packages are parsed into stable storage before their conditionals ask for imported declarations.
    // The resolver loads only imports in surviving branches, including imports needed by a condition itself.
    std::unordered_map<std::string, std::vector<ParseResult>> parsedPackages;
    std::unordered_set<std::string> loadingPackages;
    std::vector<std::string> packageOrder;
    std::unordered_map<std::string, const SourcePackage *> sourceOwners;
    for (const auto &parsed : parseResults)
        sourceOwners.emplace(parsed.module.name, &graph.Root());
    bool failed = false;
    const auto modulesOf = [](std::vector<ParseResult> &parsed) {
        std::vector<Module *> modules;
        for (auto &result : parsed) {
            modules.push_back(&result.module);
        }
        return modules;
    };
    const auto loadPackage = [&](this auto &&self, const std::string_view importName,
                                 const SourcePackage &owner) -> std::vector<Module *> {
        if (failed)
            return {};
        const auto *selected = graph.Resolve(owner, importName);
        if (!selected) {
            failed = true;
            return {};
        }
        const auto &name = selected->id;
        if (selected == &owner) {
            return modulesOf(name == graph.Root().id ? parseResults : parsedPackages.at(name));
        }
        if (loadingPackages.contains(name) || name == graph.Root().id) {
            Emit(ErrorDiagnostic("cyclic dependency while resolving declarations from package '" +
                                     std::string(importName) + "'",
                                 {"selected manifest: " + selected->manifestPath.string()}));
            failed = true;
            return {};
        }
        if (const auto loaded = parsedPackages.find(name); loaded != parsedPackages.end())
            return modulesOf(loaded->second);
        loadingPackages.insert(name);
        BeginPhase(CompilePhase::LoadingDependency, importName, selected->Root());
        auto loaded = SourceLoader::Load(selected->Root());
        const auto files = RememberSources(loaded.files);
        stats.dependencyFiles += files.size();
        if (EmitAll(loaded.diagnostics)) {
            failed = true;
            return {};
        }
        auto &parsed = parsedPackages[name];
        parsed.reserve(files.size());
        // The package's files are lexed and parsed on the worker threads, then reported in file order, stopping at
        // the first file that fails as a one-at-a-time load would.
        std::vector<LexerResult> lexedFiles(files.size());
        const auto lexingStart = std::chrono::steady_clock::now();
        Workers().ForEach(files.size(), [&](const std::size_t index) {
            lexedFiles[index] = Lexer::TokenizeSource(files[index].source.Text(), files[index].path.string());
        });
        stats.lexing += ElapsedMs(lexingStart);
        std::vector<std::size_t> tokenCounts(files.size());
        std::vector<std::optional<ParseResult>> parsedFiles(files.size());
        const auto parsingStart = std::chrono::steady_clock::now();
        Workers().ForEach(files.size(), [&](const std::size_t index) {
            tokenCounts[index] = CountTokens(lexedFiles[index].tokens);
            if (lexedFiles[index].HasErrors())
                return;
            Parser parser(std::move(lexedFiles[index].tokens), files[index].path.string(),
                          compileTimeContext.target.arch);
            parsedFiles[index] = parser.Parse();
        });
        stats.parsing += ElapsedMs(parsingStart);
        for (std::size_t index = 0; index < files.size(); ++index) {
            const auto &file = files[index];
            stats.dependencyLines += CountLines(file.source.Text());
            stats.dependencySourceSize += file.source.Text().size();
            stats.dependencyTokens += tokenCounts[index];
            if (EmitAll(lexedFiles[index].diagnostics)) {
                failed = true;
                return {};
            }
            if (EmitAll(parsedFiles[index]->diagnostics)) {
                failed = true;
                return {};
            }
            parsed.push_back(std::move(*parsedFiles[index]));
        }
        const auto modules = modulesOf(parsed);
        for (const auto *module : modules)
            sourceOwners.emplace(module->name, selected);
        std::vector<Diagnostic> foldDiagnostics;
        ResolveConditionalCompilation(modules, compileTimeContext, foldDiagnostics,
                                      [&](const std::string_view imported, const std::string_view source) {
                                          return self(imported, *sourceOwners.at(std::string(source)));
                                      });
        failed = EmitAll(foldDiagnostics) || failed;
        loadingPackages.erase(name);
        packageOrder.push_back(name);
        return modules;
    };

    const auto modules = modulesOf(parseResults);
    std::vector<Diagnostic> foldDiagnostics;
    ResolveConditionalCompilation(modules, compileTimeContext, foldDiagnostics,
                                  [&](const std::string_view imported, const std::string_view source) {
                                      return loadPackage(imported, *sourceOwners.at(std::string(source)));
                                  });
    failed = EmitAll(foldDiagnostics) || failed;
    failed = EmitAll(graph.Diagnostics()) || failed;
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
                std::string displayName = pkgName;
                if (const Driver::SourcePackage *source = dependencyGraph->Find(pkgName)) {
                    const auto &package = source->manifest.package;
                    displayName = package.ns ? package.ns->Text() + "/" + package.name.Text() : package.name.Text();
                }
                depPackages.push_back({pkgName, {}, std::move(displayName)});
            }
            depPackages[it->second].modules.push_back({loadedModuleNames[i], &depParseResults[i].module});
        }
    }
    SemanticAnalyzer analyzer(std::move(userModules), std::move(depPackages), dependencyGraph->Root().id,
                              compileTimeContext, dependencyGraph->Bindings());
    // LoadDependencies folded every package's conditionals while it decided which imports to load.
    analyzer.SetConditionalsResolved();
    analyzer.SetWorkerPool(&Workers());
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
    hirLowering.SetWorkerPool(&Workers());
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
    const ArtifactKind artifactKind =
        opts.isTest ? ArtifactKind::Executable : PackageArtifactKind(opts.manifest.package.type);
    auto optimizationPipeline =
        Optimization::OptimizationPipeline::ForProfile(compileTimeContext.profile, artifactKind);
    if (EmitAll(optimizationPipeline.Configure(opts.disabledOptimizationPasses, opts.collectOptimizationStatistics))) {
        return false;
    }
    optimizationPipeline.SetWorkerPool(&Workers());
    if (opts.dumpOptimizationPasses) {
        // Retain each invocation independently: stale files from a longer earlier pipeline must not masquerade as
        // snapshots of this build. Refuse an existing destination instead of overwriting another invocation.
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = root / "Temp" / "Optimization" / std::to_string(nonce);
        std::error_code error;
        if (!std::filesystem::create_directories(directory, error)) {
            Emit(ErrorDiagnostic(
                std::format("could not create optimization snapshot directory '{}'", directory.string()),
                {error ? error.message() : "destination already exists"}));
            return false;
        }
        optimizationPipeline.SetObservers(
            [this, directory](const HirPackage &package, const std::string_view name, const std::size_t iteration,
                              const std::size_t passIndex) {
                const auto path = directory / "Hir" / std::format("{:02}-{:02}-{}.txt", iteration, passIndex, name);
                return WriteInspectionOutput(InspectionKind::Hir, path,
                                             [&] { return HirPrinter::Dump(package, path); });
            },
            [this, directory](const LirPackage &package, const std::string_view name, const std::size_t iteration,
                              const std::size_t passIndex) {
                const auto path = directory / "Lir" / std::format("{:02}-{:02}-{}.txt", iteration, passIndex, name);
                return WriteInspectionOutput(InspectionKind::Lir, path,
                                             [&] { return LirPrinter::Dump(package, path); });
            });
    }
    BeginPhase(CompilePhase::OptimizingHir, opts.manifest.package.name.Text());
    const auto hirOptimization = optimizationPipeline.RunHir(hirPackage);
    stats.optimizationPasses = hirOptimization.passStatistics;
    if (EmitAll(hirOptimization.diagnostics) || !hirOptimization.reachedFixedPoint) {
        return false;
    }
    stats.hir = ElapsedMs(hirStart);

    // LIR
    const auto lirStart = std::chrono::steady_clock::now();
    BeginPhase(CompilePhase::LoweringToLir, opts.manifest.package.name.Text());
    HirToLirLowering lirLowering(std::move(hirPackage), compileTimeContext.target);
    lirLowering.SetWorkerPool(&Workers());
    lirLowering.SetProgramModule(true);
    auto lirPackage = lirLowering.Generate();
    if (EmitAll(lirLowering.Diagnostics())) {
        return false;
    }
    BeginPhase(CompilePhase::OptimizingLir, opts.manifest.package.name.Text());
    const auto lirOptimization = optimizationPipeline.RunLir(lirPackage);
    stats.optimizationPasses.insert(stats.optimizationPasses.end(), lirOptimization.passStatistics.begin(),
                                    lirOptimization.passStatistics.end());
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
                return AssemblyPrinter::Emit(lirPackage, asmPath, compileTimeContext.target.os,
                                             compileTimeContext.profile == BuildProfile::Release);
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
        aarch64Emitter.SetMachineSelection(compileTimeContext.profile == BuildProfile::Release);
        aarch64Emitter.SetWorkerPool(&Workers());
        aarch64Emitter.SetBacktraces(true);
        rcuFiles = aarch64Emitter.Generate();
        codegenDiagnostics = aarch64Emitter.Diagnostics();
    }
    else {
        RcuEmitter rcuEmitter(lirPackage, std::string(opts.manifest.package.name.Text()), compileTimeContext.target.os,
                              compileTimeContext.buildInfo);
        rcuEmitter.SetMachineSelection(compileTimeContext.profile == BuildProfile::Release);
        rcuEmitter.SetWorkerPool(&Workers());
        rcuEmitter.SetBacktraces(true);
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
    linker.SetEmitSymbols(opts.emitSymbols);
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
