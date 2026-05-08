/*
    Rux Compiler
    Copyright © 2026 Ivan Muzyka
    Licensed under the MIT License
*/

#include "Rux/Cli.h"
#include "Rux/Lexer.h"
#include "Rux/Manifest.h"
#include "Rux/Package.h"
#include "Rux/Parser.h"
#include "Rux/Sema.h"
#include "Rux/Hir.h"
#include "Rux/Lir.h"
#include "Rux/Asm.h"
#include "Rux/Rcu.h"
#include "Rux/Linker.h"
#include "Rux/Version.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <print>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <filesystem>

#ifdef _WIN32
#  include <windows.h>
#  include <psapi.h>
#else
#  include <sys/resource.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

#include "Rux/SourceLoader.h"

namespace Rux {
    namespace {
        struct BuildStats {
            std::chrono::milliseconds lexing{0};
            std::chrono::milliseconds parsing{0};
            std::chrono::milliseconds semantic{0};
            std::chrono::milliseconds hir{0};
            std::chrono::milliseconds lir{0};
            std::chrono::milliseconds codegen{0};
            std::chrono::milliseconds linking{0};
            std::chrono::milliseconds total{0};
            double totalSeconds = 0.0;
            std::size_t localFiles = 0;
            std::size_t dependencyFiles = 0;
            std::size_t localLines = 0;
            std::size_t dependencyLines = 0;
            std::size_t localTokens = 0;
            std::size_t dependencyTokens = 0;
            std::uintmax_t localSourceSize = 0;
            std::uintmax_t dependencySourceSize = 0;
            std::uintmax_t executableSize = 0;
            std::uintmax_t peakMemoryBytes = 0;
        };

        [[nodiscard]] std::chrono::milliseconds ElapsedMs(
            const std::chrono::steady_clock::time_point start,
            const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now()) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        }

        [[nodiscard]] double ElapsedSeconds(
            const std::chrono::steady_clock::time_point start,
            const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now()) {
            return std::chrono::duration<double>(end - start).count();
        }

        [[nodiscard]] std::size_t CountLines(std::string_view source) {
            if (source.empty()) return 0;

            std::size_t lines = 0;
            for (const char ch : source)
                if (ch == '\n') ++lines;
            if (source.back() != '\n') ++lines;
            return lines;
        }

        [[nodiscard]] std::size_t CountTokens(const LexerResult& result) {
            if (result.tokens.empty()) return 0;
            return result.tokens.back().IsEof() ? result.tokens.size() - 1 : result.tokens.size();
        }

        [[nodiscard]] std::string FormatNumber(std::uintmax_t value) {
            std::string digits = std::to_string(value);
            for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(digits.size()) - 3; i > 0; i -= 3)
                digits.insert(static_cast<std::size_t>(i), 1, ',');
            return digits;
        }

        [[nodiscard]] std::string FormatDecimal(double value, int decimals) {
            std::string text = std::format("{:.{}f}", value, decimals);
            auto dot = text.find('.');
            if (dot == std::string::npos) return text;

            while (!text.empty() && text.back() == '0')
                text.pop_back();
            if (!text.empty() && text.back() == '.')
                text.pop_back();
            return text;
        }

        [[nodiscard]] std::string FormatCompactNumber(double value) {
            const double absValue = std::fabs(value);
            if (absValue >= 1'000'000.0)
                return FormatDecimal(value / 1'000'000.0, 1) + "M";
            if (absValue >= 1'000.0)
                return FormatDecimal(value / 1'000.0, 1) + "K";
            return FormatNumber(static_cast<std::uintmax_t>(std::llround(value)));
        }

        [[nodiscard]] std::string FormatTokenThroughput(double tokensPerSecond) {
            const double absValue = std::fabs(tokensPerSecond);
            if (absValue >= 1'000'000.0)
                return FormatDecimal(tokensPerSecond / 1'000'000.0, 1) + " M tok/s";
            if (absValue >= 1'000.0)
                return FormatDecimal(tokensPerSecond / 1'000.0, 1) + " K tok/s";
            return FormatNumber(static_cast<std::uintmax_t>(std::llround(tokensPerSecond))) + " tok/s";
        }

        [[nodiscard]] std::string FormatSize(std::uintmax_t bytes) {
            const double kb = static_cast<double>(bytes) / 1024.0;
            if (kb < 1024.0)
                return FormatNumber(static_cast<std::uintmax_t>(std::llround(kb))) + " KB";

            const double mb = kb / 1024.0;
            return FormatDecimal(mb, 2) + " MB";
        }

        [[nodiscard]] std::string TargetName() {
#if defined(_WIN32)
            std::string os = "Windows";
#elif defined(__APPLE__)
            std::string os = "macOS";
#elif defined(__linux__)
            std::string os = "Linux";
#else
            std::string os = "Unknown";
#endif

#if defined(_M_X64) || defined(__x86_64__) || defined(__amd64__)
            return os + " x64";
#elif defined(_M_IX86) || defined(__i386__)
            return os + " x86";
#elif defined(_M_ARM64) || defined(__aarch64__)
            return os + " arm64";
#elif defined(_M_ARM) || defined(__arm__)
            return os + " arm";
#else
            return os;
#endif
        }

        [[nodiscard]] std::uintmax_t PeakMemoryBytes() {
#ifdef _WIN32
            PROCESS_MEMORY_COUNTERS counters{};
            if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
                return counters.PeakWorkingSetSize;
            return 0;
#else
            rusage usage{};
            if (getrusage(RUSAGE_SELF, &usage) == 0)
#  if defined(__APPLE__)
                return static_cast<std::uintmax_t>(usage.ru_maxrss);
#  else
                return static_cast<std::uintmax_t>(usage.ru_maxrss) * 1024;
#  endif
            return 0;
#endif
        }

        void PrintBuildStats(const std::filesystem::path& exePath,
                             std::string_view profileName,
                             const BuildStats& stats) {
            const auto totalMs = stats.total.count();
            const double seconds = stats.totalSeconds;
            const std::size_t totalFiles = stats.localFiles + stats.dependencyFiles;
            const std::size_t totalLines = stats.localLines + stats.dependencyLines;
            const std::size_t totalTokens = stats.localTokens + stats.dependencyTokens;
            const std::uintmax_t totalSourceSize = stats.localSourceSize + stats.dependencySourceSize;
            const double tokenThroughput = seconds > 0.0
                                               ? static_cast<double>(totalTokens) / seconds
                                               : 0.0;
            const double compileSpeed = seconds > 0.0
                                            ? static_cast<double>(totalLines) / seconds
                                            : 0.0;
            const double throughput = seconds > 0.0
                                          ? static_cast<double>(totalSourceSize) / 1024.0 / 1024.0 / seconds
                                          : 0.0;

            std::print(
                "Rux Compiler {}\n"
                "Target: {}\n"
                "Mode: {}\n\n"
                "Build finished successfully.\n\n"
                "Total build time:            {} ms\n"
                "  Lexing:                    {} ms\n"
                "  Parsing:                   {} ms\n"
                "  Semantic:                  {} ms\n"
                "  HIR:                       {} ms\n"
                "  LIR:                       {} ms\n"
                "  Codegen:                   {} ms\n"
                "  Linking:                   {} ms\n\n"
                "Total files:                 {}\n"
                "  Local files:               {}\n"
                "  Dependency files:          {}\n\n"
                "Total lines:                 {}\n"
                "  Local lines:               {}\n"
                "  Dependency lines:          {}\n\n"
                "Total tokens:                {}\n"
                "  Local tokens:              {}\n"
                "  Dependency tokens:         {}\n\n"
                "Total source size:           {}\n"
                "  Local source size:         {}\n"
                "  Dependency source size:    {}\n\n"
                "Output:\n"
                "  Executable:                {}\n"
                "  Executable size:           {}\n"
                "  Peak memory:               {}\n\n"
                "Performance:\n"
                "  Compile speed:             {} LOC/s\n"
                "  Token throughput:          {}\n"
                "  Total throughput:          {} MB/s\n",
                RUX_VERSION,
                TargetName(),
                profileName,
                totalMs,
                stats.lexing.count(),
                stats.parsing.count(),
                stats.semantic.count(),
                stats.hir.count(),
                stats.lir.count(),
                stats.codegen.count(),
                stats.linking.count(),
                FormatNumber(totalFiles),
                FormatNumber(stats.localFiles),
                FormatNumber(stats.dependencyFiles),
                FormatNumber(totalLines),
                FormatNumber(stats.localLines),
                FormatNumber(stats.dependencyLines),
                FormatNumber(totalTokens),
                FormatNumber(stats.localTokens),
                FormatNumber(stats.dependencyTokens),
                FormatSize(totalSourceSize),
                FormatSize(stats.localSourceSize),
                FormatSize(stats.dependencySourceSize),
                exePath.filename().string(),
                FormatSize(stats.executableSize),
                FormatSize(stats.peakMemoryBytes),
                FormatNumber(static_cast<std::uintmax_t>(std::llround(compileSpeed))),
                FormatTokenThroughput(tokenThroughput),
                FormatDecimal(throughput, 2));
        }

        void PrintBuildSummary(const std::filesystem::path& exePath,
                               std::string_view profileName,
                               const BuildStats& stats) {
            const auto totalMs = stats.total.count();
            const std::size_t totalFiles = stats.localFiles + stats.dependencyFiles;
            const std::size_t totalLines = stats.localLines + stats.dependencyLines;
            const std::size_t totalTokens = stats.localTokens + stats.dependencyTokens;
            const double compileSpeed = stats.totalSeconds > 0.0
                                            ? static_cast<double>(totalLines) / stats.totalSeconds
                                            : 0.0;

            std::print("Built `{}` [{}] in {} ms\n",
                       profileName,
                       exePath.string(),
                       totalMs);
            std::print("{} files | {} LOC | {} tokens | {} LOC/s | {} {}\n",
                       FormatNumber(totalFiles),
                       FormatNumber(totalLines),
                       FormatCompactNumber(static_cast<double>(totalTokens)),
                       FormatCompactNumber(compileSpeed),
                       exePath.filename().string(),
                       FormatSize(stats.executableSize));
        }
    }

    Cli::Cli(const int argc, char* argv[])
        : args(argv, argc) {}

    int Cli::Run() const {
        // Collect all arguments as string_views (skip argv[0])
        std::vector<std::string_view> sv;
        sv.reserve(static_cast<std::size_t>(args.size()));
        for (auto* a : args.subspan(1))
            sv.emplace_back(a);

        if (sv.empty()) {
            PrintHelp();
            return 0;
        }

        // Walk through arguments collecting global flags and finding the command.
        // Global flags may appear before OR after the command name.
        std::string_view command;
        bool foundCommand = false;
        std::vector<std::string_view> preCommandGlobals;
        std::vector<std::string_view> cmdArgs;

        for (std::size_t i = 0; i < sv.size(); ++i) {
            std::string_view arg = sv[i];

            if (!foundCommand) {
                if (arg == "-h" || arg == "--help") {
                    PrintHelp();
                    return 0;
                }
                if (arg == "-V" || arg == "--version") {
                    PrintVersion();
                    return 0;
                }
                if (arg == "-q" || arg == "--quiet" ||
                    arg == "-v" || arg == "--verbose") {
                    preCommandGlobals.push_back(arg);
                    continue;
                }
                if (arg == "--color") {
                    preCommandGlobals.push_back(arg);
                    if (i + 1 < sv.size()) preCommandGlobals.push_back(sv[++i]);
                    continue;
                }
                if (arg.starts_with("--color=")) {
                    preCommandGlobals.push_back(arg);
                    continue;
                }
                command = arg;
                foundCommand = true;
            }
            else {
                cmdArgs.push_back(arg);
            }
        }

        if (!foundCommand) {
            PrintHelp();
            return 0;
        }

        // Merge pre-command globals with command args for option parsing
        std::vector<std::string_view> allArgs;
        allArgs.insert(allArgs.end(), preCommandGlobals.begin(), preCommandGlobals.end());
        allArgs.insert(allArgs.end(), cmdArgs.begin(), cmdArgs.end());

        GlobalOptions opts = ParseGlobalOptions(allArgs);
        std::span<const std::string_view> rest(cmdArgs);

        if (command == "help") return RunHelp(rest, opts);
        if (command == "version") return RunVersion(opts);
        if (command == "build") return RunBuild(rest, opts);
        if (command == "clean") return RunClean(rest, opts);
        if (command == "doc") return RunDoc(rest, opts);
        if (command == "fmt") return RunFmt(rest, opts);
        if (command == "init") return RunInit(rest, opts);
        if (command == "install") return RunInstall(opts);
        if (command == "new") return RunNew(rest, opts);
        if (command == "add") return RunAdd(rest, opts);
        if (command == "remove") return RunRemove(rest, opts);
        if (command == "run") return RunRun(rest, opts);
        if (command == "test") return RunTest(rest, opts);
        if (command == "up") return RunUp(rest, opts);

        PrintUnknownCommand(command);
        return 1;
    }

    GlobalOptions Cli::ParseGlobalOptions(std::span<const std::string_view> args) {
        GlobalOptions opts;
        for (std::size_t i = 0; i < args.size(); ++i) {
            std::string_view arg = args[i];
            if (arg == "-q" || arg == "--quiet") {
                opts.quiet = true;
                continue;
            }
            if (arg == "-v" || arg == "--verbose") {
                opts.verbose = true;
                continue;
            }
            if (arg == "--color") {
                if (i + 1 < args.size()) {
                    if (const std::string_view val = args[++i]; val == "on")
                        opts.color = ColorMode::On;
                    else if (val == "off")
                        opts.color = ColorMode::Off;
                    else
                        opts.color = ColorMode::Auto;
                }
                continue;
            }
            if (arg.starts_with("--color=")) {
                if (const std::string_view val = arg.substr(8); val == "on")
                    opts.color = ColorMode::On;
                else if (val == "off")
                    opts.color = ColorMode::Off;
                else
                    opts.color = ColorMode::Auto;
            }
        }
        return opts;
    }

    static std::optional<std::filesystem::path> RequireManifest() {
        auto path = Manifest::Find();
        if (!path) {
            std::print(stderr,
                       "error: could not find 'Rux.toml' in '{}' or any parent directory\n",
                       std::filesystem::current_path().string());
        }
        return path;
    }

    static std::optional<Manifest> LoadManifest(const std::filesystem::path& path) {
        auto m = Manifest::Load(path);
        if (!m)
            std::print(stderr, "error: failed to parse '{}'\n", path.string());
        return m;
    }

    static std::filesystem::path ResolveBuildOutputDir(const std::filesystem::path& root,
                                                       const Manifest& manifest,
                                                       std::string_view profileName) {
        std::filesystem::path output = manifest.build.output.empty()
                                           ? std::filesystem::path("Bin")
                                           : std::filesystem::path(manifest.build.output);
        if (output.is_relative())
            output = root / output;
        return (output / std::string(profileName)).lexically_normal();
    }

    // Commands

    int Cli::RunHelp(std::span<const std::string_view> args, const GlobalOptions&) {
        if (!args.empty())
            PrintHelpFor(args.front());
        else
            PrintHelp();
        return 0;
    }

    int Cli::RunVersion(const GlobalOptions&) {
        PrintVersion();
        return 0;
    }

    int Cli::RunBuild(std::span<const std::string_view> args, const GlobalOptions& opts) {
        const auto t0 = std::chrono::steady_clock::now();
        bool isRelease = false;
        bool isDebug = false;
        std::string_view profile;
        std::string_view target;
        bool dumpTokens = false;
        bool dumpAst = false;
        bool dumpSema = false;
        bool dumpHir = false;
        bool dumpLir = false;
        bool dumpAsm = false;
        bool dumpRcu = false;
        bool showStats = false;
        for (std::size_t i = 0; i < args.size(); ++i) {
            std::string_view arg = args[i];
            if (arg == "--release") {
                isRelease = true;
                continue;
            }
            if (arg == "--debug") {
                isDebug = true;
                continue;
            }
            if (arg == "-q" || arg == "--quiet") continue;
            if (arg == "-v" || arg == "--verbose") continue;
            if (arg == "--stats") {
                showStats = true;
                continue;
            }
            if (arg == "--dump-tokens") {
                dumpTokens = true;
                continue;
            }
            if (arg == "--dump-ast") {
                dumpAst = true;
                continue;
            }
            if (arg == "--dump-sema") {
                dumpSema = true;
                continue;
            }
            if (arg == "--dump-hir") {
                dumpHir = true;
                continue;
            }
            if (arg == "--dump-lir") {
                dumpLir = true;
                continue;
            }
            if (arg == "--dump-asm") {
                dumpAsm = true;
                continue;
            }
            if (arg == "--dump-rcu") {
                dumpRcu = true;
                continue;
            }
            if (arg == "--profile" && i + 1 < args.size()) {
                profile = args[++i];
                continue;
            }
            if (arg == "--target" && i + 1 < args.size()) {
                target = args[++i];
                continue;
            }
            if (arg == "-h" || arg == "--help") {
                PrintHelpBuild();
                return 0;
            }
            PrintUnknownOption(arg, "build");
            return 1;
        }

        auto manifestPath = RequireManifest();
        if (!manifestPath) return 1;

        auto manifest = LoadManifest(*manifestPath);
        if (!manifest) return 1;

        std::string_view profileName = isRelease ? "Release" : "Debug";
        if (!profile.empty()) profileName = profile;

        if (!opts.quiet && !showStats)
            std::print("Compiling {} v{} [{}]\n",
                       manifest->package.name,
                       manifest->package.version,
                       manifestPath->parent_path().string());

        // ── Lex ───────────────────────────────────────────────────────────────

        BuildStats stats;
        auto loadResult = SourceLoader::Load(manifestPath->parent_path());
        if (!loadResult) return 1;

        stats.localFiles = loadResult->files.size();
        for (const auto& file : loadResult->files) {
            stats.localLines += CountLines(file.source);
            stats.localSourceSize += file.source.size();
        }

        for (const auto& err : loadResult->errors)
            std::print(stderr, "{}", err);

        bool lexErrors = false;
        std::vector<LexerResult> lexResults;
        lexResults.reserve(loadResult->files.size());
        const auto localLexingStart = std::chrono::steady_clock::now();
        for (const auto& file : loadResult->files) {
            if (opts.verbose)
                std::print("     Lexing {}\n", file.path.string());

            Lexer lexer(file.source, file.path.string());
            auto lexResult = lexer.Tokenize();
            stats.localTokens += CountTokens(lexResult);

            for (const auto& diag : lexResult.diagnostics) {
                const auto& loc = diag.location;
                const char* sev = diag.severity == LexerDiagnostic::Severity::Error ? "error" : "warning";
                std::print(stderr, "{}:{}:{}: {}: {}\n",
                           file.path.string(), loc.line, loc.column, sev, diag.message);
            }
            if (lexResult.HasErrors()) lexErrors = true;

            if (dumpTokens) {
                auto tempDir = manifestPath->parent_path() / "Temp" / "Tokens";
                std::filesystem::create_directories(tempDir);
                auto rel = std::filesystem::relative(file.path, manifestPath->parent_path() / "Src");
                auto tokPath = tempDir / rel;
                tokPath.replace_extension(".tokens");
                Lexer::DumpTokens(lexResult, tokPath);
            }
            lexResults.push_back(std::move(lexResult));
        }
        const auto localLexingEnd = std::chrono::steady_clock::now();
        stats.lexing += ElapsedMs(localLexingStart, localLexingEnd);
        if (lexErrors) return 1;

        // Parse

        bool parseErrors = false;
        std::vector<ParseResult> parseResults;
        parseResults.reserve(loadResult->files.size());

        const auto localParsingStart = std::chrono::steady_clock::now();
        for (std::size_t fileIndex = 0; fileIndex < loadResult->files.size(); ++fileIndex) {
            const auto& file = loadResult->files[fileIndex];
            if (opts.verbose)
                std::print("    Parsing {}\n", file.path.string());

            auto& lexResult = lexResults[fileIndex];
            if (lexResult.HasErrors()) continue;

            Parser parser(std::move(lexResult.tokens), file.path.string());
            auto parseResult = parser.Parse();

            for (const auto& diag : parseResult.diagnostics) {
                const auto& loc = diag.location;
                const char* sev = diag.severity == ParserDiagnostic::Severity::Error ? "error" : "warning";
                std::print(stderr, "{}:{}:{}: {}: {}\n",
                           file.path.string(), loc.line, loc.column, sev, diag.message);
            }
            if (parseResult.HasErrors()) {
                parseErrors = true;
                continue;
            }

            if (dumpAst) {
                auto tempDir = manifestPath->parent_path() / "Temp" / "Ast";
                std::filesystem::create_directories(tempDir);
                auto rel = std::filesystem::relative(file.path, manifestPath->parent_path() / "Src");
                auto astPath = (tempDir / rel).replace_extension(".ast");
                Parser::DumpAst(parseResult, astPath);
            }
            parseResults.push_back(std::move(parseResult));
        }
        stats.parsing += ElapsedMs(localParsingStart);
        if (parseErrors) return 1;

        // Load dependency packages referenced by import declarations

        std::vector<ParseResult> depParseResults;
        std::vector<std::string> loadedPackages; // parallel: package name per entry
        std::vector<std::string> loadedModuleNames; // parallel: source name per entry
        {
            struct PendingPackage {
                std::string name;
                std::filesystem::path root;
                Manifest manifest;
            };

            std::vector<PendingPackage> pendingPackages;
            std::unordered_set<std::string> queuedPackageNames;

            auto enqueueDependency =
                [&](const std::string& pkgName, const Manifest& ownerManifest,
                    const std::filesystem::path& ownerRoot) -> bool {
                if (queuedPackageNames.count(pkgName)) return true;

                const Dependency* dep = nullptr;
                for (const auto& d : ownerManifest.dependencies)
                    if (d.name == pkgName) {
                        dep = &d;
                        break;
                    }

                if (!dep) {
                    std::print(stderr,
                               "error: package '{}' is not listed in [Dependencies]\n",
                               pkgName);
                    return false;
                }
                if (dep->path.empty()) {
                    std::print(stderr,
                               "error: registry dependencies not yet supported (package '{}')\n",
                               pkgName);
                    return false;
                }

                auto depRoot = (ownerRoot / dep->path).lexically_normal();
                auto depManifest = Manifest::Load(depRoot / "Rux.toml");
                if (!depManifest) return false;

                queuedPackageNames.insert(pkgName);
                pendingPackages.push_back({pkgName, depRoot, std::move(*depManifest)});
                return true;
            };

            std::vector<std::string> imports;
            auto collectImports = [&](this auto&& self, const Decl& decl) -> void {
                if (const auto* ud = dynamic_cast<const UseDecl*>(&decl)) {
                    if (!ud->path.empty())
                        imports.push_back(ud->path[0]);
                    return;
                }
                if (const auto* mod = dynamic_cast<const ModuleDecl*>(&decl)) {
                    for (const auto& item : mod->items)
                        if (item) self(*item);
                }
            };

            for (const auto& pr : parseResults) {
                imports.clear();
                for (const auto& decl : pr.module.items) {
                    if (decl) collectImports(*decl);
                }
                for (const auto& pkgName : imports)
                    if (!enqueueDependency(pkgName, *manifest, manifestPath->parent_path()))
                        return 1;
            }

            for (std::size_t pendingIndex = 0; pendingIndex < pendingPackages.size(); ++pendingIndex) {
                const std::filesystem::path pendingRoot = pendingPackages[pendingIndex].root;
                const Manifest pendingManifest = pendingPackages[pendingIndex].manifest;
                const std::string packageName = pendingManifest.package.name;

                if (opts.verbose)
                    std::print("  Loading package {} from {}\n",
                               packageName, pendingRoot.string());

                auto depLoadResult = SourceLoader::Load(pendingRoot);
                if (!depLoadResult) {
                    return 1;
                }
                stats.dependencyFiles += depLoadResult->files.size();
                for (const auto& depFile : depLoadResult->files) {
                    stats.dependencyLines += CountLines(depFile.source);
                    stats.dependencySourceSize += depFile.source.size();
                }

                for (const auto& error : depLoadResult->errors)
                    std::print(stderr, "{}", error);
                if (!depLoadResult->errors.empty()) return 1;

                std::vector<ParseResult> packageParseResults;
                packageParseResults.reserve(depLoadResult->files.size());

                for (const auto& depFile : depLoadResult->files) {
                    const auto depLexingStart = std::chrono::steady_clock::now();
                    Lexer depLexer(depFile.source, depFile.path.string());
                    auto depLex = depLexer.Tokenize();
                    const auto depLexingEnd = std::chrono::steady_clock::now();
                    stats.lexing += ElapsedMs(depLexingStart, depLexingEnd);
                    stats.dependencyTokens += CountTokens(depLex);
                    for (const auto& diag : depLex.diagnostics) {
                        const char* sev = diag.severity == LexerDiagnostic::Severity::Error
                                              ? "error"
                                              : "warning";
                        std::print(stderr, "{}:{}:{}: {}: {}\n",
                                   depFile.path.string(), diag.location.line,
                                   diag.location.column, sev, diag.message);
                    }
                    if (depLex.HasErrors()) return 1;

                    const auto depParsingStart = std::chrono::steady_clock::now();
                    Parser depParser(std::move(depLex.tokens), depFile.path.string());
                    auto depParse = depParser.Parse();
                    stats.parsing += ElapsedMs(depParsingStart);
                    for (const auto& diag : depParse.diagnostics) {
                        const char* sev = diag.severity == ParserDiagnostic::Severity::Error
                                              ? "error"
                                              : "warning";
                        std::print(stderr, "{}:{}:{}: {}: {}\n",
                                   depFile.path.string(), diag.location.line,
                                   diag.location.column, sev, diag.message);
                    }
                    if (depParse.HasErrors()) return 1;

                    packageParseResults.push_back(std::move(depParse));
                }

                imports.clear();
                for (const auto& pr : packageParseResults) {
                    for (const auto& decl : pr.module.items) {
                        if (decl) collectImports(*decl);
                    }
                }
                for (const auto& pkgName : imports)
                    if (!enqueueDependency(pkgName, pendingManifest, pendingRoot))
                        return 1;

                for (auto& depParse : packageParseResults) {
                    loadedModuleNames.push_back(depParse.module.name);
                    depParseResults.push_back(std::move(depParse));
                    loadedPackages.push_back(packageName);
                }
            }
        }
        // Semantic analysis

        const auto semanticStart = std::chrono::steady_clock::now();
        if (opts.verbose)
            std::print("  Analyzing {}\n", manifest->package.name);

        std::vector<const Module*> userModules;
        userModules.reserve(parseResults.size());
        for (const auto& pr : parseResults)
            userModules.push_back(&pr.module);

        // Build per-package dep info so Sema can isolate imported package symbols.
        std::vector<DepPackage> depPackages;
        {
            std::unordered_map<std::string, std::size_t> pkgIdx;
            for (std::size_t i = 0; i < depParseResults.size(); ++i) {
                const std::string& pkgName = loadedPackages[i];
                auto [it, inserted] = pkgIdx.emplace(pkgName, depPackages.size());
                if (inserted)
                    depPackages.push_back({pkgName, {}});
                depPackages[it->second].modules.push_back(
                    {loadedModuleNames[i], &depParseResults[i].module});
            }
        }

        Sema sema(std::move(userModules), std::move(depPackages));
        auto semaResult = sema.Analyze();

        for (const auto& diag : semaResult.diagnostics) {
            const auto& loc = diag.location;
            const char* sev = diag.severity == SemaDiagnostic::Severity::Error ? "error" : "warning";
            std::print(stderr, "{}:{}:{}: {}: {}\n",
                       diag.sourceName, loc.line, loc.column, sev, diag.message);
        }
        if (dumpSema) {
            auto semaDir = manifestPath->parent_path() / "Temp" / "Sema";
            std::filesystem::create_directories(semaDir);
            Sema::DumpResult(semaResult, semaDir / "sema.txt");
        }
        if (semaResult.HasErrors()) return 1;
        stats.semantic = ElapsedMs(semanticStart);

        // HIR

        const auto hirStart = std::chrono::steady_clock::now();
        if (opts.verbose)
            std::print("  Lowering {}\n", manifest->package.name);

        std::vector<const Module*> hirModules;
        hirModules.reserve(depParseResults.size() + parseResults.size());
        for (const auto& pr : depParseResults)
            hirModules.push_back(&pr.module);
        for (const auto& pr : parseResults)
            hirModules.push_back(&pr.module);

        Hir hir(hirModules);
        auto hirPackage = hir.Generate();

        if (dumpHir) {
            auto hirDir = manifestPath->parent_path() / "Temp" / "Hir";
            std::filesystem::create_directories(hirDir);
            Hir::Dump(hirPackage, hirDir / "hir.txt");
        }
        stats.hir = ElapsedMs(hirStart);

        // LIR

        const auto lirStart = std::chrono::steady_clock::now();
        if (opts.verbose)
            std::print("  Emitting LIR for {}\n", manifest->package.name);

        Lir lir(std::move(hirPackage));
        auto lirPackage = lir.Generate();

        if (dumpLir) {
            auto lirDir = manifestPath->parent_path() / "Temp" / "Lir";
            std::filesystem::create_directories(lirDir);
            Lir::Dump(lirPackage, lirDir / "lir.txt");
        }
        stats.lir = ElapsedMs(lirStart);

        // Assembly dump (optional)

        const auto codegenStart = std::chrono::steady_clock::now();
        if (dumpAsm) {
            if (opts.verbose)
                std::print("  Emitting assembly for {}\n", manifest->package.name);
            auto asmDir = manifestPath->parent_path() / "Temp" / "Asm";
            std::filesystem::create_directories(asmDir);
            Asm::Emit(lirPackage, asmDir / "out.asm");
        }

        // RCU object generation

        if (opts.verbose)
            std::print("  Emitting RCU objects for {}\n", manifest->package.name);

        Rcu rcu(lirPackage, std::string(manifest->package.name));
        auto rcuFiles = rcu.Generate();

        if (dumpRcu) {
            auto objDir = manifestPath->parent_path() / "Temp" / "Obj";
            auto dumpDir = manifestPath->parent_path() / "Temp" / "Rcu";
            std::filesystem::create_directories(objDir);
            std::filesystem::create_directories(dumpDir);

            for (const auto& rcuFile : rcuFiles) {
                std::filesystem::path stem = rcuFile.sourcePath.empty()
                                                 ? std::filesystem::path("out")
                                                 : std::filesystem::path(rcuFile.sourcePath).stem();
                Rcu::Emit(rcuFile, objDir / (stem.string() + ".rcu"));
                Rcu::Dump(rcuFile, dumpDir / (stem.string() + ".rcu.txt"));
            }
        }
        stats.codegen = ElapsedMs(codegenStart);

        // Link

        const auto linkingStart = std::chrono::steady_clock::now();
        if (opts.verbose)
            std::print("   Linking {}\n", manifest->package.name);

        const auto root = manifestPath->parent_path();
        const auto binDir = ResolveBuildOutputDir(root, *manifest, profileName);
        const auto exePath = binDir / (manifest->package.name + ".exe");

        Linker linker(std::move(rcuFiles), std::string(manifest->package.name), {root});
        if (!linker.Link(exePath)) {
            for (const auto& err : linker.Errors())
                std::print(stderr, "error: {}\n", err.message);
            return 1;
        }
        stats.linking = ElapsedMs(linkingStart);

        // Done

        const auto buildEnd = std::chrono::steady_clock::now();
        stats.total = ElapsedMs(t0, buildEnd);
        stats.totalSeconds = ElapsedSeconds(t0, buildEnd);
        std::error_code sizeError;
        stats.executableSize = std::filesystem::file_size(exePath, sizeError);
        if (sizeError) stats.executableSize = 0;
        stats.peakMemoryBytes = PeakMemoryBytes();

        if (!opts.quiet && showStats) {
            PrintBuildStats(exePath, profileName, stats);
            return 0;
        }

        if (!opts.quiet) {
            PrintBuildSummary(exePath, profileName, stats);
        }
        return 0;
    }

    int Cli::RunClean(std::span<const std::string_view> args, const GlobalOptions& opts) {
        bool tempOnly = false;
        for (auto& arg : args) {
            if (arg == "--temp") {
                tempOnly = true;
                continue;
            }
            if (arg == "-h" || arg == "--help") {
                PrintHelpClean();
                return 0;
            }
            PrintUnknownOption(arg, "clean");
            return 1;
        }
        const auto manifestPath = RequireManifest();
        if (!manifestPath) return 1;
        auto manifest = LoadManifest(*manifestPath);
        if (!manifest) return 1;
        const auto root = manifestPath->parent_path();
        const auto outputDir = manifest->build.output.empty()
                                   ? root / "Bin"
                                   : (std::filesystem::path(manifest->build.output).is_relative()
                                          ? root / manifest->build.output
                                          : std::filesystem::path(manifest->build.output));
        auto removeDir = [&](const std::filesystem::path& dir) -> bool {
            std::error_code ec;
            if (!std::filesystem::exists(dir)) return true;
            std::filesystem::remove_all(dir, ec);
            if (ec) {
                std::print(stderr,
                           "error: failed to remove '{}': {}\n",
                           dir.string(), ec.message());
                return false;
            }
            if (!opts.quiet)
                std::print("     Removed {}\n", dir.string());
            return true;
        };
        bool ok = true;
        if (!tempOnly)
            ok &= removeDir(outputDir);
        ok &= removeDir(root / "Temp");
        return ok ? 0 : 1;
    }

    int Cli::RunDoc(std::span<const std::string_view> args, const GlobalOptions& opts) {
        bool openAfter = false;
        for (auto& arg : args) {
            if (arg == "--open") {
                openAfter = true;
                continue;
            }
            if (arg == "-h" || arg == "--help") {
                PrintHelpDoc();
                return 0;
            }
            PrintUnknownOption(arg, "doc");
            return 1;
        }
        const auto manifestPath = RequireManifest();
        if (!manifestPath) return 1;
        auto manifest = LoadManifest(*manifestPath);
        if (!manifest) return 1;
        if (!opts.quiet)
            std::print("  Generating documentation for {} v{}\n",
                       manifest->package.name, manifest->package.version);

        // TODO: documentation generator

        if (openAfter && !opts.quiet)
            std::print("     Opening documentation...\n");

        return 0;
    }

    int Cli::RunFmt(std::span<const std::string_view> args, const GlobalOptions& opts) {
        bool check = false;
        bool manifestOnly = false;
        for (auto& arg : args) {
            if (arg == "--check") {
                check = true;
                continue;
            }
            if (arg == "--manifest") {
                manifestOnly = true;
                continue;
            }
            if (arg == "-h" || arg == "--help") {
                PrintHelpFmt();
                return 0;
            }
            PrintUnknownOption(arg, "fmt");
            return 1;
        }
        auto manifestPath = RequireManifest();
        if (!manifestPath) return 1;
        auto root = manifestPath->parent_path();
        if (manifestOnly) {
            if (!opts.quiet)
                std::print("  Formatting {}\n", manifestPath->string());
            // TODO: TOML formatter
            return 0;
        }
        auto sourceDir = root / "Source";
        if (!std::filesystem::exists(sourceDir)) {
            if (!opts.quiet) std::print("  No source directory found.\n");
            return 0;
        }
        int fileCount = 0;
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(sourceDir)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".rux") continue;
            ++fileCount;
            if (!opts.quiet) {
                if (check)
                    std::print("  Checking   {}\n", entry.path().string());
                else
                    std::print("  Formatting {}\n", entry.path().string());
            }
            // TODO: source formatter
        }
        if (fileCount == 0 && !opts.quiet)
            std::print("  No .rux files found.\n");
        return 0;
    }

    int Cli::RunInit(std::span<const std::string_view> args, const GlobalOptions& opts) {
        bool bin = false;
        bool lib = false;
        for (auto& arg : args) {
            if (arg == "--bin") {
                bin = true;
                continue;
            }
            if (arg == "--lib") {
                lib = true;
                continue;
            }
            if (arg == "-h" || arg == "--help") {
                PrintHelpInit();
                return 0;
            }
            PrintUnknownOption(arg, "init");
            return 1;
        }
        const auto type = (lib && !bin) ? PackageType::SharedLibrary : PackageType::Executable;
        const auto root = std::filesystem::current_path();
        auto name = root.filename().string();
        if (!opts.quiet)
            std::print("  Initializing {} package '{}'\n",
                       type == PackageType::Executable ? "binary" : "library", name);
        if (!ScaffoldPackage(root, name, type, /*initMode=*/true))
            return 1;
        if (!opts.quiet)
            std::print("   Initialized package '{}'\n", name);
        return 0;
    }

    int Cli::RunInstall(const GlobalOptions& opts) {
        const auto manifestPath = RequireManifest();
        if (!manifestPath) return 1;
        auto manifest = LoadManifest(*manifestPath);
        if (!manifest) return 1;
        if (manifest->dependencies.empty()) {
            if (!opts.quiet)
                std::print("  No dependencies to install.\n");
            return 0;
        }
        if (!opts.quiet) {
            std::print("  Installing {} dependencies for {} v{}\n",
                       manifest->dependencies.size(),
                       manifest->package.name,
                       manifest->package.version);
        }
        for (const auto& dep : manifest->dependencies) {
            std::string ver = dep.version.empty() ? "latest" : dep.version;
            if (!opts.quiet)
                std::print("   Resolving {} @ {}\n", dep.name, ver);
            // TODO: fetch and build dependency
        }
        return 0;
    }

    int Cli::RunNew(const std::span<const std::string_view> args, const GlobalOptions& opts) {
        std::string_view name;
        bool bin = false;
        bool lib = false;
        std::string_view customPath;
        for (std::size_t i = 0; i < args.size(); ++i) {
            std::string_view arg = args[i];
            if (arg == "--bin") {
                bin = true;
                continue;
            }
            if (arg == "--lib") {
                lib = true;
                continue;
            }
            if (arg == "--path" && i + 1 < args.size()) {
                customPath = args[++i];
                continue;
            }
            if (arg == "-h" || arg == "--help") {
                PrintHelpNew();
                return 0;
            }
            if (!arg.starts_with('-') && name.empty()) {
                name = arg;
                continue;
            }
            PrintUnknownOption(arg, "new");
            return 1;
        }
        if (name.empty()) {
            std::print(stderr, "error: missing package name\n\n");
            PrintHelpNew();
            return 1;
        }
        const auto type = (lib && !bin) ? PackageType::SharedLibrary : PackageType::Executable;
        std::filesystem::path root;
        if (!customPath.empty())
            root = std::filesystem::path(customPath) / name;
        else
            root = std::filesystem::current_path() / name;
        if (!opts.quiet)
            std::print("    Creating {} package '{}'\n",
                       type == PackageType::Executable ? "binary" : "library",
                       std::string(name));
        if (!ScaffoldPackage(root, std::string(name), type, /*initMode=*/false))
            return 1;
        if (!opts.quiet)
            std::print("    Created package '{}' at {}\n",
                       std::string(name), root.string());
        return 0;
    }

    int Cli::RunAdd(std::span<const std::string_view> args, const GlobalOptions& opts) {
        std::string_view spec;
        for (auto arg : args) {
            if (arg == "-h" || arg == "--help") {
                PrintHelpAdd();
                return 0;
            }
            if (!arg.starts_with('-') && spec.empty()) {
                spec = arg;
                continue;
            }
            PrintUnknownOption(arg, "add");
            return 1;
        }
        if (spec.empty()) {
            std::print(stderr, "error: missing package name\n\n");
            PrintHelpAdd();
            return 1;
        }
        auto manifestPath = RequireManifest();
        if (!manifestPath) return 1;
        auto manifest = LoadManifest(*manifestPath);
        if (!manifest) return 1;
        auto [pkgName, pkgVersion] = ParsePackageSpec(spec);
        bool changed = manifest->AddDependency(pkgName, pkgVersion);
        if (!manifest->Save(*manifestPath)) {
            std::print(stderr,
                       "error: failed to write '{}'\n", manifestPath->string());
            return 1;
        }
        if (!opts.quiet) {
            std::string ver = pkgVersion.empty() ? "latest" : pkgVersion;
            if (changed)
                std::print("      Added {} @ {}\n", pkgName, ver);
            else
                std::print("   Up-to-date {} @ {}\n", pkgName, ver);
        }
        return 0;
    }

    int Cli::RunRemove(std::span<const std::string_view> args, const GlobalOptions& opts) {
        std::string_view name;
        for (auto arg : args) {
            if (arg == "-h" || arg == "--help") {
                PrintHelpRemove();
                return 0;
            }
            if (!arg.starts_with('-') && name.empty()) {
                name = arg;
                continue;
            }
            PrintUnknownOption(arg, "remove");
            return 1;
        }
        if (name.empty()) {
            std::print(stderr, "error: missing package name\n\n");
            PrintHelpRemove();
            return 1;
        }
        auto manifestPath = RequireManifest();
        if (!manifestPath) return 1;
        auto manifest = LoadManifest(*manifestPath);
        if (!manifest) return 1;
        std::string pkgName(name);
        if (!manifest->RemoveDependency(pkgName)) {
            std::print(stderr,
                       "error: package '{}' is not a dependency\n", pkgName);
            return 1;
        }
        if (!manifest->Save(*manifestPath)) {
            std::print(stderr,
                       "error: failed to write '{}'\n", manifestPath->string());
            return 1;
        }
        if (!opts.quiet)
            std::print("     Removed {}\n", pkgName);
        return 0;
    }

    int Cli::RunRun(std::span<const std::string_view> args, const GlobalOptions& opts) {
        bool isRelease = false;
        std::vector<std::string_view> runArgs;
        bool passThroughMode = false;
        for (auto arg : args) {
            if (passThroughMode) {
                runArgs.push_back(arg);
                continue;
            }
            if (arg == "--") {
                passThroughMode = true;
                continue;
            }
            if (arg == "--release") {
                isRelease = true;
                continue;
            }
            if (arg == "-h" || arg == "--help") {
                PrintHelpRun();
                return 0;
            }
            PrintUnknownOption(arg, "run");
            return 1;
        }
        auto manifestPath = RequireManifest();
        if (!manifestPath) return 1;
        auto manifest = LoadManifest(*manifestPath);
        if (!manifest) return 1;
        // Build first
        GlobalOptions buildOpts = opts;
        if (!opts.verbose)
            buildOpts.quiet = true;

        std::vector<std::string_view> buildArgs;
        if (isRelease) buildArgs.emplace_back("--release");
        if (buildOpts.quiet) buildArgs.emplace_back("--quiet");
        if (buildOpts.verbose) buildArgs.emplace_back("--verbose");
        int rc = RunBuild(buildArgs, buildOpts);
        if (rc != 0) return rc;
        std::string_view profileName = isRelease ? "Release" : "Debug";
        auto root = manifestPath->parent_path();
        auto binDir = ResolveBuildOutputDir(root, *manifest, profileName);
        std::string exeName = manifest->package.name;
#ifdef _WIN32
        exeName += ".exe";
#endif
        auto exePath = binDir / exeName;
        if (!std::filesystem::exists(exePath)) {
            std::print(stderr, "error: executable not found: '{}'\n", exePath.string());
            return 1;
        }
        if (opts.verbose && !opts.quiet)
            std::print("     Running `{}`\n", exePath.string());
#ifdef _WIN32
        std::string cmdLine = "\"" + exePath.string() + "\"";
        for (const auto& a : runArgs) {
            cmdLine += " \"";
            cmdLine += std::string(a);
            cmdLine += '"';
        }
        STARTUPINFOA si{};
        PROCESS_INFORMATION pi{};
        si.cb = sizeof(si);
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        si.dwFlags = STARTF_USESTDHANDLES;
        if (!CreateProcessA(nullptr, cmdLine.data(), nullptr, nullptr,
                            TRUE, 0, nullptr, nullptr, &si, &pi)) {
            std::print(stderr, "error: failed to launch '{}' (code {})\n",
                       exePath.string(), GetLastError());
            return 1;
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return static_cast<int>(exitCode);
#else
        std::vector<std::string> argStrings;
        argStrings.push_back(exePath.string());
        for (const auto& a : runArgs)
            argStrings.emplace_back(a);

        std::vector<char*> argv;
        for (auto& s : argStrings)
            argv.push_back(s.data());
        argv.push_back(nullptr);

        pid_t pid = fork();
        if (pid < 0) {
            std::print(stderr, "error: fork failed\n");
            return 1;
        }
        if (pid == 0) {
            execv(exePath.c_str(), argv.data());
            std::print(stderr, "error: failed to launch '{}'\n", exePath.string());
            _exit(127);
        }

        int status = 0;
        waitpid(pid, &status, 0);
        return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
    }

    int Cli::RunTest(std::span<const std::string_view> args, const GlobalOptions& opts) {
        bool isRelease = false;
        for (auto& arg : args) {
            if (arg == "--release") {
                isRelease = true;
                continue;
            }
            if (arg == "-h" || arg == "--help") {
                PrintHelpTest();
                return 0;
            }
            PrintUnknownOption(arg, "test");
            return 1;
        }
        auto manifestPath = RequireManifest();
        if (!manifestPath) return 1;
        auto manifest = LoadManifest(*manifestPath);
        if (!manifest) return 1;
        if (!opts.quiet)
            std::print("     Testing {} v{}\n",
                       manifest->package.name, manifest->package.version);
        // TODO: build and run test targets
        std::println("Running executable...");
        std::println("Release: {}", isRelease);
        if (!opts.quiet)
            std::print("    Finished running tests\n");
        return 0;
    }

    int Cli::RunUp(std::span<const std::string_view> args, const GlobalOptions& opts) {
        bool selfOnly = false;
        for (auto& arg : args) {
            if (arg == "--self") {
                selfOnly = true;
                continue;
            }
            if (arg == "-h" || arg == "--help") {
                PrintHelpUp();
                return 0;
            }
            PrintUnknownOption(arg, "up");
            return 1;
        }
        if (!opts.quiet)
            std::print("  Checking for updates...\n");
        // TODO: query release feed
        if (!opts.quiet) {
            if (selfOnly)
                std::print("  Rux {} is the latest compiler version.\n", RUX_VERSION);
            else
                std::print("  Rux {} is up to date.\n", RUX_VERSION);
        }
        return 0;
    }

    void Cli::PrintHelp() {
        std::print(
            "Rux compiler and package manager\n"
            "\n"
            "Usage: rux [command] [options] [-- args...]\n"
            "\n"
            "Commands:\n"
            "  add            Add a dependency to the manifest\n"
            "  build          Build the current package\n"
            "  clean          Remove build artifacts\n"
            "  doc            Generate package documentation\n"
            "  fmt            Format source files and manifests\n"
            "  help           Show help information\n"
            "  init           Initialize a Rux package in the current directory\n"
            "  install        Download and build dependencies\n"
            "  new            Create a new Rux package\n"
            "  remove         Remove a dependency from the manifest\n"
            "  run            Build and run the main executable\n"
            "  test           Run all test targets\n"
            "  up             Update Rux toolchain\n"
            "  version        Show version information\n"
            "\n"
            "Options:\n"
            "  --color <auto|on|off>  Control colored output\n"
            "  -h, --help             Show help information\n"
            "  -q, --quiet            Do not show log messages\n"
            "  -v, --verbose          Use verbose output\n"
            "  -V, --version          Show version information\n"
            "\n"
            "Use 'rux help <command>' for more information about a command.\n"
        );
    }

    void Cli::PrintHelpFor(std::string_view command) {
        if (command == "add") {
            PrintHelpAdd();
            return;
        }
        if (command == "build") {
            PrintHelpBuild();
            return;
        }
        if (command == "clean") {
            PrintHelpClean();
            return;
        }
        if (command == "doc") {
            PrintHelpDoc();
            return;
        }
        if (command == "fmt") {
            PrintHelpFmt();
            return;
        }
        if (command == "help") {
            PrintHelp();
            return;
        }
        if (command == "init") {
            PrintHelpInit();
            return;
        }
        if (command == "install") {
            PrintHelpInstall();
            return;
        }
        if (command == "new") {
            PrintHelpNew();
            return;
        }
        if (command == "remove") {
            PrintHelpRemove();
            return;
        }
        if (command == "run") {
            PrintHelpRun();
            return;
        }
        if (command == "test") {
            PrintHelpTest();
            return;
        }
        if (command == "up") {
            PrintHelpUp();
            return;
        }
        if (command == "version") {
            PrintHelpVersion();
            return;
        }
        PrintUnknownCommand(command);
    }

    void Cli::PrintHelpAdd() {
        std::print(
            "Add a dependency to the current package\n"
            "\n"
            "Usage: rux add [package]\n"
            "       rux add [package]@[version]\n"
            "\n"
            "This command updates Rux.toml accordingly.\n"
            "\n"
            "Examples:\n"
            "  rux add Json\n"
            "  rux add [email protected]\n"
        );
    }

    void Cli::PrintHelpBuild() {
        std::print(
            "Build the current package\n"
            "\n"
            "Usage: rux build [options]\n"
            "\n"
            "Options:\n"
            "  --debug              Build with debug symbols (unoptimized output)\n"
            "  --profile <n>        Build using a custom profile defined in Rux.toml\n"
            "  --release            Build with release profile (optimized, no debug info)\n"
            "  --stats              Print build timing, source, performance, and output statistics\n"
            "  --target <triple>    Build for the specified target platform (e.g. x86, x64)\n"
            "  -q, --quiet          Suppress non-essential output (only errors are shown)\n"
            "  -v, --verbose        Enable verbose output for detailed build information\n"
            "  --dump-asm           Write x86-64 assembly to Temp/Asm/out.asm\n"
            "  --dump-ast           Write the parsed AST to Temp/Ast/<file>.ast\n"
            "  --dump-hir           Write the high-level IR to Temp/Hir/hir.txt\n"
            "  --dump-lir           Write the low-level IR to Temp/Lir/lir.txt\n"
            "  --dump-rcu           Write RCU object files to Temp/Obj/ and text dumps to Temp/Rcu/\n"
            "  --dump-sema          Write semantic analysis results to Temp/Sema/sema.txt\n"
            "  --dump-tokens        Write the token stream to Temp/Tokens/<file>.tokens\n"
            "\n"
            "Artifacts are stored under [Build].Output, defaulting to Bin/Debug/ or Bin/Release/.\n"
            "\n"
            "Examples:\n"
            "  rux build\n"
            "  rux build --debug\n"
            "  rux build --release\n"
            "  rux build --stats\n"
            "  rux build --verbose --release\n"
            "  rux build --dump-ast\n"
            "  rux build --dump-hir\n"
            "  rux build --dump-lir\n"
            "  rux build --dump-asm\n"
            "  rux build --dump-rcu\n"
        );
    }

    void Cli::PrintHelpClean() {
        std::print(
            "Remove all build artifacts and temporary files\n"
            "\n"
            "Usage: rux clean [options]\n"
            "\n"
            "Removes the configured build output directory and Temp/ folder.\n"
            "\n"
            "Options:\n"
            "  --temp    Removes only Temp/ directory\n"
            "\n"
            "Examples:\n"
            "  rux clean\n"
            "  rux clean --temp\n"
        );
    }

    void Cli::PrintHelpDoc() {
        std::print(
            "Generate documentation for the package\n"
            "\n"
            "Usage: rux doc [options]\n"
            "\n"
            "Options:\n"
            "  --open    Open documentation after the generation\n"
            "\n"
            "Examples:\n"
            "  rux doc\n"
            "  rux doc --open\n"
        );
    }

    void Cli::PrintHelpFmt() {
        std::print(
            "Format all *.rux source files\n"
            "\n"
            "Usage: rux fmt [options]\n"
            "\n"
            "Options:\n"
            "  --check       Check formatting without modifying files\n"
            "  --manifest    Format only the manifest (Rux.toml)\n"
            "\n"
            "Examples:\n"
            "  rux fmt\n"
            "  rux fmt --check\n"
            "  rux fmt --manifest\n"
        );
    }

    void Cli::PrintHelpInit() {
        std::print(
            "Initialize a new package in the current directory\n"
            "\n"
            "Usage: rux init [options]\n"
            "\n"
            "If Rux.toml does not exist, it will be created.\n"
            "\n"
            "Options:\n"
            "  --bin    Create a binary package\n"
            "  --lib    Create a library package\n"
            "\n"
            "Examples:\n"
            "  rux init\n"
            "  rux init --bin\n"
        );
    }

    void Cli::PrintHelpInstall() {
        std::print(
            "Download and build all required dependencies defined in Rux.toml\n"
            "\n"
            "Usage: rux install\n"
            "\n"
            "Examples:\n"
            "  rux install\n"
        );
    }

    void Cli::PrintHelpNew() {
        std::print(
            "Create a new Rux package in a new directory\n"
            "\n"
            "Usage: rux new [name] [options]\n"
            "\n"
            "Options:\n"
            "  --bin           Create a binary application (default)\n"
            "  --lib           Create a library package\n"
            "  --path <dir>    Create in a specific directory\n"
            "\n"
            "Examples:\n"
            "  rux new Program\n"
            "  rux new Program --bin\n"
        );
    }

    void Cli::PrintHelpRemove() {
        std::print(
            "Remove a dependency from the manifest\n"
            "\n"
            "Usage: rux remove [name]\n"
            "\n"
            "Examples:\n"
            "  rux remove Json\n"
            "  rux remove Random\n"
        );
    }

    void Cli::PrintHelpRun() {
        std::print(
            "Build and execute a runnable target\n"
            "\n"
            "Usage: rux run [options] [-- args...]\n"
            "\n"
            "Arguments after '--' are forwarded to the executable.\n"
            "\n"
            "Options:\n"
            "  --release    Build with release profile\n"
            "\n"
            "Examples:\n"
            "  rux run\n"
            "  rux run --release\n"
            "  rux run -- --port 8080\n"
        );
    }

    void Cli::PrintHelpTest() {
        std::print(
            "Run package unit tests\n"
            "\n"
            "Usage: rux test [options]\n"
            "\n"
            "Options:\n"
            "  --release    Build with release profile\n"
            "\n"
            "Examples:\n"
            "  rux test\n"
            "  rux test --release\n"
        );
    }

    void Cli::PrintHelpUp() {
        std::print(
            "Update Rux toolchain\n"
            "\n"
            "Usage: rux up [options]\n"
            "\n"
            "Checks for updates to the Rux compiler, package registry, and tools.\n"
            "By default, updates all installed toolchain components.\n"
            "\n"
            "Options:\n"
            "  --self    Update only the compiler\n"
            "\n"
            "Examples:\n"
            "  rux up\n"
            "  rux up --self\n"
        );
    }

    void Cli::PrintHelpVersion() {
        std::print(
            "Show information about the Rux toolchain version\n"
            "\n"
            "Usage: rux version\n"
            "\n"
            "Examples:\n"
            "  rux version\n"
            "  rux -V\n"
            "  rux --version\n"
        );
    }

    void Cli::PrintVersion() {
        std::print("Rux {} ({} {})\n", RUX_VERSION, RUX_BUILD_DATE, RUX_BUILD_TIME);
    }

    void Cli::PrintUnknownCommand(std::string_view command) {
        std::print(stderr,
                   "error: unknown command '{}'\n\n"
                   "Use 'rux help' for a list of available commands.\n",
                   command);
    }

    void Cli::PrintUnknownOption(std::string_view option, std::string_view command) {
        if (command.empty())
            std::print(stderr, "error: unknown option '{}'\n", option);
        else
            std::print(stderr,
                       "error: unknown option '{}' for command '{}'\n", option, command);
    }
}
