#pragma once

// CLI-owned manifest discovery and diagnostic presentation. The reusable
// Driver and Package components return values and diagnostics; only this
// command-boundary adapter writes them to stderr.

#include "Package/Manifest.h"

#include <filesystem>
#include <format>
#include <optional>
#include <print>
#include <set>
#include <string>
#include <vector>

namespace Rux::CliSupport {
/// Find the nearest `Rux.toml`, searching upward from the working directory.
///
/// @return nullopt when none was found, having already reported why
[[nodiscard]] inline std::optional<std::filesystem::path> RequireManifest() {
    auto path = Manifest::Find();
    if (!path) {
        std::print(stderr, "error: could not find 'Rux.toml' in '{}' or any parent directory\n",
                   std::filesystem::current_path().string());
    }
    return path;
}

/// Use an explicitly given manifest path, falling back to the upward search when it is empty. An explicit path that
/// does not exist is an error rather than a reason to search, since the user named a specific file.
[[nodiscard]] inline std::optional<std::filesystem::path> RequireManifest(const std::filesystem::path &manifestPath) {
    if (manifestPath.empty()) {
        return RequireManifest();
    }
    std::error_code error;
    if (!std::filesystem::exists(manifestPath, error)) {
        std::print(stderr, "error: specified manifest '{}' not found\n", manifestPath.string());
        if (error) {
            std::print(stderr, "  note: system error {}: {}\n", error.value(), error.message());
        }
        return std::nullopt;
    }
    return manifestPath;
}

inline void ReportManifestDiagnostics(const ManifestResult &result) {
    for (const auto &diagnostic : result.diagnostics) {
        std::print(stderr, "{}", diagnostic.Render());
    }
}

[[nodiscard]] inline std::optional<Manifest> LoadManifest(const std::filesystem::path &path) {
    auto result = Manifest::Load(path);
    ReportManifestDiagnostics(result);
    return std::move(result.manifest);
}

/// One package a workspace manifest declares. `manifest` is empty when the member could not be loaded: `problem` then
/// says why, or is empty when the manifest's own diagnostics were already reported.
struct WorkspaceMember {
    std::filesystem::path manifestPath;
    std::string label;
    std::optional<Manifest> manifest;
    std::string problem;
};

/// Load every member `workspace` declares, in declaration order, relative to the workspace manifest's directory.
[[nodiscard]] inline std::vector<WorkspaceMember> LoadWorkspaceMembers(const std::filesystem::path &workspacePath,
                                                                       const Manifest &workspace) {
    std::vector<WorkspaceMember> members;
    for (const auto &member : workspace.workspace.packages) {
        WorkspaceMember loaded{.manifestPath = (workspacePath.parent_path() / member / "Rux.toml").lexically_normal(),
                               .label = std::filesystem::path(member).lexically_normal().generic_string(),
                               .manifest = std::nullopt,
                               .problem = {}};
        std::error_code error;
        if (!std::filesystem::exists(loaded.manifestPath, error)) {
            loaded.problem = std::format("workspace member '{}' has no 'Rux.toml'", member);
        }
        else if (auto manifest = LoadManifest(loaded.manifestPath)) {
            if (manifest->IsWorkspace() || manifest->package.name.Empty())
                loaded.problem = std::format("workspace member '{}' is not a package", member);
            else
                loaded.manifest = std::move(manifest);
        }
        members.push_back(std::move(loaded));
    }
    return members;
}

/// The sources a workspace offers its members' dependencies: every loaded member's root, and the namespaces those
/// members declare, which the workspace owns.
struct WorkspaceSources {
    std::vector<std::filesystem::path> packageRoots;
    std::set<std::string> namespaces;
};

[[nodiscard]] inline WorkspaceSources CollectWorkspaceSources(const std::vector<WorkspaceMember> &members) {
    WorkspaceSources sources;
    for (const auto &member : members) {
        if (!member.manifest)
            continue;
        sources.packageRoots.push_back(member.manifestPath.parent_path());
        if (member.manifest->package.ns)
            sources.namespaces.insert(member.manifest->package.ns->Normalized());
    }
    return sources;
}
} // namespace Rux::CliSupport
