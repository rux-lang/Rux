#include "Driver/CompilerDriver.h"

#include "BuildInfo/CompilerMetadata.h"
#include "CodeGen/AArch64/RcuEmitter.h"
#include "CodeGen/X86_64/AssemblyPrinter.h"
#include "CodeGen/X86_64/RcuEmitter.h"
#include "Driver/BuildTarget.h"
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
#include "Semantic/ConditionalCompilation.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Semantic/SemanticPrinter.h"
#include "Source/SourceLoader.h"
#include "Syntax/Ast/Ast.h"
#include "System/Os.h"

#include <charconv>
#include <chrono>
#include <limits>
#include <print>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Rux::Driver {
using namespace Target;
using namespace System;

CompilerDriver::CompilerDriver(CompileOptions options)
    : opts(std::move(options)) {
    root = opts.manifestPath.parent_path();
    InitializeCompileTimeContext();
}

void CompilerDriver::InitializeCompileTimeContext() {
    compileTimeContext.target = TargetContextForTriple(opts.target);
    compileTimeContext.targetTriple = opts.target.CanonicalName();
    compileTimeContext.profile = opts.profile;
    compileTimeContext.isTest = opts.isTest;
    compileTimeContext.sourceRoot = root.lexically_normal();
    compileTimeContext.config = opts.manifest.build.ConfigValues();
    // Which import name means the intrinsics package is the root manifest's
    // choice; each dependency's own manifest answers it again below.
    compileTimeContext.intrinsicsAliases = IntrinsicsAliases(opts.manifest, root);
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

void CompilerDriver::Emit(const Diagnostic &diag) const {
    if (opts.emitDiagnostic) {
        opts.emitDiagnostic(diag);
    }
    else {
        PrintDiagnostic(diag);
    }
}

void CompilerDriver::EmitErrorLine(const std::string_view line) const {
    if (opts.emitError) {
        opts.emitError(line);
    }
    else {
        std::print(stderr, "{}", line);
    }
}

bool CompilerDriver::EmitAll(std::span<const Diagnostic> diags) const {
    bool hasErrors = false;
    for (const auto &diag : diags) {
        Emit(diag);
        hasErrors |= diag.IsError();
    }
    return hasErrors;
}

std::string CompilerDriver::TargetSystemName() const {
    return std::string(Target::ToString(opts.target.Os()));
}

CompileResult CompilerDriver::Compile() {
    CompileResult result;
    const auto t0 = std::chrono::steady_clock::now();
    if (invalidSourceDateEpoch) {
        Emit(ErrorDiagnostic("SOURCE_DATE_EPOCH must be a non-negative integer number of seconds"));
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
        return result;
    }

    if (!LexAndParseSources()) {
        return result;
    }
    if (!LoadDependencies()) {
        return result;
    }
    // In checkOnly mode frontend errors are accumulated instead of aborting;
    // semantic analysis only runs on a package whose frontend is clean.
    if (hadErrors) {
        return result;
    }
    if (!Analyze()) {
        return result;
    }
    if (opts.checkOnly) {
        result.stats = stats;
        if (opts.captureFrontend) {
            result.modules = std::move(parseResults);
        }
        result.ok = true;
        return result;
    }

    std::filesystem::path artifactPath;
    if (!GenerateArtifact(artifactPath, result.secondaryArtifactPaths)) {
        return result;
    }

    const auto buildEnd = std::chrono::steady_clock::now();
    stats.total = ElapsedMs(t0, buildEnd);
    stats.totalSeconds = ElapsedSeconds(t0, buildEnd);
    std::error_code sizeError;
    stats.executableSize = std::filesystem::file_size(artifactPath, sizeError);
    if (sizeError) {
        stats.executableSize = 0;
    }
    stats.peakMemoryBytes = PeakMemoryBytes();

    result.stats = stats;
    result.primaryArtifactPath = std::move(artifactPath);
    result.ok = true;
    return result;
}

bool CompilerDriver::LexAndParseSources() {
    auto loadResult = SourceLoader::Load(root);
    stats.localFiles = loadResult.files.size();
    for (const auto &file : loadResult.files) {
        stats.localLines += CountLines(file.source);
        stats.localSourceSize += file.source.size();
    }
    if (EmitAll(loadResult.diagnostics)) {
        hadErrors = true;
        if (!opts.checkOnly) {
            return false;
        }
    }

    // Lex
    bool lexErrors = false;
    std::vector<LexerResult> lexResults;
    lexResults.reserve(loadResult.files.size());
    const auto lexingStart = std::chrono::steady_clock::now();
    for (const auto &file : loadResult.files) {
        if (opts.verbose) {
            std::print("Lexing {}\n", file.path.string());
        }
        Lexer lexer(file.source, file.path.string());
        auto lexResult = lexer.Tokenize();
        stats.localTokens += CountTokens(lexResult);
        if (EmitAll(lexResult.diagnostics)) {
            lexErrors = true;
        }
        if (opts.dumpTokens) {
            auto tempDir = root / "Temp" / "Tokens";
            std::filesystem::create_directories(tempDir);
            auto rel = std::filesystem::relative(file.path, root / "Src");
            auto tokPath = tempDir / rel;
            tokPath.replace_extension(".tokens");
            Lexer::DumpTokens(lexResult, tokPath);
        }
        lexResults.push_back(std::move(lexResult));
    }
    stats.lexing += ElapsedMs(lexingStart);
    if (lexErrors) {
        hadErrors = true;
        if (!opts.checkOnly) {
            return false;
        }
    }

    // Parse
    bool parseErrors = false;
    parseResults.reserve(loadResult.files.size());
    const auto parsingStart = std::chrono::steady_clock::now();
    for (std::size_t fileIndex = 0; fileIndex < loadResult.files.size(); ++fileIndex) {
        const auto &file = loadResult.files[fileIndex];
        if (opts.verbose) {
            std::print("Parsing {}\n", file.path.string());
        }
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
        // Fold `when` before anything reads the module: which imports a file has
        // depends on which branches survive, and dependency loading below is the
        // first thing to ask.
        {
            std::vector<Diagnostic> foldDiagnostics;
            ResolveConditionalCompilation({&parseResult.module}, compileTimeContext, foldDiagnostics);
            if (EmitAll(foldDiagnostics)) {
                parseErrors = true;
            }
        }
        if (opts.dumpAst) {
            auto tempDir = root / "Temp" / "Ast";
            std::filesystem::create_directories(tempDir);
            auto rel = std::filesystem::relative(file.path, root / "Src");
            auto astPath = (tempDir / rel).replace_extension(".ast");
            Parser::DumpAst(parseResult, astPath);
        }
        parseResults.push_back(std::move(parseResult));
    }
    stats.parsing += ElapsedMs(parsingStart);
    if (parseErrors) {
        hadErrors = true;
        if (!opts.checkOnly) {
            return false;
        }
    }
    return true;
}

bool CompilerDriver::LoadDependencies() {
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
                                 (ownerRoot / "Rux.toml").string() + "'"));
            return false;
        }
        if (!dep->MatchesTarget(opts.target.Os())) {
            Emit(ErrorDiagnostic("dependency '" + pkgName + "' is not available for target '" +
                                 std::string(opts.target.CanonicalName()) + "' according to TargetOS in '" +
                                 (ownerRoot / "Rux.toml").string() + "'"));
            return false;
        }
        std::filesystem::path depRoot;
        if (dep->IsPath()) {
            depRoot = (ownerRoot / dep->Path()).lexically_normal();
        }
        else if (const auto local = opts.localPackageRoots.find(DependencyPackageName(*dep));
                 local != opts.localPackageRoots.end()) {
            depRoot = local->second;
        }
        else {
            if (opts.localDependenciesOnly) {
                Emit(ErrorDiagnostic("package '" + DependencyPackageName(*dep) +
                                     "' is not a local workspace member; registry dependencies are disabled"));
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
                                     registry->version.Text() + "'" +
                                     (listed.empty() ? "" : " (installed: " + listed + ")") + " — run 'rux install'"));
                return false;
            }
            depRoot = installed->root;
        }
        auto depManifest = Manifest::Load(depRoot / "Rux.toml");
        if (!depManifest.Ok()) {
            for (const auto &diagnostic : depManifest.diagnostics) {
                Emit(ErrorDiagnostic(diagnostic.Format()));
            }
            Emit(ErrorDiagnostic("dependency package '" + pkgName + "' was not found at '" + depRoot.string() + "'"));
            return false;
        }
        queuedPackageNames.insert(pkgName);
        // Keep the import name as the package namespace loaded into Sema,
        // even when the files came from another package name.
        pendingPackages.push_back({dep->importName.Text(), depRoot, std::move(*depManifest.manifest)});
        return true;
    };

    std::vector<std::string> imports;
    auto collectImports = [&](this auto &&self, const Decl &decl) -> void {
        if (const auto *ud = dynamic_cast<const UseDecl *>(&decl)) {
            if (!ud->path.empty()) {
                imports.push_back(ud->path[0]);
            }
            return;
        }
        if (const auto *mod = dynamic_cast<const ModuleDecl *>(&decl)) {
            for (const auto &item : mod->items) {
                if (item) {
                    self(*item);
                }
            }
        }
    };

    for (const auto &pr : parseResults) {
        imports.clear();
        for (const auto &decl : pr.module.items) {
            if (decl) {
                collectImports(*decl);
            }
        }
        for (const auto &pkgName : imports) {
            if (pkgName == opts.manifest.package.name.Text()) {
                continue;
            }
            if (!enqueueDependency(pkgName, opts.manifest, root)) {
                return false;
            }
        }
    }

    for (std::size_t pendingIndex = 0; pendingIndex < pendingPackages.size(); ++pendingIndex) {
        const std::filesystem::path pendingRoot = pendingPackages[pendingIndex].root;
        const Manifest pendingManifest = pendingPackages[pendingIndex].manifest;
        const std::string packageName = pendingPackages[pendingIndex].name;
        // A dependency's `when` conditions are written against the import names
        // its own manifest chose, which need not match the root's.
        compileTimeContext.intrinsicsAliases = IntrinsicsAliases(pendingManifest, pendingRoot);
        if (opts.verbose) {
            std::print("Loading package {} from {}\n", packageName, pendingRoot.string());
        }
        auto depLoadResult = SourceLoader::Load(pendingRoot);
        stats.dependencyFiles += depLoadResult.files.size();
        for (const auto &depFile : depLoadResult.files) {
            stats.dependencyLines += CountLines(depFile.source);
            stats.dependencySourceSize += depFile.source.size();
        }
        if (EmitAll(depLoadResult.diagnostics)) {
            return false;
        }
        std::vector<ParseResult> packageParseResults;
        packageParseResults.reserve(depLoadResult.files.size());
        for (const auto &depFile : depLoadResult.files) {
            const auto depLexingStart = std::chrono::steady_clock::now();
            Lexer depLexer(depFile.source, depFile.path.string());
            auto depLex = depLexer.Tokenize();
            stats.lexing += ElapsedMs(depLexingStart);
            stats.dependencyTokens += CountTokens(depLex);
            EmitAll(depLex.diagnostics);
            if (depLex.HasErrors()) {
                return false;
            }
            const auto depParsingStart = std::chrono::steady_clock::now();
            Parser depParser(std::move(depLex.tokens), depFile.path.string(), compileTimeContext.target.arch);
            auto depParse = depParser.Parse();
            stats.parsing += ElapsedMs(depParsingStart);
            EmitAll(depParse.diagnostics);
            if (depParse.HasErrors()) {
                return false;
            }
            std::vector<Diagnostic> foldDiagnostics;
            ResolveConditionalCompilation({&depParse.module}, compileTimeContext, foldDiagnostics);
            if (EmitAll(foldDiagnostics)) {
                return false;
            }
            packageParseResults.push_back(std::move(depParse));
        }
        imports.clear();
        for (const auto &pr : packageParseResults) {
            for (const auto &decl : pr.module.items) {
                if (decl) {
                    collectImports(*decl);
                }
            }
        }
        for (const auto &pkgName : imports) {
            if (pkgName == pendingManifest.package.name.Text() || pkgName == packageName) {
                continue;
            }
            if (!enqueueDependency(pkgName, pendingManifest, pendingRoot)) {
                return false;
            }
        }
        for (auto &depParse : packageParseResults) {
            loadedModuleNames.push_back(depParse.module.name);
            depParseResults.push_back(std::move(depParse));
            loadedPackages.push_back(packageName);
        }
    }
    return true;
}

bool CompilerDriver::Analyze() {
    const auto semanticStart = std::chrono::steady_clock::now();
    if (opts.verbose) {
        std::print("Analyzing {}\n", opts.manifest.package.name.Text());
    }
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
        auto semaDir = root / "Temp" / "Sema";
        std::filesystem::create_directories(semaDir);
        SemanticPrinter::Dump(*semanticModel, semaDir / "sema.txt");
    }
    if (semanticModel->HasErrors()) {
        hadErrors = true;
        return false;
    }
    stats.semantic = ElapsedMs(semanticStart);
    return true;
}

bool CompilerDriver::GenerateArtifact(std::filesystem::path &artifactPath,
                                      std::vector<std::filesystem::path> &secondaryArtifactPaths) {
    // HIR
    const auto hirStart = std::chrono::steady_clock::now();
    if (opts.verbose) {
        std::print("Lowering {}\n", opts.manifest.package.name.Text());
    }
    AstToHirLowering hirLowering(*semanticModel);
    auto hirPackage = hirLowering.Generate();
    if (opts.dumpHir) {
        auto hirDir = root / "Temp" / "Hir";
        std::filesystem::create_directories(hirDir);
        HirPrinter::Dump(hirPackage, hirDir / "hir.txt");
    }
    stats.hir = ElapsedMs(hirStart);

    // LIR
    const auto lirStart = std::chrono::steady_clock::now();
    if (opts.verbose) {
        std::print("Emitting LIR for {}\n", opts.manifest.package.name.Text());
    }
    HirToLirLowering lirLowering(std::move(hirPackage), compileTimeContext.target, compileTimeContext.profile);
    auto lirPackage = lirLowering.Generate();
    if (opts.dumpLir) {
        auto lirDir = root / "Temp" / "Lir";
        std::filesystem::create_directories(lirDir);
        LirPrinter::Dump(lirPackage, lirDir / "lir.txt");
    }
    stats.lir = ElapsedMs(lirStart);

    const auto codegenStart = std::chrono::steady_clock::now();
    const bool isAArch64 = compileTimeContext.target.arch == Target::Arch::AArch64;

    // Assembly dump (optional). AssemblyPrinter prints x86-64; the AArch64 back
    // end has no printer of its own yet.
    if (opts.dumpAsm && !isAArch64) {
        if (opts.verbose) {
            std::print("Emitting assembly for {}\n", opts.manifest.package.name.Text());
        }
        auto asmDir = root / "Temp" / "Asm";
        std::filesystem::create_directories(asmDir);
        AssemblyPrinter::Emit(lirPackage, asmDir / "out.asm");
    }

    // RCU object generation
    if (opts.verbose) {
        std::print("Emitting RCU objects for {}\n", opts.manifest.package.name.Text());
    }
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
        auto objDir = root / "Temp" / "Obj";
        auto dumpDir = root / "Temp" / "Rcu";
        std::filesystem::create_directories(objDir);
        std::filesystem::create_directories(dumpDir);

        for (const auto &rcuFile : rcuFiles) {
            std::filesystem::path stem = rcuFile.sourcePath.empty() ? std::filesystem::path("out")
                                                                    : std::filesystem::path(rcuFile.sourcePath).stem();
            RcuWriter::Write(rcuFile, objDir / (stem.string() + ".rcu"));
            RcuDumper::Dump(rcuFile, dumpDir / (stem.string() + ".rcu.txt"));
        }
    }
    stats.codegen = ElapsedMs(codegenStart);

    // Link
    const auto linkingStart = std::chrono::steady_clock::now();
    if (opts.verbose) {
        std::print("Linking {}\n", opts.manifest.package.name.Text());
    }
    const auto binDir =
        ResolveBuildOutputDir(root, opts.manifest, ToString(opts.profile), opts.target.CanonicalName(), !opts.isTest);
    const ArtifactKind artifactKind = PackageArtifactKind(opts.manifest.package.type);
    const OS targetOs = opts.target.Os();
    const Arch targetArch = opts.target.Architecture();
    artifactPath = binDir / OutputFileName(opts.manifest.package.name.Text(), artifactKind, targetOs);
    Linker linker(std::move(rcuFiles), std::string(opts.manifest.package.name.Text()), {root}, artifactKind, targetOs,
                  targetArch);
    if (!linker.Link(artifactPath)) {
        for (const auto &err : linker.Errors()) {
            Emit(ErrorDiagnostic(err.message));
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
