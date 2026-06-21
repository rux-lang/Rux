#include "Rux/Ast.h"
#include "Rux/BuildReport.h"
#include "Rux/BuildTarget.h"
#include "Rux/Cli.h"
#include "Rux/Hir.h"
#include "Rux/Lexer.h"
#include "Rux/Manifest.h"
#include "Rux/Parser.h"
#include "Rux/Platform.h"
#include "Rux/Process.h"
#include "Rux/Sema.h"
#include "Rux/Target.h"
#include "Rux/Terminal.h"
#include "Rux/Version.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <functional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/*
 * This is separate from the other ifdef because otherwise clang-format attempts
 * to change the order, which makes MSVC cry.
 */

#if RUX_OS_WINDOWS
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif

    #ifndef NOMINMAX
        #define NOMINMAX
    #endif

    #include <windows.h>
#endif

#if RUX_OS_WINDOWS
    #include <psapi.h>
#else
    #include <sys/resource.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

#include "Rux/SourceLoader.h"

using namespace Rux;
using namespace Platform;
using namespace Misc;

struct JsonDiagnostic {
    std::string file;
    int line = 0;
    int column = 0;
    std::string severity;
    std::string message;
};

struct PendingPackage {
    std::string name;
    std::filesystem::path root;
    Manifest manifest;
};

struct ImportCollector {
    std::vector<std::string> &imports;
    std::string_view target;

    void collect(const Decl &decl) {
        if (const auto *ud = dynamic_cast<const UseDecl *>(&decl)) {
            if (!DeclMatchesTarget(*ud, target)) {
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
                    collect(*item);
                }
            }
        }
    }
};

struct DependencyQueue {
    const std::string pkgName;
    const Manifest ownerManifest;
    const std::filesystem::path ownerRoot;
    std::unordered_set<std::string> queuedPackageNames;
    std::vector<PendingPackage> pendingPackages;
    std::string targetName;
};

auto JsonEscape(std::string_view s) -> std::string {
    std::string out;
    if (s.size() < ((std::numeric_limits<size_t>::max)() - 128)) {
        out.reserve(s.size() + (s.size() / 10) + 16);
    }
    for (char ch : s) {
        unsigned char u_ch = static_cast<unsigned char>(ch);
        switch (u_ch) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default: {
            if (u_ch < 0x20) {
                char buf[7];
                std::snprintf(buf, sizeof(buf), "\\u%04x", u_ch);
                out += buf;
            }
            else {
                out += ch;
            }
            break;
        }
        }
    }
    return out;
}

auto enqueueDependency(DependencyQueue &queue,
                       const std::function<void(std::string, int, int, std::string, std::string)> &EmitDiag) -> bool {
    if (queue.queuedPackageNames.count(queue.pkgName)) {
        return true;
    }

    const auto deps = queue.ownerManifest.EffectiveDependencies(queue.targetName);
    std::optional<Rux::Dependency> targetDep; // Add Rux:: here

    for (const auto &d : deps) {
        if (d.name == queue.pkgName) {
            targetDep = d;
            break;
        }
    }

    if (!targetDep) {
        EmitDiag("", 0, 0, "error", "package '" + queue.pkgName + "' is not listed in [Dependencies]");
        return false;
    }

    std::filesystem::path depRoot;
    if (targetDep->path.empty()) {
        depRoot = RegistryPackagesDir() / DependencyPackageName(*targetDep);
        if (!std::filesystem::exists(depRoot)) {
            EmitDiag("", 0, 0, "error",
                     "package '" + DependencyPackageName(*targetDep) + "' is not installed — run 'rux install'");
            return false;
        }
    }
    else {
        depRoot = (queue.ownerRoot / targetDep->path).lexically_normal();

        auto rel = depRoot.lexically_relative(queue.ownerRoot);
        if (!rel.empty() && rel.begin()->string() == "..") {
            EmitDiag("", 0, 0, "error",
                     "package '" + queue.pkgName + "' contains an invalid path escaping root bounds");
            return false;
        }
    }

    auto depManifest = Manifest::Load(depRoot / "Rux.toml");
    if (!depManifest) {
        EmitDiag("", 0, 0, "error",
                 "dependency package '" + queue.pkgName + "' was not found at '" + depRoot.string() + "'");
        return false;
    }

    queue.queuedPackageNames.insert(queue.pkgName);
    queue.pendingPackages.push_back({targetDep->name, depRoot, std::move(*depManifest)});
    return true;
}

int HandleJsonOutput(bool hadErrors, const std::vector<JsonDiagnostic> &jsonDiags) {
    std::print("{{\n");
    std::print("  \"success\": {},\n", hadErrors ? "false" : "true");
    std::print("  \"diagnostics\": [\n");

    for (std::size_t i = 0; i < jsonDiags.size(); ++i) {
        const auto &d = jsonDiags[i];
        std::print("    {{");
        std::print("\"file\":\"{}\",", JsonEscape(d.file));
        std::print("\"line\":{},", d.line);
        std::print("\"column\":{},", d.column);
        std::print("\"severity\":\"{}\",", JsonEscape(d.severity));
        std::print("\"message\":\"{}\"", JsonEscape(d.message));
        std::print("}}{}\n", (i + 1 < jsonDiags.size()) ? "," : "");
    }

    std::print("  ]\n");
    std::print("}}\n");
    return hadErrors ? 1 : 0;
}

void HandleErrors(bool &hadErrors, const std::vector<ParseResult> &parseResults,
                  const std::vector<ParseResult> &depParseResults, const std::vector<std::string> &loadedPackages,
                  const std::vector<std::string> &loadedModuleNames, const Manifest &manifest,
                  const std::string &targetName,
                  const std::function<void(std::string, int, int, std::string, std::string)> &EmitDiag) {
    std::vector<const Module *> userModules;
    userModules.reserve(parseResults.size());
    for (const auto &pr : parseResults) {
        userModules.push_back(&pr.module);
    }

    std::vector<DepPackage> depPackages;
    std::unordered_map<std::string, std::size_t> pkgIdx;
    for (std::size_t i = 0; i < depParseResults.size(); ++i) {
        const std::string &pkgName = loadedPackages[i];
        auto [it, inserted] = pkgIdx.emplace(pkgName, depPackages.size());
        if (inserted) {
            depPackages.push_back({pkgName, {}});
        }
        depPackages[it->second].modules.push_back({loadedModuleNames[i], &depParseResults[i].module});
    }

    Sema sema(std::move(userModules), std::move(depPackages), manifest.package.name,
              std::string(TargetOsName(targetName)));
    auto semaResult = sema.Analyze();

    for (const auto &diag : semaResult.diagnostics) {
        const auto &loc = diag.location;
        const char *sev = diag.severity == SemaDiagnostic::Severity::Error ? "error" : "warning";
        EmitDiag(diag.sourceName, static_cast<int>(loc.line), static_cast<int>(loc.column), sev, diag.message);
        if (diag.severity == SemaDiagnostic::Severity::Error) {
            hadErrors = true;
        }
    }

    if (semaResult.HasErrors()) {
        hadErrors = true;
    }
}

int HandlePendingIndex(bool &hadErrors, const GlobalOptions &opts, bool jsonOutput,
                       std::vector<PendingPackage> &pendingPackages, const std::string &targetName,
                       std::vector<std::string> &imports, ImportCollector &collector,
                       std::vector<ParseResult> &depParseResults, std::vector<std::string> &loadedPackages,
                       std::vector<std::string> &loadedModuleNames, std::unordered_set<std::string> &queuedPackageNames,
                       const std::function<void(std::string, int, int, std::string, std::string)> &EmitDiag) {
    for (std::size_t pendingIndex = 0; pendingIndex < pendingPackages.size(); ++pendingIndex) {
        const auto &pendingPkg = pendingPackages[pendingIndex];

        if (opts.verbose && !jsonOutput) {
            std::print(" Loading package {} from {}\n", pendingPkg.name, pendingPkg.root.string());
        }

        auto depLoadResult = SourceLoader::Load(pendingPkg.root);
        if (!depLoadResult) {
            hadErrors = true;
            break;
        };

        for (const auto &error : depLoadResult->errors) {
            if (jsonOutput) {
                EmitDiag("", 0, 0, "error", error);
                hadErrors = true;
            }
            else {
                std::print(stderr, "{}", error);
            }
        }

        if (!depLoadResult->errors.empty()) {
            hadErrors = true;
            break;
        }

        std::vector<ParseResult> packageParseResults;
        packageParseResults.reserve(depLoadResult->files.size());

        for (const auto &depFile : depLoadResult->files) {
            Lexer depLexer(depFile.source, depFile.path.string());
            auto depLex = depLexer.Tokenize();

            for (const auto &diag : depLex.diagnostics) {
                const char *sev = diag.severity == LexerDiagnostic::Severity::Error ? "error" : "warning";
                EmitDiag(depFile.path.string(), static_cast<int>(diag.location.line),
                         static_cast<int>(diag.location.column), sev, diag.message);
                if (diag.severity == LexerDiagnostic::Severity::Error) {
                    hadErrors = true;
                }
            }
            if (depLex.HasErrors()) {
                hadErrors = true;
                break;
            }

            Parser depParser(std::move(depLex.tokens), depFile.path.string());
            auto depParse = depParser.Parse();

            for (const auto &diag : depParse.diagnostics) {
                const char *sev = diag.severity == ParserDiagnostic::Severity::Error ? "error" : "warning";
                EmitDiag(depFile.path.string(), static_cast<int>(diag.location.line),
                         static_cast<int>(diag.location.column), sev, diag.message);
                if (diag.severity == ParserDiagnostic::Severity::Error) {
                    hadErrors = true;
                }
            }
            if (depParse.HasErrors()) {
                hadErrors = true;
                break;
            }
            PruneModuleForTarget(depParse.module, targetName);

            packageParseResults.push_back(std::move(depParse));
        }

        if (hadErrors) {
            break;
        }

        imports.clear();
        for (const auto &pr : packageParseResults) {
            for (const auto &decl : pr.module.items) {
                if (decl) {
                    collector.collect(*decl);
                }
            }
        }

        for (const auto &pkgName : imports) {
            const auto &currentPkg = pendingPackages[pendingIndex];
            if (pkgName == currentPkg.manifest.package.name || pkgName == currentPkg.name) {
                continue;
            }

            DependencyQueue depQueue{.pkgName = pkgName,
                                     .ownerManifest = currentPkg.manifest,
                                     .ownerRoot = currentPkg.root,
                                     .queuedPackageNames = queuedPackageNames,
                                     .pendingPackages = pendingPackages,
                                     .targetName = targetName};

            if (!enqueueDependency(depQueue, EmitDiag)) {
                hadErrors = true;
                break;
            }
        }

        if (hadErrors) {
            break;
        }

        for (auto &depParse : packageParseResults) {
            loadedModuleNames.push_back(depParse.module.name);
            depParseResults.push_back(std::move(depParse));
            loadedPackages.push_back(pendingPackages[pendingIndex].name);
        }
    }
    return 0;
}

int Cli::RunCheck(std::span<const std::string_view> args, const GlobalOptions &opts) {
    bool jsonOutput = false;
    std::string_view target;

    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string_view arg = args[i];

        if (arg == "-q" || arg == "--quiet") {
            continue;
        }
        if (arg == "-v" || arg == "--verbose") {
            continue;
        }

        if (arg == "--json") {
            jsonOutput = true;
            continue;
        }

        if (arg == "--target" && i + 1 < args.size()) {
            target = args[++i];
            continue;
        }

        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("check");
            return 0;
        }

        PrintUnknownOption(arg, "check");
        return 1;
    }

    std::vector<JsonDiagnostic> jsonDiags;
    bool hadErrors = false;

    auto EmitDiag = [&](std::string file, int line, int column, std::string severity, std::string message) {
        if (jsonOutput) {
            jsonDiags.push_back({std::move(file), line, column, std::move(severity), std::move(message)});
        }
        else {
            if (file.empty()) {
                std::print(stderr, "error: {}\n", message);
            }
            else {
                std::print(stderr, "{}:{}:{}: {}: {}\n", file, line, column, severity, message);
            }
        }
    };

    auto EmitFatal = [&](std::string message) {
        EmitDiag("", 0, 0, "error", std::move(message));
        hadErrors = true;
    };

    auto manifestPath = RequireManifest();
    if (!manifestPath) {
        if (jsonOutput) {
            EmitFatal("could not find 'Rux.toml' in current directory or any "
                      "parent directory");
        }
        return 1;
    }

    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) {
        if (jsonOutput) {
            EmitFatal("failed to parse 'Rux.toml'");
        }
        return 1;
    }

    std::string targetName = target.empty() ? HostTargetTriple() : std::string(target);
    if (!IsSupportedTargetTriple(targetName)) {
        if (jsonOutput) {
            EmitFatal("unsupported target '" + targetName + "'");
        }
        else {
            std::print(stderr,
                       "error: unsupported target '{}'; supported targets are "
                       "linux-x64, windows-x64, macos-x64, macos-aarch64, "
                       "freebsd-x64, openbsd-x64, netbsd-x64, dragonfly-x64, "
                       "illumos-x64\n",
                       targetName);
        }
        return 1;
    }

    const std::string hostTarget = HostTargetTriple();
    if (hostTarget != "unknown" && targetName != hostTarget) {
        if (jsonOutput) {
            EmitFatal("cross-target build from '" + hostTarget + "' to '" + targetName + "' is not supported yet");
        }
        else {
            std::print(stderr,
                       "error: cross-target build from '{}' to '{}' is not "
                       "supported yet\n",
                       hostTarget, targetName);
        }
        return 1;
    }

    if (!opts.quiet && !jsonOutput) {
        std::print("Checking {} v{} [{}]\n", manifest->package.name, manifest->package.version,
                   manifestPath->parent_path().string());
    }

    auto loadResult = SourceLoader::Load(manifestPath->parent_path());
    if (!loadResult) {
        if (jsonOutput) {
            EmitFatal("failed to load source files");
        }
        return 1;
    }

    for (const auto &err : loadResult->errors) {
        if (jsonOutput) {
            EmitDiag("", 0, 0, "error", err);
            hadErrors = true;
        }
        else {
            std::print(stderr, "{}", err);
        }
    }

    bool lexErrors = false;
    std::vector<LexerResult> lexResults;
    lexResults.reserve(loadResult->files.size());

    for (const auto &file : loadResult->files) {
        if (opts.verbose && !jsonOutput) {
            std::print("    Lexing {}\n", file.path.string());
        }

        Lexer lexer(file.source, file.path.string());
        auto lexResult = lexer.Tokenize();

        for (const auto &diag : lexResult.diagnostics) {
            const auto &loc = diag.location;
            const char *sev = diag.severity == LexerDiagnostic::Severity::Error ? "error" : "warning";
            EmitDiag(file.path.string(), static_cast<int>(loc.line), static_cast<int>(loc.column), sev, diag.message);
            if (diag.severity == LexerDiagnostic::Severity::Error) {
                lexErrors = true;
            }
        }
        lexResults.push_back(std::move(lexResult));
    }

    if (lexErrors) {
        hadErrors = true;
    }

    bool parseErrors = false;
    std::vector<ParseResult> parseResults;
    parseResults.reserve(loadResult->files.size());

    for (std::size_t fileIndex = 0; fileIndex < loadResult->files.size(); ++fileIndex) {
        const auto &file = loadResult->files[fileIndex];
        if (opts.verbose && !jsonOutput) {
            std::print("    Parsing {}\n", file.path.string());
        }

        auto &lexResult = lexResults[fileIndex];
        if (lexResult.HasErrors()) {
            continue;
        }

        Parser parser(std::move(lexResult.tokens), file.path.string());
        auto parseResult = parser.Parse();

        for (const auto &diag : parseResult.diagnostics) {
            const auto &loc = diag.location;
            const char *sev = diag.severity == ParserDiagnostic::Severity::Error ? "error" : "warning";
            EmitDiag(file.path.string(), static_cast<int>(loc.line), static_cast<int>(loc.column), sev, diag.message);
            if (diag.severity == ParserDiagnostic::Severity::Error) {
                parseErrors = true;
            }
        }

        if (!parseResult.HasErrors()) {
            PruneModuleForTarget(parseResult.module, targetName);
            parseResults.push_back(std::move(parseResult));
        }
    }

    if (parseErrors) {
        hadErrors = true;
    }

    std::vector<ParseResult> depParseResults;
    std::vector<std::string> loadedPackages;
    std::vector<std::string> loadedModuleNames;

    std::vector<PendingPackage> pendingPackages;
    std::unordered_set<std::string> queuedPackageNames;

    std::vector<std::string> imports;

    ImportCollector collector{imports, targetName};

    for (const auto &pr : parseResults) {
        imports.clear();
        for (const auto &decl : pr.module.items) {
            if (decl) {
                collector.collect(*decl);
            }
        }

        for (const auto &pkgName : imports) {
            if (pkgName == manifest->package.name) {
                continue;
            }

            DependencyQueue depQueue{.pkgName = pkgName,
                                     .ownerManifest = *manifest,
                                     .ownerRoot = manifestPath->parent_path(),
                                     .queuedPackageNames = queuedPackageNames,
                                     .pendingPackages = pendingPackages,
                                     .targetName = targetName};

            if (!enqueueDependency(depQueue, EmitDiag)) {
                hadErrors = true;
                break;
            }
        }
    }

    if (!hadErrors) {
        HandlePendingIndex(hadErrors, opts, jsonOutput, pendingPackages, targetName, imports, collector,
                           depParseResults, loadedPackages, loadedModuleNames, queuedPackageNames, EmitDiag);
    }

    if (!hadErrors) {
        HandleErrors(hadErrors, parseResults, depParseResults, loadedPackages, loadedModuleNames, *manifest, targetName,
                     EmitDiag);
    }

    if (jsonOutput) {
        return HandleJsonOutput(hadErrors, jsonDiags);
    }

    return hadErrors ? 1 : 0;
}
