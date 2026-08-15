#include "Source/SourceLoader.h"
#include "System/Os.h"

#include <chrono>
#include <doctest.h>
#include <filesystem>
#include <fstream>
#include <string>

using namespace Rux;
using namespace Rux::System;

namespace {
class SourceFixture {
public:
    SourceFixture() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        root = TempDirectory() / ("rux-source-loader-test-" + std::to_string(nonce));
        REQUIRE(std::filesystem::create_directories(root));
    }

    ~SourceFixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    SourceFixture(const SourceFixture &) = delete;
    SourceFixture &operator=(const SourceFixture &) = delete;

    void Write(const std::filesystem::path &relativePath, const std::string &contents) const {
        const auto path = root / relativePath;
        std::filesystem::create_directories(path.parent_path());
        REQUIRE(std::filesystem::is_directory(path.parent_path()));
        std::ofstream output(path, std::ios::binary);
        REQUIRE(output.good());
        output << contents;
        REQUIRE(output.good());
    }

    std::filesystem::path root;
};
} // namespace

TEST_CASE("source loader reports a missing source directory as a diagnostic") {
    const SourceFixture fixture;
    const auto result = SourceLoader::Load(fixture.root);

    CHECK(result.files.empty());
    REQUIRE_EQ(result.diagnostics.size(), 1);
    CHECK(result.HasErrors());
    CHECK(result.diagnostics.front().IsError());
    CHECK(result.diagnostics.front().sourceName.empty());
    CHECK_EQ(result.diagnostics.front().location.line, 0);
    CHECK_EQ(result.diagnostics.front().location.column, 0);
    CHECK_EQ(result.diagnostics.front().location.offset, 0);
    CHECK_EQ(result.diagnostics.front().message,
             "source directory '" + (fixture.root / "Src").string() + "' does not exist");
    CHECK(result.diagnostics.front().help->contains("'.rux' file"));
}

TEST_CASE("source loader rejects a source path that is not a directory") {
    SourceFixture fixture;
    fixture.Write("Src", "not a directory\n");

    const auto result = SourceLoader::Load(fixture.root);
    CHECK(result.files.empty());
    REQUIRE_EQ(result.diagnostics.size(), 1);
    CHECK(result.HasErrors());
    CHECK_EQ(result.diagnostics.front().message,
             "source path '" + (fixture.root / "Src").string() + "' is not a directory");
    CHECK(result.diagnostics.front().help->contains("'Src' directory"));
}

TEST_CASE("source loader reports an empty source tree without failing") {
    SourceFixture fixture;
    REQUIRE(std::filesystem::create_directory(fixture.root / "Src"));

    const auto result = SourceLoader::Load(fixture.root);
    CHECK(result.files.empty());
    REQUIRE_EQ(result.diagnostics.size(), 1);
    CHECK_FALSE(result.HasErrors());
    CHECK_EQ(result.diagnostics.front().severity, Diagnostic::Severity::Warning);
    CHECK_EQ(result.diagnostics.front().message,
             "no *.rux files found under '" + (fixture.root / "Src").string() + "'");
}

TEST_CASE("source loader returns deterministic absolute source identities") {
    SourceFixture fixture;
    fixture.Write("Src/Zeta.rux", "zeta\n");
    fixture.Write("Src/Nested/Alpha.rux", "alpha\n");
    fixture.Write("Src/Ignored.txt", "ignored\n");

    const auto result = SourceLoader::Load(fixture.root);
    CHECK_FALSE(result.HasErrors());
    CHECK(result.diagnostics.empty());
    REQUIRE_EQ(result.files.size(), 2);
    CHECK_EQ(result.files[0].path, std::filesystem::absolute(fixture.root / "Src/Nested/Alpha.rux"));
    CHECK_EQ(result.files[0].source, "alpha\n");
    CHECK_EQ(result.files[1].path, std::filesystem::absolute(fixture.root / "Src/Zeta.rux"));
    CHECK_EQ(result.files[1].source, "zeta\n");
}
