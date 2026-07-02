#include "Driver/BuildTarget.h"

#include "Platform/Platform.h"
#include "Platform/Target.h"
#include "Platform/WinApi.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <format>
#include <print>
#include <ranges>

namespace Rux::Misc {

using namespace Platform;

std::string TargetName() {
    if constexpr (HostArch == Arch::Unknown) {
        return std::string{ToString(HostOS)};
    }

    return std::format("{} {}", ToString(HostOS), ToString(HostArch));
}

std::string HostTargetTriple() {
    auto triple = std::format("{}-{}", ToString(HostOS), ToString(HostArch));
    std::transform(std::begin(triple), std::end(triple), std::begin(triple),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return triple;
}

bool IsSupportedTargetTriple(const std::string_view target) {
    constexpr std::array supported_targets{"linux-x64",     "windows-x64",   "macos-x64",
                                           "macos-aarch64", "freebsd-x64",   "openbsd-x64",
                                           "netbsd-x64",    "dragonfly-x64", "illumos-x64"};

    return std::ranges::contains(supported_targets, target);
}

std::string_view TargetOsName(const std::string_view target) {
    const auto dash_pos = target.find('-');
    if (dash_pos == std::string_view::npos) {
        return "";
    }

    const auto os_prefix = target.substr(0, dash_pos);

    if (os_prefix == "linux") {
        return "Linux";
    }
    if (os_prefix == "windows") {
        return "Windows";
    }
    if (os_prefix == "macos") {
        return "macOS";
    }
    if (os_prefix == "freebsd" || os_prefix == "openbsd" || os_prefix == "netbsd" || os_prefix == "dragonfly") {
        return "BSD";
    }
    if (os_prefix == "illumos") {
        return "Illumos";
    }

    return "";
}

Platform::OS TargetTripleOs(const std::string_view target) {
    const auto dash_pos = target.find('-');
    const auto os_prefix = dash_pos == std::string_view::npos ? target : target.substr(0, dash_pos);

    if (os_prefix == "linux") {
        return Platform::OS::Linux;
    }
    if (os_prefix == "windows") {
        return Platform::OS::Windows;
    }
    if (os_prefix == "macos") {
        return Platform::OS::MacOS;
    }
    if (os_prefix == "freebsd") {
        return Platform::OS::FreeBSD;
    }
    if (os_prefix == "openbsd") {
        return Platform::OS::OpenBSD;
    }
    if (os_prefix == "netbsd") {
        return Platform::OS::NetBSD;
    }
    if (os_prefix == "dragonfly") {
        return Platform::OS::DragonFlyBSD;
    }
    if (os_prefix == "illumos") {
        return Platform::OS::Illumos;
    }

    return Platform::HostOS;
}

bool DeclMatchesTarget(const Decl &decl, const std::string_view target) {
    if (decl.targetOs.empty()) {
        return true;
    }
    const std::string_view targetOs = TargetOsName(target);
    // Normalize both sides for robust comparison.
    if (decl.targetOs.size() != targetOs.size()) {
        return false;
    }
    // Case-insensitive comparison handles any casing in @[Target("...")].
    for (std::size_t i = 0; i < decl.targetOs.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(decl.targetOs[i])) !=
            std::tolower(static_cast<unsigned char>(targetOs[i]))) {
            return false;
        }
    }
    return true;
}

bool IsPlatformPackageName(const std::string_view name) {
    return name == "Windows" || name == "Linux" || name == "macOS" || name == "BSD" || name == "Illumos";
}

bool PlatformPackageMatchesTarget(const std::string_view name, const std::string_view target) {
    return name == TargetOsName(target);
}

namespace {

void PruneDeclForTarget(Decl &decl, const std::string_view target) {
    if (auto *module = dynamic_cast<ModuleDecl *>(&decl)) {
        PruneDeclsForTarget(module->items, target);
    }
    else if (auto *block = dynamic_cast<ExternBlockDecl *>(&decl)) {
        PruneDeclsForTarget(block->items, target);
    }
}

} // namespace

void PruneDeclsForTarget(std::vector<DeclPtr> &decls, const std::string_view target) {
    std::erase_if(decls, [&](const DeclPtr &decl) { return !decl || !DeclMatchesTarget(*decl, target); });
    for (const auto &decl : decls) {
        PruneDeclForTarget(*decl, target);
    }
}

void PruneModuleForTarget(Module &module, const std::string_view target) {
    PruneDeclsForTarget(module.items, target);
}

std::string DependencyPackageName(const Dependency &dep) {
    return dep.package.empty() ? dep.name : dep.package;
}

std::optional<std::filesystem::path> RequireManifest() {
    auto path = Manifest::Find();
    if (!path) {
        std::print(stderr,
                   "error: could not find 'Rux.toml' in '{}' or any parent "
                   "directory\n",
                   std::filesystem::current_path().string());
    }
    return path;
}

std::optional<std::filesystem::path> RequireManifest(const std::filesystem::path &manifestPath) {
    // When no explicit path is given, fall back to directory-walking discovery.
    if (manifestPath.empty()) {
        return RequireManifest();
    }
    // Validate that the explicitly-provided manifest exists.
    std::error_code ec;
    if (!std::filesystem::exists(manifestPath, ec)) {
        std::print(stderr, "error: specified manifest '{}' not found\n", manifestPath.string());
        return std::nullopt;
    }
    return manifestPath;
}

std::optional<Manifest> LoadManifest(const std::filesystem::path &path) {
    auto m = Manifest::Load(path);
    if (!m) {
        std::print(stderr, "error: failed to parse '{}'\n", path.string());
    }
    return m;
}

std::filesystem::path ResolveBuildOutputDir(const std::filesystem::path &root, const Manifest &manifest,
                                            std::string_view profileName) {
    std::filesystem::path output =
        manifest.build.output.empty() ? std::filesystem::path("Bin") : std::filesystem::path(manifest.build.output);
    if (output.is_relative()) {
        output = root / output;
    }
    return (output / std::string(profileName)).lexically_normal();
}

std::filesystem::path RegistryPackagesDir() {
#if RUX_OS_WINDOWS
    wchar_t buf[MAX_PATH]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    return std::filesystem::path(buf) / "Rux" / "Packages";
#else
    const char *home = std::getenv("HOME");
    return std::filesystem::path(home ? home : "/tmp") / ".rux" / "packages";
#endif
}

} // namespace Rux::Misc
