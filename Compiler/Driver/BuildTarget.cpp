#include "Driver/BuildTarget.h"

#include "System/Host.h"
#include "System/Os.h"
#include "Target/Target.h"

#include <algorithm>

namespace Rux::Driver {
using namespace Target;
using namespace System;

std::string TargetName() {
    return Target::TargetTriple::Host().DisplayName();
}

std::string TargetDisplayName(const Target::TargetTriple target) {
    return target.DisplayName();
}

std::string HostTargetTriple() {
    return std::string(Target::TargetTriple::Host().CanonicalName());
}

bool IsSupportedTargetTriple(const std::string_view target) {
    return Target::TargetTriple::Parse(target).has_value();
}

const std::string &SupportedTargetTriples() {
    return Target::SupportedTargetTripleNames();
}

std::string CanonicalTargetTriple(const std::string_view target) {
    const auto parsed = Target::TargetTriple::Parse(target);
    return parsed ? std::string(parsed->CanonicalName()) : std::string(target);
}

std::string_view TargetOsName(const Target::TargetTriple target) {
    return Target::ToString(target.Os());
}

TargetContext TargetContextForTriple(const Target::TargetTriple target) {
    const Target::OS os = target.Os();
    const Target::Arch arch = target.Architecture();
    const Target::DataModel dataModel = os == Target::OS::Windows ? Target::DataModel::LLP64 : Target::DataModel::LP64;
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

bool HostCanExecuteTarget(const Target::TargetTriple target) {
    return CanExecuteTargetDirectly(HostOS, GetHostArchitectureInfo(), target.Os(), target.Architecture());
}

bool CanExecuteTargetDirectly(const OS hostOs, const HostArchitectureInfo hostArchitectures, const OS targetOs,
                              const Arch targetArch) noexcept {
    return targetOs == hostOs &&
           (targetArch == hostArchitectures.processArch || targetArch == hostArchitectures.nativeArch);
}

bool IsPlatformPackageName(const std::string_view name) {
    const std::string normalized = NormalizeIdentity(name);
    return normalized == "freebsd" || normalized == "linux" || normalized == "macos" || normalized == "windows";
}

bool PlatformPackageMatchesTarget(const std::string_view name, const Target::TargetTriple target) {
    return NormalizeIdentity(name) == NormalizeIdentity(TargetOsName(target));
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

std::filesystem::path ResolveRawOutputRoot(const std::filesystem::path &root, const Manifest &manifest) {
    std::filesystem::path output =
        manifest.build.output.empty() ? std::filesystem::path("Bin") : std::filesystem::path(manifest.build.output);
    if (output.is_relative()) {
        output = root / output;
    }
    return output.lexically_normal();
}

std::filesystem::path TargetOutputPath(const Target::TargetTriple target) {
    return std::filesystem::path(Target::ToString(target.Os())) / Target::ToDisplayString(target.Architecture());
}

std::filesystem::path ResolveArtifactOutputDir(const std::filesystem::path &root, const Manifest &manifest,
                                               const BuildProfile profile, const Target::TargetTriple target) {
    return ResolveRawOutputRoot(root, manifest) / ToString(profile) / TargetOutputPath(target);
}

std::filesystem::path ResolveTestOutputDir(const std::filesystem::path &root, const Manifest &manifest,
                                           const Target::TargetTriple target) {
    auto output = ResolveRawOutputRoot(root, manifest);
    if (target != Target::TargetTriple::Host()) {
        output /= TargetOutputPath(target);
    }
    return output;
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
