#pragma once

// Helpers for resolving the active build target (OS/arch triple), pruning the
// AST to that target, and locating workspace/registry directories.

#include "Linker/ArtifactKind.h"
#include "Package/Manifest.h"
#include "Syntax/Ast/Ast.h"
#include "Target/Target.h"

#include <filesystem>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Rux::Driver {
// ---- Target triples ---------------------------------------------------------

// Human-readable name of the host target, e.g. "Windows x86-64".
[[nodiscard]] std::string TargetName();

// Human-readable name of an "os-arch" triple, e.g. "Windows x86-64". Unknown
// components fall back to the host the way TargetTripleOs does.
[[nodiscard]] std::string TargetDisplayName(std::string_view target);

// Lower-case "os-arch" triple of the host, e.g. "windows-x86_64".
[[nodiscard]] std::string HostTargetTriple();

// Convert accepted architecture aliases to the canonical machine spelling.
// For example, "windows-x64" and "windows-amd64" become "windows-x86_64".
[[nodiscard]] std::string CanonicalTargetTriple(std::string_view target);

// True if `target` is one of the officially supported "os-arch" triples.
// Compatibility aliases are accepted and normalized by CanonicalTargetTriple.
[[nodiscard]] bool IsSupportedTargetTriple(std::string_view target);

// Comma-separated list used by CLI diagnostics.
[[nodiscard]] std::string_view SupportedTargetTriples();

// Canonical OS name ("Linux", "Windows", "macOS", "BSD", "Illumos") for the OS
// component of an "os-arch" triple, or "" if it cannot be determined.
[[nodiscard]] std::string_view TargetOsName(std::string_view target);

// Target::OS for the OS component of an "os-arch" triple; drives the linker's
// object-format choice. Falls back to the host OS for an unrecognized triple.
[[nodiscard]] Target::OS TargetTripleOs(std::string_view target);

// Architecture and fully-derived target description for an "os-arch" triple.
// Unknown components fall back to the native target, matching TargetTripleOs.
[[nodiscard]] Target::Arch TargetTripleArch(std::string_view target);
[[nodiscard]] TargetContext TargetContextForTriple(std::string_view target);

// Whether this process was asked to lower AArch64 with the native RCU back end
// (CodeGen/AArch64/RcuEmitter.cpp) rather than with the Clang emitter, through
// the RUX_AARCH64_RCU environment variable. That back end is being written a
// group of opcodes at a time (BACKLOG.md Phases 3-5) and refuses what it cannot
// lower, so it stays opt-in until it can build every program the Clang path
// can; task 34 removes the Clang path and this question with it.
[[nodiscard]] bool NativeAArch64BackendRequested();

// Why no back end can generate machine code for `target` on this host, or an
// empty string when one can. Selecting a target is otherwise free: `check`,
// `doc`, `install` and `update` only read the target, so they accept every
// supported triple. Callers must validate the triple first.
[[nodiscard]] std::string UnsupportedBackendReason(std::string_view target);

// True when this host can execute an artifact built for `target` directly,
// without an emulator standing between the two.
[[nodiscard]] bool HostCanExecuteTarget(std::string_view target);

// ---- Executing what was built -----------------------------------------------

// How this host launches an artifact built for some target: directly when the
// artifact was built for the host, and through a user-mode emulator when it was
// not.
struct ExecutionCommand {
    std::filesystem::path emulator;        ///< Empty when the artifact runs directly.
    std::vector<std::string> emulatorArgs; ///< Arguments the emulator takes before the artifact path.

    [[nodiscard]] bool IsEmulated() const noexcept {
        return !emulator.empty();
    }
};

// One resolved command line: the program to start, and every argument it takes.
struct LaunchCommand {
    std::filesystem::path program;
    std::vector<std::string> args;

    /// The command as a shell would spell it, for verbose output.
    [[nodiscard]] std::string CommandLine() const;
};

// The command line that runs `artifact` with `args` under `command`. The
// artifact is the program itself when nothing emulates it, and the emulator's
// first non-option argument when something does.
[[nodiscard]] LaunchCommand PrepareLaunch(const ExecutionCommand &command, const std::filesystem::path &artifact,
                                          std::span<const std::string_view> args = {});

// Either how to execute a `target` artifact on this host, or why it cannot be
// executed here. Exactly one of the two is set. Callers must validate the
// triple first.
struct ExecutionCommandResult {
    std::optional<ExecutionCommand> command;
    std::string error;
};

// Resolve the emulator, if any, that `run` and `test` launch a `target`
// artifact through. A foreign architecture is emulated by the command named by
// RUX_EMULATOR, or by this architecture's usual QEMU binary; RUX_QEMU_SYSROOT
// adds the `-L` that points a dynamically linked program at its loader and
// shared libraries. A foreign operating system has no answer: an emulator
// supplies an instruction set, not a kernel.
[[nodiscard]] ExecutionCommandResult ResolveExecutionCommand(std::string_view target);

// ---- Platform packages ------------------------------------------------------

// Names that denote a platform package rather than a normal dependency.
[[nodiscard]] bool IsPlatformPackageName(std::string_view name);
[[nodiscard]] bool PlatformPackageMatchesTarget(std::string_view name, std::string_view target);

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

// Resolve the build output directory (defaults to "Bin"), optionally appending
// the selected profile. Test runs use a profile-independent output directory.
//
// A target other than the host adds its triple as a final component, so builds
// for two targets do not overwrite each other. The host keeps the shorter path
// it has always had, and an empty `targetTriple` — used by target-independent
// output such as a published `.ruxpkg` — adds nothing.
[[nodiscard]] std::filesystem::path ResolveBuildOutputDir(const std::filesystem::path &root, const Manifest &manifest,
                                                          std::string_view profileName, std::string_view targetTriple,
                                                          bool includeProfile = true);

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
