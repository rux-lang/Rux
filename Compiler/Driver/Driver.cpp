#include "Driver/Driver.h"

#include "Backend/Link/Linker.h"
#include "Backend/Rcu/Rcu.h"
#include "Backend/X64/Asm.h"
#include "Driver/BuildTarget.h"
#include "Frontend/Ast/Ast.h"
#include "Frontend/Lexer.h"
#include "Frontend/Sema/Sema.h"
#include "Frontend/SourceLoader.h"
#include "Ir/Hir/Hir.h"
#include "Ir/Lir/Lir.h"
#include "Platform/Target.h"

#include <chrono>
#include <print>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Rux {

using namespace Misc;
using namespace Platform;

Driver::Driver(CompileOptions options)
    : opts(std::move(options)) {
    root = opts.manifestPath.parent_path();
}

void Driver::Emit(const Diagnostic &diag) const {
    if (opts.emitDiagnostic) {
        opts.emitDiagnostic(diag);
    }
    else {
        PrintDiagnostic(diag);
    }
}

void Driver::EmitErrorLine(const std::string_view line) const {
    if (opts.emitError) {
        opts.emitError(line);
    }
    else {
        std::print(stderr, "{}", line);
    }
}

bool Driver::EmitAll(std::span<const Diagnostic> diags) const {
    bool hasErrors = false;
    for (const auto &diag : diags) {
        Emit(diag);
        hasErrors |= diag.IsError();
    }
    return hasErrors;
}

CompileResult Driver::Compile() {
    CompileResult result;
    const auto t0 = std::chrono::steady_clock::now();

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
        result.ok = true;
        return result;
    }

    std::filesystem::path exePath;
    if (!GenerateExecutable(exePath)) {
        return result;
    }

    const auto buildEnd = std::chrono::steady_clock::now();
    stats.total = ElapsedMs(t0, buildEnd);
    stats.totalSeconds = ElapsedSeconds(t0, buildEnd);
    std::error_code sizeError;
    stats.executableSize = std::filesystem::file_size(exePath, sizeError);
    if (sizeError) {
        stats.executableSize = 0;
    }
    stats.peakMemoryBytes = PeakMemoryBytes();

    result.stats = stats;
    result.executablePath = std::move(exePath);
    result.ok = true;
    return result;
}

bool Driver::LexAndParseSources() {
    auto loadResult = SourceLoader::Load(root);
    if (!loadResult) {
        return false;
    }
    stats.localFiles = loadResult->files.size();
    for (const auto &file : loadResult->files) {
        stats.localLines += CountLines(file.source);
        stats.localSourceSize += file.source.size();
    }
    for (const auto &err : loadResult->errors) {
        EmitErrorLine(err);
    }
    if (!loadResult->errors.empty() && opts.checkOnly) {
        hadErrors = true;
    }

    // Lex
    bool lexErrors = false;
    std::vector<LexerResult> lexResults;
    lexResults.reserve(loadResult->files.size());
    const auto lexingStart = std::chrono::steady_clock::now();
    for (const auto &file : loadResult->files) {
        if (opts.verbose) {
            std::print("     Lexing {}\n", file.path.string());
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
    parseResults.reserve(loadResult->files.size());
    const auto parsingStart = std::chrono::steady_clock::now();
    for (std::size_t fileIndex = 0; fileIndex < loadResult->files.size(); ++fileIndex) {
        const auto &file = loadResult->files[fileIndex];
        if (opts.verbose) {
            std::print("    Parsing {}\n", file.path.string());
        }
        auto &lexResult = lexResults[fileIndex];
        if (lexResult.HasErrors()) {
            continue;
        }
        Parser parser(std::move(lexResult.tokens), file.path.string());
        auto parseResult = parser.Parse();
        if (EmitAll(parseResult.diagnostics)) {
            parseErrors = true;
        }
        if (parseResult.HasErrors()) {
            continue;
        }
        PruneModuleForTarget(parseResult.module, opts.targetName);
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

bool Driver::LoadDependencies() {
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
        // Resolve imports through the target view of the owning manifest.
        // This is what maps Platform to Linux on linux-x64.
        const auto deps = ownerManifest.EffectiveDependencies(opts.targetName);
        const Dependency *dep = nullptr;
        for (const auto &d : deps) {
            if (d.name == pkgName) {
                dep = &d;
                break;
            }
        }
        if (!dep) {
            Emit(ErrorDiagnostic("package '" + pkgName + "' is not listed in [Dependencies]"));
            return false;
        }
        std::filesystem::path depRoot;
        if (dep->path.empty()) {
            depRoot = RegistryPackagesDir() / DependencyPackageName(*dep);
            if (!std::filesystem::exists(depRoot)) {
                Emit(ErrorDiagnostic("package '" + DependencyPackageName(*dep) +
                                     "' is not installed — run 'rux install'"));
                return false;
            }
        }
        else {
            depRoot = (ownerRoot / dep->path).lexically_normal();
        }
        auto depManifest = Manifest::Load(depRoot / "Rux.toml");
        if (!depManifest) {
            Emit(ErrorDiagnostic("dependency package '" + pkgName + "' was not found at '" + depRoot.string() + "'"));
            return false;
        }
        queuedPackageNames.insert(pkgName);
        // Keep the import name as the package namespace loaded into Sema,
        // even when the files came from another package name.
        pendingPackages.push_back({dep->name, depRoot, std::move(*depManifest)});
        return true;
    };

    std::vector<std::string> imports;
    auto collectImports = [&](this auto &&self, const Decl &decl) -> void {
        if (const auto *ud = dynamic_cast<const UseDecl *>(&decl)) {
            if (!DeclMatchesTarget(*ud, opts.targetName)) {
                return;
            }
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
            if (pkgName == opts.manifest.package.name) {
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
        if (opts.verbose) {
            std::print("  Loading package {} from {}\n", packageName, pendingRoot.string());
        }
        auto depLoadResult = SourceLoader::Load(pendingRoot);
        if (!depLoadResult) {
            return false;
        }
        stats.dependencyFiles += depLoadResult->files.size();
        for (const auto &depFile : depLoadResult->files) {
            stats.dependencyLines += CountLines(depFile.source);
            stats.dependencySourceSize += depFile.source.size();
        }
        for (const auto &error : depLoadResult->errors) {
            EmitErrorLine(error);
        }
        if (!depLoadResult->errors.empty()) {
            return false;
        }
        std::vector<ParseResult> packageParseResults;
        packageParseResults.reserve(depLoadResult->files.size());
        for (const auto &depFile : depLoadResult->files) {
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
            Parser depParser(std::move(depLex.tokens), depFile.path.string());
            auto depParse = depParser.Parse();
            stats.parsing += ElapsedMs(depParsingStart);
            EmitAll(depParse.diagnostics);
            if (depParse.HasErrors()) {
                return false;
            }
            PruneModuleForTarget(depParse.module, opts.targetName);
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
            if (pkgName == pendingManifest.package.name || pkgName == packageName) {
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

bool Driver::Analyze() {
    const auto semanticStart = std::chrono::steady_clock::now();
    if (opts.verbose) {
        std::print("  Analyzing {}\n", opts.manifest.package.name);
    }
    std::vector<const Module *> userModules;
    userModules.reserve(parseResults.size());
    for (const auto &pr : parseResults) {
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
    Sema sema(std::move(userModules), std::move(depPackages), opts.manifest.package.name,
              std::string(TargetOsName(opts.targetName)));
    auto semaResult = sema.Analyze();
    EmitAll(semaResult.diagnostics);
    if (opts.dumpSema) {
        auto semaDir = root / "Temp" / "Sema";
        std::filesystem::create_directories(semaDir);
        Sema::DumpResult(semaResult, semaDir / "sema.txt");
    }
    if (semaResult.HasErrors()) {
        hadErrors = true;
        return false;
    }
    stats.semantic = ElapsedMs(semanticStart);
    return true;
}

bool Driver::GenerateExecutable(std::filesystem::path &exePath) {
    // HIR
    const auto hirStart = std::chrono::steady_clock::now();
    if (opts.verbose) {
        std::print("  Lowering {}\n", opts.manifest.package.name);
    }
    std::vector<const Module *> hirModules;
    hirModules.reserve(depParseResults.size() + parseResults.size());
    for (const auto &pr : depParseResults) {
        hirModules.push_back(&pr.module);
    }
    for (const auto &pr : parseResults) {
        hirModules.push_back(&pr.module);
    }
    Hir hir(hirModules);
    auto hirPackage = hir.Generate();
    if (opts.dumpHir) {
        auto hirDir = root / "Temp" / "Hir";
        std::filesystem::create_directories(hirDir);
        Hir::Dump(hirPackage, hirDir / "hir.txt");
    }
    stats.hir = ElapsedMs(hirStart);

    // LIR
    const auto lirStart = std::chrono::steady_clock::now();
    if (opts.verbose) {
        std::print("  Emitting LIR for {}\n", opts.manifest.package.name);
    }
    Lir lir(std::move(hirPackage));
    auto lirPackage = lir.Generate();
    if (opts.dumpLir) {
        auto lirDir = root / "Temp" / "Lir";
        std::filesystem::create_directories(lirDir);
        Lir::Dump(lirPackage, lirDir / "lir.txt");
    }
    stats.lir = ElapsedMs(lirStart);

    // Assembly dump (optional)
    const auto codegenStart = std::chrono::steady_clock::now();
    if (opts.dumpAsm) {
        if (opts.verbose) {
            std::print("  Emitting assembly for {}\n", opts.manifest.package.name);
        }
        auto asmDir = root / "Temp" / "Asm";
        std::filesystem::create_directories(asmDir);
        Asm::Emit(lirPackage, asmDir / "out.asm");
    }

    // RCU object generation
    if (opts.verbose) {
        std::print("  Emitting RCU objects for {}\n", opts.manifest.package.name);
    }
    Rcu rcu(lirPackage, std::string(opts.manifest.package.name));
    auto rcuFiles = rcu.Generate();
    if (opts.dumpRcu) {
        auto objDir = root / "Temp" / "Obj";
        auto dumpDir = root / "Temp" / "Rcu";
        std::filesystem::create_directories(objDir);
        std::filesystem::create_directories(dumpDir);

        for (const auto &rcuFile : rcuFiles) {
            std::filesystem::path stem = rcuFile.sourcePath.empty() ? std::filesystem::path("out")
                                                                    : std::filesystem::path(rcuFile.sourcePath).stem();
            Rcu::Emit(rcuFile, objDir / (stem.string() + ".rcu"));
            Rcu::Dump(rcuFile, dumpDir / (stem.string() + ".rcu.txt"));
        }
    }
    stats.codegen = ElapsedMs(codegenStart);

    // Link
    const auto linkingStart = std::chrono::steady_clock::now();
    if (opts.verbose) {
        std::print("   Linking {}\n", opts.manifest.package.name);
    }
    const auto binDir = ResolveBuildOutputDir(root, opts.manifest, opts.profileName);
    const bool buildDll = (opts.manifest.package.type == "Dll" || opts.manifest.package.type == "dll");
    std::string outputName = opts.manifest.package.name;
    if constexpr (HostOS == OS::Windows) {
        outputName += buildDll ? ".dll" : ".exe";
    }
    exePath = binDir / outputName;
    Linker linker(std::move(rcuFiles), std::string(opts.manifest.package.name), {root}, buildDll);
    if (!linker.Link(exePath)) {
        for (const auto &err : linker.Errors()) {
            Emit(ErrorDiagnostic(err.message));
        }
        return false;
    }
    stats.linking = ElapsedMs(linkingStart);
    return true;
}

} // namespace Rux
