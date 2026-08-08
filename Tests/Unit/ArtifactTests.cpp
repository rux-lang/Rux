#include "Package/Artifact.h"
#include "Package/Manifest.h"
#include "System/Os.h"

#include <chrono>
#include <doctest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using namespace Rux;
using namespace Rux::System;

namespace {

constexpr std::string_view publishableManifest = R"(#Comment
[Manifest]
Version = 1
MinRux = "0.4.0"

[Package]
Namespace = "Acme"
Name = "Widget"
Version = "1.2.3+native"
Type = "Source"
)";

// Owns a scratch package tree so each case builds from a clean directory.
class ArtifactFixture {
public:
    ArtifactFixture() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        root = TempDirectory() / ("rux-artifact-test-" + std::to_string(nonce));
        std::filesystem::create_directories(root);
    }

    ~ArtifactFixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    ArtifactFixture(const ArtifactFixture &) = delete;
    ArtifactFixture &operator=(const ArtifactFixture &) = delete;

    void Write(const std::string &relative, const std::string_view content) const {
        const auto path = root / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary);
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    /// Write a manifest plus one source, the minimum a publishable package needs.
    void WriteMinimalPackage(const std::string_view manifest = publishableManifest) const {
        Write("Rux.toml", manifest);
        Write("Src/Widget.rux", "module Widget;\n");
    }

    [[nodiscard]] std::filesystem::path ManifestPath() const {
        return root / "Rux.toml";
    }

    [[nodiscard]] std::expected<PackageArtifact, std::string> Build() const {
        const auto result = Manifest::Load(ManifestPath());
        REQUIRE(result.Ok());
        return BuildPackageArtifact(ManifestPath(), *result.manifest);
    }

private:
    std::filesystem::path root;
};

// Read the entry names out of a ZIP central directory. Deliberately independent
// of the writer so a bug in one is not mirrored by the other.
std::vector<std::string> ArchiveEntryNames(const std::string &archive) {
    std::vector<std::string> names;
    const std::string_view signature("\x50\x4B\x01\x02", 4);
    std::size_t pos = 0;
    while ((pos = archive.find(signature, pos)) != std::string::npos) {
        const auto byteAt = [&archive](const std::size_t index) { return static_cast<unsigned char>(archive[index]); };
        const std::size_t nameLength = byteAt(pos + 28) | (byteAt(pos + 29) << 8);
        names.emplace_back(archive.substr(pos + 46, nameLength));
        pos += 46 + nameLength;
    }
    return names;
}

} // namespace

TEST_CASE("an archive stores the manifest at its root with the uploaded bytes") {
    const ArtifactFixture fixture;
    fixture.WriteMinimalPackage();

    const auto artifact = fixture.Build();
    REQUIRE(artifact.has_value());

    // Publication uploads manifestSource beside the archive and the registry
    // rejects the pair unless the embedded copy matches it exactly.
    CHECK(artifact->manifestSource == std::string(publishableManifest));
    CHECK(artifact->sourceFileCount == 1);
    CHECK(artifact->fileCount == 2);

    const auto names = ArchiveEntryNames(artifact->archive);
    REQUIRE(names.size() == 2);
    CHECK(names[0] == "Rux.toml");
    CHECK(names[1] == "Src/Widget.rux");
}

TEST_CASE("nested sources use forward slashes on every platform") {
    const ArtifactFixture fixture;
    fixture.WriteMinimalPackage();
    fixture.Write("Src/Nested/Deep/Other.rux", "module Other;\n");

    const auto artifact = fixture.Build();
    REQUIRE(artifact.has_value());
    CHECK(artifact->sourceFileCount == 2);

    const auto names = ArchiveEntryNames(artifact->archive);
    REQUIRE(names.size() == 3);
    CHECK(names[1] == "Src/Nested/Deep/Other.rux");
    for (const auto &name : names) {
        CHECK(name.find('\\') == std::string::npos);
    }
}

TEST_CASE("packing the same tree twice produces identical bytes") {
    const ArtifactFixture fixture;
    fixture.WriteMinimalPackage();
    fixture.Write("Src/Second.rux", "module Second;\n");
    fixture.Write("Src/First.rux", "module First;\n");

    const auto first = fixture.Build();
    const auto second = fixture.Build();
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(first->archive == second->archive);

    // Reproducibility depends on a stable entry order, not on the order the
    // filesystem happened to hand the files back.
    const auto names = ArchiveEntryNames(first->archive);
    CHECK(std::ranges::is_sorted(names));
}

TEST_CASE("the referenced readme is packed and license text is not") {
    const ArtifactFixture fixture;
    fixture.WriteMinimalPackage(R"([Manifest]
Version = 1
MinRux = "0.4.0"

[Package]
Namespace = "Acme"
Name = "Widget"
Version = "1.0.0"
Type = "Source"
License = "MIT"
LicenseUrl = "https://example.com/LICENSE.md"
Readme = "Docs/README.md"
)");
    fixture.Write("Docs/README.md", "# Widget\n");
    fixture.Write("LICENSE.md", "MIT\n");

    const auto artifact = fixture.Build();
    REQUIRE(artifact.has_value());

    // LicenseUrl names the terms rather than shipping them, so a license file
    // sitting beside the manifest is an ordinary unreferenced file.
    const auto names = ArchiveEntryNames(artifact->archive);
    REQUIRE(names.size() == 3);
    CHECK(std::ranges::contains(names, "Docs/README.md"));
    CHECK_FALSE(std::ranges::contains(names, "LICENSE.md"));
}

TEST_CASE("a declared readme that does not exist is rejected") {
    const ArtifactFixture fixture;
    fixture.WriteMinimalPackage(R"([Manifest]
Version = 1
MinRux = "0.4.0"

[Package]
Namespace = "Acme"
Name = "Widget"
Version = "1.0.0"
Type = "Source"
Readme = "README.md"
)");

    const auto artifact = fixture.Build();
    REQUIRE_FALSE(artifact.has_value());
    CHECK(artifact.error().contains("README.md"));
}

TEST_CASE("a package without a Rux source is rejected") {
    const ArtifactFixture fixture;
    fixture.Write("Rux.toml", publishableManifest);
    fixture.Write("Src/Notes.txt", "not a source\n");

    const auto artifact = fixture.Build();
    REQUIRE_FALSE(artifact.has_value());
    CHECK(artifact.error().contains("Src/**/*.rux"));
}

TEST_CASE("a package without a Src directory is rejected") {
    const ArtifactFixture fixture;
    fixture.Write("Rux.toml", publishableManifest);

    const auto artifact = fixture.Build();
    REQUIRE_FALSE(artifact.has_value());
    CHECK(artifact.error().contains("Src"));
}

TEST_CASE("a source that is not valid UTF-8 is rejected") {
    const ArtifactFixture fixture;
    fixture.WriteMinimalPackage();
    fixture.Write("Src/Broken.rux", std::string_view("\xFF\xFE bad\n", 8));

    const auto artifact = fixture.Build();
    REQUIRE_FALSE(artifact.has_value());
    CHECK(artifact.error().contains("UTF-8"));
}

TEST_CASE("the default archive name carries the display identity and exact version") {
    const ArtifactFixture fixture;
    fixture.WriteMinimalPackage();

    const auto result = Manifest::Load(fixture.ManifestPath());
    REQUIRE(result.Ok());

    // Build metadata is part of publication identity, so it survives into the name.
    CHECK(ArtifactFileName(*result.manifest) == "Widget-1.2.3+native.ruxpkg");
}
