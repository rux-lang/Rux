#include "Driver/CompilerDriver.h"
#include "System/Os.h"
#include "Target/TargetTriple.h"

#include <array>
#include <chrono>
#include <doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using namespace Rux;
using namespace Rux::Driver;
using namespace Rux::System;

namespace {
constexpr std::array TargetNames = {
    "freebsd-x86_64", "freebsd-aarch64", "linux-x86_64",   "linux-aarch64",
    "macos-x86_64",   "macos-aarch64",   "windows-x86_64", "windows-aarch64",
};

struct ControlledBuild {
    CompileResult result;
    std::string rcuDump;
    std::size_t rcuObjectSize = 0;
};

class OptimizerArtifactFixture {
public:
    OptimizerArtifactFixture() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        root = TempDirectory() / ("rux-optimizer-regression-" + std::to_string(nonce));
        std::filesystem::create_directories(root / "Src");

        manifest.package.name = *IdentitySegment::Parse("OptimizerFixture");
        manifest.package.version = *SemanticVersion::Parse("0.1.0");
        manifest.package.type = ManifestPackageType::Executable;
        REQUIRE(manifest.Save(root / "Rux.toml"));

        REQUIRE(WriteFile(root / "Src" / "Main.rux", R"(
import OptimizerFixture::Support::LiveCrossModule;

pub func PublicApi(value: int) -> int {
    return PrivateLibraryHelper(value);
}

func PrivateLibraryHelper(value: int) -> int {
    return value + 1;
}

func UnusedProgramCode(value: int) -> int {
    let first = value * 3;
    let second = first + 17;
    let third = second * second;
    return third - first;
}

func Main() -> int {
    return LiveCrossModule(41) - 42;
}
)"));
        REQUIRE(WriteFile(root / "Src" / "Support.rux", R"(
module OptimizerFixture::Support {
    pub func LiveCrossModule(value: int) -> int {
        return PrivateCrossModule(value);
    }

    func PrivateCrossModule(value: int) -> int {
        return value + 1;
    }

    func UnusedSupportCode(value: int) -> int {
        let first = value * 5;
        let second = first + 23;
        let third = second * second;
        return third - first;
    }
}
)"));
    }

    ~OptimizerArtifactFixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    OptimizerArtifactFixture(const OptimizerArtifactFixture &) = delete;
    OptimizerArtifactFixture &operator=(const OptimizerArtifactFixture &) = delete;

    ControlledBuild Build(const std::string_view targetName, const BuildProfile profile,
                          const ManifestPackageType packageType) {
        manifest.package.type = packageType;
        REQUIRE(manifest.Save(root / "Rux.toml"));

        std::error_code cleanupError;
        std::filesystem::remove_all(root / "Temp", cleanupError);

        std::vector<Diagnostic> diagnostics;
        CompileOptions options;
        options.manifestPath = root / "Rux.toml";
        options.manifest = manifest;
        options.target = *Target::TargetTriple::Parse(targetName);
        options.profile = profile;
        options.quiet = true;
        options.dumpRcu = true;
        options.emitDiagnostic = [&](const Diagnostic &diagnostic) { diagnostics.push_back(diagnostic); };
        options.emitError = [&](const std::string_view message) {
            diagnostics.push_back(ErrorDiagnostic(std::string(message)));
        };

        ControlledBuild build;
        build.result = CompilerDriver(std::move(options)).Compile();
        CAPTURE(targetName);
        CAPTURE(profile == BuildProfile::Debug ? "Debug" : "Release");
        CAPTURE(diagnostics.size());
        if (!diagnostics.empty()) {
            CAPTURE(diagnostics.front().message);
        }
        REQUIRE(build.result.ok);
        REQUIRE(diagnostics.empty());
        REQUIRE(std::filesystem::is_regular_file(build.result.primaryArtifactPath));

        const auto dumpRoot = root / "Temp" / "Rcu";
        for (const auto &entry : std::filesystem::directory_iterator(dumpRoot)) {
            if (entry.is_regular_file()) {
                build.rcuDump += ReadFile(entry.path());
            }
        }

        const auto objectRoot = root / "Temp" / "Obj";
        for (const auto &entry : std::filesystem::directory_iterator(objectRoot)) {
            if (entry.is_regular_file()) {
                build.rcuObjectSize += static_cast<std::size_t>(entry.file_size());
            }
        }
        return build;
    }

    [[nodiscard]] static std::string ReadFile(const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

private:
    static bool WriteFile(const std::filesystem::path &path, const std::string_view contents) {
        std::ofstream output(path);
        output << contents;
        return output.good();
    }

    std::filesystem::path root;
    Manifest manifest;
};
} // namespace

TEST_CASE("controlled Release artifacts prune private code through every backend and image path") {
    OptimizerArtifactFixture fixture;

    for (const std::string_view targetName : TargetNames) {
        CAPTURE(targetName);
        const auto debug = fixture.Build(targetName, BuildProfile::Debug, ManifestPackageType::Executable);
        const auto release = fixture.Build(targetName, BuildProfile::Release, ManifestPackageType::Executable);

        CHECK(debug.rcuDump.contains("UnusedProgramCode"));
        CHECK(debug.rcuDump.contains("UnusedSupportCode"));
        CHECK_FALSE(release.rcuDump.contains("UnusedProgramCode"));
        CHECK_FALSE(release.rcuDump.contains("UnusedSupportCode"));
        CHECK(release.rcuDump.contains("Main"));
        CHECK(release.rcuDump.contains("LiveCrossModule"));
        CHECK(release.rcuDump.contains("PrivateCrossModule"));
        CHECK(release.result.stats.prunedFunctionDefinitions >= 4);

        CHECK(release.rcuObjectSize < debug.rcuObjectSize);
        CHECK_FALSE(
            OptimizerArtifactFixture::ReadFile(release.result.primaryArtifactPath).contains("UnusedProgramCode"));

        const bool imageLayoutExposesTheReduction =
            !targetName.starts_with("macos-") && (targetName.ends_with("x86_64") || targetName.starts_with("windows-"));
        if (imageLayoutExposesTheReduction) {
            CHECK(release.result.stats.executableSize < debug.result.stats.executableSize);
        }
        else {
            CHECK(release.result.stats.executableSize <= debug.result.stats.executableSize);
        }
    }
}

TEST_CASE("controlled Release libraries retain public APIs and prune private declarations") {
    OptimizerArtifactFixture fixture;

    for (const auto packageType : {ManifestPackageType::SharedLibrary, ManifestPackageType::StaticLibrary}) {
        for (const std::string_view targetName : TargetNames) {
            CAPTURE(targetName);
            CAPTURE(packageType == ManifestPackageType::SharedLibrary ? "SharedLibrary" : "StaticLibrary");
            const auto release = fixture.Build(targetName, BuildProfile::Release, packageType);

            CHECK(release.rcuDump.contains("PublicApi"));
            CHECK(release.rcuDump.contains("PrivateLibraryHelper"));
            CHECK_FALSE(release.rcuDump.contains("UnusedProgramCode"));
            CHECK_FALSE(release.rcuDump.contains("UnusedSupportCode"));
            CHECK(release.result.stats.prunedFunctionDefinitions > 0);

            const auto artifact = OptimizerArtifactFixture::ReadFile(release.result.primaryArtifactPath);
            CHECK(artifact.contains("PublicApi"));
            CHECK_FALSE(artifact.contains("UnusedProgramCode"));
        }
    }
}
