#include "Driver/Credentials.h"

#include "System/Json.h"
#include "System/Os.h"
#include "System/Process.h"
#include "Target/Target.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

namespace Rux::Driver {
using namespace Target;
using namespace System;

namespace {
/// Preamble written above every generated credentials file.
constexpr std::string_view fileHeader = "# Rux registry credentials, written by 'rux login'.\n"
                                        "# Anyone who can read this file can publish as you.\n";

/// One registry's stored token, kept in the order the file lists them.
struct Entry {
    std::string registry;
    std::string token;
};

/// Strip ASCII spaces, tabs and carriage returns from both ends of `text`.
std::string_view Trim(std::string_view text) {
    constexpr std::string_view blanks = " \t\r";
    const auto first = text.find_first_not_of(blanks);
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(blanks) - first + 1);
}

/// The contents of a `"..."` literal, or nullopt when `text` is not one.
std::optional<std::string_view> Unquote(const std::string_view text) {
    if (text.size() < 2 || !text.starts_with('"') || !text.ends_with('"')) {
        return std::nullopt;
    }
    return text.substr(1, text.size() - 2);
}

/// The registry named by a `[Registry."<url>"]` header, or nullopt for any
/// other line. An unrecognised section stops the entry it would have opened
/// from being attributed to the previous registry.
std::optional<std::string_view> ParseSectionRegistry(const std::string_view line) {
    if (!line.starts_with('[') || !line.ends_with(']')) {
        return std::nullopt;
    }
    constexpr std::string_view prefix = "Registry.";
    const std::string_view inner = Trim(line.substr(1, line.size() - 2));
    if (!inner.starts_with(prefix)) {
        return std::nullopt;
    }
    return Unquote(Trim(inner.substr(prefix.size())));
}

/// The value of a `Token = "<value>"` assignment, or nullopt for any other line.
std::optional<std::string_view> ParseTokenValue(const std::string_view line) {
    const auto equals = line.find('=');
    if (equals == std::string_view::npos || Trim(line.substr(0, equals)) != "Token") {
        return std::nullopt;
    }
    return Unquote(Trim(line.substr(equals + 1)));
}

/// Every entry the credentials file holds, in file order.
///
/// A missing, unreadable or malformed file reads as no entries rather than an
/// error. A corrupt file must not wedge `publish` into an unexplainable
/// failure, and `rux login` rewrites the file wholesale anyway.
std::vector<Entry> ReadEntries() {
    std::ifstream file(CredentialsPath(), std::ios::binary);
    if (!file) {
        return {};
    }

    std::vector<Entry> entries;
    std::string registry;
    bool inSection = false;
    std::string line;
    while (std::getline(file, line)) {
        const std::string_view trimmed = Trim(line);
        if (trimmed.empty() || trimmed.starts_with('#')) {
            continue;
        }
        if (trimmed.starts_with('[')) {
            const auto section = ParseSectionRegistry(trimmed);
            inSection = section.has_value();
            registry = inSection ? std::string(*section) : std::string{};
            continue;
        }
        if (!inSection) {
            continue;
        }
        if (const auto token = ParseTokenValue(trimmed); token && !registry.empty()) {
            entries.push_back({.registry = registry, .token = std::string(*token)});
        }
    }
    return entries;
}

/// Replace the credentials file with `entries`, or delete it when they are
/// empty so logging out of the last registry leaves nothing behind.
///
/// The write goes through a temporary that is restricted to the owner while it
/// is still empty, so a token is never briefly world-readable on disk, and is
/// renamed into place only once complete.
std::expected<void, std::string> WriteEntries(const std::vector<Entry> &entries) {
    const std::filesystem::path path = CredentialsPath();
    std::error_code ec;

    if (entries.empty()) {
        std::filesystem::remove(path, ec);
        if (ec) {
            return std::unexpected(std::format("failed to remove '{}': {}", path.generic_string(), ec.message()));
        }
        return {};
    }

    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return std::unexpected(
            std::format("failed to create '{}': {}", path.parent_path().generic_string(), ec.message()));
    }

    std::filesystem::path temp = path;
    temp += ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) {
            return std::unexpected(std::format("failed to write '{}'", temp.generic_string()));
        }
        file.flush(); // the file now exists and is still empty
        if (!SetPrivateFilePermissions(temp)) {
            file.close();
            std::filesystem::remove(temp, ec);
            return std::unexpected(std::format("failed to restrict '{}' to your account; the token was not stored",
                                               temp.generic_string()));
        }

        file << fileHeader;
        for (const auto &entry : entries) {
            file << std::format("\n[Registry.\"{}\"]\nToken = \"{}\"\n", entry.registry, entry.token);
        }
        file.close();
        if (!file) {
            std::filesystem::remove(temp, ec);
            return std::unexpected(std::format("failed to write '{}'", temp.generic_string()));
        }
    }

    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return std::unexpected(std::format("failed to update '{}': {}", path.generic_string(), ec.message()));
    }
    return {};
}
} // namespace

std::string NormalizeRegistryBase(const std::string_view base) {
    std::string normalized(Trim(base));
    while (normalized.ends_with('/')) {
        normalized.pop_back();
    }
    return normalized;
}

std::string ResolveRegistryBase(const std::string_view flagValue) {
    if (!Trim(flagValue).empty()) {
        return NormalizeRegistryBase(flagValue);
    }
    return NormalizeRegistryBase(GetEnv(kRegistryVariable).value_or(std::string(kRegistryApiBase)));
}

std::filesystem::path CredentialsPath() {
    // The leaf follows each platform's own casing, as RegistryPackagesDir does.
    return UserDataDir() / (HostOS == OS::Windows ? "Credentials.toml" : "credentials.toml");
}

std::optional<Credential> LoadCredential(const std::string_view registryBase) {
    const std::string key = NormalizeRegistryBase(registryBase);
    for (const auto &entry : ReadEntries()) {
        if (entry.registry == key && !entry.token.empty()) {
            return Credential{.token = entry.token, .source = CredentialsPath().generic_string()};
        }
    }
    return std::nullopt;
}

std::optional<Credential> ResolveCredential(const std::string_view registryBase) {
    // The environment wins: a CI job must not be shadowed by a file left on a
    // self-hosted runner.
    if (const auto fromEnv = GetEnv(kCredentialVariable); fromEnv && !fromEnv->empty()) {
        return Credential{.token = *fromEnv, .source = kCredentialVariable};
    }
    return LoadCredential(registryBase);
}

std::expected<void, std::string> StoreCredential(const std::string_view registryBase, const std::string_view token) {
    const std::string key = NormalizeRegistryBase(registryBase);
    auto entries = ReadEntries();
    const auto existing = std::ranges::find(entries, key, &Entry::registry);
    if (existing != entries.end()) {
        existing->token = std::string(token);
    }
    else {
        entries.push_back({.registry = key, .token = std::string(token)});
    }
    return WriteEntries(entries);
}

std::expected<bool, std::string> EraseCredential(const std::string_view registryBase) {
    const std::string key = NormalizeRegistryBase(registryBase);
    auto entries = ReadEntries();
    const auto removed = std::erase_if(entries, [&key](const Entry &entry) { return entry.registry == key; });
    if (removed == 0) {
        return false;
    }
    if (auto written = WriteEntries(entries); !written) {
        return std::unexpected(std::move(written.error()));
    }
    return true;
}

CredentialVerification DecodeCredentialVerification(const unsigned status, const std::string_view body) {
    if (status == 404 || status == 405 || status == 501) {
        return {.kind = CredentialVerificationKind::Unsupported, .status = status, .identity = {}, .detail = {}};
    }
    if (status == 401 || status == 403) {
        const std::string code = JsonLookupString(body, "code");
        return {.kind = code == "insufficient_scope" ? CredentialVerificationKind::MissingPublishScope
                                                     : CredentialVerificationKind::Rejected,
                .status = status,
                .identity = {},
                .detail = {}};
    }
    if (status != 200) {
        return {.kind = CredentialVerificationKind::Rejected, .status = status, .identity = {}, .detail = {}};
    }

    const auto document = ParseJson(body);
    if (!document || !document->IsObject()) {
        return {.kind = CredentialVerificationKind::Malformed,
                .status = status,
                .identity = {},
                .detail = document ? "the response is not a JSON object" : document.error().message};
    }
    const JsonValue *data = document->Find("data");
    if (data == nullptr || !data->IsObject()) {
        return {.kind = CredentialVerificationKind::Malformed,
                .status = status,
                .identity = {},
                .detail = "the response has no 'data' object"};
    }
    const std::string identity = data->StringAt("github_login");
    const JsonValue *scopes = data->Find("scopes");
    if (identity.empty() || scopes == nullptr || !scopes->IsArray()) {
        return {.kind = CredentialVerificationKind::Malformed,
                .status = status,
                .identity = {},
                .detail = "the response does not contain an identity and scopes array"};
    }
    const bool canPublish =
        std::ranges::any_of(scopes->Elements(), [](const JsonValue &scope) { return scope.AsString() == "publish"; });
    return {.kind = canPublish ? CredentialVerificationKind::Accepted : CredentialVerificationKind::MissingPublishScope,
            .status = status,
            .identity = identity,
            .detail = {}};
}

CredentialVerification VerifyCredential(const std::string_view registryBase, const std::string_view token) {
    auto response = HttpSend({.method = "GET",
                              .url = std::string(registryBase) + "/v1/me",
                              .headers = {{.name = "Authorization", .value = "Bearer " + std::string(token)}},
                              .body = {}});
    if (!response) {
        return {.kind = CredentialVerificationKind::Unreachable, .status = 0, .identity = {}, .detail = {}};
    }
    return DecodeCredentialVerification(response->status, response->body);
}
} // namespace Rux::Driver
