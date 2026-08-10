#include "Package/Artifact.h"
#include "Package/Manifest.h"
#include "System/Os.h"

#include <algorithm>
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
Type = "SourceLibrary"
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

    /// A directory below the fixture, cleaned up with it, to extract into.
    [[nodiscard]] std::filesystem::path Scratch(const std::string &name) const {
        return root / name;
    }

    [[nodiscard]] std::string Read(const std::string &relative) const {
        std::ifstream input(root / relative, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
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

/// IEEE CRC-32, so a hand-built archive can carry a correct or a wrong one.
std::uint32_t ReferenceCrc32(const std::string_view data) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const unsigned char byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) != 0 ? 0xEDB88320U ^ (crc >> 1U) : crc >> 1U;
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

/// How a hand-built archive should deviate from a well-formed one.
struct ArchiveDefect {
    std::uint16_t method = 0; ///< 8 marks the entries Deflate without compressing them.
    bool corruptCrc = false;
};

/**
 * @brief Assemble a ZIP by hand so a case can choose exactly what is wrong.
 *
 * Writing this independently of Package/Artifact keeps a defect the writer
 * cannot produce -- a traversing path, a compressed entry, a bad checksum --
 * reachable from a test.
 */
std::string BuildArchive(const std::vector<std::pair<std::string, std::string>> &entries,
                         const ArchiveDefect defect = {}) {
    const auto put16 = [](std::string &out, const std::uint16_t value) {
        out.push_back(static_cast<char>(value & 0xFFU));
        out.push_back(static_cast<char>((value >> 8U) & 0xFFU));
    };
    const auto put32 = [](std::string &out, const std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            out.push_back(static_cast<char>((value >> shift) & 0xFFU));
        }
    };

    std::string archive;
    std::string directory;
    for (const auto &[path, data] : entries) {
        const auto offset = static_cast<std::uint32_t>(archive.size());
        const std::uint32_t crc = ReferenceCrc32(data) ^ (defect.corruptCrc ? 0xFFFFFFFFU : 0U);
        const auto size = static_cast<std::uint32_t>(data.size());
        const auto nameLength = static_cast<std::uint16_t>(path.size());

        put32(archive, 0x04034B50);
        put16(archive, 20);
        put16(archive, 0x0800);
        put16(archive, defect.method);
        put16(archive, 0);
        put16(archive, 0x0021);
        put32(archive, crc);
        put32(archive, size);
        put32(archive, size);
        put16(archive, nameLength);
        put16(archive, 0);
        archive.append(path);
        archive.append(data);

        put32(directory, 0x02014B50);
        put16(directory, 20);
        put16(directory, 20);
        put16(directory, 0x0800);
        put16(directory, defect.method);
        put16(directory, 0);
        put16(directory, 0x0021);
        put32(directory, crc);
        put32(directory, size);
        put32(directory, size);
        put16(directory, nameLength);
        put16(directory, 0);
        put16(directory, 0);
        put16(directory, 0);
        put16(directory, 0);
        put32(directory, 0);
        put32(directory, offset);
        directory.append(path);
    }

    const auto directoryOffset = static_cast<std::uint32_t>(archive.size());
    const auto entryCount = static_cast<std::uint16_t>(entries.size());
    archive.append(directory);
    put32(archive, 0x06054B50);
    put16(archive, 0);
    put16(archive, 0);
    put16(archive, entryCount);
    put16(archive, entryCount);
    put32(archive, static_cast<std::uint32_t>(directory.size()));
    put32(archive, directoryOffset);
    put16(archive, 0);
    return archive;
}

/// The entries a well-formed package archive must carry at minimum.
std::vector<std::pair<std::string, std::string>> MinimalEntries() {
    return {{"Rux.toml", std::string(publishableManifest)}, {"Src/Widget.rux", "module Widget;\n"}};
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

TEST_CASE("both referenced text files are packed") {
    const ArtifactFixture fixture;
    fixture.WriteMinimalPackage(R"([Manifest]
Version = 1
MinRux = "0.4.0"

[Package]
Namespace = "Acme"
Name = "Widget"
Version = "1.0.0"
Type = "SourceLibrary"
License = "MIT"
LicenseFile = "LICENSE.md"
ReadmeFile = "Docs/README.md"
)");
    fixture.Write("Docs/README.md", "# Widget\n");
    fixture.Write("LICENSE.md", "MIT\n");

    const auto artifact = fixture.Build();
    REQUIRE(artifact.has_value());

    const auto names = ArchiveEntryNames(artifact->archive);
    REQUIRE(names.size() == 4);
    CHECK(std::ranges::contains(names, "Docs/README.md"));
    CHECK(std::ranges::contains(names, "LICENSE.md"));
}

TEST_CASE("an unreferenced license file stays out of the archive") {
    const ArtifactFixture fixture;
    fixture.WriteMinimalPackage(R"([Manifest]
Version = 1
MinRux = "0.4.0"

[Package]
Namespace = "Acme"
Name = "Widget"
Version = "1.0.0"
Type = "SourceLibrary"
License = "MIT"
)");
    fixture.Write("LICENSE.md", "MIT\n");

    const auto artifact = fixture.Build();
    REQUIRE(artifact.has_value());

    // The SPDX expression names the terms without shipping them, so a license
    // file the manifest never references is an ordinary unreferenced file.
    const auto names = ArchiveEntryNames(artifact->archive);
    CHECK_FALSE(std::ranges::contains(names, "LICENSE.md"));
}

TEST_CASE("a declared license file that does not exist is rejected") {
    const ArtifactFixture fixture;
    fixture.WriteMinimalPackage(R"([Manifest]
Version = 1
MinRux = "0.4.0"

[Package]
Namespace = "Acme"
Name = "Widget"
Version = "1.0.0"
Type = "SourceLibrary"
LicenseFile = "LICENSE.md"
)");

    const auto artifact = fixture.Build();
    REQUIRE_FALSE(artifact.has_value());
    CHECK(artifact.error().contains("LICENSE.md"));
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
Type = "SourceLibrary"
ReadmeFile = "README.md"
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
    fixture.Write("Src/Broken.rux", std::string_view("\xFF\xFE bad\n", 7));

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

TEST_CASE("packing and extracting round-trips every entry byte for byte") {
    const ArtifactFixture fixture;
    fixture.WriteMinimalPackage();
    fixture.Write("Src/Nested/Deep.rux", "module Deep;\n");
    fixture.Write("README.md", "# Widget\n");

    const auto artifact = fixture.Build();
    REQUIRE(artifact.has_value());

    const auto dest = fixture.Scratch("extracted");
    const auto extracted = ExtractPackageArtifact(artifact->archive, dest);
    REQUIRE(extracted.has_value());
    CHECK(extracted->fileCount == artifact->fileCount);
    CHECK(extracted->sourceFileCount == artifact->sourceFileCount);

    const auto same = [&dest, &fixture](const std::string &relative) {
        std::ifstream input(dest / relative, std::ios::binary);
        const std::string actual((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        return actual == fixture.Read(relative);
    };
    CHECK(same("Rux.toml"));
    CHECK(same("Src/Widget.rux"));
    CHECK(same("Src/Nested/Deep.rux"));
}

TEST_CASE("extraction rebuilds the manifest bytes publication was checked against") {
    const ArtifactFixture fixture;
    fixture.WriteMinimalPackage();

    const auto artifact = fixture.Build();
    REQUIRE(artifact.has_value());

    const auto dest = fixture.Scratch("extracted");
    REQUIRE(ExtractPackageArtifact(artifact->archive, dest).has_value());

    std::ifstream input(dest / "Rux.toml", std::ios::binary);
    const std::string manifest((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    CHECK(manifest == artifact->manifestSource);
}

TEST_CASE("an archive without an end-of-central-directory record is rejected") {
    const ArtifactFixture fixture;
    const auto dest = fixture.Scratch("extracted");
    CHECK_FALSE(ExtractPackageArtifact("", dest).has_value());
    CHECK_FALSE(ExtractPackageArtifact("not an archive at all, but long enough to scan", dest).has_value());
}

TEST_CASE("a truncated archive is rejected rather than partly extracted") {
    const ArtifactFixture fixture;
    fixture.WriteMinimalPackage();
    const auto artifact = fixture.Build();
    REQUIRE(artifact.has_value());

    const auto dest = fixture.Scratch("extracted");
    const std::string truncated = artifact->archive.substr(0, artifact->archive.size() / 2);
    REQUIRE_FALSE(ExtractPackageArtifact(truncated, dest).has_value());
    CHECK_FALSE(std::filesystem::exists(dest / "Rux.toml"));
}

TEST_CASE("a compressed entry is rejected because package archives are Stored only") {
    const ArtifactFixture fixture;
    const auto archive = BuildArchive(MinimalEntries(), ArchiveDefect{.method = 8, .corruptCrc = false});
    const auto extracted = ExtractPackageArtifact(archive, fixture.Scratch("extracted"));
    REQUIRE_FALSE(extracted.has_value());
    CHECK(extracted.error().contains("compressed"));
}

TEST_CASE("an entry whose checksum does not match its bytes is rejected") {
    const ArtifactFixture fixture;
    const auto archive = BuildArchive(MinimalEntries(), ArchiveDefect{.method = 0, .corruptCrc = true});
    const auto extracted = ExtractPackageArtifact(archive, fixture.Scratch("extracted"));
    REQUIRE_FALSE(extracted.has_value());
    CHECK(extracted.error().contains("checksum"));
}

TEST_CASE("an entry path that escapes the destination is rejected") {
    const ArtifactFixture fixture;
    auto entries = MinimalEntries();
    entries.emplace_back("../Escaped.rux", "module Escaped;\n");

    const auto dest = fixture.Scratch("extracted");
    const auto extracted = ExtractPackageArtifact(BuildArchive(entries), dest);
    REQUIRE_FALSE(extracted.has_value());
    CHECK(extracted.error().contains(".."));
    CHECK_FALSE(std::filesystem::exists(dest.parent_path() / "Escaped.rux"));
}

TEST_CASE("an absolute or drive-qualified entry path is rejected") {
    const ArtifactFixture fixture;
    auto rooted = MinimalEntries();
    rooted.emplace_back("/etc/passwd", "root\n");
    CHECK_FALSE(ExtractPackageArtifact(BuildArchive(rooted), fixture.Scratch("a")).has_value());

    auto drive = MinimalEntries();
    drive.emplace_back("C:/Windows/System32/evil.dll", "MZ");
    CHECK_FALSE(ExtractPackageArtifact(BuildArchive(drive), fixture.Scratch("b")).has_value());

    auto backslash = MinimalEntries();
    backslash.emplace_back("Src\\Sneaky.rux", "module Sneaky;\n");
    CHECK_FALSE(ExtractPackageArtifact(BuildArchive(backslash), fixture.Scratch("c")).has_value());
}

TEST_CASE("an archive without a root manifest is rejected") {
    const ArtifactFixture fixture;
    const auto archive = BuildArchive({{"Src/Widget.rux", "module Widget;\n"}});
    const auto extracted = ExtractPackageArtifact(archive, fixture.Scratch("extracted"));
    REQUIRE_FALSE(extracted.has_value());
    CHECK(extracted.error().contains("Rux.toml"));
}

TEST_CASE("an archive without a Rux source is rejected") {
    const ArtifactFixture fixture;
    const auto archive = BuildArchive({{"Rux.toml", std::string(publishableManifest)}, {"README.md", "# Widget\n"}});
    const auto extracted = ExtractPackageArtifact(archive, fixture.Scratch("extracted"));
    REQUIRE_FALSE(extracted.has_value());
    CHECK(extracted.error().contains("Src"));
}

TEST_CASE("entries differing only in case are rejected as a collision") {
    const ArtifactFixture fixture;
    auto entries = MinimalEntries();
    entries.emplace_back("Src/widget.rux", "module Other;\n");
    const auto extracted = ExtractPackageArtifact(BuildArchive(entries), fixture.Scratch("extracted"));
    REQUIRE_FALSE(extracted.has_value());
    CHECK(extracted.error().contains("case"));
}

TEST_CASE("an archive past the publication size limit is rejected before it is read") {
    const ArtifactFixture fixture;
    auto entries = MinimalEntries();
    // Entries are Stored, so an archive is as large as its contents; several
    // files under the per-file cap carry it past the archive cap.
    for (int i = 0; i < 4; ++i) {
        entries.emplace_back("Src/Bulk" + std::to_string(i) + ".rux", std::string(artifactMaxFileBytes, 'a'));
    }
    const auto archive = BuildArchive(entries);
    REQUIRE(archive.size() > artifactMaxBytes);

    const auto dest = fixture.Scratch("extracted");
    const auto extracted = ExtractPackageArtifact(archive, dest);
    REQUIRE_FALSE(extracted.has_value());
    CHECK(extracted.error().contains("publication limit"));
    CHECK_FALSE(std::filesystem::exists(dest));
}

TEST_CASE("an entry larger than the per-file limit is rejected") {
    const ArtifactFixture fixture;
    auto entries = MinimalEntries();
    entries.emplace_back("Src/Huge.rux", std::string(artifactMaxFileBytes + 1, 'a'));
    const auto extracted = ExtractPackageArtifact(BuildArchive(entries), fixture.Scratch("extracted"));
    REQUIRE_FALSE(extracted.has_value());
    CHECK(extracted.error().contains("file limit"));
}

TEST_CASE("a directory entry is ignored rather than written as a file") {
    const ArtifactFixture fixture;
    auto entries = MinimalEntries();
    entries.emplace_back("Src/", "");

    const auto dest = fixture.Scratch("extracted");
    const auto extracted = ExtractPackageArtifact(BuildArchive(entries), dest);
    REQUIRE(extracted.has_value());
    CHECK(extracted->fileCount == 2);
    CHECK(std::filesystem::is_directory(dest / "Src"));
}
