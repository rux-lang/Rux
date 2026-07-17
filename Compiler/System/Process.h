#pragma once

// Subprocess, network, and registry helpers used by the package commands.

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Rux::System {
// Outcome of a captured subprocess run.
struct RunResult {
    int exitCode = 0;
    std::string output; // combined stdout + stderr
};

// Run `exe` with `args`, inheriting this process's stdin/stdout/stderr (used
// by `rux run`). Returns the child's exit code, or nullopt when the process
// could not be launched.
[[nodiscard]] std::optional<int> RunInherited(const std::filesystem::path &exe,
                                              std::span<const std::string_view> args = {});

// Run `exe` with no arguments, stdin redirected from the null device, and its
// combined stdout+stderr captured (used by `rux test`). Returns nullopt when
// the process could not be launched.
[[nodiscard]] std::optional<RunResult> RunCaptured(const std::filesystem::path &exe);

// Location of the package registry index, served by the Rux Web API. Returns a
// JSON array of package objects (see JsonFindPackageRepository).
inline constexpr std::string_view kRegistryUrl = "https://api.rux-lang.dev/packages";

// Look up a string value in a flat JSON object: { "Key": "value", ... }.
// Returns an empty string if the key is missing.
[[nodiscard]] std::string JsonLookupString(std::string_view json, std::string_view key);

// Find a package by name in the registry index and return one of its string
// fields (e.g. "repository" or "folder").
//
// The index is a JSON array of flat package objects, e.g.
//   [ { "name": "Rux", "repository": "https://github.com/...", "folder": "Rux", ... }, ... ]
// Returns an empty string if no package with that name (or no such field) exists.
[[nodiscard]] std::string JsonFindPackageField(std::string_view json, std::string_view name, std::string_view field);

// Convenience wrapper for the "repository" field. See JsonFindPackageField.
[[nodiscard]] std::string JsonFindPackageRepository(std::string_view json, std::string_view name);

// Return every path whose object in a GitHub tree response has type "blob".
[[nodiscard]] std::vector<std::string> JsonFindGitBlobPaths(std::string_view json);

// Fetch the body of an HTTPS URL. Returns nullopt on failure.
[[nodiscard]] std::optional<std::string> FetchUrl(const std::string &url);

// Download Rux.toml and Src/ from a package repository into dest. A non-empty
// folder selects a package inside a monorepo. Existing contents are replaced
// atomically after the complete package has been downloaded.
[[nodiscard]] bool DownloadPackage(const std::string &repoUrl, const std::string &folder,
                                   const std::filesystem::path &dest, bool devBranch);
} // namespace Rux::System
