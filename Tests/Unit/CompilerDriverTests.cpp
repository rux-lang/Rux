#include "Driver/BuildTarget.h"
#include "Driver/CompilerDriver.h"
#include "System/Os.h"

#include <chrono>
#include <doctest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

using namespace Rux;
using namespace Rux::Driver;
using namespace Rux::System;

namespace {

class DependencyFixture {
public:
    DependencyFixture() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        root = TempDirectory() / ("rux-dependency-test-" + std::to_string(nonce));
        appRoot = root / "App";
        depRoot = root / "Dependency";
        transitiveRoot = root / "Transitive";

        std::filesystem::create_directories(appRoot / "Src");
        std::filesystem::create_directories(depRoot / "Src");
        std::filesystem::create_directories(transitiveRoot / "Src");

        dependency.package.name = "Dependency";
        dependency.package.type = "source";
        REQUIRE(dependency.Save(depRoot / "Rux.toml"));
        REQUIRE(WriteFile(depRoot / "Src" / "Api.rux", R"(
module Api {
    pub func Answer() -> int {
        return 42;
    }
}
)"));

        application.package.name = "App";
        application.package.type = "bin";
        application.dependencies.push_back({"Dependency", {}, {}, "../Dependency"});
        REQUIRE(application.Save(appRoot / "Rux.toml"));
        REQUIRE(WriteFile(appRoot / "Src" / "Main.rux", R"(
import Dependency::Api::Answer;

func Main() -> int {
    return Answer();
}
)"));
    }

    ~DependencyFixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    DependencyFixture(const DependencyFixture &) = delete;
    DependencyFixture &operator=(const DependencyFixture &) = delete;

    void SetApplicationSource(const std::string_view source) const {
        REQUIRE(WriteFile(appRoot / "Src" / "Main.rux", source));
    }

    void SetManifestDefine(std::string name, std::string value) {
        application.build.defines[std::move(name)] = std::move(value);
    }

    void UseRegistryDeclaredTransitiveDependency() {
        Manifest transitive;
        transitive.package.name = "Transitive";
        transitive.package.type = "source";
        REQUIRE(transitive.Save(transitiveRoot / "Rux.toml"));
        REQUIRE(WriteFile(transitiveRoot / "Src" / "Api.rux", R"(
module Api {
    pub func Value() -> int {
        return 42;
    }
}
)"));

        dependency.dependencies.push_back({"Transitive", {}, "*", {}});
        REQUIRE(dependency.Save(depRoot / "Rux.toml"));
        REQUIRE(WriteFile(depRoot / "Src" / "Api.rux", R"(
import Transitive::Api::Value;

module Api {
    pub func Answer() -> int {
        return Value();
    }
}
)"));
    }

    void ConfigureLocalWorkspace(CompileOptions &options) const {
        options.localPackageRoots.emplace("Transitive", transitiveRoot);
        options.localDependenciesOnly = true;
    }

    [[nodiscard]] CompileOptions Options(const bool checkOnly, std::vector<Diagnostic> &diagnostics) const {
        CompileOptions options;
        options.manifestPath = appRoot / "Rux.toml";
        options.manifest = application;
        options.targetName = HostTargetTriple();
        options.profileName = "Debug";
        options.quiet = true;
        options.checkOnly = checkOnly;
        options.emitDiagnostic = [&](const Diagnostic &diagnostic) { diagnostics.push_back(diagnostic); };
        options.emitError = [&](const std::string_view message) {
            diagnostics.push_back(ErrorDiagnostic(std::string(message)));
        };
        return options;
    }

private:
    static bool WriteFile(const std::filesystem::path &path, const std::string_view contents) {
        std::ofstream output(path);
        output << contents;
        return output.good();
    }

    std::filesystem::path root;
    std::filesystem::path appRoot;
    std::filesystem::path depRoot;
    std::filesystem::path transitiveRoot;
    Manifest application;
    Manifest dependency;
};

} // namespace

TEST_CASE("test output directories omit the build profile") {
    Manifest manifest;
    manifest.build.output = "Artifacts";
    const std::filesystem::path root = "Workspace";

    CHECK(ResolveBuildOutputDir(root, manifest, "Release") == root / "Artifacts" / "Release");
    CHECK(ResolveBuildOutputDir(root, manifest, "Release", false) == root / "Artifacts");
}

TEST_CASE("compiler driver loads path dependencies when checking") {
    DependencyFixture fixture;
    std::vector<Diagnostic> diagnostics;

    const auto result = CompilerDriver(fixture.Options(true, diagnostics)).Compile();

    CHECK(result.ok);
    CHECK(diagnostics.empty());
    CHECK(result.stats.dependencyFiles == 1);
}

TEST_CASE("compiler driver loads path dependencies when building") {
    DependencyFixture fixture;
    std::vector<Diagnostic> diagnostics;

    const auto result = CompilerDriver(fixture.Options(false, diagnostics)).Compile();

    CHECK(result.ok);
    CHECK(diagnostics.empty());
    CHECK(result.stats.dependencyFiles == 1);
    CHECK(std::filesystem::is_regular_file(result.executablePath));
}

TEST_CASE("compiler driver resolves transitive dependencies from local workspace members") {
    DependencyFixture fixture;
    fixture.UseRegistryDeclaredTransitiveDependency();
    std::vector<Diagnostic> diagnostics;
    auto options = fixture.Options(true, diagnostics);
    fixture.ConfigureLocalWorkspace(options);

    const auto result = CompilerDriver(std::move(options)).Compile();

    CHECK(result.ok);
    CHECK(diagnostics.empty());
    CHECK(result.stats.dependencyFiles == 2);
}

TEST_CASE("compiler driver supplies manifest and command-line build context") {
    DependencyFixture fixture;
    fixture.SetManifestDefine("allocator", "system");
    fixture.SetApplicationSource(R"(
struct Build {
    timestamp: uint64;
    date: char8[];
    time: char8[];
}

intrinsic #build: Build;

struct Config {}
intrinsic #config: Config;

struct Compiler {}
intrinsic #compiler: Compiler;

enum BuildMode { Debug }

when #config.Has("allocator") &&
     #config.Get("allocator") == "mimalloc" &&
     #build.isTest &&
     #build.mode == BuildMode::Debug &&
     #compiler.HasFeature("namespaced-intrinsics") {
    func Main() -> int {
        let timestamp = #build.timestamp;
        let date = #build.date;
        let time = #build.time;
        return 0;
    }
} else {
    func Main() -> int {
        return MissingConfiguration;
    }
}
)");

    std::vector<Diagnostic> diagnostics;
    auto options = fixture.Options(false, diagnostics);
    options.defines["allocator"] = "mimalloc";
    options.isTest = true;

    const auto result = CompilerDriver(std::move(options)).Compile();

    CHECK(result.ok);
    CHECK(diagnostics.empty());
    CHECK(std::filesystem::is_regular_file(result.executablePath));
}
