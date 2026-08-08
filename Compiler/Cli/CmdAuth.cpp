// Authentication commands: login stores a registry token for later publishes,
// logout removes it.
//
// The token is read from stdin rather than an option, so it never reaches shell
// history or the process list. That is the same reasoning that keeps `publish`
// free of a --token flag.

#include "Cli/Cli.h"
#include "Driver/Credentials.h"
#include "System/Json.h"
#include "System/Os.h"
#include "System/Process.h"

#include <cstdio>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>

using namespace Rux;
using namespace Driver;
using namespace System;

namespace {
/// Obtain the token, prompting only when a person is there to read the prompt.
/// The prompt goes to stderr so `rux login` stays usable in a pipeline.
std::optional<std::string> ReadToken(const std::string_view base) {
    if (StdinIsInteractive()) {
        std::print(stderr, "Token for {}: ", base);
        std::fflush(stderr);
    }
    return ReadSecretLine();
}

/// Outcome of checking a token against the registry before it is stored.
enum class Verification {
    Accepted, ///< The registry confirmed the token.
    Rejected, ///< The registry refused it; it must not be stored.
    Unknown,  ///< No usable answer; store it, but say so.
};

/// Whether the identity document lists the `publish` scope.
///
/// The scopes are a JSON array, so the flat string lookup does not reach them;
/// the search is confined to the bracketed value to keep a scope name appearing
/// elsewhere in the document from counting.
bool GrantsPublish(const std::string_view body) {
    const auto key = body.find("\"scopes\"");
    if (key == std::string_view::npos) {
        return false;
    }
    const auto open = body.find('[', key);
    if (open == std::string_view::npos) {
        return false;
    }
    const auto close = body.find(']', open);
    if (close == std::string_view::npos) {
        return false;
    }
    return body.substr(open, close - open).find("\"publish\"") != std::string_view::npos;
}

/// Check `token` against the registry's identity endpoint.
///
/// A registry that does not implement the endpoint answers 404, and an offline
/// user gets no answer at all. Neither is a reason to refuse a login, so both
/// fall through to Unknown and the token is stored unverified.
Verification VerifyToken(const std::string_view base, const std::string_view token, std::string &identity) {
    auto response = HttpSend({.method = "GET",
                              .url = std::string(base) + "/v1/me",
                              .headers = {{.name = "Authorization", .value = "Bearer " + std::string(token)}},
                              .body = {}});
    if (!response) {
        std::print(stderr, "     Warning {} could not be reached; the token was not verified\n", base);
        return Verification::Unknown;
    }
    if (response->status == 200) {
        identity = JsonLookupString(response->body, "github_login");
        // A token without 'publish' is still worth storing -- it may be a yank
        // or namespace token -- but publishing with it would fail later.
        if (!GrantsPublish(response->body)) {
            std::print(stderr, "     Warning that token lacks the 'publish' scope\n");
        }
        return Verification::Accepted;
    }
    if (response->status == 401 || response->status == 403) {
        const std::string code = JsonLookupString(response->body, "code");
        const std::string detail = JsonLookupString(response->body, "detail");
        if (code == "insufficient_scope") {
            std::print(stderr, "error: that token lacks the 'publish' scope\n");
        }
        else if (!detail.empty()) {
            std::print(stderr, "error: {}\n", detail);
        }
        else {
            std::print(stderr, "error: {} rejected that token\n", base);
        }
        return Verification::Rejected;
    }
    std::print(stderr, "     Warning {} did not verify the token (status {})\n", base, response->status);
    return Verification::Unknown;
}
} // namespace

int Cli::RunLogin(std::span<const std::string_view> args, const GlobalOptions &opts) {
    std::string_view registryArg;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("login");
            return 0;
        }
        if (arg == "--registry") {
            if (i + 1 >= args.size()) {
                std::print(stderr, "error: '--registry' requires an argument\n");
                return 1;
            }
            registryArg = args[++i];
            continue;
        }
        PrintUnknownOption(arg, "login");
        return 1;
    }

    const std::string base = ResolveRegistryBase(registryArg);
    const auto token = ReadToken(base);
    if (!token) {
        std::print(stderr, "error: no token was supplied on stdin\n");
        return 1;
    }
    if (token->empty()) {
        std::print(stderr, "error: the token is empty\n");
        return 1;
    }
    if (token->find_first_of(" \t\r\n") != std::string::npos) {
        std::print(stderr, "error: the token contains whitespace\n");
        return 1;
    }

    std::string identity;
    if (VerifyToken(base, *token, identity) == Verification::Rejected) {
        return 1;
    }

    if (auto stored = StoreCredential(base, *token); !stored) {
        std::print(stderr, "error: {}\n", stored.error());
        return 1;
    }

    if (!opts.quiet) {
        if (identity.empty()) {
            std::print("   Logged in to {}\n", base);
        }
        else {
            std::print("   Logged in to {} as {}\n", base, identity);
        }
        std::print("      Stored {}\n", CredentialsPath().generic_string());
    }
    // A set RUX_TOKEN outranks what was just stored, so silently storing a token
    // that will not be used would be the confusing outcome.
    if (HasEnv(kCredentialVariable)) {
        std::print(stderr, "     Warning {} is set and takes precedence over the stored token\n", kCredentialVariable);
    }
    return 0;
}

int Cli::RunLogout(std::span<const std::string_view> args, const GlobalOptions &opts) {
    std::string_view registryArg;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("logout");
            return 0;
        }
        if (arg == "--registry") {
            if (i + 1 >= args.size()) {
                std::print(stderr, "error: '--registry' requires an argument\n");
                return 1;
            }
            registryArg = args[++i];
            continue;
        }
        PrintUnknownOption(arg, "logout");
        return 1;
    }

    const std::string base = ResolveRegistryBase(registryArg);
    const auto erased = EraseCredential(base);
    if (!erased) {
        std::print(stderr, "error: {}\n", erased.error());
        return 1;
    }

    if (!opts.quiet) {
        if (*erased) {
            std::print("  Logged out of {}\n", base);
        }
        else {
            std::print("  No stored token for {}\n", base);
        }
    }
    return 0;
}
