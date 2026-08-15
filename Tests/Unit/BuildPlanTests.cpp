#include "Driver/BuildPlan.h"
#include "Driver/BuildTarget.h"

#include <doctest.h>
#include <filesystem>
#include <set>
#include <string>

using namespace Rux;
using namespace Rux::Driver;

TEST_CASE("build matrix follows profile and target catalog order") {
    Manifest manifest;
    manifest.build.output = "Artifacts";
    const std::filesystem::path root = "Workspace";
    const auto targets = Target::TargetTriple::Supported();

    const auto cells = GenerateBuildMatrix(root, manifest);

    REQUIRE(targets.size() == 8);
    REQUIRE(cells.size() == 16);
    for (std::size_t index = 0; index < targets.size(); ++index) {
        const auto &debug = cells[index];
        CHECK(debug.profile == BuildProfile::Debug);
        CHECK(debug.target == targets[index]);
        CHECK(debug.outputDirectory == root / "Artifacts" / "Debug" / TargetOutputPath(targets[index]));

        const auto &release = cells[targets.size() + index];
        CHECK(release.profile == BuildProfile::Release);
        CHECK(release.target == targets[index]);
        CHECK(release.outputDirectory == root / "Artifacts" / "Release" / TargetOutputPath(targets[index]));
    }
}

TEST_CASE("build matrix output directories are unique and deterministic") {
    Manifest manifest;
    manifest.build.output = "Dist/Products";
    const std::filesystem::path root = "Workspace/Package";

    const auto first = GenerateBuildMatrix(root, manifest);
    const auto second = GenerateBuildMatrix(root, manifest);
    std::set<std::filesystem::path> outputDirectories;
    for (const auto &cell : first) {
        outputDirectories.insert(cell.outputDirectory);
    }

    CHECK(first == second);
    CHECK(outputDirectories.size() == 16);
    CHECK(outputDirectories.size() == first.size());
}
