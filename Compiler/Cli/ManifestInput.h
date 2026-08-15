#pragma once

// CLI-owned manifest discovery and diagnostic presentation. The reusable
// Driver and Package components return values and diagnostics; only this
// command-boundary adapter writes them to stderr.

#include "Package/Manifest.h"

#include <filesystem>
#include <optional>
#include <print>

namespace Rux::CliSupport {
[[nodiscard]] inline std::optional<std::filesystem::path> RequireManifest() {
    auto path = Manifest::Find();
    if (!path) {
        std::print(stderr, "error: could not find 'Rux.toml' in '{}' or any parent directory\n",
                   std::filesystem::current_path().string());
    }
    return path;
}

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
} // namespace Rux::CliSupport
