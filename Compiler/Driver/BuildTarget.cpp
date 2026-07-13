#include "Driver/BuildTarget.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <print>
#include <ranges>

#include "System/Os.h"
#include "Target/Target.h"

namespace Rux::Driver {

using namespace Target;
using namespace System;

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

Target::OS TargetTripleOs(const std::string_view target) {
    const auto dash_pos = target.find('-');
    const auto os_prefix = dash_pos == std::string_view::npos ? target : target.substr(0, dash_pos);

    if (os_prefix == "linux") {
        return Target::OS::Linux;
    }
    if (os_prefix == "windows") {
        return Target::OS::Windows;
    }
    if (os_prefix == "macos") {
        return Target::OS::MacOS;
    }
    if (os_prefix == "freebsd") {
        return Target::OS::FreeBSD;
    }
    if (os_prefix == "openbsd") {
        return Target::OS::OpenBSD;
    }
    if (os_prefix == "netbsd") {
        return Target::OS::NetBSD;
    }
    if (os_prefix == "dragonfly") {
        return Target::OS::DragonFlyBSD;
    }
    if (os_prefix == "illumos") {
        return Target::OS::Illumos;
    }

    return Target::HostOS;
}

Target::Arch TargetTripleArch(const std::string_view target) {
    const auto dashPos = target.rfind('-');
    const auto arch = dashPos == std::string_view::npos ? target : target.substr(dashPos + 1);
    if (arch == "x86") {
        return Target::Arch::X86_32;
    }
    if (arch == "x64" || arch == "x86_64") {
        return Target::Arch::X86_64;
    }
    if (arch == "arm" || arch == "arm32") {
        return Target::Arch::ARM32;
    }
    if (arch == "arm64" || arch == "aarch64") {
        return Target::Arch::ARM64;
    }
    if (arch == "riscv32") {
        return Target::Arch::RISCV32;
    }
    if (arch == "riscv64") {
        return Target::Arch::RISCV64;
    }
    return Target::HostArch;
}

TargetContext TargetContextForTriple(const std::string_view target) {
    const Target::OS os = TargetTripleOs(target);
    const Target::Arch arch = TargetTripleArch(target);
    const bool is64 = Target::Is64Bit(arch);
    const Target::DataModel dataModel = os == Target::OS::Windows
                                          ? (is64 ? Target::DataModel::LLP64 : Target::DataModel::ILP32)
                                          : (is64 ? Target::DataModel::LP64 : Target::DataModel::ILP32);
    const Target::ABIInfo abi = Target::GetABIInfo(os, arch, dataModel);
    const bool native = os == Target::HostOS && arch == Target::HostArch;
    return TargetContext{.os = os,
                         .arch = arch,
                         .data_model = dataModel,
                         .abi = abi.abi,
                         .default_cc = abi.cc,
                         .endianness = native ? Target::HostEndianness : Target::Endian::Little,
                         .object_format = Target::GetObjectFormat(os),
                         .pointer_size = Target::GetPointerSize(arch),
                         .cpu_features = native ? Target::HostCpuFeatures : Target::CpuFeature::None};
}

bool IsPlatformPackageName(const std::string_view name) {
    return name == "Windows" || name == "Linux" || name == "macOS" || name == "BSD" || name == "Illumos";
}

bool PlatformPackageMatchesTarget(const std::string_view name, const std::string_view target) {
    return name == TargetOsName(target);
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
    if constexpr (HostOS == OS::Windows) {
        return GetEnvPath("LOCALAPPDATA").value_or(std::filesystem::path{}) / "Rux" / "Packages";
    }
    else {
        return GetEnvPath("HOME").value_or(std::filesystem::path("/tmp")) / ".rux" / "packages";
    }
}

} // namespace Rux::Driver
