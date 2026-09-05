// Publication commands: pack builds the .ruxpkg archive, publish uploads it.

#include "Cli/BuildReport.h"
#include "Cli/Cli.h"
#include "Cli/ManifestInput.h"
#include "Cli/PublicationProblem.h"
#include "Cli/Reporter.h"
#include "Driver/BuildTarget.h"
#include "Package/Artifact.h"
#include "Package/Credentials.h"
#include "Package/Manifest.h"
#include "Package/PublicationValidation.h"
#include "Reporting/Reporting.h"
#include "System/Http.h"
#include "System/Os.h"
#include "System/Process.h"

#include <array>
#include <chrono>
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

using namespace Rux::Packages;

using namespace Rux;
using namespace Driver;
using namespace System;
using namespace CliSupport;

namespace {
/**
 * @brief Load the manifest of the package a publication command targets.
 *
 * A workspace groups packages but is not one itself, so it is rejected here
 * rather than after an archive has been built.
 */
std::optional<std::pair<std::filesystem::path, Manifest>> LoadPackageManifest(const GlobalOptions &opts,
                                                                              const std::string_view command,
                                                                              const CliSupport::Reporter &diagnostics) {
    auto manifestPath = RequireManifest(opts.manifest);
    if (!manifestPath) {
        return std::nullopt;
    }
    auto manifest = LoadManifest(*manifestPath);
    if (!manifest) {
        return std::nullopt;
    }
    if (manifest->IsWorkspace()) {
        diagnostics.Error(std::format("workspace manifest '{}' cannot be {}", manifestPath->string(),
                                      command == "pack" ? "packed" : "published"));
        diagnostics.Note("publication requires the manifest of one workspace member package");
        if (!manifest->workspace.packages.empty()) {
            const auto memberManifest =
                (*manifestPath).parent_path() / manifest->workspace.packages.front() / "Rux.toml";
            diagnostics.Help(std::format("run 'rux --manifest \"{}\" {}'", memberManifest.string(), command));
        }
        else {
            diagnostics.Help("add a package to [Workspace].Packages, then select its Rux.toml with '--manifest'");
        }
        return std::nullopt;
    }
    return std::pair{std::move(*manifestPath), std::move(*manifest)};
}

/// The display identity of a package, `Namespace/Name`.
std::string QualifiedName(const Manifest &manifest) {
    const std::string &name = manifest.package.name.Text();
    return manifest.package.ns ? std::format("{}/{}", manifest.package.ns->Text(), name) : name;
}

/// Group every publication-profile rejection under the package that carries it.
void ReportPublicationRejections(const std::filesystem::path &manifestPath, const Manifest &manifest,
                                 const std::vector<PackageProblem> &rejections, const std::string_view command,
                                 const CliSupport::Reporter &diagnostics) {
    diagnostics.Error(std::format("{} {} does not meet publication requirements", QualifiedName(manifest),
                                  manifest.package.version.Text()));
    for (const auto &rejection : rejections) {
        diagnostics.Note(rejection.message);
        for (const auto &note : rejection.notes) {
            diagnostics.Note(note);
        }
    }
    diagnostics.Help(std::format("update '{}', then run 'rux {}' again", manifestPath.string(), command));
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

void ReportPublicationProblem(const HttpResponse &response, const Manifest &manifest, const std::string_view base,
                              const std::string_view credentialSource, const CliSupport::Reporter &diagnostics) {
    const auto problem = DescribePublicationProblem(
        response.status, response.body, QualifiedName(manifest), manifest.package.version.Text(),
        manifest.package.ns ? manifest.package.ns->Text() : std::string_view{}, base, credentialSource);
    diagnostics.Error(problem.error);
    for (const auto &note : problem.notes) {
        diagnostics.Note(note);
    }
    diagnostics.Help(problem.help);
}
} // namespace

int Cli::RunPack(std::span<const std::string_view> args, const GlobalOptions &opts) {
    const Reporter result(stdout, {.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose});
    const Reporter diagnostics(stderr, {.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose});
    std::string_view outputArg;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "-h" || arg == "--help") {
            PrintHelpFor("pack");
            return 0;
        }
        if (arg == "--output" || arg == "-o") {
            if (i + 1 >= args.size()) {
                diagnostics.Error(std::format("option '{}' requires an archive path", arg));
                diagnostics.Help("try 'rux pack --output Bin/Package.ruxpkg'");
                return 1;
            }
            outputArg = args[++i];
            continue;
        }
        PrintUnknownOption(arg, "pack");
        return 1;
    }

    auto target = LoadPackageManifest(opts, "pack", diagnostics);
    if (!target) {
        return 1;
    }
    const auto &[manifestPath, manifest] = *target;

    if (const auto rejections = ValidateForPublication(manifest); !rejections.empty()) {
        ReportPublicationRejections(manifestPath, manifest, rejections, "pack", diagnostics);
        return 1;
    }

    const auto started = std::chrono::steady_clock::now();
    result.Progress("Packing", std::format("{} {}", QualifiedName(manifest), manifest.package.version.Text()));
    auto artifact = BuildPackageArtifact(manifestPath, manifest);
    if (!artifact) {
        diagnostics.Error(artifact.error().message);
        diagnostics.Note(std::format("package: {} {}", QualifiedName(manifest), manifest.package.version.Text()));
        for (const auto &note : artifact.error().notes) {
            diagnostics.Note(note);
        }
        diagnostics.Help("correct the package files, then run 'rux pack' again");
        return 1;
    }

    const std::filesystem::path root = manifestPath.parent_path();
    std::filesystem::path output = outputArg.empty()
                                     // A `.ruxpkg` carries sources, not machine code, so it belongs at the
                                     // output root rather than under any one profile or target.
                                     ? ResolveRawOutputRoot(root, manifest) / ArtifactFileName(manifest)
                                     : std::filesystem::path(outputArg);
    if (output.is_relative()) {
        output = std::filesystem::current_path() / output;
    }

    std::error_code ec;
    std::filesystem::create_directories(output.parent_path(), ec);
    if (ec) {
        diagnostics.Error(std::format("could not create archive directory '{}'", output.parent_path().string()));
        diagnostics.Note(ec.message());
        diagnostics.Help("choose a writable archive path with '--output'");
        return 1;
    }
    std::ofstream file(output, std::ios::binary);
    if (!file || !file.write(artifact->archive.data(), static_cast<std::streamsize>(artifact->archive.size()))) {
        diagnostics.Error(std::format("could not write package archive '{}'", output.string()));
        diagnostics.Help("choose a writable archive path with '--output'");
        return 1;
    }

    result.Success("Packed", std::format("{} {} in {}", QualifiedName(manifest), manifest.package.version.Text(),
                                         Reporting::FormatDuration(ElapsedMs(started))));
    result.Detail(std::format("Files: {} ({})", artifact->fileCount, FormatBytes(artifact->archive.size())));
    result.Detail(std::format("Output: '{}'", output.string()));
    return 0;
}

int Cli::RunPublish(std::span<const std::string_view> args, const GlobalOptions &opts) {
    const Reporter output(stdout, {.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose});
    const Reporter diagnostics(stderr, {.color = opts.color, .quiet = opts.quiet, .verbose = opts.verbose});
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
                diagnostics.Error("option '--registry' requires a registry URL");
                diagnostics.Help("try 'rux publish --registry https://registry.example'");
                return 1;
            }
            registryArg = args[++i];
            continue;
        }
        PrintUnknownOption(arg, "publish");
        return 1;
    }

    auto target = LoadPackageManifest(opts, "publish", diagnostics);
    if (!target) {
        return 1;
    }
    const auto &[manifestPath, manifest] = *target;

    if (const auto rejections = ValidateForPublication(manifest); !rejections.empty()) {
        ReportPublicationRejections(manifestPath, manifest, rejections, "publish", diagnostics);
        return 1;
    }

    // The credential is read before the archive is built so an unauthenticated
    // run fails immediately rather than after the work.
    const std::string base = ResolveRegistryBase(registryArg);
    Credential credential;
    if (!dryRun) {
        auto resolved = ResolveCredential(base);
        if (!resolved) {
            diagnostics.Error(std::format("could not read publication credentials for '{}'", base));
            diagnostics.Note(resolved.error());
            diagnostics.Help("check the credentials file permissions, then retry 'rux publish'");
            return 1;
        }
        if (!*resolved) {
            diagnostics.Error(std::format("no publication credential was found for '{}'", base));
            diagnostics.Note(std::format("'rux publish' requires a token with the 'publish' scope from {} or the "
                                         "credentials file",
                                         kCredentialVariable));
            diagnostics.Help(std::format("run 'rux login --registry {}'", base));
            return 1;
        }
        credential = std::move(**resolved);
        if (credential.token.find_first_of(" \t\r\n") != std::string::npos) {
            diagnostics.Error(std::format("the publication credential from '{}' is invalid", credential.source));
            diagnostics.Note("registry tokens cannot contain whitespace");
            diagnostics.Help(std::format("run 'rux login --registry {}' with only the token value", base));
            return 1;
        }
        output.Verbose(std::format("Credentials: '{}'", credential.source));
    }

    const auto started = std::chrono::steady_clock::now();
    output.Progress("Packing", std::format("{} {}", QualifiedName(manifest), manifest.package.version.Text()));
    auto artifact = BuildPackageArtifact(manifestPath, manifest);
    if (!artifact) {
        diagnostics.Error(artifact.error().message);
        diagnostics.Note(std::format("package: {} {}", QualifiedName(manifest), manifest.package.version.Text()));
        for (const auto &note : artifact.error().notes) {
            diagnostics.Note(note);
        }
        diagnostics.Help("correct the package files, then run 'rux publish' again");
        return 1;
    }

    output.Success("Packed",
                   std::format("{} {} ({} files, {})", QualifiedName(manifest), manifest.package.version.Text(),
                               artifact->fileCount, FormatBytes(artifact->archive.size())));
    if (dryRun) {
        output.Success("Validated",
                       std::format("{} {} for publication in {}", QualifiedName(manifest),
                                   manifest.package.version.Text(), Reporting::FormatDuration(ElapsedMs(started))));
        output.Detail("Dry run: the package was not uploaded");
        return 0;
    }

    const std::array<MultipartPart, 2> parts{
        MultipartPart{.name = "manifest", .content = artifact->manifestSource},
        MultipartPart{.name = "package", .content = artifact->archive},
    };
    auto body = BuildMultipartBody(parts);
    if (!body) {
        diagnostics.Error("could not encode the publication request");
        diagnostics.Help("retry 'rux publish'; report the package if encoding fails again");
        return 1;
    }

    output.Progress("Uploading",
                    std::format("{} {} to '{}'", QualifiedName(manifest), manifest.package.version.Text(), base));
    std::string failureDetail;
    auto response = HttpSend({.method = "POST",
                              .url = base + "/v1/packages",
                              .headers = {{.name = "Authorization", .value = "Bearer " + credential.token},
                                          {.name = "Content-Type", .value = body->contentType}},
                              .body = std::move(body->body)},
                             &failureDetail);
    if (!response) {
        diagnostics.Error(std::format("could not connect to registry '{}'", base));
        if (!failureDetail.empty()) {
            diagnostics.Note(failureDetail);
        }
        diagnostics.Help("check the registry URL and network connection, then retry 'rux publish'");
        return 1;
    }
    if (response->status != 201) {
        ReportPublicationProblem(*response, manifest, base, credential.source, diagnostics);
        return 1;
    }

    output.Success("Published", std::format("{} {} in {}", QualifiedName(manifest), manifest.package.version.Text(),
                                            Reporting::FormatDuration(ElapsedMs(started))));
    output.Detail(std::format("Registry: '{}'", base));
    return 0;
}
