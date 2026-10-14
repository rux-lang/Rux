#include "BuildInfo/CompilerMetadata.h"
#include "CodeGen/AArch64/RcuEmitter.h"
#include "CodeGen/X86_64/AssemblyPrinter.h"
#include "CodeGen/X86_64/RcuEmitter.h"
#include "Driver/BuildCache.h"
#include "Driver/BuildTarget.h"
#include "Driver/CompileContext.h"
#include "Driver/ModuleCache.h"
#include "Driver/PackageCache.h"
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
#include "Object/Rcu/RcuSerialization.h"
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
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <format>
#include <fstream>
#include <iterator>
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
    if (opts.buildTimestamp) {
        buildTimestamp = *opts.buildTimestamp;
    }
    else if (const auto epoch = System::GetEnv("SOURCE_DATE_EPOCH")) {
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

bool CompilerDriver::Impl::ModuleCacheEligible() const {
    // A check compiles nothing, and inspection output shows the IR of every module.
    return opts.useBuildCache && !opts.checkOnly && !opts.captureFrontend && !opts.dumpTokens && !opts.dumpAst &&
           !opts.dumpSema && !opts.dumpHir && !opts.dumpLir && !opts.dumpOptimizationPasses && !opts.dumpAsm &&
           !opts.dumpRcu;
}

std::filesystem::path CompilerDriver::Impl::ModulePackPath() const {
    auto path = BuildRecordPath();
    path.replace_extension(".modules");
    return path;
}

void CompilerDriver::Impl::PrepareModuleReuse() {
    if (!ModuleCacheEligible())
        return;
    std::vector<const Module *> modules;
    modules.reserve(depParseResults.size() + parseResults.size());
    for (const auto &parsed : depParseResults)
        modules.push_back(&parsed.module);
    for (const auto &parsed : parseResults)
        modules.push_back(&parsed.module);
    moduleReuse.pack = ModulePack::Read(ModulePackPath());
    // A file whose size and modification time are what the pack recorded keeps the digests recorded with it, as an
    // up-to-date build record trusts them; any other file is hashed.
    std::vector<ModuleReuse::SourceDigests> digests(modules.size());
    Workers().ForEach(modules.size(), [&](const std::size_t index) {
        const std::string &name = modules[index]->name;
        const auto text = loadedSourceTexts.find(name);
        if (text == loadedSourceTexts.end())
            return;
        auto &digest = digests[index];
        if (const auto stamp = loadedSourceStamps.find(name); stamp != loadedSourceStamps.end()) {
            digest.size = stamp->second.first;
            digest.modified = stamp->second.second;
        }
        const CachedModule *recorded = moduleReuse.pack.FindAny(name);
        if (recorded != nullptr && digest.modified != 0 && recorded->sourceSize == digest.size &&
            recorded->sourceModified == digest.modified && !recorded->sourceDigest.empty()) {
            digest.source = recorded->sourceDigest;
            digest.interface = recorded->interfaceDigest;
            return;
        }
        digest.source = HashText(text->second.Text());
        digest.interface = HashText(ModuleInterface(*modules[index], text->second.Text()));
    });
    // The program's interface: what every module declares, in name order, under the request this build serves.
    std::vector<std::size_t> byName(modules.size());
    for (std::size_t index = 0; index < byName.size(); ++index)
        byName[index] = index;
    std::ranges::sort(byName, {}, [&](const std::size_t index) { return std::string_view(modules[index]->name); });
    std::string program = BuildKey() + "\n";
    // Manifests decide what is compiled beyond the sources: the artifact kind, and the configuration `when` folds
    // against, which the interface text, holding every branch, does not show.
    for (const auto &manifest : dependencyGraph->ManifestPaths()) {
        std::ifstream stream(manifest, std::ios::binary);
        const std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        program += std::format("manifest {} {}\n", HashText(content), manifest.string());
    }
    for (const std::size_t index : byName)
        program += std::format("{} {}\n", digests[index].interface.empty() ? "-" : digests[index].interface,
                               modules[index]->name);
    const std::string programKey = HashText(program);
    moduleReuse.keys.clear();
    moduleReuse.digests.clear();
    for (std::size_t index = 0; index < modules.size(); ++index) {
        if (digests[index].source.empty())
            continue;
        moduleReuse.keys.emplace(modules[index], HashText(std::format("{}\n{}\n{}", programKey, modules[index]->name,
                                                                      digests[index].source)));
        moduleReuse.digests.emplace(modules[index], std::move(digests[index]));
    }
    moduleReuse.enabled = true;
    PreparePackageReuse();
    SelectReusedModules();
}

std::string CompilerDriver::Impl::PackageContextKey() const {
    // Everything but its sources and its place that changes what a dependency package compiles to: the compiler, the
    // target and profile, and the compile-time context its `when` branches and intrinsics read.
    std::string key = std::format("compiler {}\n", CompilerBuild::compilerVersion);
    const auto compiler = System::CurrentExecutablePath();
    std::error_code error;
    const auto compilerSize = std::filesystem::file_size(compiler, error);
    const auto compilerTime =
        error ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(compiler, error);
    key += std::format("executable {} {} {}\n", compiler.string(), error ? 0 : compilerSize,
                       compilerTime.time_since_epoch().count());
    key += std::format("target {}\nprofile {}\ntest {}\noutput {}\nsymbols {}\n", opts.target.CanonicalName(),
                       ToString(opts.profile), opts.isTest, static_cast<int>(compileTimeContext.outputKind),
                       opts.emitSymbols);
    for (const auto &[name, value] : compileTimeContext.config)
        key += std::format("config {}={}\n", name, value);
    for (const auto &pass : opts.disabledOptimizationPasses)
        key += std::format("disable {}\n", pass);
    key += std::format("epoch {}\n", System::GetEnv("SOURCE_DATE_EPOCH").value_or("-"));
    return key;
}

void CompilerDriver::Impl::PreparePackageReuse() {
    moduleReuse.packages.clear();
    std::unordered_map<std::string, std::size_t> indexOf;
    for (std::size_t index = 0; index < depParseResults.size(); ++index) {
        const auto [found, inserted] = indexOf.try_emplace(loadedPackages[index], moduleReuse.packages.size());
        if (inserted) {
            auto &package = moduleReuse.packages.emplace_back();
            package.id = loadedPackages[index];
        }
        moduleReuse.packages[found->second].modules.push_back(&depParseResults[index].module);
    }
    const std::string context = PackageContextKey();
    const auto cache = PackageCacheDirectory(opts.packageCacheDirectory);
    std::unordered_map<std::string, std::string> keys;
    for (auto &package : moduleReuse.packages) {
        if (const auto bindings = dependencyGraph->Bindings().find(package.id);
            bindings != dependencyGraph->Bindings().end()) {
            for (const auto &[alias, id] : bindings->second)
                if (id != package.id && indexOf.contains(id))
                    package.dependencies.push_back(id);
        }
        std::ranges::sort(package.dependencies);
        package.dependencies.erase(std::ranges::unique(package.dependencies).begin(), package.dependencies.end());
        const SourcePackage *source = dependencyGraph->Find(package.id);
        bool keyed = source != nullptr;
        std::string text = context + std::format("package {}\n", package.id);
        if (source != nullptr) {
            std::ifstream stream(source->manifestPath, std::ios::binary);
            const std::string manifest((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
            // Where the package lies decides the paths its code reports: relative to the package being built when it
            // lies inside it, else absolute.
            const auto packageRoot = source->Root().lexically_normal();
            const auto relative = packageRoot.lexically_relative(compileTimeContext.sourceRoot);
            const bool inside = !relative.empty() && relative.begin()->generic_string() != "..";
            text += std::format("manifest {}\nroot {}\nreported {}\n", HashText(manifest), packageRoot.generic_string(),
                                inside ? relative.generic_string() : std::string("absolute"));
        }
        for (const Module *module : package.modules) {
            const auto digest = moduleReuse.digests.find(module);
            if (digest == moduleReuse.digests.end() || digest->second.source.empty()) {
                keyed = false;
                break;
            }
            text += std::format("module {} {}\n", digest->second.source, module->name);
        }
        for (const auto &dependency : package.dependencies) {
            const auto key = keys.find(dependency);
            if (key == keys.end() || key->second.empty()) {
                keyed = false;
                break;
            }
            text += std::format("dependency {}\n", key->second);
        }
        if (!keyed) {
            keys[package.id] = {};
            continue;
        }
        package.key = HashText(text);
        keys[package.id] = package.key;
        package.directory = cache / package.key.substr(0, 32);
        package.variant = ReadNewestPackageVariant(package.directory);
    }
}

bool CompilerDriver::Impl::RejectStalePackages() {
    std::unordered_map<std::string, std::string> names;
    bool stale = false;
    for (auto &package : moduleReuse.packages) {
        std::string text = NamesDigest(*semanticModel, package.modules) + "\n";
        for (const auto &dependency : package.dependencies)
            if (const auto found = names.find(dependency); found != names.end())
                text += found->second + "\n";
        package.names = HashText(text);
        names[package.id] = package.names;
        if (!package.variant)
            continue;
        const bool reusedUnderOtherNames = std::ranges::any_of(package.modules, [&](const Module *module) {
            const auto cached = moduleReuse.cached.find(module);
            const CachedModule *entry = package.variant->FindAny(module->name);
            return cached != moduleReuse.cached.end() && cached->second == entry && entry->key != package.names;
        });
        if (reusedUnderOtherNames) {
            for (const Module *module : package.modules)
                moduleReuse.fresh.insert(module->name);
            stale = true;
        }
    }
    return stale;
}

bool CompilerDriver::Impl::AnalyzeReusing() {
    if (!Analyze())
        return false;
    while (!moduleReuse.packages.empty() && RejectStalePackages()) {
        // A package was kept under other names than this program gives its declarations, so its code would not link
        // here: it is analyzed again whole, and compiled.
        repeatingAnalysis = true;
        SelectReusedModules();
        if (!Analyze())
            return false;
    }
    return true;
}

bool CompilerDriver::Impl::VerifyingModuleReuse() {
    const auto value = System::GetEnv("RUX_VERIFY_MODULE_CACHE");
    return value.has_value() && !value->empty() && *value != "0";
}

bool CompilerDriver::Impl::VerifyReusedArtifact(const std::filesystem::path &artifactPath) {
    const auto read = [](const std::filesystem::path &path) {
        std::ifstream stream(path, std::ios::binary);
        return std::vector<char>((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    };
    const auto reused = read(artifactPath);
    CompileOptions fresh = opts;
    fresh.useBuildCache = false;
    // At the same time, so code that holds the build's time compiles the same.
    fresh.buildTimestamp = compileTimeContext.buildInfo.Timestamp();
    fresh.emitDiagnostic = nullptr;
    fresh.emitProgress = nullptr;
    const auto compiled = CompilerDriver(std::move(fresh)).Compile();
    if (!compiled.ok) {
        Emit(ErrorDiagnostic("module cache verification: the package does not compile without reusing modules"));
        return false;
    }
    const bool same = reused == read(compiled.primaryArtifactPath);
    if (!same) {
        Emit(ErrorDiagnostic(
            std::format("module cache verification: reusing {} modules produced a different artifact than compiling "
                        "every module",
                        moduleReuse.cached.size()),
            {std::format("artifact: '{}'", artifactPath.string())}));
        return false;
    }
    return true;
}

void CompilerDriver::Impl::SelectReusedModules() {
    moduleReuse.declarationOnly.clear();
    moduleReuse.cached.clear();
    // `RUX_VERIFY_MODULE_CACHE=user` compiles the package's own modules on every build and reuses its dependencies,
    // which is how most edits are rebuilt.
    std::unordered_set<const Module *> own;
    if (System::GetEnv("RUX_VERIFY_MODULE_CACHE") == std::optional<std::string>("user"))
        for (const auto &parsed : parseResults)
            own.insert(&parsed.module);
    for (const auto &[module, key] : moduleReuse.keys) {
        // A body that declares items adds to the program's indexes, so such a module is always analyzed whole.
        if (module->bodiesDeclareItems || moduleReuse.fresh.contains(module->name) || own.contains(module))
            continue;
        if (const CachedModule *entry = moduleReuse.pack.Find(module->name, key)) {
            moduleReuse.declarationOnly.insert(module);
            moduleReuse.cached.emplace(module, entry);
        }
    }
    // A dependency package kept in the shared cache supplies every module of it the pack did not, whole, as its names
    // are checked for the package as one.
    for (const auto &package : moduleReuse.packages) {
        if (!package.variant)
            continue;
        const bool whole = std::ranges::all_of(package.modules, [&](const Module *module) {
            return !module->bodiesDeclareItems && !moduleReuse.fresh.contains(module->name) &&
                   package.variant->FindAny(module->name) != nullptr;
        });
        if (!whole)
            continue;
        for (const Module *module : package.modules)
            if (moduleReuse.cached.try_emplace(module, package.variant->FindAny(module->name)).second)
                moduleReuse.declarationOnly.insert(module);
    }
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
    // The digests module reuse already took are the same SHA-256 of the same text; the rest are hashed side by side.
    std::unordered_map<std::string_view, const std::string *> knownDigests;
    for (const auto &[module, digest] : moduleReuse.digests)
        knownDigests.emplace(module->name, &digest.source);
    std::vector<std::pair<const std::string *, const SourceText *>> sources;
    sources.reserve(loadedSourceTexts.size());
    for (const auto &[path, text] : loadedSourceTexts)
        sources.emplace_back(&path, &text);
    std::vector<BuildRecord::File> inputs(sources.size());
    Workers().ForEach(sources.size(), [&](const std::size_t index) {
        const auto &[path, text] = sources[index];
        const auto &[size, modified] = loadedSourceStamps.at(*path);
        const auto known = knownDigests.find(*path);
        inputs[index] = {*path, size, modified, known != knownDigests.end() ? *known->second : HashText(text->Text())};
    });
    record.inputs = std::move(inputs);
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
    if (repeatingAnalysis && std::ranges::any_of(diagnostics, [&](const Diagnostic &earlier) {
            return earlier.severity == diag.severity && earlier.sourceName == diag.sourceName &&
                   earlier.location == diag.location && earlier.message == diag.message;
        }))
        return;
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
            // The loader stamped the file before reading it, so an edit made while the build runs is not mistaken
            // for what was built.
            auto modified = file.modified;
            // A file written moments ago may be written again within the same tick of the file clock without its
            // size changing, and its stamp would then say it never changed. Such a file is stamped with no time, so
            // the next build compares its content instead of trusting the stamp.
            if (modified > std::filesystem::file_time_type::clock::now() - std::chrono::seconds(2))
                modified = std::filesystem::file_time_type{};
            loadedSourceStamps.emplace(
                stored->first, std::pair{file.size, static_cast<std::int64_t>(modified.time_since_epoch().count())});
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
    if (BuildCacheEligible() && !VerifyingModuleReuse() && ReuseRecordedBuild(result)) {
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
    const auto dependenciesStart = std::chrono::steady_clock::now();
    const auto frontEndBefore = stats.lexing + stats.parsing;
    const bool dependenciesLoaded = LoadDependencies();
    stats.dependencies = std::max(std::chrono::milliseconds{0},
                                  ElapsedMs(dependenciesStart) - (stats.lexing + stats.parsing - frontEndBefore));
    if (!dependenciesLoaded) {
        finish();
        return result;
    }
    // In checkOnly mode frontend errors are accumulated instead of aborting;
    // semantic analysis only runs on a package whose frontend is clean.
    if (hadErrors) {
        finish();
        return result;
    }
    const auto reuseStart = std::chrono::steady_clock::now();
    PrepareModuleReuse();
    stats.reuse = ElapsedMs(reuseStart);
    if (!AnalyzeReusing()) {
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
    while (!GenerateArtifact(artifactPath, result.secondaryArtifactPaths)) {
        if (!retryWithFreshModules) {
            finish();
            return result;
        }
        // A reused module is needed differently than when it was compiled: analyze the program again, compiling it.
        retryWithFreshModules = false;
        repeatingAnalysis = true;
        SelectReusedModules();
        result.secondaryArtifactPaths.clear();
        if (!AnalyzeReusing()) {
            finish();
            return result;
        }
    }

    if (moduleReuse.enabled && !moduleReuse.cached.empty() && VerifyingModuleReuse() &&
        !VerifyReusedArtifact(artifactPath)) {
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
    auto loadResult = SourceLoader::Load(root, &Workers());
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
        auto loaded = SourceLoader::Load(selected->Root(), &Workers());
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
    if (moduleReuse.enabled)
        analyzer.SetDeclarationOnlyModules(moduleReuse.declarationOnly);
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
    if (moduleReuse.enabled)
        hirLowering.SetDeclarationOnlyModules(moduleReuse.declarationOnly);
    auto hirPackage = hirLowering.Generate();
    if (EmitAll(hirLowering.Diagnostics())) {
        return false;
    }
    // Code that holds the build's time is never kept for another build. A constant holding it may be inlined into any
    // module, so then no module is.
    const bool constantReadsBuildTime = std::ranges::any_of(hirPackage.modules, [](const HirModule &module) {
        return std::ranges::any_of(module.consts, &HirConst::readsBuildTime);
    });
    std::vector<bool> readsBuildTime;
    readsBuildTime.reserve(hirPackage.modules.size());
    for (const HirModule &module : hirPackage.modules)
        readsBuildTime.push_back(constantReadsBuildTime || module.readsBuildTime);
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
    // Nothing after HIR reads the semantic model but its module order, so its memory serves the stages that follow.
    // Analyzing again, as a retry does, builds a new one.
    const std::vector<const Module *> analyzedModules = semanticModel->modules;
    semanticModel.reset();
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
    std::optional<HirToLirLowering> hirToLir(std::in_place, std::move(hirPackage), compileTimeContext.target);
    HirToLirLowering &lirLowering = *hirToLir;
    lirLowering.SetWorkerPool(&Workers());
    lirLowering.SetProgramModule(true);
    // A reused module is not lowered again, so whether it can fail is what was recorded with it.
    lirLowering.SetExternalFailures(
        std::ranges::any_of(moduleReuse.cached, [](const auto &entry) { return entry.second->fails; }));
    auto lirPackage = lirLowering.Generate();
    if (EmitAll(lirLowering.Diagnostics())) {
        return false;
    }
    // The HIR the lowering holds is not read again.
    const std::vector<char> moduleFailures = lirLowering.ModuleFailures();
    lirLowering.ReleaseHir();
    hirToLir.reset();
    BeginPhase(CompilePhase::OptimizingLir, opts.manifest.package.name.Text());
    // The source module each LIR module was lowered from, in package order, and the recorded entry of every one whose
    // object this build reuses. The program module that follows them is compiled on every build.
    const std::size_t moduleCount = lirPackage.modules.size();
    std::vector<const Module *> sourceModules(moduleCount, nullptr);
    std::vector<const CachedModule *> reused(moduleCount, nullptr);
    for (std::size_t index = 0; index < std::min(moduleCount, analyzedModules.size()); ++index) {
        sourceModules[index] = analyzedModules[index];
        if (const auto found = moduleReuse.cached.find(sourceModules[index]); found != moduleReuse.cached.end())
            reused[index] = found->second;
    }
    std::unordered_set<const Module *> ownModules;
    for (const auto &parsed : parseResults)
        ownModules.insert(&parsed.module);
    stats.localReusedFiles = 0;
    stats.dependencyReusedFiles = 0;
    for (std::size_t index = 0; index < moduleCount; ++index)
        if (reused[index] != nullptr)
            ++(ownModules.contains(sourceModules[index]) ? stats.localReusedFiles : stats.dependencyReusedFiles);
    const bool release = compileTimeContext.profile == BuildProfile::Release;
    std::vector<Optimization::LirModuleReferences> references(moduleCount);
    std::vector<std::vector<Optimization::LirModuleRoot>> roots;
    if (release) {
        // Each module is pruned from what the rest of the program needs of it, as the unoptimized code says, so its
        // optimized code never depends on how the other modules were optimized. A reused module was not lowered
        // again, so what its code references is what was recorded with it.
        std::vector<const Optimization::LirModuleReferences *> moduleReferences(moduleCount);
        Workers().ForEach(moduleCount, [&](const std::size_t index) {
            if (reused[index] == nullptr)
                references[index] = Optimization::CollectModuleReferences(lirPackage.modules[index]);
            moduleReferences[index] = reused[index] != nullptr ? &reused[index]->references : &references[index];
        });
        // When every module references what the pack recorded for it, the roots are the ones it recorded too.
        bool recordedGraph = moduleReuse.enabled && moduleReuse.pack.Size() == moduleCount;
        for (std::size_t index = 0; recordedGraph && index < moduleCount; ++index) {
            const CachedModule *recorded = moduleReuse.pack.FindAny(lirPackage.modules[index].name);
            recordedGraph = recorded != nullptr && recorded->references == *moduleReferences[index];
        }
        if (recordedGraph) {
            roots.resize(moduleCount);
            for (std::size_t index = 0; index < moduleCount; ++index)
                roots[index] = moduleReuse.pack.FindAny(lirPackage.modules[index].name)->roots;
        }
        else {
            roots = Optimization::ModuleRoots(moduleReferences, artifactKind);
        }
        bool stale = false;
        for (std::size_t index = 0; index < moduleCount; ++index) {
            if (reused[index] != nullptr && roots[index] != reused[index]->roots) {
                moduleReuse.fresh.insert(sourceModules[index]->name);
                stale = true;
            }
        }
        if (stale) {
            retryWithFreshModules = true;
            return false;
        }
        optimizationPipeline.SetModuleRoots(roots);
    }
    const auto lirOptimizationStart = std::chrono::steady_clock::now();
    const auto lirOptimization = optimizationPipeline.RunLir(lirPackage);
    stats.lirOptimization = ElapsedMs(lirOptimizationStart);
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
    stats.lir = ElapsedMs(lirStart) - stats.lirOptimization;

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
    std::vector<bool> emitted(moduleCount, true);
    for (std::size_t index = 0; index < moduleCount; ++index)
        emitted[index] = reused[index] == nullptr;
    std::vector<RcuFile> rcuFiles;
    std::vector<Diagnostic> codegenDiagnostics;
    if (isAArch64) {
        AArch64RcuEmitter aarch64Emitter(lirPackage, std::string(opts.manifest.package.name.Text()),
                                         compileTimeContext.target.os, compileTimeContext.buildInfo);
        aarch64Emitter.SetMachineSelection(compileTimeContext.profile == BuildProfile::Release);
        aarch64Emitter.SetWorkerPool(&Workers());
        aarch64Emitter.SetBacktraces(true);
        aarch64Emitter.SetEmittedModules(emitted);
        rcuFiles = aarch64Emitter.Generate();
        codegenDiagnostics = aarch64Emitter.Diagnostics();
    }
    else {
        RcuEmitter rcuEmitter(lirPackage, std::string(opts.manifest.package.name.Text()), compileTimeContext.target.os,
                              compileTimeContext.buildInfo);
        rcuEmitter.SetMachineSelection(compileTimeContext.profile == BuildProfile::Release);
        rcuEmitter.SetWorkerPool(&Workers());
        rcuEmitter.SetBacktraces(true);
        rcuEmitter.SetEmittedModules(emitted);
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
    if (moduleReuse.enabled) {
        // The objects of reused modules come from the pack; one that no longer reads back is compiled again.
        std::atomic<bool> unreadable = false;
        Workers().ForEach(moduleCount, [&](const std::size_t index) {
            if (reused[index] == nullptr)
                return;
            if (auto object = DeserializeRcuFile(reused[index]->object))
                rcuFiles[index] = std::move(*object);
            else {
                unreadable = true;
            }
        });
        if (unreadable) {
            for (std::size_t index = 0; index < moduleCount; ++index)
                if (reused[index] != nullptr)
                    moduleReuse.fresh.insert(sourceModules[index]->name);
            retryWithFreshModules = true;
            return false;
        }
    }
    // What this build records of the modules it compiled, encoded before the linker takes the objects.
    std::vector<std::vector<std::uint8_t>> freshObjects(moduleCount);
    if (moduleReuse.enabled) {
        Workers().ForEach(moduleCount, [&](const std::size_t index) {
            if (reused[index] == nullptr && sourceModules[index] != nullptr &&
                moduleReuse.keys.contains(sourceModules[index]))
                freshObjects[index] = SerializeRcuFile(rcuFiles[index]);
        });
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

    // The objects are all the linker reads: the LIR is released first.
    std::vector<std::string> lirModuleNames;
    lirModuleNames.reserve(lirPackage.modules.size());
    for (const LirModule &module : lirPackage.modules)
        lirModuleNames.push_back(module.name);
    // Freed a module per thread: the LIR is large, and freeing it on one thread is a noticeable wait of its own.
    Workers().ForEach(lirPackage.modules.size(),
                      [&](const std::size_t index) { lirPackage.modules[index] = LirModule{}; });
    lirPackage = LirPackage{};

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
    linker.SetCollectGarbage(true);
    linker.SetWorkerPool(&Workers());
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
    if (moduleReuse.enabled) {
        const auto recordStart = std::chrono::steady_clock::now();
        // Record every module this build has an object of, except one something was reported about: reusing it would
        // not report that again.
        std::unordered_set<std::string_view> reported;
        for (const auto &diagnostic : diagnostics)
            reported.insert(diagnostic.sourceName);
        // Keep every dependency package this build compiled, in whole or in part, for later builds of any package to
        // reuse, as a variant of its key named by the names and roots its modules were compiled under.
        std::unordered_map<const Module *, std::size_t> moduleIndex;
        for (std::size_t index = 0; index < moduleCount; ++index)
            if (sourceModules[index] != nullptr)
                moduleIndex.emplace(sourceModules[index], index);
        for (const auto &package : moduleReuse.packages) {
            if (package.key.empty())
                continue;
            const bool keptAlready =
                package.variant && std::ranges::all_of(package.modules, [&](const Module *module) {
                    const auto found = moduleReuse.cached.find(module);
                    return found != moduleReuse.cached.end() && found->second == package.variant->FindAny(module->name);
                });
            if (keptAlready)
                continue;
            ModulePack variant;
            std::string compiledUnder = package.names + "\n";
            bool complete = true;
            for (const Module *module : package.modules) {
                const auto found = moduleIndex.find(module);
                const std::size_t index = found == moduleIndex.end() ? moduleCount : found->second;
                if (index == moduleCount || reported.contains(module->name) ||
                    (index < readsBuildTime.size() && readsBuildTime[index]) ||
                    (reused[index] == nullptr && freshObjects[index].empty())) {
                    complete = false;
                    break;
                }
                CachedModule entry;
                if (reused[index] != nullptr) {
                    entry = *reused[index];
                }
                else {
                    const auto &digest = moduleReuse.digests.at(module);
                    entry.references = references[index];
                    entry.fails = index < moduleFailures.size() && moduleFailures[index] != 0;
                    entry.object = freshObjects[index];
                    entry.sourceSize = digest.size;
                    entry.sourceModified = digest.modified;
                    entry.sourceDigest = digest.source;
                    entry.interfaceDigest = digest.interface;
                }
                entry.key = package.names;
                entry.roots = release ? roots[index] : std::vector<Optimization::LirModuleRoot>{};
                for (const auto &moduleRoot : entry.roots)
                    compiledUnder +=
                        std::format("{} {} {}\n", module->name, static_cast<int>(moduleRoot.kind), moduleRoot.name);
                variant.Put(module->name, std::move(entry));
            }
            if (complete)
                WritePackageVariant(package.directory, HashText(compiledUnder).substr(0, 32), variant);
        }
        ModulePack pack;
        for (std::size_t index = 0; index < moduleCount; ++index) {
            const Module *module = sourceModules[index];
            if (module == nullptr) {
                // The program module has no object to keep, but what it references is part of the program's graph.
                if (release && lirModuleNames[index] == kProgramModuleName) {
                    CachedModule program;
                    program.roots = roots[index];
                    program.references = std::move(references[index]);
                    pack.Put(lirModuleNames[index], std::move(program));
                }
                continue;
            }
            if (reused[index] != nullptr) {
                // The objects were read back long ago, so the entry moves over rather than copying its object.
                if (auto entry = moduleReuse.pack.Take(module->name))
                    pack.Put(module->name, std::move(*entry));
                continue;
            }
            const auto key = moduleReuse.keys.find(module);
            if (key == moduleReuse.keys.end() || freshObjects[index].empty() || reported.contains(module->name) ||
                (index < readsBuildTime.size() && readsBuildTime[index]))
                continue;
            const bool fails = index < moduleFailures.size() && moduleFailures[index] != 0;
            const auto &digest = moduleReuse.digests.at(module);
            pack.Put(module->name, {key->second, release ? roots[index] : std::vector<Optimization::LirModuleRoot>{},
                                    std::move(references[index]), fails, std::move(freshObjects[index]), digest.size,
                                    digest.modified, digest.source, digest.interface});
        }
        static_cast<void>(pack.Write(ModulePackPath()));
        stats.reuse += ElapsedMs(recordStart);
    }
    return true;
}
} // namespace Rux::Driver
