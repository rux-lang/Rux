// Copyright (c) Rux contributors.
// SPDX-License-Identifier: MIT

// File contains functions and values that were inside the Anonymous namespace.
// I gave the functions and values a proper namespace so they can be used
// elsewhere. I also Refactored some of the code to use standard library
// features.
//
// These utilities are primarily used by:
//   - BuildCmd.cpp (build statistics, manifest loading)
//   - CheckCmd.cpp (package resolution, dependency handling)
//   - InstallCmd.cpp (registry operations, git helpers)
//   - PackageCmd.cpp (manifest management)
//   - UtilityCmd.cpp (package listing, info display)

#pragma once

#include "Rux/Asm.h"
#include "Rux/Ast.h"
#include "Rux/Cli/Cli.h"
#include "Rux/Hir.h"
#include "Rux/Lexer.h"
#include "Rux/Linker.h"
#include "Rux/Lir.h"
#include "Rux/Manifest.h"
#include "Rux/Package.h"
#include "Rux/Parser.h"
#include "Rux/Platform/Defines.h"
#include "Rux/Platform/Host.h"
#include "Rux/Rcu.h"
#include "Rux/Sema.h"
#include "Rux/Version.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <optional>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// This is separate from the other ifdef because otherwise clang-format attempts
// to change the order, which makes MSVC cry.

#if RUX_OS_WINDOWS
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

#if RUX_OS_WINDOWS
#  include <psapi.h>
#  include <winhttp.h>
#else
#  include <sys/resource.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

#include "Rux/SourceLoader.h"

namespace Rux::Misc {
    using namespace Platform;

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

    inline std::chrono::milliseconds
    ElapsedMs(const std::chrono::steady_clock::time_point start,
              const std::chrono::steady_clock::time_point end =
                  std::chrono::steady_clock::now()) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(end -
                                                                     start);
    }

    inline double
    ElapsedSeconds(const std::chrono::steady_clock::time_point start,
                   const std::chrono::steady_clock::time_point end =
                       std::chrono::steady_clock::now()) {
        return std::chrono::duration<double>(end - start).count();
    }

    inline std::size_t CountLines(std::string_view source) {
        if (source.empty()) return 0;

        std::size_t lines = 0;
        for (const char ch : source)
            if (ch == '\n') ++lines;
        if (source.back() != '\n') ++lines;
        return lines;
    }

    inline std::size_t CountTokens(const LexerResult& result) {
        if (result.tokens.empty()) return 0;
        return result.tokens.back().IsEof() ? result.tokens.size() - 1
                                            : result.tokens.size();
    }

    inline std::string FormatNumber(std::uintmax_t value) {
        std::string digits = std::to_string(value);
        for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(digits.size()) - 3;
             i > 0;
             i -= 3)
            digits.insert(static_cast<std::size_t>(i), 1, ',');
        return digits;
    }

    inline std::string FormatDecimal(double value, int decimals) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(decimals) << value;
        std::string text = oss.str();
        auto dot = text.find('.');
        if (dot == std::string::npos) return text;

        while (!text.empty() && text.back() == '0')
            text.pop_back();
        if (!text.empty() && text.back() == '.') text.pop_back();
        return text;
    }

    inline std::string FormatCompactNumber(double value) {
        const double absValue = std::fabs(value);
        if (absValue >= 1'000'000.0)
            return FormatDecimal(value / 1'000'000.0, 1) + "M";
        if (absValue >= 1'000.0) return FormatDecimal(value / 1'000.0, 1) + "K";
        return FormatNumber(static_cast<std::uintmax_t>(std::llround(value)));
    }

    inline std::string FormatTokenThroughput(double tokensPerSecond) {
        const double absValue = std::fabs(tokensPerSecond);
        if (absValue >= 1'000'000.0)
            return FormatDecimal(tokensPerSecond / 1'000'000.0, 1) + " M tok/s";
        if (absValue >= 1'000.0)
            return FormatDecimal(tokensPerSecond / 1'000.0, 1) + " K tok/s";
        return FormatNumber(
                   static_cast<std::uintmax_t>(std::llround(tokensPerSecond))) +
            " tok/s";
    }

    inline std::string FormatSize(std::uintmax_t bytes) {
        const double kb = static_cast<double>(bytes) / 1024.0;
        if (kb < 1024.0)
            return FormatNumber(static_cast<std::uintmax_t>(std::llround(kb))) +
                " KB";

        const double mb = kb / 1024.0;
        return FormatDecimal(mb, 2) + " MB";
    }

    inline std::string TargetName() {
        if constexpr (HostArch == Arch::Unknown) {
            return std::string{ToString(HostOS)};
        }

        return std::format("{} {}", ToString(HostOS), ToString(HostArch));
    }

    inline std::string HostTargetTriple() {
        auto triple =
            std::format("{}-{}", ToString(HostOS), ToString(HostArch));
        std::transform(
            std::begin(triple),
            std::end(triple),
            std::begin(triple),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return triple;
    }

    inline bool IsSupportedTargetTriple(const std::string_view target) {
        constexpr std::array supported_targets{"linux-x64",
                                               "windows-x64",
                                               "macos-x64",
                                               "macos-aarch64",
                                               "freebsd-x64",
                                               "openbsd-x64",
                                               "netbsd-x64",
                                               "dragonfly-x64",
                                               "illumos-x64"};

        return std::ranges::contains(supported_targets, target);
    }

    inline std::string_view TargetOsName(const std::string_view target) {
        const auto dash_pos = target.find('-');
        if (dash_pos == std::string_view::npos) {
            return "";
        }

        const auto os_prefix = target.substr(0, dash_pos);

        if (os_prefix == "linux") return "Linux";
        if (os_prefix == "windows") return "Windows";
        if (os_prefix == "macos") return "macOS";
        if (os_prefix == "freebsd" || os_prefix == "openbsd" ||
            os_prefix == "netbsd" || os_prefix == "dragonfly")
            return "BSD";
        if (os_prefix == "illumos") return "Illumos";

        return "";
    }

    inline bool DeclMatchesTarget(const Decl& decl,
                                  const std::string_view target) {
        if (decl.targetOs.empty()) return true;
        const std::string_view targetOs = TargetOsName(target);
        // Normalize both sides for robust comparison.
        if (decl.targetOs.size() != targetOs.size()) return false;
        // Case-insensitive comparison handles any casing in @[Target("...")].
        for (std::size_t i = 0; i < decl.targetOs.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(decl.targetOs[i])) !=
                std::tolower(static_cast<unsigned char>(targetOs[i])))
                return false;
        return true;
    }

    // Known platform package names.  If a source file imports one of these
    // and the name does not match the current build target it is a platform-
    // specific import that should have been pruned; skip it gracefully.
    inline bool IsPlatformPackageName(const std::string_view name) {
        return name == "Windows" || name == "Linux" || name == "macOS" ||
            name == "BSD" || name == "Illumos";
    }

    inline bool PlatformPackageMatchesTarget(const std::string_view name,
                                             const std::string_view target) {
        return name == TargetOsName(target);
    }

    inline void PruneDeclsForTarget(std::vector<DeclPtr>& decls,
                                    const std::string_view target);

    inline void PruneDeclForTarget(Decl& decl, const std::string_view target) {
        if (auto* module = dynamic_cast<ModuleDecl*>(&decl)) {
            PruneDeclsForTarget(module->items, target);
        }
        else if (auto* block = dynamic_cast<ExternBlockDecl*>(&decl)) {
            PruneDeclsForTarget(block->items, target);
        }
    }

    inline void PruneDeclsForTarget(std::vector<DeclPtr>& decls,
                                    const std::string_view target) {
        std::erase_if(decls, [&](const DeclPtr& decl) {
            return !decl || !DeclMatchesTarget(*decl, target);
        });
        for (const auto& decl : decls)
            PruneDeclForTarget(*decl, target);
    }

    inline void PruneModuleForTarget(Module& module,
                                     const std::string_view target) {
        PruneDeclsForTarget(module.items, target);
    }

    inline std::string DependencyPackageName(const Dependency& dep) {
        return dep.package.empty() ? dep.name : dep.package;
    }

    inline std::uintmax_t PeakMemoryBytes() noexcept {
#if RUX_OS_WINDOWS
        PROCESS_MEMORY_COUNTERS counters{};
        if (GetProcessMemoryInfo(
                GetCurrentProcess(), &counters, sizeof(counters))) {
            return static_cast<std::uintmax_t>(counters.PeakWorkingSetSize);
        }
#elif RUX_IS_UNIX
        rusage usage{};
        if (getrusage(RUSAGE_SELF, &usage) == 0) {
            // macOS reports bytes directly; other Unices report in KB.
            constexpr std::uintmax_t unitMultiplier =
                (HostOS == OS::MacOS) ? 1ULL : 1024ULL;
            return static_cast<std::uintmax_t>(usage.ru_maxrss) *
                unitMultiplier;
        }
#endif
        return 0;
    }

    inline void PrintBuildStats(const std::filesystem::path& exePath,
                                std::string_view profileName,
                                const BuildStats& stats) {
        const auto totalMs = stats.total.count();
        const double seconds = stats.totalSeconds;
        const std::size_t totalFiles = stats.localFiles + stats.dependencyFiles;
        const std::size_t totalLines = stats.localLines + stats.dependencyLines;
        const std::size_t totalTokens =
            stats.localTokens + stats.dependencyTokens;
        const std::uintmax_t totalSourceSize =
            stats.localSourceSize + stats.dependencySourceSize;
        const double tokenThroughput =
            seconds > 0.0 ? static_cast<double>(totalTokens) / seconds : 0.0;
        const double compileSpeed =
            seconds > 0.0 ? static_cast<double>(totalLines) / seconds : 0.0;
        const double throughput = seconds > 0.0
            ? static_cast<double>(totalSourceSize) / 1024.0 / 1024.0 / seconds
            : 0.0;

        std::print("Rux Compiler {}\n"
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
                   FormatNumber(
                       static_cast<std::uintmax_t>(std::llround(compileSpeed))),
                   FormatTokenThroughput(tokenThroughput),
                   FormatDecimal(throughput, 2));
    }

    inline void PrintBuildSummary(const std::filesystem::path& exePath,
                                  std::string_view profileName,
                                  const BuildStats& stats) {
        const auto totalMs = stats.total.count();
        const std::size_t totalFiles = stats.localFiles + stats.dependencyFiles;
        const std::size_t totalLines = stats.localLines + stats.dependencyLines;
        const std::size_t totalTokens =
            stats.localTokens + stats.dependencyTokens;
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

    inline std::optional<std::filesystem::path> RequireManifest() {
        auto path = Manifest::Find();
        if (!path) {
            std::print(stderr,
                       "error: could not find 'Rux.toml' in '{}' or any parent "
                       "directory\n",
                       std::filesystem::current_path().string());
        }
        return path;
    }

    inline std::optional<Manifest>
    LoadManifest(const std::filesystem::path& path) {
        auto m = Manifest::Load(path);
        if (!m)
            std::print(stderr, "error: failed to parse '{}'\n", path.string());
        return m;
    }

    inline std::filesystem::path
    ResolveBuildOutputDir(const std::filesystem::path& root,
                          const Manifest& manifest,
                          std::string_view profileName) {
        std::filesystem::path output = manifest.build.output.empty()
            ? std::filesystem::path("Bin")
            : std::filesystem::path(manifest.build.output);
        if (output.is_relative()) output = root / output;
        return (output / std::string(profileName)).lexically_normal();
    }

    inline std::filesystem::path RegistryPackagesDir() {
#if RUX_OS_WINDOWS
        wchar_t buf[MAX_PATH]{};
        GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
        return std::filesystem::path(buf) / "Rux" / "Packages";
#else
        const char* home = getenv("HOME");
        return std::filesystem::path(home ? home : "/tmp") / ".rux" /
            "packages";
#endif
    }
} // namespace Rux::Misc

inline constexpr std::string_view kRegistryUrl =
    "https://raw.githubusercontent.com/rux-lang/Registry/refs/heads/main/"
    "Packages.json";

#if RUX_OS_WINDOWS
// Fetch the body of an HTTPS URL using WinHTTP. Returns nullopt on failure.
inline std::optional<std::string> FetchUrl(const std::string& url) {
    std::string cmd = "curl -s " + url;
    std::array<char, 128> buffer;
    std::string result;

    FILE* pipe = _popen(cmd.c_str(), "r");

    if (!pipe) return std::nullopt;

    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }

    _pclose(pipe);
    return result;
}
#else
inline std::string ShellQuote(const std::string& value) {
    std::size_t single_quotes = 0;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\'') {
            ++single_quotes;
        }
    }

    std::string quoted;
    quoted.reserve(value.size() + (single_quotes * 3) + 2);

    quoted += '\'';
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char ch = value[i];
        if (ch == '\'') {
            quoted += "'\\''";
        }
        else {
            quoted += ch;
        }
    }
    quoted += '\'';
    return quoted;
}

inline std::optional<std::string>
RunCommandCapture(const std::string& command) {
    FILE* pipe = ::popen(command.c_str(), "r");
    if (!pipe) return std::nullopt;

    std::string output;
    std::array<char, 4096> buffer{};

    while (::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe))
        output.append(buffer.data());

    const int status = ::pclose(pipe);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return std::nullopt;
    }
    return output;
}

inline std::optional<std::string> FetchUrl(const std::string& url) {
    const std::string quotedUrl = ShellQuote(url);
    if (auto body = RunCommandCapture("curl -fsSL " + quotedUrl)) return body;
    return RunCommandCapture("wget -qO- " + quotedUrl);
}
#endif

namespace Rux {

    // Parse global options from the command line arguments.
    inline GlobalOptions
    Cli::ParseGlobalOptions(std::span<const std::string_view> args) {
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

    // Lookup a string value in a flat JSON object: { "Key": "value", ... }
    inline std::string JsonLookupString(std::string_view json,
                                        std::string_view key) {
        const std::string needle = "\"" + std::string(key) + "\"";
        std::size_t pos = 0;
        while ((pos = json.find(needle, pos)) != std::string_view::npos) {
            std::size_t i = pos + needle.size();
            while (i < json.size() &&
                   (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' ||
                    json[i] == '\n'))
                ++i;
            if (i >= json.size() || json[i] != ':') {
                pos = i;
                continue;
            }
            ++i;
            while (i < json.size() &&
                   (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' ||
                    json[i] == '\n'))
                ++i;
            if (i >= json.size() || json[i] != '"') {
                pos = i;
                continue;
            }
            ++i;
            const auto end = json.find('"', i);
            if (end == std::string_view::npos) break;
            return std::string(json.substr(i, end - i));
        }
        return {};
    }

    // Resolve the build output directory for a given profile.
    inline std::filesystem::path
    ResolveBuildOutputDir(const std::filesystem::path& root,
                          const Manifest& manifest,
                          std::string_view profileName) {
        std::filesystem::path output = manifest.build.output.empty()
            ? std::filesystem::path("bin")
            : std::filesystem::path(manifest.build.output);
        if (output.is_relative()) output = root / output;
        return (output / std::string(profileName)).lexically_normal();
    }

    // Clone a git repository into dest. Returns true on success.
    inline bool GitClone(const std::string& repoUrl,
                         const std::filesystem::path& dest,
                         bool devBranch) {
#if RUX_OS_WINDOWS
        std::wstring cmd{};
        if (!devBranch) {
            cmd = L"git clone " + std::wstring(repoUrl.begin(), repoUrl.end()) +
                L" \"" + dest.wstring() + L"\"";
        }
        else {
            cmd = L"git clone --branch dev " +
                std::wstring(repoUrl.begin(), repoUrl.end()) + L" \"" +
                dest.wstring() + L"\"";
        }
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr,
                            cmd.data(),
                            nullptr,
                            nullptr,
                            FALSE,
                            0,
                            nullptr,
                            nullptr,
                            &si,
                            &pi))
            return false;
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exitCode = 1;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return exitCode == 0;
#else
        const std::string cmd = devBranch
            ? "git clone -b dev " + repoUrl + " \"" + dest.string() + "\""
            : "git clone " + repoUrl + " \"" + dest.string() + "\"";
        return std::system(cmd.c_str()) == 0;
#endif
    }

    // Pull latest changes in an existing git repository. Returns true on
    // success.
    inline bool GitPull(const std::filesystem::path& repoDir) {
#if RUX_OS_WINDOWS
        std::wstring cmd = L"git -C \"" + repoDir.wstring() + L"\" pull";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr,
                            cmd.data(),
                            nullptr,
                            nullptr,
                            FALSE,
                            0,
                            nullptr,
                            nullptr,
                            &si,
                            &pi))
            return false;
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exitCode = 1;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return exitCode == 0;
#else
        const std::string cmd = "git -C \"" + repoDir.string() + "\" pull";
        return std::system(cmd.c_str()) == 0;
#endif
    }

}; // namespace Rux
