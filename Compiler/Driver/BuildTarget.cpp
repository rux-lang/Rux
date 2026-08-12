#include "Driver/BuildTarget.h"

#include "System/Os.h"
#include "System/Process.h"
#include "Target/Target.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <print>
#include <ranges>

namespace Rux::Driver {
using namespace Target;
using namespace System;

std::string TargetName() {
    return TargetDisplayName(HostTargetTriple());
}

std::string TargetDisplayName(const std::string_view target) {
    const Arch arch = TargetTripleArch(target);
    if (arch == Arch::Unknown) {
        return std::string{ToString(TargetTripleOs(target))};
    }
    return std::format("{} {}", ToString(TargetTripleOs(target)), ToDisplayString(arch));
}

std::string HostTargetTriple() {
    auto triple = std::format("{}-{}", ToString(HostOS), ToString(HostArch));
    std::transform(std::begin(triple), std::end(triple), std::begin(triple),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return triple;
}

bool IsSupportedTargetTriple(const std::string_view target) {
    constexpr std::array supported_targets{
        "linux-x86_64",   "linux-aarch64",   "windows-x86_64", "windows-aarch64", "macos-x86_64",     "macos-aarch64",
        "freebsd-x86_64", "freebsd-aarch64", "openbsd-x86_64", "netbsd-x86_64",   "dragonfly-x86_64", "illumos-x86_64",
    };

    return std::ranges::contains(supported_targets, CanonicalTargetTriple(target));
}

std::string_view SupportedTargetTriples() {
    return "linux-x86_64, linux-aarch64, windows-x86_64, windows-aarch64, "
           "macos-x86_64, macos-aarch64, freebsd-x86_64, freebsd-aarch64, "
           "openbsd-x86_64, netbsd-x86_64, dragonfly-x86_64, illumos-x86_64";
}

std::string CanonicalTargetTriple(const std::string_view target) {
    const auto dashPos = target.find('-');
    if (dashPos == std::string_view::npos) {
        return std::string(target);
    }

    const auto os = target.substr(0, dashPos);
    auto arch = target.substr(dashPos + 1);
    if (arch == "x64" || arch == "amd64" || arch == "x86-64" || arch == "x86_64") {
        arch = "x86_64";
    }
    else if (arch == "arm64" || arch == "aarch64") {
        arch = "aarch64";
    }
    return std::format("{}-{}", os, arch);
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
    const auto dashPos = target.find('-');
    const auto arch = dashPos == std::string_view::npos ? target : target.substr(dashPos + 1);
    if (arch == "x86") {
        return Target::Arch::X86_32;
    }
    if (arch == "x64" || arch == "amd64" || arch == "x86-64" || arch == "x86_64") {
        return Target::Arch::X86_64;
    }
    if (arch == "arm" || arch == "arm32") {
        return Target::Arch::ARM32;
    }
    if (arch == "arm64" || arch == "aarch64") {
        return Target::Arch::AArch64;
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

bool NativeAArch64BackendRequested() {
    const auto requested = System::GetEnv("RUX_AARCH64_RCU");
    return requested && !requested->empty() && *requested != "0";
}

std::string UnsupportedBackendReason(const std::string_view target, const bool nativeAArch64Backend) {
    const auto triple = CanonicalTargetTriple(target);
    const Arch arch = TargetTripleArch(triple);
    // x86-64 artifacts are encoded and linked in-process, so any supported
    // operating system is reachable from any host. AArch64 still lowers through
    // the platform Clang driver, which only produces artifacts for the machine
    // the compiler runs on; Phases 3-5 of BACKLOG.md replace it.
    if (arch == Arch::X86_64) {
        return {};
    }
    // The native AArch64 back end encodes and links in-process the way the
    // x86-64 one does, so once a build has opted into it `linux-aarch64` is
    // reachable from any host. It is the only AArch64 target that back end
    // reaches; the rest keep the Clang path and its host requirement.
    if (arch == Arch::AArch64 && TargetTripleOs(triple) == OS::Linux && nativeAArch64Backend) {
        return {};
    }
    if (arch != HostArch) {
        return std::format("code generation for architecture '{}' is not implemented yet; building for '{}' "
                           "requires an {} host",
                           ToString(arch), triple, ToString(arch));
    }
    if (TargetTripleOs(triple) != HostOS) {
        return std::format("code generation for '{}' is not implemented yet; the {} back end targets the host "
                           "operating system only",
                           triple, ToDisplayString(arch));
    }
    return {};
}

std::string UnsupportedBackendReason(const std::string_view target) {
    return UnsupportedBackendReason(target, NativeAArch64BackendRequested());
}

bool HostCanExecuteTarget(const std::string_view target) {
    const auto triple = CanonicalTargetTriple(target);
    return TargetTripleArch(triple) == HostArch && TargetTripleOs(triple) == HostOS;
}

namespace {
/// The QEMU user-mode binary that runs `arch` programs. An architecture with no
/// entry has no default, and only RUX_EMULATOR can name one for it.
std::string_view DefaultEmulatorName(const Arch arch) {
    switch (arch) {
    case Arch::AArch64:
        return "qemu-aarch64";
    case Arch::ARM32:
        return "qemu-arm";
    case Arch::RISCV32:
        return "qemu-riscv32";
    case Arch::RISCV64:
        return "qemu-riscv64";
    case Arch::X86_32:
        return "qemu-i386";
    case Arch::X86_64:
        return "qemu-x86_64";
    case Arch::Unknown:
        break;
    }
    return {};
}

/// The words of an emulator command line. A value that names an existing file
/// is one program however it is spelled, so a path containing spaces survives;
/// anything else is split on whitespace, which is how a program and its options
/// are written.
std::vector<std::string> SplitCommandWords(const std::string &value) {
    std::error_code error;
    if (std::filesystem::exists(value, error)) {
        return {value};
    }
    std::vector<std::string> words;
    std::string_view remaining = value;
    while (!remaining.empty()) {
        const auto start = remaining.find_first_not_of(" \t");
        if (start == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(start);
        const auto end = remaining.find_first_of(" \t");
        words.emplace_back(remaining.substr(0, end));
        if (end == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(end);
    }
    return words;
}
} // namespace

std::string LaunchCommand::CommandLine() const {
    std::string text = program.string();
    for (const auto &argument : args) {
        text += ' ';
        text += argument;
    }
    return text;
}

LaunchCommand PrepareLaunch(const ExecutionCommand &command, const std::filesystem::path &artifact,
                            const std::span<const std::string_view> args) {
    LaunchCommand launch{.program = command.IsEmulated() ? command.emulator : artifact, .args = command.emulatorArgs};
    if (command.IsEmulated()) {
        launch.args.push_back(artifact.string());
    }
    for (const auto argument : args) {
        launch.args.emplace_back(argument);
    }
    return launch;
}

ExecutionCommandResult ResolveExecutionCommand(const std::string_view target) {
    const auto triple = CanonicalTargetTriple(target);
    if (HostCanExecuteTarget(triple)) {
        return {.command = ExecutionCommand{}, .error = {}};
    }
    const auto host = HostTargetTriple();
    // A user-mode emulator runs another machine's programs against this
    // system's kernel, so it answers a foreign architecture and nothing else.
    if (TargetTripleOs(triple) != HostOS) {
        return {.command = std::nullopt,
                .error = std::format("cannot run a '{}' artifact on '{}'; build it with 'rux build --target {}' and "
                                     "run it on that target",
                                     triple, host, triple)};
    }

    std::vector<std::string> words;
    if (const auto configured = System::GetEnv("RUX_EMULATOR")) {
        words = SplitCommandWords(*configured);
    }
    const bool named = !words.empty();
    if (!named) {
        const auto fallback = DefaultEmulatorName(TargetTripleArch(triple));
        if (fallback.empty()) {
            return {.command = std::nullopt,
                    .error = std::format("cannot run a '{}' artifact on '{}': no emulator is known for that "
                                         "architecture; set RUX_EMULATOR to one that runs '{}' programs",
                                         triple, host, triple)};
        }
        words.emplace_back(fallback);
    }

    const auto program = System::FindExecutable(words.front());
    if (!program) {
        return {.command = std::nullopt,
                .error = named ? std::format("cannot run a '{}' artifact on '{}': the emulator '{}' named by "
                                             "RUX_EMULATOR was not found",
                                             triple, host, words.front())
                               : std::format("cannot run a '{}' artifact on '{}': the emulator '{}' is not on PATH; "
                                             "install it — it is in the 'qemu-user' package on most distributions — "
                                             "or set RUX_EMULATOR to an emulator that runs '{}' programs",
                                             triple, host, words.front(), triple)};
    }

    ExecutionCommand command{.emulator = *program, .emulatorArgs = {words.begin() + 1, words.end()}};
    // The sysroot is where a dynamically linked guest program finds its loader
    // and its shared libraries; a freestanding one needs none, which is why an
    // unset variable is not an error.
    if (const auto sysroot = System::GetEnvPath("RUX_QEMU_SYSROOT"); sysroot && !sysroot->empty()) {
        std::error_code error;
        if (!std::filesystem::is_directory(*sysroot, error)) {
            return {.command = std::nullopt,
                    .error = std::format("RUX_QEMU_SYSROOT '{}' is not a directory", sysroot->string())};
        }
        command.emulatorArgs.emplace_back("-L");
        command.emulatorArgs.push_back(sysroot->string());
    }
    return {.command = std::move(command), .error = {}};
}

bool IsPlatformPackageName(const std::string_view name) {
    return name == "Windows" || name == "Linux" || name == "macOS" || name == "MacOS" || name == "BSD" ||
           name == "Bsd" || name == "Illumos";
}

bool PlatformPackageMatchesTarget(const std::string_view name, const std::string_view target) {
    const auto targetOs = TargetOsName(target);
    if (name == "MacOS") {
        return targetOs == "macOS";
    }
    if (name == "Bsd") {
        return targetOs == "BSD";
    }
    return name == targetOs;
}

const std::string &DependencyPackageName(const ManifestDependency &dep) {
    return dep.package.Text();
}

std::set<std::string> IntrinsicsAliases(const Manifest &manifest, const std::filesystem::path &root) {
    std::set<std::string> aliases;
    if (manifest.IsWorkspace()) {
        return aliases;
    }
    const std::string wantedNamespace = NormalizeIdentity(intrinsicsPackageNamespace);
    const std::string wantedName = NormalizeIdentity(intrinsicsPackageName);

    for (const auto &dep : manifest.dependencies) {
        if (const RegistryDependencySource *registry = dep.Registry()) {
            if (registry->ns.Normalized() == wantedNamespace && dep.package.Normalized() == wantedName) {
                aliases.insert(dep.importName.Text());
            }
            continue;
        }
        // A path entry names no package, so the only way to know what it points
        // at is to read it. Repository test packages reach the first-party
        // packages this way, so it is the common case rather than a corner.
        const auto target = Manifest::Load((root / dep.Path() / "Rux.toml").lexically_normal());
        if (target.Ok() && IsIntrinsicsPackage(*target.manifest)) {
            aliases.insert(dep.importName.Text());
        }
    }
    return aliases;
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

void ReportManifestDiagnostics(const ManifestResult &result) {
    for (const auto &diagnostic : result.diagnostics) {
        std::print(stderr, "error: {}\n", diagnostic.Format());
    }
}

std::optional<Manifest> LoadManifest(const std::filesystem::path &path) {
    auto result = Manifest::Load(path);
    ReportManifestDiagnostics(result);
    return std::move(result.manifest);
}

std::filesystem::path ResolveBuildOutputDir(const std::filesystem::path &root, const Manifest &manifest,
                                            std::string_view profileName, const std::string_view targetTriple,
                                            const bool includeProfile) {
    std::filesystem::path output =
        manifest.build.output.empty() ? std::filesystem::path("Bin") : std::filesystem::path(manifest.build.output);
    if (output.is_relative()) {
        output = root / output;
    }
    if (includeProfile) {
        output /= profileName;
    }
    // Only a foreign target takes a subdirectory. Keeping the host on its
    // historical path means no manifest, script or test has to learn a new
    // location for the build everyone already runs.
    if (const auto triple = CanonicalTargetTriple(targetTriple); !triple.empty() && triple != HostTargetTriple()) {
        output /= triple;
    }
    return output.lexically_normal();
}

ArtifactKind PackageArtifactKind(const ManifestPackageType type) {
    switch (type) {
    case ManifestPackageType::SharedLibrary:
        return ArtifactKind::SharedLibrary;
    case ManifestPackageType::StaticLibrary:
        return ArtifactKind::StaticLibrary;
    case ManifestPackageType::Executable:
    case ManifestPackageType::SourceLibrary:
        break;
    }
    return ArtifactKind::Executable;
}

std::string OutputFileName(const std::string_view packageName, const ArtifactKind kind, const OS os) {
    switch (kind) {
    case ArtifactKind::SharedLibrary:
        return SharedLibraryFileName(std::string(packageName), os);
    case ArtifactKind::StaticLibrary:
        return StaticLibraryFileName(std::string(packageName), os);
    case ArtifactKind::Executable:
        break;
    }
    return ExecutableFileName(std::string(packageName), os);
}

std::filesystem::path RegistryPackagesDir() {
    // The leaf follows each platform's own casing, matching the parent that
    // UserDataDir picks: %LOCALAPPDATA%\Rux\Packages, or $HOME/.rux/packages.
    return UserDataDir() / (HostOS == OS::Windows ? "Packages" : "packages");
}

namespace {
/**
 * @brief The existing child directory of `parent` whose name normalizes to `segment`.
 *
 * Comparing normalized names, rather than joining the caller's spelling, is
 * what lets a manifest that spells a dependency differently from the registry
 * still find the one cache entry: an install writes the spelling the registry
 * publishes, while a build looks the package up with the spelling the consuming
 * manifest happens to use.
 *
 * `exists(parent / segment.Text())` cannot stand in for the scan. On Windows and
 * macOS it answers yes for a directory that is really spelled otherwise, so the
 * path would not name the directory that is actually there.
 *
 * The choice is deterministic because a case-sensitive filesystem can hold two
 * normalized-equal siblings while directory iteration order is unspecified: the
 * display spelling wins outright, and the lowest name breaks any other tie.
 */
std::optional<std::filesystem::path> ExistingChildDir(const std::filesystem::path &parent,
                                                      const IdentitySegment &segment) {
    std::optional<std::filesystem::path> match;
    std::error_code ec;
    // The error_code overload yields end() on failure, so an unreadable parent
    // is "nothing installed" rather than an exception out of a lookup.
    for (const auto &entry : std::filesystem::directory_iterator(parent, ec)) {
        if (!entry.is_directory(ec)) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (name == segment.Text()) {
            return entry.path();
        }
        // Unlike the enumeration in the CLI, this deliberately normalizes rather
        // than parsing: a lookup has to find the directory whichever way it was
        // spelled, while enumeration must not invent identities out of stray names.
        if (NormalizeIdentity(name) != segment.Normalized()) {
            continue;
        }
        if (!match || name < match->filename().string()) {
            match = entry.path();
        }
    }
    return match;
}

/// The directory to use for `segment` under `parent`: the one already there, or
/// the path an install would create.
std::filesystem::path ResolveSegmentDir(const std::filesystem::path &parent, const IdentitySegment &segment) {
    if (auto existing = ExistingChildDir(parent, segment)) {
        return *existing;
    }
    return parent / segment.Text();
}
} // namespace

std::filesystem::path RegistryPackageParentDir(const IdentitySegment &ns, const IdentitySegment &name) {
    return ResolveSegmentDir(ResolveSegmentDir(RegistryPackagesDir(), ns), name);
}

std::filesystem::path RegistryPackageDir(const IdentitySegment &ns, const IdentitySegment &name,
                                         const SemanticVersion &version) {
    return RegistryPackageParentDir(ns, name) / version.Text();
}

std::vector<InstalledPackage> InstalledVersions(const IdentitySegment &ns, const IdentitySegment &name) {
    std::vector<InstalledPackage> installed;
    const std::filesystem::path packageDir = RegistryPackageParentDir(ns, name);

    std::error_code ec;
    if (!std::filesystem::is_directory(packageDir, ec)) {
        return installed;
    }
    for (const auto &entry : std::filesystem::directory_iterator(packageDir, ec)) {
        if (!entry.is_directory(ec)) {
            continue;
        }
        // A directory whose name is not a version was left by something other
        // than an install; skipping it keeps one stray entry from failing every
        // later resolution.
        auto version = SemanticVersion::Parse(entry.path().filename().string());
        if (!version) {
            continue;
        }
        installed.push_back(InstalledPackage{.version = std::move(*version), .root = entry.path()});
    }
    std::ranges::sort(installed, [](const InstalledPackage &left, const InstalledPackage &right) {
        const int precedence = SemanticVersion::ComparePrecedence(left.version, right.version);
        if (precedence != 0) {
            return precedence < 0;
        }
        return left.version.Text() < right.version.Text();
    });
    return installed;
}

std::optional<InstalledPackage> FindInstalledPackage(const IdentitySegment &ns, const IdentitySegment &name,
                                                     const VersionRange &range) {
    std::optional<InstalledPackage> best;
    for (auto &candidate : InstalledVersions(ns, name)) {
        if (!range.Matches(candidate.version)) {
            continue;
        }
        if (!best || SemanticVersion::ComparePrecedence(candidate.version, best->version) >= 0) {
            best = std::move(candidate);
        }
    }
    return best;
}
} // namespace Rux::Driver
