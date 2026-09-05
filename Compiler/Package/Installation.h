#pragma once
#include "Package/PackageResolution.h"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>

namespace Rux::Packages {
struct InstallationResult {
    bool alreadyInstalled = false;
    std::size_t fileCount = 0;
};

/// Verify the digest, archive and manifest before committing a downloaded package. The callback reports download
/// progress only; package services return structured failures and never write process output.
[[nodiscard]] std::expected<InstallationResult, ResolutionFailure>
InstallPackage(std::string_view registryBase, const ResolvedPackage &resolution,
               const std::function<void()> &beginDownload = {});

/// Replace `dest` with the fully prepared directory `staging`, keeping the previous contents until the swap succeeds. A
/// half-written package therefore never becomes the installed one.
[[nodiscard]] bool CommitDownloadedPackage(const std::filesystem::path &staging, const std::filesystem::path &dest);
} // namespace Rux::Packages
