#pragma once

// Helpers for resolving the active build target (OS/arch triple), pruning the
// AST to that target, and locating workspace/registry directories.

#include "Rux/Ast.h"
#include "Rux/Manifest.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Rux::Misc {

// ---- Target triples ---------------------------------------------------------

// Human-readable name of the host target, e.g. "Windows x64".
[[nodiscard]] std::string TargetName();

// Lower-case "os-arch" triple of the host, e.g. "windows-x64".
[[nodiscard]] std::string HostTargetTriple();

// True if `target` is one of the officially supported "os-arch" triples.
[[nodiscard]] bool IsSupportedTargetTriple(std::string_view target);

// Canonical OS name ("Linux", "Windows", "macOS", "BSD", "Illumos") for the OS
// component of an "os-arch" triple, or "" if it cannot be determined.
[[nodiscard]] std::string_view TargetOsName(std::string_view target);

// ---- Target-conditional declarations ----------------------------------------

[[nodiscard]] bool DeclMatchesTarget(const Decl &decl, std::string_view target);

// Names that denote a platform package rather than a normal dependency.
[[nodiscard]] bool IsPlatformPackageName(std::string_view name);
[[nodiscard]] bool PlatformPackageMatchesTarget(std::string_view name, std::string_view target);

// Recursively drop declarations that do not apply to `target`.
void PruneDeclsForTarget(std::vector<DeclPtr> &decls, std::string_view target);
void PruneModuleForTarget(Module &module, std::string_view target);

[[nodiscard]] std::string DependencyPackageName(const Dependency &dep);

// ---- Workspace / registry locations -----------------------------------------

// Locate the nearest Rux.toml, printing an error if none is found.
// When manifestPath is non-empty, use that path directly instead of searching.
[[nodiscard]] std::optional<std::filesystem::path> RequireManifest();
[[nodiscard]] std::optional<std::filesystem::path> RequireManifest(const std::filesystem::path &manifestPath);

// Parse a manifest, printing an error on failure.
[[nodiscard]] std::optional<Manifest> LoadManifest(const std::filesystem::path &path);

// Resolve the build output directory for a given profile (defaults to "Bin").
[[nodiscard]] std::filesystem::path ResolveBuildOutputDir(const std::filesystem::path &root, const Manifest &manifest,
                                                          std::string_view profileName);

// Per-user directory where installed registry packages are cached.
[[nodiscard]] std::filesystem::path RegistryPackagesDir();

} // namespace Rux::Misc
