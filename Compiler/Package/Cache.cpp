#include "Package/Cache.h"

#include "System/Os.h"

#include <algorithm>

namespace Rux::Packages {
using namespace Target;
using namespace System;

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
} // namespace Rux::Packages
