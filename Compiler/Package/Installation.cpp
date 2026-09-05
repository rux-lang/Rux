#include "Package/Installation.h"

#include "Package/Artifact.h"
#include "Package/Cache.h"
#include "Package/Checksum.h"

#include <format>
#include <system_error>

namespace Rux::Packages {
namespace {
bool CacheEntryMatches(const std::filesystem::path &root, const ResolvedPackage &resolution) {
    const auto loaded = Manifest::Load(root / "Rux.toml");
    if (!loaded.Ok() || loaded.manifest->IsWorkspace()) {
        return false;
    }
    const Package &package = loaded.manifest->package;
    return package.ns && *package.ns == resolution.ns && package.name == resolution.package &&
           package.version.Text() == resolution.version.Text();
}

} // namespace

std::expected<InstallationResult, ResolutionFailure> InstallPackage(const std::string_view registryBase,
                                                                    const ResolvedPackage &resolution,
                                                                    const std::function<void()> &beginDownload) {
    const std::string identity = QualifiedIdentity(resolution.ns, resolution.package);
    const std::filesystem::path packageDir = RegistryPackageDir(resolution.ns, resolution.package, resolution.version);

    if (CacheEntryMatches(packageDir, resolution)) {
        return InstallationResult{.alreadyInstalled = true};
    }
    if (beginDownload) {
        beginDownload();
    }
    ResolutionFailure failure;
    auto digest = FetchArtifactChecksum(registryBase, resolution.ns, resolution.package, resolution.version);
    if (!digest) {
        const auto problem = Describe(digest.error(), registryBase, identity);
        failure.message =
            std::format("could not fetch checksum metadata for {} {}", identity, resolution.version.Text());
        failure.notes.push_back(problem.message);
        for (const auto &note : problem.notes) {
            failure.notes.push_back(note);
        }
        failure.help = problem.help.value_or("check the registry URL and network, then retry the installation");
        return std::unexpected(std::move(failure));
    }
    auto archive = DownloadArtifact(registryBase, resolution.ns, resolution.package, resolution.version);
    if (!archive) {
        const auto problem = Describe(archive.error(), registryBase, identity);
        failure.message = std::format("could not download {} {}", identity, resolution.version.Text());
        failure.notes.push_back(problem.message);
        for (const auto &note : problem.notes) {
            failure.notes.push_back(note);
        }
        failure.help = problem.help.value_or("check the registry URL and network, then retry the installation");
        return std::unexpected(std::move(failure));
    }
    if (const std::string actual = Sha256Hex(*archive); !DigestsEqual(actual, *digest)) {
        failure.message = std::format("downloaded archive for {} {} failed checksum verification", identity,
                                      resolution.version.Text());
        failure.notes.push_back(std::format("expected sha256: {}", *digest));
        failure.notes.push_back(std::format("downloaded sha256: {}", actual));
        failure.help = "retry the download; contact the registry operator if the mismatch persists";
        return std::unexpected(std::move(failure));
    }

    std::filesystem::path staging = packageDir;
    staging += ".download";
    std::error_code ec;
    std::filesystem::remove_all(staging, ec);
    std::filesystem::create_directories(packageDir.parent_path(), ec);
    if (ec) {
        failure.message =
            std::format("could not create package cache directory '{}'", packageDir.parent_path().string());
        failure.notes.push_back(ec.message());
        failure.help = "check the cache directory permissions, then retry";
        return std::unexpected(std::move(failure));
    }

    auto extracted = ExtractPackageArtifact(*archive, staging);
    if (!extracted) {
        std::filesystem::remove_all(staging, ec);
        failure.message = std::format("downloaded archive for {} {} was rejected", identity, resolution.version.Text());
        failure.notes.push_back(extracted.error().message);
        for (const auto &note : extracted.error().notes) {
            failure.notes.push_back(note);
        }
        failure.help = "contact the registry operator; the published archive is unsafe or invalid";
        return std::unexpected(std::move(failure));
    }

    const auto staged = Manifest::Load(staging / "Rux.toml");
    if (!staged.Ok()) {
        std::filesystem::remove_all(staging, ec);
        failure.message =
            std::format("archive published as {} {} contains an invalid manifest", identity, resolution.version.Text());
        if (!staged.diagnostics.empty()) {
            failure.notes.push_back(staged.diagnostics.front().Format());
        }
        failure.help = "contact the registry operator; the artifact metadata is inconsistent";
        return std::unexpected(std::move(failure));
    }
    if (staged.manifest->IsWorkspace() || !staged.manifest->package.ns ||
        *staged.manifest->package.ns != resolution.ns || staged.manifest->package.name != resolution.package ||
        staged.manifest->package.version.Text() != resolution.version.Text()) {
        std::filesystem::remove_all(staging, ec);
        failure.message =
            std::format("archive published as {} {} contains a different package", identity, resolution.version.Text());
        if (staged.manifest->IsWorkspace()) {
            failure.notes.push_back("archive manifest declares a workspace instead of a package");
        }
        else {
            const std::string actualIdentity =
                staged.manifest->package.ns
                    ? std::format("{}/{}", staged.manifest->package.ns->Text(), staged.manifest->package.name.Text())
                    : staged.manifest->package.name.Text();
            failure.notes.push_back(std::format("archive manifest declares {} {}", actualIdentity,
                                                staged.manifest->package.version.Text()));
        }
        failure.help = "contact the registry operator; the artifact metadata is inconsistent";
        return std::unexpected(std::move(failure));
    }

    if (!CommitDownloadedPackage(staging, packageDir)) {
        std::filesystem::remove_all(staging, ec);
        failure.message =
            std::format("could not install {} {} into '{}'", identity, resolution.version.Text(), packageDir.string());
        failure.help = "check the cache directory permissions, then retry";
        return std::unexpected(std::move(failure));
    }

    return InstallationResult{.fileCount = extracted->fileCount};
}

bool CommitDownloadedPackage(const std::filesystem::path &staging, const std::filesystem::path &dest) {
    std::error_code ec;
    std::filesystem::path backup = dest;
    backup += ".previous";
    std::filesystem::remove_all(backup, ec);
    ec.clear();

    const bool hadExisting = std::filesystem::exists(dest, ec);
    if (ec) {
        return false;
    }
    if (hadExisting) {
        std::filesystem::rename(dest, backup, ec);
        if (ec) {
            return false;
        }
    }

    std::filesystem::rename(staging, dest, ec);
    if (ec) {
        if (hadExisting) {
            std::error_code restoreError;
            std::filesystem::rename(backup, dest, restoreError);
        }
        return false;
    }
    std::filesystem::remove_all(backup, ec);
    return true;
}

} // namespace Rux::Packages
