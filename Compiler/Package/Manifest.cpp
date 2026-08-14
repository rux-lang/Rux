#include "Package/Manifest.h"

#include "Package/ManifestSyntax.h"
#include "Package/ManifestValidation.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <ranges>
#include <sstream>

namespace Rux {
std::string_view ToString(const ManifestPackageType type) noexcept {
    switch (type) {
    case ManifestPackageType::Executable:
        return "Executable";
    case ManifestPackageType::SharedLibrary:
        return "SharedLibrary";
    case ManifestPackageType::StaticLibrary:
        return "StaticLibrary";
    case ManifestPackageType::SourceLibrary:
        return "SourceLibrary";
    }
    return "Executable";
}

std::optional<ManifestPackageType> ParseManifestPackageType(const std::string_view value) noexcept {
    if (value == "Executable") {
        return ManifestPackageType::Executable;
    }
    if (value == "SharedLibrary") {
        return ManifestPackageType::SharedLibrary;
    }
    if (value == "StaticLibrary") {
        return ManifestPackageType::StaticLibrary;
    }
    if (value == "SourceLibrary") {
        return ManifestPackageType::SourceLibrary;
    }
    return std::nullopt;
}

std::string_view ManifestTargetOSName(const Target::OS os) noexcept {
    switch (os) {
    case Target::OS::FreeBSD:
        return "FreeBSD";
    case Target::OS::Linux:
        return "Linux";
    case Target::OS::MacOS:
        return "MacOS";
    case Target::OS::Windows:
        return "Windows";
    default:
        return "";
    }
}

std::optional<Target::OS> ParseManifestTargetOS(const std::string_view value) noexcept {
    constexpr Target::OS supported[] = {Target::OS::FreeBSD, Target::OS::Linux, Target::OS::MacOS, Target::OS::Windows};
    for (const Target::OS os : supported) {
        if (ManifestTargetOSName(os) == value) {
            return os;
        }
    }
    return std::nullopt;
}

std::string ManifestDiagnostic::Format() const {
    return std::format("{}:{}:{}: {}", path.string(), line, column, message);
}

const std::string &ManifestDependency::Path() const noexcept {
    static const std::string none;
    if (const auto *local = std::get_if<PathDependencySource>(&source)) {
        return local->path;
    }
    return none;
}

bool ManifestDependency::MatchesTarget(const Target::OS os) const noexcept {
    return targetOS.empty() || std::ranges::contains(targetOS, os);
}

std::map<std::string, std::string> Build::ConfigValues() const {
    std::map<std::string, std::string> values;
    for (const auto &[name, value] : defines) {
        values.emplace(name, value.text);
    }
    return values;
}

ManifestResult Manifest::Parse(const std::string_view text, const std::filesystem::path &path) {
    ManifestResult result;
    if (text.size() > manifestMaxBytes) {
        result.diagnostics.push_back({path, 1, 1, std::format("manifest is larger than {} bytes", manifestMaxBytes)});
        return result;
    }

    auto syntax = ManifestDetail::ParseManifestSyntax(text);
    if (!syntax) {
        result.diagnostics.push_back(
            {path, syntax.error().location.line, syntax.error().location.column, syntax.error().message});
        return result;
    }

    auto validation = ManifestDetail::ValidateManifestV1(std::move(*syntax));
    if (!validation) {
        result.diagnostics.push_back(
            {path, validation.error().location.line, validation.error().location.column, validation.error().message});
        return result;
    }
    result.manifest = std::move(*validation);
    return result;
}

ManifestResult Manifest::Load(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        ManifestResult result;
        result.diagnostics.push_back({path, 1, 1, "could not open the manifest"});
        return result;
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return Parse(contents.str(), path);
}

const ManifestDependency *Manifest::FindDependency(const IdentitySegment &importName) const {
    const auto found = std::ranges::find(dependencies, importName, &ManifestDependency::importName);
    return found == dependencies.end() ? nullptr : &*found;
}

bool Manifest::AddRegistryDependency(IdentitySegment importName, IdentitySegment ns, VersionRange version) {
    RegistryDependencySource source{std::move(ns), std::move(version)};
    const auto found = std::ranges::find(dependencies, importName, &ManifestDependency::importName);
    if (found != dependencies.end()) {
        const auto *existing = found->Registry();
        if (existing != nullptr && existing->ns == source.ns && existing->version.Text() == source.version.Text() &&
            found->package == importName) {
            return false;
        }
        found->package = importName;
        found->source = std::move(source);
        return true;
    }
    dependencies.push_back({importName, importName, std::move(source), {}});
    return true;
}

bool Manifest::AddPathDependency(IdentitySegment importName, std::string path) {
    const auto found = std::ranges::find(dependencies, importName, &ManifestDependency::importName);
    if (found != dependencies.end()) {
        if (found->IsPath() && found->Path() == path && found->package == importName) {
            return false;
        }
        found->package = importName;
        found->source = PathDependencySource{std::move(path)};
        return true;
    }
    dependencies.push_back({importName, importName, PathDependencySource{std::move(path)}, {}});
    return true;
}

bool Manifest::RemoveDependency(const IdentitySegment &importName) {
    return std::erase_if(dependencies,
                         [&](const ManifestDependency &dependency) { return dependency.importName == importName; }) > 0;
}

std::optional<std::filesystem::path> Manifest::Find(const std::filesystem::path &start) {
    auto dir = std::filesystem::absolute(start);

    while (true) {
        if (auto candidate = dir / "Rux.toml"; std::filesystem::exists(candidate)) {
            return candidate;
        }
        auto parent = dir.parent_path();
        if (parent == dir) {
            break;
        }
        dir = std::move(parent);
    }

    return std::nullopt;
}

bool IsIntrinsicsPackage(const Manifest &manifest) {
    return !manifest.IsWorkspace() && manifest.package.ns &&
           manifest.package.ns->Normalized() == NormalizeIdentity(intrinsicsPackageNamespace) &&
           manifest.package.name.Normalized() == NormalizeIdentity(intrinsicsPackageName);
}

std::vector<std::filesystem::path> DiscoverManifestlessWorkspaceManifests(const std::filesystem::path &root) {
    std::vector<std::filesystem::path> manifests;

    // Match `rux test`: directories without a manifest are grouping
    // directories, searched only a few levels deep so output trees and other
    // unrelated directory hierarchies are not walked indefinitely.
    auto DiscoverTests = [&](const std::filesystem::path &testsRoot) {
        std::error_code ec;
        if (!std::filesystem::exists(testsRoot, ec)) {
            return;
        }
        constexpr int maxGroupDepth = 3;
        std::vector<std::pair<std::filesystem::path, int>> pending;
        pending.emplace_back(testsRoot, 0);
        while (!pending.empty()) {
            const auto [dir, depth] = std::move(pending.back());
            pending.pop_back();
            for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
                if (!entry.is_directory()) {
                    continue;
                }
                const auto manifestPath = entry.path() / "Rux.toml";
                if (std::filesystem::exists(manifestPath, ec)) {
                    manifests.push_back(manifestPath.lexically_normal());
                }
                else if (depth + 1 < maxGroupDepth) {
                    pending.emplace_back(entry.path(), depth + 1);
                }
            }
        }
    };

    const auto workspaceRoot = std::filesystem::absolute(root).lexically_normal();
    DiscoverTests(workspaceRoot / "Tests");

    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(workspaceRoot, ec)) {
        if (!entry.is_directory()) {
            continue;
        }
        const auto memberManifest = entry.path() / "Rux.toml";
        if (!std::filesystem::exists(memberManifest, ec)) {
            continue;
        }
        manifests.push_back(memberManifest.lexically_normal());
        DiscoverTests(entry.path() / "Tests");
    }

    std::ranges::sort(manifests);
    const auto duplicates = std::ranges::unique(manifests);
    manifests.erase(duplicates.begin(), duplicates.end());
    return manifests;
}

std::expected<PackageSpec, std::string> ParsePackageSpec(const std::string_view spec) {
    std::string_view rest = spec;
    PackageSpec parsed;

    if (const auto at = rest.find('@'); at != std::string_view::npos) {
        const std::string_view requirement = rest.substr(at + 1);
        if (requirement.empty()) {
            return std::unexpected("missing version requirement after '@'");
        }
        auto range = VersionRange::Parse(requirement);
        if (!range) {
            return std::unexpected(
                std::format("invalid version requirement '{}': {}", requirement, Describe(range.error())));
        }
        parsed.version = std::move(*range);
        rest = rest.substr(0, at);
    }

    std::string_view nameText = rest;
    if (const auto slash = rest.find('/'); slash != std::string_view::npos) {
        auto ns = IdentitySegment::Parse(rest.substr(0, slash));
        if (!ns) {
            return std::unexpected(
                std::format("invalid namespace '{}': {}", rest.substr(0, slash), Describe(ns.error())));
        }
        parsed.ns = std::move(*ns);
        nameText = rest.substr(slash + 1);
    }

    auto name = IdentitySegment::Parse(nameText);
    if (!name) {
        return std::unexpected(std::format("invalid package name '{}': {}", nameText, Describe(name.error())));
    }
    parsed.name = std::move(*name);
    return parsed;
}
} // namespace Rux
