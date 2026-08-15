#pragma once

// Helpers for resolving the active build target (OS/arch triple), pruning the
// AST to that target, and locating workspace/registry directories.

#include "BuildInfo/BuildProfile.h"
#include "Linker/ArtifactKind.h"
#include "Package/Manifest.h"
#include "Syntax/Ast/Ast.h"
#include "System/Host.h"
#include "Target/Target.h"
#include "Target/TargetTriple.h"

#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace Rux::Driver {
// ---- Target triples ---------------------------------------------------------

// Human-readable name of the host target, e.g. "Windows x86-64".
[[nodiscard]] std::string TargetName();

// Human-readable name of a validated target, e.g. "Windows x86-64".
[[nodiscard]] std::string TargetDisplayName(Target::TargetTriple target);

// Lower-case "os-arch" triple of the host, e.g. "windows-x86_64".
[[nodiscard]] std::string HostTargetTriple();

// Convert accepted architecture aliases to the canonical machine spelling.
// For example, "windows-x64" and "windows-amd64" become "windows-x86_64".
[[nodiscard]] std::string CanonicalTargetTriple(std::string_view target);

// True if `target` is one of the officially supported "os-arch" triples.
// Compatibility aliases are accepted and normalized by CanonicalTargetTriple.
[[nodiscard]] bool IsSupportedTargetTriple(std::string_view target);

// Comma-separated list used by CLI diagnostics.
[[nodiscard]] const std::string &SupportedTargetTriples();

// Canonical OS name ("FreeBSD", "Linux", "macOS", "Windows") for a target.
[[nodiscard]] std::string_view TargetOsName(Target::TargetTriple target);

[[nodiscard]] TargetContext TargetContextForTriple(Target::TargetTriple target);

// True when this host can execute an artifact built for `target` directly.
[[nodiscard]] bool HostCanExecuteTarget(Target::TargetTriple target);

// Pure form of HostCanExecuteTarget. A target is directly executable when its
// OS matches and its architecture is either the compiler process architecture
// or the native OS architecture. Keeping the host values injectable makes
// translated-host behavior testable on every CTest host.
[[nodiscard]] bool CanExecuteTargetDirectly(Target::OS hostOs, System::HostArchitectureInfo hostArchitectures,
                                            Target::OS targetOs, Target::Arch targetArch) noexcept;

// ---- Platform packages ------------------------------------------------------

// Names that denote a platform package rather than a normal dependency.
[[nodiscard]] bool IsPlatformPackageName(std::string_view name);
[[nodiscard]] bool PlatformPackageMatchesTarget(std::string_view name, Target::TargetTriple target);

// The package name a dependency resolves to, which is its import name unless
// the entry overrides it with `Package`.
[[nodiscard]] const std::string &DependencyPackageName(const ManifestDependency &dep);

// Import names under which `manifest` binds the intrinsics package, so the
// conditional-compilation pass can tell an imported build intrinsic from an
// identically named one out of some other package. A registry entry is matched
// on the identity it declares; a path entry is matched by reading the manifest
// it points at, since the entry itself names no package. `root` is the
// directory holding `manifest`.
[[nodiscard]] std::set<std::string> IntrinsicsAliases(const Manifest &manifest, const std::filesystem::path &root);

// ---- Workspace / registry locations -----------------------------------------

// Locate the nearest Rux.toml, printing an error if none is found.
// When manifestPath is non-empty, use that path directly instead of searching.
[[nodiscard]] std::optional<std::filesystem::path> RequireManifest();
[[nodiscard]] std::optional<std::filesystem::path> RequireManifest(const std::filesystem::path &manifestPath);

// Parse a manifest, printing its source-located diagnostics on failure.
[[nodiscard]] std::optional<Manifest> LoadManifest(const std::filesystem::path &path);

// Print manifest diagnostics in the standard `path:line:column: error:` form.
void ReportManifestDiagnostics(const ManifestResult &result);

// Resolve the configured raw output root, defaulting to <package root>/Bin.
// Target-independent products such as documentation and source archives are
// placed relative to this path rather than in a machine-artifact directory.
[[nodiscard]] std::filesystem::path ResolveRawOutputRoot(const std::filesystem::path &root, const Manifest &manifest);

// Target components of an output directory: the OS name and the architecture
// display name, e.g. "FreeBSD/x86-64" or "macOS/AArch64".
[[nodiscard]] std::filesystem::path TargetOutputPath(Target::TargetTriple target);

// Directory for an ordinary machine artifact. Every build receives its typed
// profile plus both target components, including a host build.
[[nodiscard]] std::filesystem::path ResolveArtifactOutputDir(const std::filesystem::path &root,
                                                             const Manifest &manifest, BuildProfile profile,
                                                             Target::TargetTriple target);

// Test manifests intentionally name a central raw output root and omit the
// profile. A selected foreign target gets the same OS and architecture suffix
// so it cannot overwrite host test artifacts.
[[nodiscard]] std::filesystem::path ResolveTestOutputDir(const std::filesystem::path &root, const Manifest &manifest,
                                                         Target::TargetTriple target);

// ---- Output artifacts -------------------------------------------------------

// The artifact `type` produces. A SourceLibrary has none — it is compiled into
// its dependents — and callers must reject it before asking.
[[nodiscard]] ArtifactKind PackageArtifactKind(ManifestPackageType type);

// File name of that artifact on the target operating system: `App.exe` for a
// Windows executable, `libApp.dylib` for a macOS shared library. The name
// follows the target, never the host, so a cross build is spelled the way the
// machine it was built for expects.
[[nodiscard]] std::string OutputFileName(std::string_view packageName, ArtifactKind kind, Target::OS os);

// Per-user directory where installed registry packages are cached.
[[nodiscard]] std::filesystem::path RegistryPackagesDir();

// Cache directory of one package: <cache>/<namespace>/<name>. An existing
// directory is found by comparing normalized names, so `Rux/My_Pkg` and
// `rux/my-pkg` resolve to one directory the way they share one registry entry.
// When nothing is installed yet, the path carries the display spelling, which
// is the name an install creates.
//
// This reads the filesystem rather than only composing a path, because the
// spelling on disk is the publisher's and the spelling asked for is the
// consuming manifest's, and the two need not match.
[[nodiscard]] std::filesystem::path RegistryPackageParentDir(const IdentitySegment &ns, const IdentitySegment &name);

// Cache directory of one exact version: <cache>/<namespace>/<name>/<version>,
// resolved as RegistryPackageParentDir does, with the version keeping its exact
// text including build metadata.
[[nodiscard]] std::filesystem::path RegistryPackageDir(const IdentitySegment &ns, const IdentitySegment &name,
                                                       const SemanticVersion &version);

// An installed version of a package, paired with the directory holding it.
struct InstalledPackage {
    SemanticVersion version;
    std::filesystem::path root;
};

// Every installed version of one package, ascending. Directory names that are
// not semantic versions are ignored, so a cache entry left by an older layout
// is inert rather than a failure.
[[nodiscard]] std::vector<InstalledPackage> InstalledVersions(const IdentitySegment &ns, const IdentitySegment &name);

// The installed version a requirement resolves to: the highest one it matches,
// or nullopt when none is installed. Build and check use this instead of
// contacting the registry, so a build never needs the network.
[[nodiscard]] std::optional<InstalledPackage>
FindInstalledPackage(const IdentitySegment &ns, const IdentitySegment &name, const VersionRange &range);
} // namespace Rux::Driver
