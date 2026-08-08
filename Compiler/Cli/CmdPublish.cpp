// Publication commands: pack builds the .ruxpkg archive, publish uploads it.

#include "Cli/Cli.h"
#include "Driver/BuildTarget.h"
#include "Driver/Credentials.h"
#include "Package/Artifact.h"
#include "Package/Manifest.h"
#include "System/Os.h"
#include "System/Process.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace Rux;
using namespace Driver;
using namespace System;

namespace {
/**
 * @brief Load the manifest of the package a publication command targets.
 *
 * A workspace groups packages but is not one itself, so it is rejected here
 * rather than after an archive has been built.
 */
std::optional<std::pair<std::filesystem::path, Manifest>> LoadPackageManifest(const GlobalOptions &opts) {
    auto manifestPath = RequireManifest(opts.manifest);
    if (!manifestPath) {
        return std::nullopt;
    }
    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) {
        return std::nullopt;
    }
    if (manifest->IsWorkspace()) {
        std::print(stderr, "error: '{}' is a workspace manifest; run this from a member package\n",
                   manifestPath->generic_string());
        return std::nullopt;
    }
    return std::pair{std::move(*manifestPath), std::move(*manifest)};
}

/// Print every publication-profile rejection against the manifest that carries it.
void ReportPublicationRejections(const std::filesystem::path &manifestPath,
                                 const std::vector<std::string> &rejections) {
    for (const auto &rejection : rejections) {
        std::print(stderr, "error: {}: {}\n", manifestPath.generic_string(), rejection);
    }
}

/// Render a byte count the way the build reports sizes.
std::string FormatBytes(const std::size_t bytes) {
    if (bytes < 1024) {
        return std::format("{} B", bytes);
    }
    if (bytes < 1024 * 1024) {
        return std::format("{:.1f} KiB", static_cast<double>(bytes) / 1024.0);
    }
    return std::format("{:.1f} MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
}

/// The display identity of a package, `Namespace/Name`.
std::string QualifiedName(const Manifest &manifest) {
    const std::string &name = manifest.package.name.Text();
    return manifest.package.ns ? std::format("{}/{}", manifest.package.ns->Text(), name) : name;
}

/**
 * @brief Explain a failed publication from its problem document.
 *
 * The registry answers with RFC 9457 problem details, whose `code` is stable
 * and machine-readable. Codes a user can act on get a specific message; the
 * rest fall back to the server's own `detail`.
 *
 * Credential problems name `credentialSource` rather than a fixed variable,
 * because the token may equally have come from RUX_TOKEN or from the file
 * `rux login` wrote.
 */
void ReportPublicationProblem(const HttpResponse &response, const Manifest &manifest, const std::string_view base,
                              const std::string_view credentialSource) {
    const std::string code = JsonLookupString(response.body, "code");
    const std::string detail = JsonLookupString(response.body, "detail");
    const std::string identity = QualifiedName(manifest);

    if (code == "version_conflict") {
        std::print(stderr,
                   "error: version {} of {} is already published; published versions are immutable, so publish a new "
                   "version\n",
                   manifest.package.version.Text(), identity);
    }
    else if (code == "namespace_not_found") {
        std::print(stderr, "error: namespace '{}' is not claimed on {}\n",
                   manifest.package.ns ? manifest.package.ns->Text() : std::string{}, base);
    }
    else if (code == "publication_forbidden") {
        std::print(stderr, "error: the credential in {} does not own or maintain namespace '{}'\n", credentialSource,
                   manifest.package.ns ? manifest.package.ns->Text() : std::string{});
    }
    else if (code == "insufficient_scope") {
        std::print(stderr, "error: the credential in {} lacks the 'publish' scope\n", credentialSource);
    }
    else if (code == "authentication_required") {
        std::print(stderr, "error: {} rejected the credential in {}\n", base, credentialSource);
    }
    else if (code == "rate_limited") {
        std::print(stderr, "error: {} is rate-limiting publication; retry shortly\n", base);
    }
    else if (!detail.empty()) {
        std::print(stderr, "error: {}\n", detail);
    }
    else {
        std::print(stderr, "error: the registry rejected the publication with status {}\n", response.status);
    }

    for (const auto &entry : JsonFindProblemErrors(response.body)) {
        if (entry.detail.empty()) {
            std::print(stderr, "  {}\n", entry.code);
        }
        else {
            std::print(stderr, "  {}: {}\n", entry.code, entry.detail);
        }
    }
}
} // namespace

int Cli::RunPack(std::span<const std::string_view> args, const GlobalOptions &opts) {
    std::string_view outputArg;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("pack");
            return 0;
        }
        if (arg == "--output" || arg == "-o") {
            if (i + 1 >= args.size()) {
                std::print(stderr, "error: '{}' requires an argument\n", arg);
                return 1;
            }
            outputArg = args[++i];
            continue;
        }
        PrintUnknownOption(arg, "pack");
        return 1;
    }

    auto target = LoadPackageManifest(opts);
    if (!target) {
        return 1;
    }
    const auto &[manifestPath, manifest] = *target;

    if (const auto rejections = ValidateForPublication(manifest); !rejections.empty()) {
        ReportPublicationRejections(manifestPath, rejections);
        return 1;
    }

    auto artifact = BuildPackageArtifact(manifestPath, manifest);
    if (!artifact) {
        std::print(stderr, "error: {}\n", artifact.error());
        return 1;
    }

    const std::filesystem::path root = manifestPath.parent_path();
    std::filesystem::path output = outputArg.empty()
                                     ? ResolveBuildOutputDir(root, manifest, {}, false) / ArtifactFileName(manifest)
                                     : std::filesystem::path(outputArg);
    if (output.is_relative()) {
        output = std::filesystem::current_path() / output;
    }

    std::error_code ec;
    std::filesystem::create_directories(output.parent_path(), ec);
    std::ofstream file(output, std::ios::binary);
    if (!file.write(artifact->archive.data(), static_cast<std::streamsize>(artifact->archive.size()))) {
        std::print(stderr, "error: failed to write '{}'\n", output.generic_string());
        return 1;
    }

    if (!opts.quiet) {
        std::print("      Packed {} {} ({} files, {})\n", QualifiedName(manifest), manifest.package.version.Text(),
                   artifact->fileCount, FormatBytes(artifact->archive.size()));
        std::print("     Written {}\n", output.generic_string());
    }
    return 0;
}

int Cli::RunPublish(std::span<const std::string_view> args, const GlobalOptions &opts) {
    std::string_view registryArg;
    bool dryRun = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("publish");
            return 0;
        }
        if (arg == "--dry-run") {
            dryRun = true;
            continue;
        }
        if (arg == "--registry") {
            if (i + 1 >= args.size()) {
                std::print(stderr, "error: '--registry' requires an argument\n");
                return 1;
            }
            registryArg = args[++i];
            continue;
        }
        PrintUnknownOption(arg, "publish");
        return 1;
    }

    auto target = LoadPackageManifest(opts);
    if (!target) {
        return 1;
    }
    const auto &[manifestPath, manifest] = *target;

    if (const auto rejections = ValidateForPublication(manifest); !rejections.empty()) {
        ReportPublicationRejections(manifestPath, rejections);
        return 1;
    }

    // The credential is read before the archive is built so an unauthenticated
    // run fails immediately rather than after the work.
    const std::string base = ResolveRegistryBase(registryArg);
    Credential credential;
    if (!dryRun) {
        auto resolved = ResolveCredential(base);
        if (!resolved) {
            std::print(stderr,
                       "error: no credential for {}; run 'rux login' or set {} to a token with the 'publish' "
                       "scope\n",
                       base, kCredentialVariable);
            return 1;
        }
        credential = std::move(*resolved);
        if (credential.token.find_first_of(" \t\r\n") != std::string::npos) {
            std::print(stderr, "error: the credential in {} contains whitespace\n", credential.source);
            return 1;
        }
    }

    auto artifact = BuildPackageArtifact(manifestPath, manifest);
    if (!artifact) {
        std::print(stderr, "error: {}\n", artifact.error());
        return 1;
    }

    if (!opts.quiet) {
        std::print("      Packed {} {} ({} files, {})\n", QualifiedName(manifest), manifest.package.version.Text(),
                   artifact->fileCount, FormatBytes(artifact->archive.size()));
    }
    if (dryRun) {
        if (!opts.quiet) {
            std::print("     Dry run: the package is publishable and was not uploaded\n");
        }
        return 0;
    }

    const std::array<MultipartPart, 2> parts{
        MultipartPart{.name = "manifest", .content = artifact->manifestSource},
        MultipartPart{.name = "package", .content = artifact->archive},
    };
    auto body = BuildMultipartBody(parts);
    if (!body) {
        std::print(stderr, "error: failed to encode the publication request\n");
        return 1;
    }

    if (!opts.quiet) {
        std::print("   Uploading to {}\n", base);
    }
    auto response = HttpSend({.method = "POST",
                              .url = base + "/v1/packages",
                              .headers = {{.name = "Authorization", .value = "Bearer " + credential.token},
                                          {.name = "Content-Type", .value = body->contentType}},
                              .body = std::move(body->body)});
    if (!response) {
        std::print(stderr, "error: failed to reach the registry at {}\n", base);
        return 1;
    }
    if (response->status != 201) {
        ReportPublicationProblem(*response, manifest, base, credential.source);
        return 1;
    }

    if (!opts.quiet) {
        std::print("   Published {} {}\n", QualifiedName(manifest), manifest.package.version.Text());
    }
    return 0;
}
