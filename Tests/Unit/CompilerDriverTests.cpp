#include "Driver/BuildTarget.h"
#include "Driver/CompilerDriver.h"

#include <chrono>
#include <doctest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "System/Os.h"

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

        std::filesystem::create_directories(appRoot / "Src");
        std::filesystem::create_directories(depRoot / "Src");

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
    Manifest application;
    Manifest dependency;
};

} // namespace

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
