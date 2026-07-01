#pragma once

// Subprocess, network, and registry helpers used by the package commands.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Rux::Misc {

// Location of the package registry index, served by the Rux Web API. Returns a
// JSON array of package objects (see JsonFindPackageRepository).
inline constexpr std::string_view kRegistryUrl = "https://api.rux-lang.dev/packages";

// Look up a string value in a flat JSON object: { "Key": "value", ... }.
// Returns an empty string if the key is missing.
[[nodiscard]] std::string JsonLookupString(std::string_view json, std::string_view key);

// Find a package by name in the registry index and return its repository URL.
//
// The index is a JSON array of flat package objects, e.g.
//   [ { "name": "Std", "repository": "https://github.com/...", ... }, ... ]
// Returns an empty string if no package with that name exists.
[[nodiscard]] std::string JsonFindPackageRepository(std::string_view json, std::string_view name);

// Fetch the body of an HTTPS URL. Returns nullopt on failure.
//
// NOTE: this shells out to `curl` (and falls back to `wget` on Unix) rather than
// linking an HTTP client, so those tools must be on PATH.
[[nodiscard]] std::optional<std::string> FetchUrl(const std::string &url);

// Clone a git repository into `dest`. Pass devBranch=true to clone the `dev`
// branch. Returns true on success.
[[nodiscard]] bool GitClone(const std::string &repoUrl, const std::filesystem::path &dest, bool devBranch);

// Pull the latest changes in an existing git repository. Returns true on success.
[[nodiscard]] bool GitPull(const std::filesystem::path &repoDir);

} // namespace Rux::Misc
