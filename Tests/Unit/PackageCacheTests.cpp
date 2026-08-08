// The version-keyed package cache: where an installed version lives, and which
// installed version a requirement resolves to. Build and check answer that
// question from disk alone, so it is the one piece of dependency resolution
// that must work with no registry in reach.

#include "Driver/BuildTarget.h"
#include "System/Os.h"
#include "Target/Target.h"

#include <doctest.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

using namespace Rux;
using namespace Rux::Driver;
using namespace Rux::System;
using namespace Rux::Target;

namespace {
/// The variable UserDataDir derives the per-user root from on this host.
constexpr const char *homeVariable = (HostOS == OS::Windows) ? "LOCALAPPDATA" : "HOME";

/**
 * @brief Point the package cache at a private temporary tree.
 *
 * Without this the cases below would create and delete directories inside the
 * real cache of whoever is running the tests.
 */
class ScopedPackageCache {
public:
    ScopedPackageCache() {
        static int sequence = 0;
        savedHome_ = GetEnvPath(homeVariable);

        root_ = TempDirectory() / ("RuxPackageCacheTest-" + std::to_string(++sequence));
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
        std::filesystem::create_directories(root_, ec);
        REQUIRE(!ec);
        REQUIRE(SetEnvPath(homeVariable, root_));
        // A redirect that silently failed would point the cases at the real
        // cache, so prove the path moved before any of them run.
        REQUIRE(RegistryPackagesDir().string().starts_with(root_.string()));
    }

    ScopedPackageCache(const ScopedPackageCache &) = delete;
    ScopedPackageCache &operator=(const ScopedPackageCache &) = delete;

    ~ScopedPackageCache() {
        if (savedHome_) {
            static_cast<void>(SetEnvPath(homeVariable, *savedHome_));
        }
        else {
            static_cast<void>(UnsetEnv(homeVariable));
        }
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

private:
    std::optional<std::filesystem::path> savedHome_;
    std::filesystem::path root_;
};

IdentitySegment Segment(const std::string_view text) {
    auto parsed = IdentitySegment::Parse(text);
    REQUIRE(parsed.has_value());
    return *parsed;
}

SemanticVersion Version(const std::string_view text) {
    auto parsed = SemanticVersion::Parse(text);
    REQUIRE(parsed.has_value());
    return *parsed;
}

VersionRange Range(const std::string_view text) {
    auto parsed = VersionRange::Parse(text);
    REQUIRE(parsed.has_value());
    return *parsed;
}

/// Create a cache entry the way an install would, manifest included.
void Install(const std::string_view ns, const std::string_view name, const std::string_view version) {
    const auto dir = RegistryPackageDir(Segment(ns), Segment(name), Version(version));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    REQUIRE(!ec);
    std::ofstream(dir / "Rux.toml", std::ios::binary) << "[Manifest]\nVersion = 1\n";
}

/// Create a directory in the cache that no install would have produced.
void PlaceStrayDirectory(const std::string_view ns, const std::string_view name, const std::string_view leaf) {
    std::error_code ec;
    std::filesystem::create_directories(RegistryPackagesDir() / std::string(ns) / std::string(name) / std::string(leaf),
                                        ec);
    REQUIRE(!ec);
}
} // namespace

TEST_CASE("a package directory is keyed by normalized identity and exact version") {
    const ScopedPackageCache cache;
    const auto dir = RegistryPackageDir(Segment("Rux"), Segment("My_Pkg"), Version("1.2.3+native"));

    CHECK(dir.filename() == "1.2.3+native");
    CHECK(dir.parent_path().filename() == "my-pkg");
    CHECK(dir.parent_path().parent_path().filename() == "rux");
    CHECK(dir.parent_path().parent_path().parent_path() == RegistryPackagesDir());

    // Two spellings of one identity share one directory, as they share one
    // registry entry.
    CHECK(RegistryPackageDir(Segment("rux"), Segment("my-pkg"), Version("1.2.3+native")) == dir);
}

TEST_CASE("installed versions are reported in ascending order") {
    const ScopedPackageCache cache;
    Install("Rux", "Io", "0.2.0");
    Install("Rux", "Io", "0.1.0");
    Install("Rux", "Io", "1.0.0");

    const auto installed = InstalledVersions(Segment("Rux"), Segment("Io"));
    REQUIRE(installed.size() == 3);
    CHECK(installed[0].version.Text() == "0.1.0");
    CHECK(installed[1].version.Text() == "0.2.0");
    CHECK(installed[2].version.Text() == "1.0.0");
    CHECK(installed[2].root.filename() == "1.0.0");
}

TEST_CASE("a package that was never installed has no versions") {
    const ScopedPackageCache cache;
    CHECK(InstalledVersions(Segment("Rux"), Segment("Io")).empty());

    Install("Rux", "Io", "0.1.0");
    CHECK(InstalledVersions(Segment("Rux"), Segment("Json")).empty());
    CHECK(InstalledVersions(Segment("Acme"), Segment("Io")).empty());
}

TEST_CASE("a directory whose name is not a version is ignored") {
    const ScopedPackageCache cache;
    Install("Rux", "Io", "0.1.0");
    PlaceStrayDirectory("rux", "io", "Src");
    PlaceStrayDirectory("rux", "io", "not-a-version");

    const auto installed = InstalledVersions(Segment("Rux"), Segment("Io"));
    REQUIRE(installed.size() == 1);
    CHECK(installed[0].version.Text() == "0.1.0");
}

TEST_CASE("a requirement resolves to the highest installed version it matches") {
    const ScopedPackageCache cache;
    Install("Rux", "Io", "0.1.0");
    Install("Rux", "Io", "0.1.5");
    Install("Rux", "Io", "0.2.0");
    Install("Rux", "Io", "1.0.0");

    const auto caret = FindInstalledPackage(Segment("Rux"), Segment("Io"), Range("^0.1.0"));
    REQUIRE(caret.has_value());
    CHECK(caret->version.Text() == "0.1.5");

    const auto any = FindInstalledPackage(Segment("Rux"), Segment("Io"), Range("*"));
    REQUIRE(any.has_value());
    CHECK(any->version.Text() == "1.0.0");

    const auto exact = FindInstalledPackage(Segment("Rux"), Segment("Io"), Range("=0.2.0"));
    REQUIRE(exact.has_value());
    CHECK(exact->version.Text() == "0.2.0");
    CHECK(std::filesystem::exists(exact->root / "Rux.toml"));
}

TEST_CASE("a requirement no installed version matches resolves to nothing") {
    const ScopedPackageCache cache;
    Install("Rux", "Io", "0.1.0");
    CHECK_FALSE(FindInstalledPackage(Segment("Rux"), Segment("Io"), Range("^1.0.0")).has_value());
    CHECK_FALSE(FindInstalledPackage(Segment("Rux"), Segment("Json"), Range("*")).has_value());
}

TEST_CASE("resolution finds a package installed under a different spelling") {
    const ScopedPackageCache cache;
    Install("rux", "my-pkg", "1.0.0");

    const auto found = FindInstalledPackage(Segment("Rux"), Segment("My_Pkg"), Range("^1.0.0"));
    REQUIRE(found.has_value());
    CHECK(found->version.Text() == "1.0.0");
}

TEST_CASE("build metadata breaks a tie between otherwise equal versions") {
    const ScopedPackageCache cache;
    Install("Rux", "Io", "1.0.0+1");
    Install("Rux", "Io", "1.0.0+2");

    const auto found = FindInstalledPackage(Segment("Rux"), Segment("Io"), Range("=1.0.0"));
    REQUIRE(found.has_value());
    // Ascending order puts the higher build metadata last, and the last of a
    // precedence tie wins, so the choice is deterministic rather than arbitrary.
    CHECK(found->version.Text() == "1.0.0+2");
}
