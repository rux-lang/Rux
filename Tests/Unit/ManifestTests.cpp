#include "Package/Manifest.h"
#include "System/Os.h"

#include <algorithm>
#include <chrono>
#include <doctest.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace Rux;
using namespace Rux::System;

TEST_CASE("Manifest preserves author arrays and uses canonical assignment spacing") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root = TempDirectory() / ("rux-manifest-test-" + std::to_string(nonce));
    const std::filesystem::path path = root / "Rux.toml";
    std::filesystem::create_directories(root);

    {
        std::ofstream input(path);
        input << R"([Package]
Name = "App"
Version = "0.1.0"
Type = "bin"
Authors = ["Rux Contributors <info@rux-lang.dev>"]

[Build]
Output = "Bin"
)";
        REQUIRE(input.good());
    }

    const auto manifest = Manifest::Load(path);
    REQUIRE(manifest.has_value());
    CHECK(manifest->package.authors == std::vector<std::string>{"Rux Contributors <info@rux-lang.dev>"});

    Manifest updated = *manifest;
    REQUIRE(updated.AddDependency("Io", ""));
    REQUIRE(updated.Save(path));

    std::ifstream output(path);
    std::ostringstream contents;
    contents << output.rdbuf();
    const std::string expected = R"([Package]
Name = "App"
Version = "0.1.0"
Type = "bin"
Authors = ["Rux Contributors <info@rux-lang.dev>"]

[Dependencies]
Io = "*"

[Build]
Output = "Bin"
)";
    CHECK(contents.str() == expected);

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

TEST_CASE("Manifest-less workspace discovery finds members and test packages") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root = TempDirectory() / ("rux-workspace-discovery-test-" + std::to_string(nonce));

    const std::vector<std::filesystem::path> expected = {
        root / "Member" / "Rux.toml",
        root / "Member" / "Tests" / "MemberTest" / "Rux.toml",
        root / "Tests" / "Direct" / "Rux.toml",
        root / "Tests" / "Integration" / "Nested" / "Rux.toml",
    };
    for (const auto &manifestPath : expected) {
        std::filesystem::create_directories(manifestPath.parent_path());
        std::ofstream manifest(manifestPath);
        manifest << "[Package]\nName = \"Fixture\"\n";
        REQUIRE(manifest.good());
    }

    // Package roots are terminal: manifests nested inside a discovered package
    // do not become additional workspace members.
    const auto hiddenNestedManifest = root / "Tests" / "Direct" / "Nested" / "Rux.toml";
    std::filesystem::create_directories(hiddenNestedManifest.parent_path());
    {
        std::ofstream manifest(hiddenNestedManifest);
        manifest << "[Package]\nName = \"Hidden\"\n";
        REQUIRE(manifest.good());
    }

    auto actual = DiscoverManifestlessWorkspaceManifests(root);
    auto sortedExpected = expected;
    std::ranges::sort(sortedExpected);
    CHECK(actual == sortedExpected);

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

TEST_CASE("repository Rux tests use canonical local manifests") {
    const auto testsRoot = std::filesystem::weakly_canonical(std::filesystem::path(RUX_TESTS_DIR));
    const auto packagesRoot = std::filesystem::weakly_canonical(std::filesystem::path(RUX_PACKAGES_DIR));
    const auto binariesRoot = std::filesystem::weakly_canonical(std::filesystem::path(RUX_TEST_BIN_DIR));
    std::size_t manifestCount = 0;

    for (const auto &entry : std::filesystem::recursive_directory_iterator(testsRoot)) {
        if (!entry.is_regular_file() || entry.path().filename() != "Rux.toml") {
            continue;
        }
        ++manifestCount;
        const auto manifest = Manifest::Load(entry.path());
        REQUIRE_MESSAGE(manifest.has_value(), "invalid test manifest: ", entry.path().string());
        CHECK_MESSAGE(manifest->package.type == "bin",
                      "test package must explicitly use Type = \"bin\": ", entry.path().string());
        CHECK_MESSAGE(!manifest->package.description.empty(),
                      "test package needs a description: ", entry.path().string());
        CHECK_MESSAGE(std::filesystem::is_regular_file(entry.path().parent_path() / "Src" / "Main.rux"),
                      "test package needs Src/Main.rux: ", entry.path().string());

        const auto output = std::filesystem::weakly_canonical(entry.path().parent_path() / manifest->build.output);
        const auto outputRelative = output.lexically_relative(binariesRoot);
        const bool outputIsCentralized = !outputRelative.empty() && *outputRelative.begin() != "..";
        CHECK_MESSAGE(outputIsCentralized, "test output must stay below Bin/Tests: ", entry.path().string());

        for (const auto &dependency : manifest->dependencies) {
            REQUIRE_MESSAGE(!dependency.path.empty(),
                            "test dependencies must use local Path entries: ", entry.path().string(), " -> ",
                            dependency.name);
            const auto dependencyRoot = std::filesystem::weakly_canonical(entry.path().parent_path() / dependency.path);
            const auto dependencyRelative = dependencyRoot.lexically_relative(packagesRoot);
            const bool dependencyIsLocal = !dependencyRelative.empty() && *dependencyRelative.begin() != "..";
            CHECK_MESSAGE(dependencyIsLocal, "test dependency must resolve below Packages: ", entry.path().string(),
                          " -> ", dependency.name);
            const auto dependencyManifest = Manifest::Load(dependencyRoot / "Rux.toml");
            REQUIRE_MESSAGE(dependencyManifest.has_value(),
                            "local dependency has no valid manifest: ", dependencyRoot.string());
            const auto expectedName = dependency.package.empty() ? dependency.name : dependency.package;
            CHECK_MESSAGE(dependencyManifest->package.name == expectedName, "dependency name/path mismatch in ",
                          entry.path().string(), ": expected ", expectedName, ", found ",
                          dependencyManifest->package.name);
        }
    }

    CHECK_MESSAGE(manifestCount > 0, "no Rux test manifests found below ", testsRoot.string());
}
