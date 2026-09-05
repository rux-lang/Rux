// The version-keyed package cache: where an installed version lives, and which
// installed version a requirement resolves to. Build and check answer that
// question from disk alone, so it is the one piece of dependency resolution
// that must work with no registry in reach.

#include "Driver/BuildTarget.h"
#include "Package/Cache.h"
#include "Package/Installation.h"
#include "System/Os.h"
#include "Target/Target.h"

#include <algorithm>
#include <doctest.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Rux::Packages;

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
        savedHome = GetEnvPath(homeVariable);

        root = TempDirectory() / ("RuxPackageCacheTest-" + std::to_string(++sequence));
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);
        REQUIRE(!ec);
        REQUIRE(SetEnvPath(homeVariable, root));
        // A redirect that silently failed would point the cases at the real
        // cache, so prove the path moved before any of them run.
        REQUIRE(RegistryPackagesDir().string().starts_with(root.string()));
    }

    ScopedPackageCache(const ScopedPackageCache &) = delete;
    ScopedPackageCache &operator=(const ScopedPackageCache &) = delete;

    ~ScopedPackageCache() {
        if (savedHome) {
            static_cast<void>(SetEnvPath(homeVariable, *savedHome));
        }
        else {
            static_cast<void>(UnsetEnv(homeVariable));
        }
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

private:
    std::optional<std::filesystem::path> savedHome;
    std::filesystem::path root;
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

/// Create a directory beside a package's installed versions that no install
/// would have produced. The parent is resolved the way production resolves it,
/// so the stray always lands beside the versions it is meant to sit beside.
void PlaceStrayDirectory(const std::string_view ns, const std::string_view name, const std::string_view leaf) {
    std::error_code ec;
    std::filesystem::create_directories(RegistryPackageParentDir(Segment(ns), Segment(name)) / std::string(leaf), ec);
    REQUIRE(!ec);
}

/// Create a directory under the cache root from literal path segments, for the
/// cases that are about the spelling on disk and so cannot go through the
/// production path.
void PlaceRawDirectory(const std::string_view relative) {
    std::error_code ec;
    std::filesystem::create_directories(RegistryPackagesDir() / std::filesystem::path(relative), ec);
    REQUIRE(!ec);
}

/// Filenames directly under `dir`, sorted. Reading the name back is the only
/// portable way to assert casing: exists() is case-blind on Windows and macOS.
std::vector<std::string> ChildNames(const std::filesystem::path &dir) {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
        names.push_back(entry.path().filename().string());
    }
    std::ranges::sort(names);
    return names;
}
} // namespace

TEST_CASE("a package directory is keyed by display spelling and exact version") {
    const ScopedPackageCache cache;
    const auto dir = RegistryPackageDir(Segment("Rux"), Segment("My_Pkg"), Version("1.2.3+native"));

    CHECK(dir.filename() == "1.2.3+native");
    CHECK(dir.parent_path().filename() == "My_Pkg");
    CHECK(dir.parent_path().parent_path().filename() == "Rux");
    CHECK(dir.parent_path().parent_path().parent_path() == RegistryPackagesDir());
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
    PlaceStrayDirectory("Rux", "Io", "Src");
    PlaceStrayDirectory("Rux", "Io", "not-a-version");

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

    // An install writes the spelling the registry publishes; a build looks the
    // package up with the spelling the consuming manifest uses. This is the case
    // that fails if lookup ever goes back to joining the caller's spelling, and
    // it is meaningful on either kind of filesystem: `rux` and `Rux` differ only
    // where case matters, but `my-pkg` and `My_Pkg` differ everywhere.
    const auto found = FindInstalledPackage(Segment("Rux"), Segment("My_Pkg"), Range("^1.0.0"));
    REQUIRE(found.has_value());
    CHECK(found->version.Text() == "1.0.0");
}

TEST_CASE("an install creates directories under the published spelling") {
    const ScopedPackageCache cache;
    Install("Rux", "Windows", "0.1.0");

    CHECK(ChildNames(RegistryPackagesDir()) == std::vector<std::string>{"Rux"});
    CHECK(ChildNames(RegistryPackagesDir() / "Rux") == std::vector<std::string>{"Windows"});
}

TEST_CASE("looking a package up creates nothing") {
    const ScopedPackageCache cache;
    PlaceRawDirectory("rux/io/0.1.0");

    const auto found = FindInstalledPackage(Segment("Rux"), Segment("Io"), Range("*"));
    REQUIRE(found.has_value());
    CHECK(found->version.Text() == "0.1.0");
    // Resolving an identity must not leave a second tree behind for it.
    CHECK(ChildNames(RegistryPackagesDir()) == std::vector<std::string>{"rux"});
}

TEST_CASE("a new version joins the directory a package already has") {
    const ScopedPackageCache cache;
    PlaceRawDirectory("rux/io");

    // Only the missing leaf takes the display spelling; an existing parent is
    // kept, because forking a second tree would hide the versions already in it.
    const auto dir = RegistryPackageDir(Segment("Rux"), Segment("Io"), Version("1.0.0"));
    CHECK(dir.parent_path() == RegistryPackagesDir() / "rux" / "io");
}

// Two directories that differ only by case need a case-sensitive filesystem to
// coexist, but `-` and `_` are distinct bytes everywhere while normalizing to
// the same identity. The pair below therefore reproduces a split tree on all
// four supported platforms rather than only on Linux and FreeBSD.

TEST_CASE("one of two spellings of an identity is preferred consistently") {
    const ScopedPackageCache cache;
    PlaceRawDirectory("Rux/My-Pkg");
    PlaceRawDirectory("Rux/My_Pkg");

    // Directory iteration order is unspecified, so without a tie-break a build
    // could resolve to a different directory on each run.
    const auto first = RegistryPackageParentDir(Segment("Rux"), Segment("my-pkg"));
    CHECK(RegistryPackageParentDir(Segment("Rux"), Segment("my-pkg")) == first);
    CHECK(first.filename() == "My-Pkg");
}

TEST_CASE("the display spelling wins over another spelling of the same identity") {
    const ScopedPackageCache cache;
    PlaceRawDirectory("Rux/My-Pkg");
    PlaceRawDirectory("Rux/My_Pkg");

    // "My-Pkg" is the lower name, so only an exact match can select "My_Pkg".
    CHECK(RegistryPackageParentDir(Segment("Rux"), Segment("My_Pkg")).filename() == "My_Pkg");
}

TEST_CASE("a file where a namespace directory would be is not a package") {
    const ScopedPackageCache cache;
    std::error_code ec;
    std::filesystem::create_directories(RegistryPackagesDir(), ec);
    REQUIRE(!ec);
    std::ofstream(RegistryPackagesDir() / "Rux", std::ios::binary) << "not a directory";

    CHECK(InstalledVersions(Segment("Rux"), Segment("Io")).empty());
}

TEST_CASE("a directory that is not an identity at all is ignored") {
    const ScopedPackageCache cache;
    Install("Rux", "Io", "0.1.0");
    PlaceRawDirectory("not a valid identity!");

    const auto found = FindInstalledPackage(Segment("Rux"), Segment("Io"), Range("*"));
    REQUIRE(found.has_value());
    CHECK(found->version.Text() == "0.1.0");
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

TEST_CASE("installation reuses a verified cache identity without starting a download") {
    ScopedPackageCache cache;
    const ResolvedPackage resolved{Segment("Rux"), Segment("CacheProbe"), Version("1.2.3")};
    Install("Rux", "CacheProbe", "1.2.3");
    Manifest manifest;
    manifest.package.ns = resolved.ns;
    manifest.package.name = resolved.package;
    manifest.package.version = resolved.version;
    manifest.package.type = ManifestPackageType::SourceLibrary;
    REQUIRE(manifest.Save(RegistryPackageDir(resolved.ns, resolved.package, resolved.version) / "Rux.toml"));
    bool started = false;
    const auto result = InstallPackage("unused://cache-test", resolved, [&] { started = true; });
    REQUIRE(result.has_value());
    CHECK(result->alreadyInstalled);
    CHECK(result->fileCount == 0);
    CHECK_FALSE(started);
}
