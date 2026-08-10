#include "Package/Manifest.h"
#include "Package/Package.h"
#include "System/Os.h"

#include <chrono>
#include <doctest.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace Rux;
using namespace Rux::System;

namespace {

// Owns a scratch directory so each case scaffolds into a clean tree.
class ScaffoldFixture {
public:
    ScaffoldFixture() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        root = TempDirectory() / ("rux-scaffold-test-" + std::to_string(nonce));
        std::filesystem::create_directories(root);
    }

    ~ScaffoldFixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    ScaffoldFixture(const ScaffoldFixture &) = delete;
    ScaffoldFixture &operator=(const ScaffoldFixture &) = delete;

    [[nodiscard]] std::filesystem::path Package(const std::string &name) const {
        return root / name;
    }

    [[nodiscard]] static std::string Read(const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        std::ostringstream buffer;
        buffer << input.rdbuf();
        return buffer.str();
    }

private:
    std::filesystem::path root;
};

} // namespace

TEST_CASE("scaffolding writes a Version 1 Executable package") {
    const ScaffoldFixture fixture;
    const auto packageRoot = fixture.Package("App");

    REQUIRE(ScaffoldPackage({.root = packageRoot, .name = "App", .type = ManifestPackageType::Executable}));

    const auto result = Manifest::Load(packageRoot / "Rux.toml");
    REQUIRE(result.Ok());
    CHECK(result.manifest->header.schemaVersion == manifestSchemaVersion);
    CHECK(result.manifest->package.name.Text() == "App");
    CHECK(result.manifest->package.version.Text() == "0.1.0");
    CHECK(result.manifest->package.type == ManifestPackageType::Executable);
    CHECK_FALSE(result.manifest->package.ns.has_value());

    CHECK(std::filesystem::is_regular_file(packageRoot / "Src" / "Main.rux"));
    CHECK(ScaffoldFixture::Read(packageRoot / "Src" / "Main.rux").contains("func Main()"));
    CHECK(std::filesystem::is_directory(packageRoot / "Bin" / "Debug"));
    CHECK(std::filesystem::is_directory(packageRoot / "Bin" / "Release"));
    CHECK(std::filesystem::is_directory(packageRoot / "Temp"));
    CHECK(std::filesystem::is_regular_file(packageRoot / ".gitignore"));
}

TEST_CASE("scaffolding writes a SharedLibrary package without an entry point") {
    const ScaffoldFixture fixture;
    const auto packageRoot = fixture.Package("Io");

    REQUIRE(ScaffoldPackage({.root = packageRoot, .name = "Io", .type = ManifestPackageType::SharedLibrary}));

    const auto result = Manifest::Load(packageRoot / "Rux.toml");
    REQUIRE(result.Ok());
    CHECK(result.manifest->package.type == ManifestPackageType::SharedLibrary);

    CHECK_FALSE(std::filesystem::exists(packageRoot / "Src" / "Main.rux"));
    REQUIRE(std::filesystem::is_regular_file(packageRoot / "Src" / "Lib.rux"));
    CHECK(ScaffoldFixture::Read(packageRoot / "Src" / "Lib.rux") == "module Io {\n}\n");
    CHECK(std::filesystem::is_directory(packageRoot / "Bin" / "Debug"));
}

TEST_CASE("scaffolding writes a StaticLibrary package without an entry point") {
    const ScaffoldFixture fixture;
    const auto packageRoot = fixture.Package("Math");

    REQUIRE(ScaffoldPackage({.root = packageRoot, .name = "Math", .type = ManifestPackageType::StaticLibrary}));

    const auto result = Manifest::Load(packageRoot / "Rux.toml");
    REQUIRE(result.Ok());
    CHECK(result.manifest->package.type == ManifestPackageType::StaticLibrary);
    CHECK(std::filesystem::is_regular_file(packageRoot / "Src" / "Lib.rux"));
    CHECK_FALSE(std::filesystem::exists(packageRoot / "Src" / "Main.rux"));
    CHECK(std::filesystem::is_directory(packageRoot / "Bin" / "Debug"));
}

TEST_CASE("scaffolding writes a SourceLibrary package with no output directories") {
    const ScaffoldFixture fixture;
    const auto packageRoot = fixture.Package("Json");

    REQUIRE(ScaffoldPackage({.root = packageRoot, .name = "Json", .type = ManifestPackageType::SourceLibrary}));

    const auto result = Manifest::Load(packageRoot / "Rux.toml");
    REQUIRE(result.Ok());
    CHECK(result.manifest->package.type == ManifestPackageType::SourceLibrary);

    CHECK(std::filesystem::is_regular_file(packageRoot / "Src" / "Lib.rux"));
    // A SourceLibrary package is compiled into its dependents and links nothing itself.
    CHECK_FALSE(std::filesystem::exists(packageRoot / "Bin"));
}

TEST_CASE("scaffolding records an optional namespace") {
    const ScaffoldFixture fixture;
    const auto packageRoot = fixture.Package("Io");

    REQUIRE(ScaffoldPackage({.root = packageRoot,
                             .name = "Io",
                             .type = ManifestPackageType::SharedLibrary,
                             .ns = *IdentitySegment::Parse("Rux")}));

    const auto result = Manifest::Load(packageRoot / "Rux.toml");
    REQUIRE(result.Ok());
    REQUIRE(result.manifest->package.ns.has_value());
    CHECK(result.manifest->package.ns->Text() == "Rux");
    CHECK(ScaffoldFixture::Read(packageRoot / "Rux.toml").contains("Namespace = \"Rux\""));
}

TEST_CASE("scaffolding rejects a name that is not an identity segment") {
    const ScaffoldFixture fixture;

    CHECK_FALSE(ScaffoldPackage({.root = fixture.Package("dotted"), .name = "My.Pkg"}));
    CHECK_FALSE(ScaffoldPackage({.root = fixture.Package("adjacent"), .name = "My__Pkg"}));
    CHECK_FALSE(ScaffoldPackage({.root = fixture.Package("empty"), .name = ""}));
    // A rejected name leaves nothing behind.
    CHECK_FALSE(std::filesystem::exists(fixture.Package("dotted")));
}

TEST_CASE("scaffolding refuses an existing directory unless initializing") {
    const ScaffoldFixture fixture;
    const auto packageRoot = fixture.Package("App");
    std::filesystem::create_directories(packageRoot);

    CHECK_FALSE(ScaffoldPackage({.root = packageRoot, .name = "App"}));
    CHECK(ScaffoldPackage({.root = packageRoot, .name = "App", .initMode = true}));
}

TEST_CASE("initializing preserves an existing manifest and sources") {
    const ScaffoldFixture fixture;
    const auto packageRoot = fixture.Package("App");
    REQUIRE(ScaffoldPackage({.root = packageRoot,
                             .name = "App",
                             .type = ManifestPackageType::SourceLibrary,
                             .ns = *IdentitySegment::Parse("Rux")}));
    const auto manifestBefore = ScaffoldFixture::Read(packageRoot / "Rux.toml");
    const auto sourceBefore = ScaffoldFixture::Read(packageRoot / "Src" / "Lib.rux");

    // Re-initializing with different options must not rewrite what is there.
    REQUIRE(ScaffoldPackage(
        {.root = packageRoot, .name = "App", .type = ManifestPackageType::SharedLibrary, .initMode = true}));

    CHECK(ScaffoldFixture::Read(packageRoot / "Rux.toml") == manifestBefore);
    CHECK(ScaffoldFixture::Read(packageRoot / "Src" / "Lib.rux") == sourceBefore);
}
