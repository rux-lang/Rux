#include "CompilerDriverTestSupport.h"
#include "Driver/BuildCache.h"

using namespace Rux;
using namespace Rux::Testing::CompilerDriverTestSupport;

namespace {
class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        path = TempDirectory() / ("rux-build-record-test-" + std::to_string(nonce));
        std::filesystem::create_directories(path / "Src");
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    TemporaryDirectory(const TemporaryDirectory &) = delete;
    TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

    std::filesystem::path path;
};

bool WriteFile(const std::filesystem::path &path, const std::string_view contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
    return output.good();
}

void Touch(const std::filesystem::path &path) {
    std::filesystem::last_write_time(path, std::filesystem::last_write_time(path) + std::chrono::seconds(5));
}
} // namespace

TEST_CASE("build records round-trip and notice changed inputs, listings and artifacts") {
    TemporaryDirectory directory;
    const auto source = directory.path / "Src" / "Main.rux";
    const auto artifact = directory.path / "App.exe";
    REQUIRE(WriteFile(source, "func Main() -> int { return 0; }\n"));
    REQUIRE(WriteFile(artifact, "binary"));

    BuildRecord record;
    record.key = "key with spaces";
    record.stats.localFiles = 1;
    record.stats.localLines = 1;
    record.stats.executableSize = 6;
    record.inputs.push_back(*RecordFile(source));
    record.directories.push_back({directory.path / "Src", DirectoryListing(directory.path / "Src", true), true});
    auto artifactRecord = *RecordFile(artifact);
    artifactRecord.hash.clear();
    record.artifacts.push_back(artifactRecord);

    const auto recordPath = directory.path / "Temp" / "Cache" / "Debug.record";
    REQUIRE(WriteBuildRecord(recordPath, record));
    const auto read = ReadBuildRecord(recordPath);
    REQUIRE(read);
    CHECK(read->key == record.key);
    CHECK(read->stats.localFiles == 1);
    CHECK(read->stats.executableSize == 6);
    REQUIRE(read->inputs.size() == 1);
    CHECK(read->inputs[0].path == source);
    CHECK(read->inputs[0].hash == record.inputs[0].hash);
    CHECK(IsUpToDate(*read));

    SUBCASE("a touched but unchanged source is still up to date") {
        Touch(source);
        CHECK(IsUpToDate(*read));
    }
    SUBCASE("an edited source is not") {
        REQUIRE(WriteFile(source, "func Main() -> int { return 1; }\n"));
        Touch(source);
        CHECK_FALSE(IsUpToDate(*read));
    }
    SUBCASE("a new source file is not") {
        REQUIRE(WriteFile(directory.path / "Src" / "Other.rux", "\n"));
        CHECK_FALSE(IsUpToDate(*read));
    }
    SUBCASE("a replaced or missing artifact is not") {
        REQUIRE(WriteFile(artifact, "rebuilt binary"));
        CHECK_FALSE(IsUpToDate(*read));
        std::filesystem::remove(artifact);
        CHECK_FALSE(IsUpToDate(*read));
    }
    SUBCASE("a damaged record is not read") {
        REQUIRE(WriteFile(recordPath, "rux-build-record 1\nmystery line\n"));
        CHECK_FALSE(ReadBuildRecord(recordPath));
        REQUIRE(WriteFile(recordPath, "something else\n"));
        CHECK_FALSE(ReadBuildRecord(recordPath));
    }
}

TEST_CASE("compiler driver reuses an up-to-date build and rebuilds after an edit") {
    DependencyFixture fixture;
    std::vector<Diagnostic> diagnostics;

    const auto first = CompilerDriver(fixture.Options(false, diagnostics)).Compile();
    REQUIRE(first.ok);
    CHECK_FALSE(first.stats.upToDate);

    const auto second = CompilerDriver(fixture.Options(false, diagnostics)).Compile();
    REQUIRE(second.ok);
    CHECK(second.stats.upToDate);
    CHECK(second.primaryArtifactPath == first.primaryArtifactPath);
    CHECK(second.stats.dependencyFiles == first.stats.dependencyFiles);

    // A dependency's source is as much an input as the package's own.
    // The edit changes the file's size, so it is noticed even where modification times are coarse.
    fixture.SetDependencySource(R"(
pub module Api {
    pub func Answer() -> int {
        return 4242;
    }
}
)");
    const auto edited = CompilerDriver(fixture.Options(false, diagnostics)).Compile();
    REQUIRE(edited.ok);
    CHECK_FALSE(edited.stats.upToDate);

    auto uncached = fixture.Options(false, diagnostics);
    uncached.useBuildCache = false;
    const auto forced = CompilerDriver(uncached).Compile();
    REQUIRE(forced.ok);
    CHECK_FALSE(forced.stats.upToDate);

    // Options that change the artifact are part of the request.
    auto release = fixture.Options(false, diagnostics);
    release.profile = BuildProfile::Release;
    const auto releaseBuild = CompilerDriver(release).Compile();
    REQUIRE(releaseBuild.ok);
    CHECK_FALSE(releaseBuild.stats.upToDate);
    CHECK(CompilerDriver(release).Compile().stats.upToDate);
    CHECK(diagnostics.empty());
}
