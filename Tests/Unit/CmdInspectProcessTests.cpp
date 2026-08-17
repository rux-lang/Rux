// Process-level contracts for project and global package inspection. These
// fixtures isolate the package cache so installed and missing states are
// deterministic and never inspect or mutate the developer's real cache.

#include "Driver/BuildTarget.h"
#include "System/Os.h"
#include "System/Process.h"

#include <array>
#include <chrono>
#include <doctest.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

using namespace Rux;

namespace {
std::filesystem::path RuxExecutable() {
    return std::filesystem::path(RUX_ROOT_DIR) / "Bin" / System::ExecutableFileName("rux");
}

template <std::size_t N>
System::RunResult Run(const std::array<std::string_view, N> &arguments) {
    const auto result = System::RunCaptured(RuxExecutable(), arguments);
    REQUIRE(result.has_value());
    return *result;
}

constexpr const char *packageCacheHomeVariable = Target::HostOS == Target::OS::Windows ? "LOCALAPPDATA" : "HOME";

class ScopedPackageCache {
public:
    ScopedPackageCache() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        savedHome = System::GetEnvPath(packageCacheHomeVariable);
        root = System::TempDirectory() / ("rux-cmd-inspect-cache-" + std::to_string(nonce));
        std::error_code error;
        std::filesystem::create_directories(root, error);
        REQUIRE(!error);
        REQUIRE(System::SetEnvPath(packageCacheHomeVariable, root));
        REQUIRE(Driver::RegistryPackagesDir().string().starts_with(root.string()));
    }

    ScopedPackageCache(const ScopedPackageCache &) = delete;
    ScopedPackageCache &operator=(const ScopedPackageCache &) = delete;

    ~ScopedPackageCache() {
        if (savedHome) {
            static_cast<void>(System::SetEnvPath(packageCacheHomeVariable, *savedHome));
        }
        else {
            static_cast<void>(System::UnsetEnv(packageCacheHomeVariable));
        }
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

private:
    std::optional<std::filesystem::path> savedHome;
    std::filesystem::path root;
};

void WriteTextFile(const std::filesystem::path &path, const std::string_view contents) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    REQUIRE(!error);
    std::ofstream output(path, std::ios::binary);
    output << contents;
    output.close();
    REQUIRE(output);
}

void WriteCachedPackage(const std::string_view packageNamespace, const std::string_view packageName,
                        const std::string_view version) {
    const auto ns = IdentitySegment::Parse(packageNamespace);
    const auto name = IdentitySegment::Parse(packageName);
    const auto semanticVersion = SemanticVersion::Parse(version);
    REQUIRE(ns.has_value());
    REQUIRE(name.has_value());
    REQUIRE(semanticVersion.has_value());
    WriteTextFile(Driver::RegistryPackageDir(*ns, *name, *semanticVersion) / "Rux.toml",
                  "[Manifest]\nVersion = 1\n\n[Package]\nNamespace = \"Rux\"\nName = \"Json\"\nVersion = \"1.2.0\"\n"
                  "Type = \"SourceLibrary\"\n");
}
} // namespace

TEST_CASE("list and info distinguish project dependency installation states") {
    const ScopedPackageCache cache;
    WriteCachedPackage("Rux", "Json", "1.2.0");
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = System::TempDirectory() / ("rux-cli-inspect-project-" + std::to_string(nonce));
    const auto manifestPath = root / "Rux.toml";
    WriteTextFile(manifestPath, R"([Manifest]
Version = 1

[Package]
Name = "InspectFixture"
Version = "0.1.0"
Type = "SourceLibrary"

[Dependencies]
Local = { Path = "../Local" }
Json = { Namespace = "Rux", Version = "^1.0.0" }
Missing = { Namespace = "Acme", Version = "2.0.0" }
)");
    const auto manifest = manifestPath.string();

    const auto listed = Run(std::array<std::string_view, 3>{"--manifest", manifest, "list"});
    REQUIRE(listed.exitCode == 0);
    CHECK(listed.output.contains("Project dependencies (3 dependencies):"));
    CHECK(listed.output.contains("Manifest: '" + manifest + "'"));
    CHECK(listed.output.contains("Path Local at '../Local'"));
    CHECK(listed.output.contains("Resolved Rux/Json @ ^1.0.0 to 1.2.0"));
    CHECK(listed.output.contains("Missing Acme/Missing @ 2.0.0"));
    CHECK(listed.output.contains("help: install it with 'rux install Acme/Missing@2.0.0'"));

    const auto info = Run(std::array<std::string_view, 3>{"--manifest", manifest, "info"});
    REQUIRE(info.exitCode == 0);
    CHECK(info.output.contains("Project package from '" + manifest + "':"));
    CHECK(info.output.contains("Package dependencies (3 dependencies):"));
    CHECK(info.output.contains("Resolved Rux/Json @ ^1.0.0 to 1.2.0"));
    CHECK(info.output.contains("Missing Acme/Missing @ 2.0.0"));

    const auto quiet = Run(std::array<std::string_view, 4>{"--quiet", "--manifest", manifest, "list"});
    CHECK(quiet.exitCode == 0);
    CHECK(quiet.output.empty());

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

TEST_CASE("list describes empty project and global collections") {
    const ScopedPackageCache cache;
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = System::TempDirectory() / ("rux-cli-empty-inspect-" + std::to_string(nonce));
    const auto manifestPath = root / "Rux.toml";
    WriteTextFile(manifestPath, R"([Manifest]
Version = 1

[Package]
Name = "EmptyFixture"
Version = "0.1.0"
Type = "SourceLibrary"
)");
    const auto manifest = manifestPath.string();

    const auto project = Run(std::array<std::string_view, 3>{"--manifest", manifest, "list"});
    CHECK(project.exitCode == 0);
    CHECK(project.output.contains("Empty project dependency list"));
    CHECK(project.output.contains("Manifest: '" + manifest + "'"));

    const auto global = Run(std::array<std::string_view, 2>{"list", "--global"});
    CHECK(global.exitCode == 0);
    CHECK(global.output.contains("Empty global package cache"));
    CHECK(global.output.contains("Cache: '"));

    std::error_code error;
    std::filesystem::remove_all(root, error);
}
