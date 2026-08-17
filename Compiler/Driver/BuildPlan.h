#pragma once

#include "BuildInfo/BuildProfile.h"
#include "Package/Manifest.h"
#include "Target/TargetTriple.h"

#include <compare>
#include <filesystem>
#include <vector>

namespace Rux::Driver {
/// One independently addressable target/profile combination in a build plan. The output directory is resolved when the
/// plan is created so every later phase consumes the same collision-free path.
struct BuildCell {
    BuildProfile profile;
    Target::TargetTriple target;
    std::filesystem::path outputDirectory;

    auto operator<=>(const BuildCell &) const = default;
};

/// Produces every supported target in Debug catalog order, followed by the same target catalog in Release. The catalog
/// is owned by TargetTriple.
[[nodiscard]] std::vector<BuildCell> GenerateBuildMatrix(const std::filesystem::path &root, const Manifest &manifest);
} // namespace Rux::Driver
