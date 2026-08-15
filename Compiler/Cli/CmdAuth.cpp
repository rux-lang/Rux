// Authentication commands: login stores a registry token for later publishes,
// logout removes it.
//
// The token is read from stdin rather than an option, so it never reaches shell
// history or the process list. That is the same reasoning that keeps `publish`
// free of a --token flag.

#include "Cli/Cli.h"
#include "Cli/Reporter.h"
#include "Driver/BuildReport.h"
#include "Driver/Credentials.h"
#include "Reporting/Reporting.h"
#include "System/Os.h"

#include <chrono>
#include <cstdio>
#include <format>
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

bool ReportVerification(const CredentialVerification &verification, const std::string_view base,
                        const CliSupport::Reporter &diagnostics) {
    switch (verification.kind) {
    case CredentialVerificationKind::Accepted:
        return true;
    case CredentialVerificationKind::MissingPublishScope:
        if (verification.status == 200) {
            diagnostics.Warning("the registry accepted the token, but it cannot publish packages");
            diagnostics.Help("create a token with the 'publish' scope");
            return true;
        }
        diagnostics.Error(
            std::format("the registry at '{}' rejected the token because it cannot publish packages", base));
        diagnostics.Help("create a token with the 'publish' scope, then run 'rux login' again");
        return false;
    case CredentialVerificationKind::Rejected:
        diagnostics.Error(std::format("the registry at '{}' rejected the token", base));
        if (verification.status != 0) {
            diagnostics.Note(std::format("registry response status: {}", verification.status));
        }
        diagnostics.Help("check that the token is current, then run 'rux login' again");
        return false;
    case CredentialVerificationKind::Unreachable:
        diagnostics.Warning(std::format("could not reach the registry at '{}'; the token was stored unverified", base));
        if (!verification.detail.empty()) {
            diagnostics.Note(verification.detail);
        }
        diagnostics.Help("check the registry URL and network, then run 'rux login' again to verify it");
        return true;
    case CredentialVerificationKind::Unsupported:
        diagnostics.Warning(std::format("the registry at '{}' does not support token verification", base));
        diagnostics.Note("the token was stored unverified");
        return true;
    case CredentialVerificationKind::Malformed:
        diagnostics.Warning(std::format("the registry at '{}' returned a malformed verification response", base));
        if (!verification.detail.empty()) {
            diagnostics.Note(verification.detail);
        }
        diagnostics.Note("the token was stored unverified");
        diagnostics.Help("retry 'rux login'; contact the registry operator if the response remains malformed");
        return true;
    }
    return false;
}
} // namespace

int Cli::RunLogin(std::span<const std::string_view> args, const GlobalOptions &opts) {
    const CliSupport::Reporter output(stdout, {.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose});
    const CliSupport::Reporter diagnostics(stderr, {.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose});
    std::string_view registryArg;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("login");
            return 0;
        }
        if (arg == "--registry") {
            if (i + 1 >= args.size()) {
                diagnostics.Error("option '--registry' requires a registry URL");
                diagnostics.Help("try 'rux login --registry https://registry.example'");
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
        diagnostics.Error("no token was supplied on stdin");
        diagnostics.Help("pipe one token line to 'rux login' or run it in an interactive terminal");
        return 1;
    }
    if (token->empty()) {
        diagnostics.Error("the supplied token is empty");
        diagnostics.Help("supply a non-empty registry token");
        return 1;
    }
    if (token->find_first_of(" \t\r\n") != std::string::npos) {
        diagnostics.Error("the supplied token contains whitespace");
        diagnostics.Help("copy only the token value, without spaces or extra lines");
        return 1;
    }

    const auto started = std::chrono::steady_clock::now();
    const CredentialVerification verification = VerifyCredential(base, *token);
    if ((verification.kind == CredentialVerificationKind::Rejected ||
         (verification.kind == CredentialVerificationKind::MissingPublishScope && verification.status != 200)) &&
        !ReportVerification(verification, base, diagnostics)) {
        return 1;
    }

    if (auto stored = StoreCredential(base, *token); !stored) {
        diagnostics.Error("could not store the registry token");
        diagnostics.Note(stored.error());
        diagnostics.Help("check the credentials directory permissions, then run 'rux login' again");
        return 1;
    }
    if (verification.kind != CredentialVerificationKind::Accepted) {
        static_cast<void>(ReportVerification(verification, base, diagnostics));
    }

    output.Success("Logged in", verification.identity.empty()
                                    ? std::format("to '{}' in {}", base, Reporting::FormatDuration(ElapsedMs(started)))
                                    : std::format("to '{}' as '{}' in {}", base, verification.identity,
                                                  Reporting::FormatDuration(ElapsedMs(started))));
    output.Detail(std::format("Credentials: '{}'", CredentialsPath().string()));
    // A set RUX_TOKEN outranks what was just stored, so silently storing a token
    // that will not be used would be the confusing outcome.
    if (HasEnv(kCredentialVariable)) {
        diagnostics.Warning(std::format("{} is set and takes precedence over the stored token", kCredentialVariable));
        diagnostics.Help(std::format("unset {} to use the token stored by 'rux login'", kCredentialVariable));
    }
    return 0;
}

int Cli::RunLogout(std::span<const std::string_view> args, const GlobalOptions &opts) {
    const CliSupport::Reporter output(stdout, {.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose});
    const CliSupport::Reporter diagnostics(stderr, {.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose});
    std::string_view registryArg;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("logout");
            return 0;
        }
        if (arg == "--registry") {
            if (i + 1 >= args.size()) {
                diagnostics.Error("option '--registry' requires a registry URL");
                diagnostics.Help("try 'rux logout --registry https://registry.example'");
                return 1;
            }
            registryArg = args[++i];
            continue;
        }
        PrintUnknownOption(arg, "logout");
        return 1;
    }

    const std::string base = ResolveRegistryBase(registryArg);
    const auto started = std::chrono::steady_clock::now();
    const auto erased = EraseCredential(base);
    if (!erased) {
        diagnostics.Error(std::format("could not remove the stored token for '{}'", base));
        diagnostics.Note(erased.error());
        diagnostics.Help("check the credentials directory permissions, then run 'rux logout' again");
        return 1;
    }

    if (*erased) {
        output.Success("Logged out", std::format("of '{}' in {}", base, Reporting::FormatDuration(ElapsedMs(started))));
        output.Detail(std::format("Credentials: '{}'", CredentialsPath().string()));
    }
    else {
        output.Success("Unchanged", std::format("no stored token for '{}'", base));
    }
    if (HasEnv(kCredentialVariable)) {
        diagnostics.Warning(
            std::format("{} remains set and will still authenticate registry requests", kCredentialVariable));
        diagnostics.Help(std::format("unset {} to finish logging out", kCredentialVariable));
    }
    return 0;
}
