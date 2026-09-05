#pragma once

#include "Driver/BuildStats.h"
#include "Driver/BuildTarget.h"
#include "Driver/CompilerDriver.h"
#include "ElfReader.h"
#include "MachOReader.h"
#include "System/Os.h"
#include "System/Process.h"
#include "Target/Target.h"

#include <array>
#include <chrono>
#include <doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace Rux::Testing::CompilerDriverTestSupport {
using namespace Rux;
using namespace Rux::Driver;
using namespace Rux::System;

class DependencyFixture {
public:
    DependencyFixture() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        root = TempDirectory() / ("rux-dependency-test-" + std::to_string(nonce));
        appRoot = root / "App";
        depRoot = root / "Dependency";
        nonTargetRoot = root / "WindowsOnly";
        transitiveRoot = root / "Transitive";
        secondDependencyRoot = root / "SecondDependency";

        std::filesystem::create_directories(appRoot / "Src");
        std::filesystem::create_directories(depRoot / "Src");
        std::filesystem::create_directories(nonTargetRoot / "Src");
        std::filesystem::create_directories(transitiveRoot / "Src");
        std::filesystem::create_directories(secondDependencyRoot / "Src");

        dependency.package.name = *IdentitySegment::Parse("Dependency");
        dependency.package.version = *SemanticVersion::Parse("0.1.0");
        dependency.package.type = ManifestPackageType::SourceLibrary;
        REQUIRE(dependency.Save(depRoot / "Rux.toml"));
        REQUIRE(WriteFile(depRoot / "Src" / "Api.rux", R"(
pub module Api {
    pub func Answer() -> int {
        return 42;
    }
}
)"));

        application.package.name = *IdentitySegment::Parse("App");
        application.package.version = *SemanticVersion::Parse("0.1.0");
        application.package.type = ManifestPackageType::Executable;
        REQUIRE(application.AddPathDependency(*IdentitySegment::Parse("Dependency"), "../Dependency"));
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

    void SetDependencySource(const std::string_view source) const {
        REQUIRE(WriteFile(depRoot / "Src" / "Api.rux", source));
    }

    void SetApplicationType(const ManifestPackageType type) {
        application.package.type = type;
    }

    void SetManifestDefine(std::string name, std::string value) {
        application.build.defines[std::move(name)] = DefineValue{DefineValue::Kind::String, std::move(value)};
    }

    void SetDependencyTargets(std::vector<Target::OS> targetOS) {
        application.dependencies.front().targetOS = std::move(targetOS);
    }

    void ConfigureSameNamedGenericDependencies() {
        REQUIRE(WriteFile(depRoot / "Src" / "Api.rux", R"(
pub module Api {
    pub func Identity<T>(value: T) -> T {
        return value;
    }
}
)"));

        Manifest secondDependency;
        secondDependency.package.name = *IdentitySegment::Parse("SecondDependency");
        secondDependency.package.version = *SemanticVersion::Parse("0.1.0");
        secondDependency.package.type = ManifestPackageType::SourceLibrary;
        REQUIRE(secondDependency.Save(secondDependencyRoot / "Rux.toml"));
        REQUIRE(WriteFile(secondDependencyRoot / "Src" / "Api.rux", R"(
pub module Api {
    pub func Identity<T>(value: T) -> T {
        return value;
    }
}
)"));
        REQUIRE(application.AddPathDependency(*IdentitySegment::Parse("SecondDependency"), "../SecondDependency"));
        SetApplicationSource(R"(
import Dependency::Api::Identity;

func Main() -> int {
    return Identity<int>(42);
}
)");
    }

    void ConfigureMacOSTargetDependencies() {
        application.dependencies.front().targetOS = {Target::OS::MacOS};

        Manifest nonTarget;
        nonTarget.package.name = *IdentitySegment::Parse("WindowsOnly");
        nonTarget.package.version = *SemanticVersion::Parse("0.1.0");
        nonTarget.package.type = ManifestPackageType::SourceLibrary;
        REQUIRE(nonTarget.Save(nonTargetRoot / "Rux.toml"));
        REQUIRE(WriteFile(nonTargetRoot / "Src" / "Api.rux", R"(
pub module Api {
    pub func Answer() -> int {
        return 7;
    }
}
)"));
        REQUIRE(application.AddPathDependency(*IdentitySegment::Parse("WindowsOnly"), "../WindowsOnly"));
        application.dependencies.back().targetOS = {Target::OS::Windows};

        SetApplicationSource(R"(
import Dependency::Api::Answer;

pub func SelectedAnswer() -> int {
    return Answer();
}

func Main() -> int {
    return SelectedAnswer();
}
)");
    }

    void ConfigureFreeBSDTargetDependencies() {
        application.dependencies.front().targetOS = {Target::OS::FreeBSD};

        Manifest nonTarget;
        nonTarget.package.name = *IdentitySegment::Parse("WindowsOnly");
        nonTarget.package.version = *SemanticVersion::Parse("0.1.0");
        nonTarget.package.type = ManifestPackageType::SourceLibrary;
        REQUIRE(nonTarget.Save(nonTargetRoot / "Rux.toml"));
        REQUIRE(WriteFile(nonTargetRoot / "Src" / "Api.rux", R"(
pub module Api {
    pub func Answer() -> int {
        return MissingWindowsImplementation;
    }
}
)"));
        REQUIRE(application.AddPathDependency(*IdentitySegment::Parse("WindowsOnly"), "../WindowsOnly"));
        application.dependencies.back().targetOS = {Target::OS::Windows};

        SetApplicationSource(R"(
import Dependency::Api::Answer;

#Link("libc.so.7")
extern func puts(str: *char8) -> int32;

pub func SelectedAnswer() -> int {
    puts("FreeBSD AArch64 driver test".data);
    return Answer();
}

func Main() -> int {
    return SelectedAnswer();
}
)");
    }

    void UseRegistryDeclaredTransitiveDependency() {
        Manifest transitive;
        transitive.package.name = *IdentitySegment::Parse("Transitive");
        transitive.package.version = *SemanticVersion::Parse("0.1.0");
        transitive.package.type = ManifestPackageType::SourceLibrary;
        REQUIRE(transitive.Save(transitiveRoot / "Rux.toml"));
        REQUIRE(WriteFile(transitiveRoot / "Src" / "Api.rux", R"(
pub module Api {
    pub func Value() -> int {
        return 42;
    }
}
)"));

        REQUIRE(dependency.AddRegistryDependency(*IdentitySegment::Parse("Transitive"), *IdentitySegment::Parse("Rux"),
                                                 *VersionRange::Parse("*")));
        dependency.dependencies.back().package = *IdentitySegment::Parse("transitive");
        REQUIRE(dependency.Save(depRoot / "Rux.toml"));
        REQUIRE(WriteFile(depRoot / "Src" / "Api.rux", R"(
import Transitive::Api::Value;

pub module Api {
    pub func Answer() -> int {
        return Value();
    }
}
)"));
    }

    void ConfigureLocalWorkspace(CompileOptions &options) const {
        options.localPackageRoots.emplace("transitive", transitiveRoot);
        options.localDependenciesOnly = true;
    }

    [[nodiscard]] CompileOptions Options(const bool checkOnly, std::vector<Diagnostic> &diagnostics) const {
        CompileOptions options;
        options.manifestPath = appRoot / "Rux.toml";
        options.manifest = application;
        options.target = Target::TargetTriple::Host();
        options.checkOnly = checkOnly;
        options.emitDiagnostic = [&](const Diagnostic &diagnostic, const SourceLineLookup &) {
            diagnostics.push_back(diagnostic);
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
    std::filesystem::path nonTargetRoot;
    std::filesystem::path transitiveRoot;
    std::filesystem::path secondDependencyRoot;
    Manifest application;
    Manifest dependency;
};

inline std::vector<unsigned char> ReadBinaryFile(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

inline uint16_t Read16(const std::vector<unsigned char> &bytes, const std::size_t offset) {
    return static_cast<uint16_t>(bytes[offset]) | static_cast<uint16_t>(bytes[offset + 1] << 8U);
}

inline uint32_t Read32(const std::vector<unsigned char> &bytes, const std::size_t offset) {
    uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= static_cast<uint32_t>(bytes[offset + i]) << (i * 8U);
    }
    return value;
}

inline void CheckWindowsAArch64Pe(const std::filesystem::path &path) {
    const auto image = ReadBinaryFile(path);
    REQUIRE(image.size() >= 0x40);
    REQUIRE(image[0] == 'M');
    REQUIRE(image[1] == 'Z');
    const std::size_t peOffset = Read32(image, 0x3C);
    REQUIRE(peOffset + 6 <= image.size());
    CHECK(Read32(image, peOffset) == 0x00004550); // PE\0\0
    CHECK(Read16(image, peOffset + 4) == 0xAA64); // IMAGE_FILE_MACHINE_ARM64
}

struct ArchiveArchitectures {
    std::size_t objects = 0;
    std::size_t imports = 0;
};

inline ArchiveArchitectures InspectWindowsAArch64Archive(const std::filesystem::path &path) {
    const auto archive = ReadBinaryFile(path);
    if (archive.size() < 8 || std::string(archive.begin(), archive.begin() + 8) != "!<arch>\n") {
        return {};
    }

    ArchiveArchitectures architectures;
    for (std::size_t at = 8; at + 60 <= archive.size();) {
        std::string size;
        for (std::size_t i = 48; i < 58 && archive[at + i] != ' '; ++i) {
            size.push_back(static_cast<char>(archive[at + i]));
        }
        if (size.empty()) {
            break;
        }
        const auto memberSize = static_cast<std::size_t>(std::stoul(size));
        const std::size_t body = at + 60;
        if (body + memberSize > archive.size()) {
            break;
        }
        if (memberSize >= 2 && Read16(archive, body) == 0xAA64) {
            ++architectures.objects;
        }
        // A DLL import-library member has the short import header signature
        // followed by the machine at byte six.
        if (memberSize >= 8 && Read32(archive, body) == 0xFFFF0000 && Read16(archive, body + 6) == 0xAA64) {
            ++architectures.imports;
        }
        at = body + memberSize + (memberSize & 1U);
    }
    return architectures;
}

struct MachOArchiveContents {
    std::size_t objects = 0;
    std::size_t relocations = 0;
};

inline MachOArchiveContents InspectMacOSAArch64Archive(const std::filesystem::path &path) {
    const auto archive = ReadBinaryFile(path);
    if (archive.size() < 8 || std::string(archive.begin(), archive.begin() + 8) != "!<arch>\n") {
        return {};
    }

    MachOArchiveContents contents;
    for (std::size_t at = 8; at + 60 <= archive.size();) {
        std::string size;
        for (std::size_t i = 48; i < 58 && archive[at + i] != ' '; ++i) {
            size.push_back(static_cast<char>(archive[at + i]));
        }
        if (size.empty()) {
            break;
        }
        const auto memberSize = static_cast<std::size_t>(std::stoul(size));
        const std::size_t body = at + 60;
        if (body + memberSize > archive.size()) {
            break;
        }
        if (memberSize >= 32 && Read32(archive, body) == 0xFEED'FACF && Read32(archive, body + 4) == 0x0100'000C &&
            Read32(archive, body + 12) == 1) {
            ++contents.objects;
            const std::uint32_t commandCount = Read32(archive, body + 16);
            std::size_t command = body + 32;
            for (std::uint32_t index = 0; index < commandCount && command + 8 <= body + memberSize; ++index) {
                const std::uint32_t commandSize = Read32(archive, command + 4);
                if (commandSize < 8 || commandSize > body + memberSize - command) {
                    break;
                }
                if (Read32(archive, command) == 0x19 && commandSize >= 72) { // LC_SEGMENT_64
                    const std::uint32_t sectionCount = Read32(archive, command + 64);
                    for (std::uint32_t section = 0; section < sectionCount; ++section) {
                        const std::size_t sectionHeader = command + 72 + static_cast<std::size_t>(section) * 80;
                        if (sectionHeader + 64 > command + commandSize) {
                            break;
                        }
                        contents.relocations += Read32(archive, sectionHeader + 60);
                    }
                }
                command += commandSize;
            }
        }
        at = body + memberSize + (memberSize & 1U);
    }
    return contents;
}

inline Testing::MachOImage ReadMacOSAArch64Image(const std::filesystem::path &path) {
    const auto bytes = ReadBinaryFile(path);
    Testing::MachOImage image;
    std::string error;
    const bool parsed = Testing::ReadMachO64(bytes, image, error);
    CAPTURE(error);
    REQUIRE(parsed);
    CHECK(image.Architecture() == Testing::MachOArchitecture::AArch64);
    REQUIRE(image.buildVersion);
    CHECK(image.buildVersion->platform == 1);         // PLATFORM_MACOS
    CHECK(image.buildVersion->minimumOs == 0x1A0000); // macOS 26.0
    REQUIRE(image.codeSignature);
    REQUIRE(image.codeDirectory);
    CHECK(image.codeSignature->offset + image.codeSignature->size == bytes.size());
    CHECK(image.codeDirectory->codeLimit == image.codeSignature->offset);
    return image;
}

inline Testing::ElfImage ReadFreeBSDAArch64Image(const std::filesystem::path &path) {
    Testing::ElfImage image{ReadBinaryFile(path)};
    REQUIRE(image.bytes.size() >= 64);
    CHECK(image.bytes[0] == 0x7F);
    CHECK(image.bytes[1] == 'E');
    CHECK(image.bytes[4] == 2); // ELFCLASS64
    CHECK(image.OsAbi() == 9);  // ELFOSABI_FREEBSD
    CHECK(image.Machine() == 183);
    return image;
}

inline std::size_t InspectFreeBSDAArch64Archive(const std::filesystem::path &path) {
    const auto archive = ReadBinaryFile(path);
    if (archive.size() < 8 || std::string(archive.begin(), archive.begin() + 8) != "!<arch>\n") {
        return 0;
    }

    std::size_t objects = 0;
    for (std::size_t at = 8; at + 60 <= archive.size();) {
        std::string size;
        for (std::size_t i = 48; i < 58 && archive[at + i] != ' '; ++i) {
            size.push_back(static_cast<char>(archive[at + i]));
        }
        if (size.empty()) {
            break;
        }
        const auto memberSize = static_cast<std::size_t>(std::stoul(size));
        const std::size_t body = at + 60;
        if (body + memberSize > archive.size()) {
            break;
        }
        if (memberSize >= 20 && archive[body] == 0x7F && archive[body + 1] == 'E') {
            CHECK(archive[body + 7] == 9);            // ELFOSABI_FREEBSD
            CHECK(Read16(archive, body + 16) == 1);   // ET_REL
            CHECK(Read16(archive, body + 18) == 183); // EM_AARCH64
            ++objects;
        }
        at = body + memberSize + (memberSize & 1U);
    }
    return objects;
}

// The "<OS>/<Arch>" tail every ordinary artifact directory ends with, so a test
// can compare it against TargetOutputPath instead of spelling the components.
inline std::filesystem::path ArtifactTargetPath(const std::filesystem::path &artifact) {
    const auto directory = artifact.parent_path();
    return directory.parent_path().filename() / directory.filename();
}

// The profile component directly above those two.
inline std::filesystem::path ArtifactProfile(const std::filesystem::path &artifact) {
    return artifact.parent_path().parent_path().parent_path().filename();
}

} // namespace Rux::Testing::CompilerDriverTestSupport
