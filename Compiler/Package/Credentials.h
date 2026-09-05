#pragma once

// Registry bearer credentials: where they come from, and how `rux login` stores
// them between runs.
//
// A credential can arrive two ways. The RUX_TOKEN environment variable wins, so
// CI stays deterministic and a stale file on a self-hosted runner cannot shadow
// the token the job supplied. Otherwise the per-user credentials file written by
// `rux login` is consulted.
//
// Entries are keyed by registry base URL rather than stored as one flat token.
// `publish` already retargets with --registry and RUX_REGISTRY_URL, so a single
// ambient token would be sent to whichever registry happened to be selected --
// including a local test one.

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Rux::Packages {
/// Base URL of the versioned registry API, whose routes live below /v1. The package commands override it with
/// --registry or RUX_REGISTRY_URL to reach a local registry.
inline constexpr std::string_view kRegistryApiBase = "https://api.rux-lang.dev";

/// Environment variable holding the registry bearer credential.
inline constexpr const char *kCredentialVariable = "RUX_TOKEN";

/// Environment variable overriding the registry API base URL.
inline constexpr const char *kRegistryVariable = "RUX_REGISTRY_URL";

/// The registry a command talks to: `flagValue` from --registry when non-empty, otherwise RUX_REGISTRY_URL, otherwise
/// the official API. The result is normalized, so it doubles as the credentials-file key.
[[nodiscard]] std::string ResolveRegistryBase(std::string_view flagValue);

/// A bearer credential together with the origin an error message should name.
struct Credential {
    std::string token;
    std::string source; ///< "RUX_TOKEN", or the credentials file path.
};

enum class CredentialVerificationKind {
    Accepted,
    MissingPublishScope,
    Rejected,
    Unreachable,
    Unsupported,
    Malformed,
};

struct CredentialVerification {
    CredentialVerificationKind kind = CredentialVerificationKind::Unreachable;
    unsigned status = 0;
    std::string identity;
    std::string detail;
};

[[nodiscard]] CredentialVerification DecodeCredentialVerification(unsigned status, std::string_view body);

[[nodiscard]] CredentialVerification VerifyCredential(std::string_view registryBase, std::string_view token);

/// Registry base URL in the form used as a credentials-file key: trailing slashes trimmed, so `https://host/` and
/// `https://host` are one registry.
[[nodiscard]] std::string NormalizeRegistryBase(std::string_view base);

/// Path of the per-user credentials file, beside the package cache.
[[nodiscard]] std::filesystem::path CredentialsPath();

/// The stored credential for `registryBase`, ignoring the environment. Returns
[[nodiscard]] std::expected<std::optional<Credential>, std::string> LoadCredential(std::string_view registryBase);

/// The credential to authenticate to `registryBase` with: RUX_TOKEN when set, otherwise the stored entry.
[[nodiscard]] std::expected<std::optional<Credential>, std::string> ResolveCredential(std::string_view registryBase);

/// Store `token` for `registryBase`, replacing any entry it already has and preserving every other registry's. The file
/// is written through a temporary that is restricted to the owner before the token reaches it.
[[nodiscard]] std::expected<void, std::string> StoreCredential(std::string_view registryBase, std::string_view token);

/// Remove the entry for `registryBase`. The result reports whether one was there; erasing a registry that was never
/// stored is not an error.
[[nodiscard]] std::expected<bool, std::string> EraseCredential(std::string_view registryBase);
} // namespace Rux::Packages
