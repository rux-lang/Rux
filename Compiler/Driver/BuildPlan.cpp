#include "Driver/BuildPlan.h"

#include "Driver/BuildTarget.h"

#include <array>

namespace Rux::Driver {
std::vector<BuildCell> GenerateBuildMatrix(const std::filesystem::path &root, const Manifest &manifest) {
    constexpr std::array profiles = {BuildProfile::Debug, BuildProfile::Release};
    const auto targets = Target::TargetTriple::Supported();

    std::vector<BuildCell> cells;
    cells.reserve(profiles.size() * targets.size());
    for (const BuildProfile profile : profiles) {
        for (const Target::TargetTriple target : targets) {
            cells.push_back(BuildCell{.profile = profile,
                                      .target = target,
                                      .outputDirectory = ResolveArtifactOutputDir(root, manifest, profile, target)});
        }
    }
    return cells;
}
} // namespace Rux::Driver
