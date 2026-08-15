#include "Driver/BuildReport.h"
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
        nonTargetRoot = root / "WindowsOnly";
        transitiveRoot = root / "Transitive";

        std::filesystem::create_directories(appRoot / "Src");
        std::filesystem::create_directories(depRoot / "Src");
        std::filesystem::create_directories(nonTargetRoot / "Src");
        std::filesystem::create_directories(transitiveRoot / "Src");

        dependency.package.name = *IdentitySegment::Parse("Dependency");
        dependency.package.version = *SemanticVersion::Parse("0.1.0");
        dependency.package.type = ManifestPackageType::SourceLibrary;
        REQUIRE(dependency.Save(depRoot / "Rux.toml"));
        REQUIRE(WriteFile(depRoot / "Src" / "Api.rux", R"(
module Api {
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

    void SetApplicationType(const ManifestPackageType type) {
        application.package.type = type;
    }

    void SetManifestDefine(std::string name, std::string value) {
        application.build.defines[std::move(name)] = DefineValue{DefineValue::Kind::String, std::move(value)};
    }

    void SetDependencyTargets(std::vector<Target::OS> targetOS) {
        application.dependencies.front().targetOS = std::move(targetOS);
    }

    void ConfigureMacOSTargetDependencies() {
        application.dependencies.front().targetOS = {Target::OS::MacOS};

        Manifest nonTarget;
        nonTarget.package.name = *IdentitySegment::Parse("WindowsOnly");
        nonTarget.package.version = *SemanticVersion::Parse("0.1.0");
        nonTarget.package.type = ManifestPackageType::SourceLibrary;
        REQUIRE(nonTarget.Save(nonTargetRoot / "Rux.toml"));
        REQUIRE(WriteFile(nonTargetRoot / "Src" / "Api.rux", R"(
module Api {
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
module Api {
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
module Api {
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

module Api {
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
    std::filesystem::path nonTargetRoot;
    std::filesystem::path transitiveRoot;
    Manifest application;
    Manifest dependency;
};

std::vector<unsigned char> ReadBinaryFile(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

uint16_t Read16(const std::vector<unsigned char> &bytes, const std::size_t offset) {
    return static_cast<uint16_t>(bytes[offset]) | static_cast<uint16_t>(bytes[offset + 1] << 8U);
}

uint32_t Read32(const std::vector<unsigned char> &bytes, const std::size_t offset) {
    uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= static_cast<uint32_t>(bytes[offset + i]) << (i * 8U);
    }
    return value;
}

void CheckWindowsAArch64Pe(const std::filesystem::path &path) {
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

ArchiveArchitectures InspectWindowsAArch64Archive(const std::filesystem::path &path) {
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

MachOArchiveContents InspectMacOSAArch64Archive(const std::filesystem::path &path) {
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

Testing::MachOImage ReadMacOSAArch64Image(const std::filesystem::path &path) {
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

Testing::ElfImage ReadFreeBSDAArch64Image(const std::filesystem::path &path) {
    Testing::ElfImage image{ReadBinaryFile(path)};
    REQUIRE(image.bytes.size() >= 64);
    CHECK(image.bytes[0] == 0x7F);
    CHECK(image.bytes[1] == 'E');
    CHECK(image.bytes[4] == 2); // ELFCLASS64
    CHECK(image.OsAbi() == 9);  // ELFOSABI_FREEBSD
    CHECK(image.Machine() == 183);
    return image;
}

std::size_t InspectFreeBSDAArch64Archive(const std::filesystem::path &path) {
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
std::filesystem::path ArtifactTargetPath(const std::filesystem::path &artifact) {
    const auto directory = artifact.parent_path();
    return directory.parent_path().filename() / directory.filename();
}

// The profile component directly above those two.
std::filesystem::path ArtifactProfile(const std::filesystem::path &artifact) {
    return artifact.parent_path().parent_path().parent_path().filename();
}

} // namespace

TEST_CASE("output layout distinguishes raw, artifact, and test directories") {
    Manifest manifest;
    manifest.build.output = "Artifacts";
    const std::filesystem::path root = "Workspace";
    const auto host = Target::TargetTriple::Host();

    CHECK(ResolveRawOutputRoot(root, manifest) == root / "Artifacts");
    CHECK(ResolveArtifactOutputDir(root, manifest, BuildProfile::Release, host) ==
          root / "Artifacts" / "Release" / TargetOutputPath(host));
    CHECK(ResolveTestOutputDir(root, manifest, host) == root / "Artifacts");
}

TEST_CASE("output layout defaults to a normalized Bin root") {
    const Manifest manifest;
    const std::filesystem::path root = "Workspace/Package/.";
    const auto target = *Target::TargetTriple::Parse("linux-x86_64");

    CHECK(ResolveRawOutputRoot(root, manifest) == std::filesystem::path("Workspace/Package/Bin"));
    CHECK(ResolveArtifactOutputDir(root, manifest, BuildProfile::Debug, target) ==
          std::filesystem::path("Workspace/Package/Bin/Debug/Linux/x86-64"));
}

TEST_CASE("output directories canonicalize a foreign target") {
    Manifest manifest;
    manifest.build.output = "Artifacts";
    const std::filesystem::path root = "Workspace";
    // The host is one triple, so naming a second one leaves a target that is
    // foreign wherever this runs — on an AArch64 machine as much as on x86-64.
    const bool hostIsWindowsArm = HostTargetTriple() == "windows-aarch64";
    const auto foreign = *Target::TargetTriple::Parse(hostIsWindowsArm ? "linux-aarch64" : "windows-aarch64");
    const auto alias = *Target::TargetTriple::Parse(hostIsWindowsArm ? "linux-arm64" : "windows-arm64");

    CHECK(ResolveArtifactOutputDir(root, manifest, BuildProfile::Release, foreign) ==
          root / "Artifacts" / "Release" / TargetOutputPath(foreign));
    CHECK(ResolveTestOutputDir(root, manifest, foreign) == root / "Artifacts" / TargetOutputPath(foreign));
    // An alias resolves to the one canonical directory, so `--target
    // windows-arm64` and `--target windows-aarch64` are the same build.
    CHECK(ResolveArtifactOutputDir(root, manifest, BuildProfile::Release, alias) ==
          ResolveArtifactOutputDir(root, manifest, BuildProfile::Release, foreign));
    CHECK(ResolveTestOutputDir(root, manifest, alias) == ResolveTestOutputDir(root, manifest, foreign));
}

TEST_CASE("artifact names follow the target operating system, not the host") {
    CHECK(OutputFileName("App", PackageArtifactKind(ManifestPackageType::Executable),
                         Target::TargetTriple::Parse("windows-aarch64")->Os()) == "App.exe");
    CHECK(OutputFileName("App", PackageArtifactKind(ManifestPackageType::Executable),
                         Target::TargetTriple::Parse("linux-aarch64")->Os()) == "App");
    CHECK(OutputFileName("App", PackageArtifactKind(ManifestPackageType::SharedLibrary),
                         Target::TargetTriple::Parse("macos-aarch64")->Os()) == "libApp.dylib");
    CHECK(OutputFileName("App", PackageArtifactKind(ManifestPackageType::SharedLibrary),
                         Target::TargetTriple::Parse("windows-arm64")->Os()) == "App.dll");
    CHECK(OutputFileName("App", PackageArtifactKind(ManifestPackageType::StaticLibrary),
                         Target::TargetTriple::Parse("freebsd-x86_64")->Os()) == "libApp.a");
    CHECK(OutputFileName("App", PackageArtifactKind(ManifestPackageType::StaticLibrary),
                         Target::TargetTriple::Parse("windows-aarch64")->Os()) == "App.lib");
}

TEST_CASE("compiler driver loads path dependencies when checking") {
    DependencyFixture fixture;
    fixture.SetDependencyTargets({Target::HostOS});
    std::vector<Diagnostic> diagnostics;

    const auto result = CompilerDriver(fixture.Options(true, diagnostics)).Compile();

    CHECK(result.ok);
    CHECK(diagnostics.empty());
    CHECK(result.stats.dependencyFiles == 1);
}

TEST_CASE("compiler driver exposes already-loaded source text while emitting semantic diagnostics") {
    DependencyFixture fixture;
    fixture.SetApplicationSource("func Main() -> int {\n    return Missing;\n}\n");
    std::vector<Diagnostic> diagnostics;
    auto options = fixture.Options(true, diagnostics);
    std::string rendered;
    options.emitDiagnostic = [&](const Diagnostic &diagnostic, const SourceLineLookup &sourceLineLookup) {
        diagnostics.push_back(diagnostic);
        rendered += RenderDiagnostic(diagnostic, false, sourceLineLookup);
    };

    const auto result = CompilerDriver(std::move(options)).Compile();

    CHECK_FALSE(result.ok);
    REQUIRE_FALSE(diagnostics.empty());
    CHECK(rendered.contains("  2 |     return Missing;\n"));
    CHECK(rendered.contains("    |            ^\n"));
}

TEST_CASE("compiler driver enforces TargetOS on an active dependency import") {
    DependencyFixture fixture;
    const Target::OS excluded = Target::HostOS == Target::OS::Windows ? Target::OS::Linux : Target::OS::Windows;
    fixture.SetDependencyTargets({excluded});
    std::vector<Diagnostic> diagnostics;

    const auto result = CompilerDriver(fixture.Options(true, diagnostics)).Compile();

    CHECK_FALSE(result.ok);
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics.front().message.contains("not available for target"));
    CHECK(diagnostics.front().message.contains("TargetOS"));
}

TEST_CASE("compiler driver ignores a TargetOS dependency imported only by a removed branch") {
    DependencyFixture fixture;
    const Target::OS excluded = Target::HostOS == Target::OS::Windows ? Target::OS::Linux : Target::OS::Windows;
    fixture.SetDependencyTargets({excluded});
    fixture.SetApplicationSource(R"(
when false {
    import Dependency::Api::Answer;
}

func Main() -> int {
    return 0;
}
)");
    std::vector<Diagnostic> diagnostics;

    const auto result = CompilerDriver(fixture.Options(true, diagnostics)).Compile();

    CHECK(result.ok);
    CHECK(diagnostics.empty());
    CHECK(result.stats.dependencyFiles == 0);
}

TEST_CASE("compiler driver loads path dependencies when building") {
    DependencyFixture fixture;
    std::vector<Diagnostic> diagnostics;

    const auto result = CompilerDriver(fixture.Options(false, diagnostics)).Compile();

    CHECK(result.ok);
    CHECK(diagnostics.empty());
    CHECK(result.stats.dependencyFiles == 1);
    CHECK(ArtifactTargetPath(result.primaryArtifactPath) == TargetOutputPath(Target::TargetTriple::Host()));
    CHECK(ArtifactProfile(result.primaryArtifactPath) == "Debug");
    CHECK(std::filesystem::is_regular_file(result.primaryArtifactPath));
}

TEST_CASE("compiler driver builds one architecture for a foreign operating system") {
    // x86-64 objects and executables are produced in-process, so every
    // supported operating system is reachable from every host.
    constexpr std::string_view foreign = Target::HostOS == Target::OS::Windows ? "linux-x86_64" : "windows-x86_64";
    DependencyFixture fixture;
    std::vector<Diagnostic> diagnostics;
    auto options = fixture.Options(false, diagnostics);
    options.target = *Target::TargetTriple::Parse(foreign);

    const auto result = CompilerDriver(std::move(options)).Compile();

    CHECK(result.ok);
    CHECK(diagnostics.empty());
    CHECK(result.primaryArtifactPath.filename().string() ==
          ExecutableFileName("App", Target::TargetTriple::Parse(foreign)->Os()));
    // Every ordinary artifact gets its own target directory.
    CHECK(ArtifactTargetPath(result.primaryArtifactPath) == TargetOutputPath(*Target::TargetTriple::Parse(foreign)));
    CHECK(ArtifactProfile(result.primaryArtifactPath) == "Debug");
    CHECK(std::filesystem::is_regular_file(result.primaryArtifactPath));
}

TEST_CASE("Windows x86-64 Factorial reaches Main's normal return" *
          doctest::skip(Target::HostOS != Target::OS::Windows)) {
    DependencyFixture fixture;
    fixture.SetApplicationSource(R"(
func Factorial(n: uint) -> uint {
    var result: uint = 1;
    for i in 2..=n {
        result *= i as uint;
    }
    return result;
}

func Main() -> int {
    if Factorial(0) != 1 || Factorial(5) != 120 || Factorial(10) != 3628800 {
        return 1;
    }
    return 73;
}
)");
    std::vector<Diagnostic> diagnostics;
    auto options = fixture.Options(false, diagnostics);
    options.target = *Target::TargetTriple::Parse("windows-x86_64");
    options.profile = BuildProfile::Release;

    const auto build = CompilerDriver(std::move(options)).Compile();

    REQUIRE(build.ok);
    REQUIRE(diagnostics.empty());
    REQUIRE(std::filesystem::is_regular_file(build.primaryArtifactPath));
    const auto run = RunCaptured(build.primaryArtifactPath);
    REQUIRE(run.has_value());
    CHECK_MESSAGE(run->exitCode == 73, "Factorial terminated before Main returned normally: exit ", run->exitCode,
                  ", output: ", run->output);
}

TEST_CASE("compiler driver builds a Windows AArch64 executable") {
    DependencyFixture fixture;
    std::vector<Diagnostic> diagnostics;
    auto options = fixture.Options(false, diagnostics);
    options.target = *Target::TargetTriple::Parse("windows-aarch64");
    options.profile = BuildProfile::Release;

    const auto result = CompilerDriver(std::move(options)).Compile();

    REQUIRE(result.ok);
    CHECK(diagnostics.empty());
    CHECK(result.secondaryArtifactPaths.empty());
    CHECK(result.primaryArtifactPath.filename() == "App.exe");
    CHECK(ArtifactTargetPath(result.primaryArtifactPath) ==
          TargetOutputPath(*Target::TargetTriple::Parse("windows-aarch64")));
    CHECK(ArtifactProfile(result.primaryArtifactPath) == "Release");
    REQUIRE(std::filesystem::is_regular_file(result.primaryArtifactPath));
    CheckWindowsAArch64Pe(result.primaryArtifactPath);
}

TEST_CASE("compiler driver builds a Windows AArch64 shared library and import library through the arm64 alias") {
    DependencyFixture fixture;
    fixture.SetApplicationType(ManifestPackageType::SharedLibrary);
    std::vector<Diagnostic> diagnostics;
    auto options = fixture.Options(false, diagnostics);
    options.target = *Target::TargetTriple::Parse("windows-arm64");
    options.profile = BuildProfile::Release;

    const auto result = CompilerDriver(std::move(options)).Compile();

    REQUIRE(result.ok);
    CHECK(diagnostics.empty());
    CHECK(result.primaryArtifactPath.filename() == "App.dll");
    CHECK(ArtifactTargetPath(result.primaryArtifactPath) ==
          TargetOutputPath(*Target::TargetTriple::Parse("windows-aarch64")));
    REQUIRE(std::filesystem::is_regular_file(result.primaryArtifactPath));
    CheckWindowsAArch64Pe(result.primaryArtifactPath);

    REQUIRE(result.secondaryArtifactPaths.size() == 1);
    const auto &importLibrary = result.secondaryArtifactPaths.front();
    CHECK(importLibrary.parent_path() == result.primaryArtifactPath.parent_path());
    CHECK(importLibrary.filename() == "App.lib");
    REQUIRE(std::filesystem::is_regular_file(importLibrary));
    const auto archive = ReadBinaryFile(importLibrary);
    REQUIRE(archive.size() >= 8);
    CHECK(std::string(archive.begin(), archive.begin() + 8) == "!<arch>\n");
    CHECK(InspectWindowsAArch64Archive(importLibrary).imports > 0);
}

TEST_CASE("compiler driver builds a Windows AArch64 static library") {
    DependencyFixture fixture;
    fixture.SetApplicationType(ManifestPackageType::StaticLibrary);
    std::vector<Diagnostic> diagnostics;
    auto options = fixture.Options(false, diagnostics);
    options.target = *Target::TargetTriple::Parse("windows-aarch64");
    options.profile = BuildProfile::Release;

    const auto result = CompilerDriver(std::move(options)).Compile();

    REQUIRE(result.ok);
    CHECK(diagnostics.empty());
    CHECK(result.secondaryArtifactPaths.empty());
    CHECK(result.primaryArtifactPath.filename() == "App.lib");
    CHECK(ArtifactTargetPath(result.primaryArtifactPath) ==
          TargetOutputPath(*Target::TargetTriple::Parse("windows-aarch64")));
    REQUIRE(std::filesystem::is_regular_file(result.primaryArtifactPath));
    CHECK(InspectWindowsAArch64Archive(result.primaryArtifactPath).objects > 0);
}

TEST_CASE("compiler driver builds a signed macOS AArch64 executable with target source dependencies") {
    DependencyFixture fixture;
    fixture.ConfigureMacOSTargetDependencies();
    std::vector<Diagnostic> diagnostics;
    auto options = fixture.Options(false, diagnostics);
    options.target = *Target::TargetTriple::Parse("macos-aarch64");
    options.profile = BuildProfile::Release;
    std::vector<CompilePhase> phases;
    options.emitProgress = [&](const CompileProgress &progress) { phases.push_back(progress.phase); };

    const auto result = CompilerDriver(std::move(options)).Compile();

    CAPTURE(diagnostics.empty() ? std::string{} : diagnostics.front().message);
    REQUIRE(result.ok);
    CHECK(diagnostics.empty());
    CHECK(phases == std::vector{CompilePhase::Lexing, CompilePhase::Parsing, CompilePhase::LoadingDependency,
                                CompilePhase::Analyzing, CompilePhase::LoweringToHir, CompilePhase::LoweringToLir,
                                CompilePhase::EmittingObjects, CompilePhase::Linking});
    CHECK(result.stats.dependencyFiles == 1);
    CHECK(result.primaryArtifactPath.filename() == "App");
    CHECK(ArtifactTargetPath(result.primaryArtifactPath) ==
          TargetOutputPath(*Target::TargetTriple::Parse("macos-aarch64")));
    CHECK(ArtifactProfile(result.primaryArtifactPath) == "Release");
    REQUIRE(std::filesystem::is_regular_file(result.primaryArtifactPath));
    const auto image = ReadMacOSAArch64Image(result.primaryArtifactPath);
    CHECK(image.fileType == 2);          // MH_EXECUTE
    CHECK(image.HasCommand(0x32));       // LC_BUILD_VERSION
    CHECK(image.flags == 0x0020'0005);   // MH_NOUNDEFS | MH_DYLDLINK | MH_PIE
    CHECK_FALSE(image.HasCommand(0x05)); // arm64 cannot use a static LC_UNIXTHREAD entry
    CHECK(image.HasCommand(0x0E));       // LC_LOAD_DYLINKER
    CHECK(image.mainEntryOffset.has_value());
}

TEST_CASE("compiler driver canonicalizes the macOS ARM64 alias and builds a signed shared library") {
    DependencyFixture fixture;
    fixture.ConfigureMacOSTargetDependencies();
    fixture.SetApplicationType(ManifestPackageType::SharedLibrary);
    std::vector<Diagnostic> diagnostics;
    auto options = fixture.Options(false, diagnostics);
    options.target = *Target::TargetTriple::Parse("macos-arm64");
    options.profile = BuildProfile::Release;

    const auto result = CompilerDriver(std::move(options)).Compile();

    CAPTURE(diagnostics.empty() ? std::string{} : diagnostics.front().message);
    REQUIRE(result.ok);
    CHECK(diagnostics.empty());
    CHECK(result.stats.dependencyFiles == 1);
    CHECK(result.secondaryArtifactPaths.empty());
    CHECK(result.primaryArtifactPath.filename() == "libApp.dylib");
    CHECK(ArtifactTargetPath(result.primaryArtifactPath) ==
          TargetOutputPath(*Target::TargetTriple::Parse("macos-aarch64")));
    REQUIRE(std::filesystem::is_regular_file(result.primaryArtifactPath));
    const auto image = ReadMacOSAArch64Image(result.primaryArtifactPath);
    CHECK(image.fileType == 6);    // MH_DYLIB
    CHECK(image.HasCommand(0x0D)); // LC_ID_DYLIB
    CHECK(image.HasCommand(0x02)); // LC_SYMTAB
    CHECK_FALSE(image.mainEntryOffset);
    CHECK_FALSE(image.threadEntryAddress);

    const auto report = FormatBuildStats("App", result.primaryArtifactPath, {}, BuildProfile::Release, "macos-arm64",
                                         result.stats, false);
    CHECK(report.contains("Target: macOS AArch64 (macos-aarch64)\n"));
}

TEST_CASE("compiler driver builds macOS AArch64 static libraries with relocatable object members") {
    DependencyFixture fixture;
    fixture.ConfigureMacOSTargetDependencies();
    fixture.SetApplicationType(ManifestPackageType::StaticLibrary);
    std::vector<Diagnostic> diagnostics;
    auto options = fixture.Options(false, diagnostics);
    options.target = *Target::TargetTriple::Parse("macos-aarch64");
    options.profile = BuildProfile::Release;

    const auto result = CompilerDriver(std::move(options)).Compile();

    CAPTURE(diagnostics.empty() ? std::string{} : diagnostics.front().message);
    REQUIRE(result.ok);
    CHECK(diagnostics.empty());
    CHECK(result.stats.dependencyFiles == 1);
    CHECK(result.secondaryArtifactPaths.empty());
    CHECK(result.primaryArtifactPath.filename() == "libApp.a");
    CHECK(ArtifactTargetPath(result.primaryArtifactPath) ==
          TargetOutputPath(*Target::TargetTriple::Parse("macos-aarch64")));
    REQUIRE(std::filesystem::is_regular_file(result.primaryArtifactPath));
    const auto contents = InspectMacOSAArch64Archive(result.primaryArtifactPath);
    CHECK(contents.objects > 0);
    CHECK(contents.relocations > 0);
}

TEST_CASE("compiler driver checks a macOS AArch64 source library with only target dependencies") {
    DependencyFixture fixture;
    fixture.ConfigureMacOSTargetDependencies();
    fixture.SetApplicationType(ManifestPackageType::SourceLibrary);
    std::vector<Diagnostic> diagnostics;
    auto options = fixture.Options(true, diagnostics);
    options.target = *Target::TargetTriple::Parse("macos-aarch64");

    const auto result = CompilerDriver(std::move(options)).Compile();

    CAPTURE(diagnostics.empty() ? std::string{} : diagnostics.front().message);
    CHECK(result.ok);
    CHECK(diagnostics.empty());
    CHECK(result.stats.dependencyFiles == 1);
    CHECK(result.primaryArtifactPath.empty());
}

TEST_CASE("compiler driver target dependency errors name macOS AArch64 rather than the host") {
    DependencyFixture fixture;
    fixture.SetDependencyTargets({Target::OS::Windows});
    std::vector<Diagnostic> diagnostics;
    auto options = fixture.Options(true, diagnostics);
    options.target = *Target::TargetTriple::Parse("macos-aarch64");

    const auto result = CompilerDriver(std::move(options)).Compile();

    CHECK_FALSE(result.ok);
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics.front().message.contains("macos-aarch64"));
    CHECK(diagnostics.front().message.contains("TargetOS"));
}

TEST_CASE("compiler driver builds a FreeBSD AArch64 executable with target-conditioned dependencies") {
    DependencyFixture fixture;
    fixture.ConfigureFreeBSDTargetDependencies();
    std::vector<Diagnostic> diagnostics;
    auto options = fixture.Options(false, diagnostics);
    options.target = *Target::TargetTriple::Parse("freebsd-aarch64");
    options.profile = BuildProfile::Release;

    const auto result = CompilerDriver(std::move(options)).Compile();

    CAPTURE(diagnostics.empty() ? std::string{} : diagnostics.front().message);
    REQUIRE(result.ok);
    CHECK(diagnostics.empty());
    CHECK(result.stats.dependencyFiles == 1);
    CHECK(result.secondaryArtifactPaths.empty());
    CHECK(result.primaryArtifactPath.filename() == "App");
    CHECK(ArtifactTargetPath(result.primaryArtifactPath) ==
          TargetOutputPath(*Target::TargetTriple::Parse("freebsd-aarch64")));
    CHECK(ArtifactProfile(result.primaryArtifactPath) == "Release");
    REQUIRE(std::filesystem::is_regular_file(result.primaryArtifactPath));

    const auto image = ReadFreeBSDAArch64Image(result.primaryArtifactPath);
    CHECK(image.Type() == 2); // ET_EXEC
    CHECK(image.Entry() != 0);
    CHECK(image.Entry() % 4 == 0);
    CHECK(image.Interpreter() == "/libexec/ld-elf.so.1");
    CHECK(std::ranges::contains(image.NeededLibraries(), "libc.so.7"));
    const auto relocations = image.PltRelocations();
    REQUIRE_FALSE(relocations.empty());
    CHECK(std::ranges::all_of(relocations, [](const Testing::ElfImage::Rela &relocation) {
        return relocation.type == 1026; // R_AARCH64_JUMP_SLOT
    }));
}

TEST_CASE("compiler driver canonicalizes FreeBSD ARM64 as AArch64 and builds a shared library") {
    DependencyFixture fixture;
    fixture.ConfigureFreeBSDTargetDependencies();
    fixture.SetApplicationType(ManifestPackageType::SharedLibrary);
    std::vector<Diagnostic> diagnostics;
    auto options = fixture.Options(false, diagnostics);
    options.target = *Target::TargetTriple::Parse("freebsd-arm64");
    options.profile = BuildProfile::Release;

    const auto result = CompilerDriver(std::move(options)).Compile();

    CAPTURE(diagnostics.empty() ? std::string{} : diagnostics.front().message);
    REQUIRE(result.ok);
    CHECK(diagnostics.empty());
    CHECK(result.stats.dependencyFiles == 1);
    CHECK(result.secondaryArtifactPaths.empty());
    CHECK(result.primaryArtifactPath.filename() == "libApp.so");
    CHECK(ArtifactTargetPath(result.primaryArtifactPath) ==
          TargetOutputPath(*Target::TargetTriple::Parse("freebsd-aarch64")));
    REQUIRE(std::filesystem::is_regular_file(result.primaryArtifactPath));

    const auto image = ReadFreeBSDAArch64Image(result.primaryArtifactPath);
    CHECK(image.Type() == 3); // ET_DYN
    CHECK(image.Entry() == 0);
    CHECK(image.Interpreter().empty());
    CHECK(image.Soname() == "libApp.so");
    CHECK(std::ranges::contains(image.NeededLibraries(), "libc.so.7"));
    const auto symbols = image.DynamicSymbols();
    CHECK(std::ranges::contains(symbols, "SelectedAnswer", &Testing::ElfImage::DynamicSymbol::name));
    const auto relocations = image.PltRelocations();
    REQUIRE_FALSE(relocations.empty());
    CHECK(std::ranges::all_of(relocations, [](const Testing::ElfImage::Rela &relocation) {
        return relocation.type == 1026; // R_AARCH64_JUMP_SLOT
    }));

    CHECK(CanonicalTargetTriple("freebsd-arm64") == "freebsd-aarch64");
    const auto report = FormatBuildStats("App", result.primaryArtifactPath, {}, BuildProfile::Release, "freebsd-arm64",
                                         result.stats, false);
    CHECK(report.contains("Target: FreeBSD AArch64 (freebsd-aarch64)\n"));
}

TEST_CASE("compiler driver builds a FreeBSD AArch64 static library from relocatable ELF members") {
    DependencyFixture fixture;
    fixture.ConfigureFreeBSDTargetDependencies();
    fixture.SetApplicationType(ManifestPackageType::StaticLibrary);
    std::vector<Diagnostic> diagnostics;
    auto options = fixture.Options(false, diagnostics);
    options.target = *Target::TargetTriple::Parse("freebsd-aarch64");
    options.profile = BuildProfile::Release;

    const auto result = CompilerDriver(std::move(options)).Compile();

    CAPTURE(diagnostics.empty() ? std::string{} : diagnostics.front().message);
    REQUIRE(result.ok);
    CHECK(diagnostics.empty());
    CHECK(result.stats.dependencyFiles == 1);
    CHECK(result.secondaryArtifactPaths.empty());
    CHECK(result.primaryArtifactPath.filename() == "libApp.a");
    CHECK(ArtifactTargetPath(result.primaryArtifactPath) ==
          TargetOutputPath(*Target::TargetTriple::Parse("freebsd-aarch64")));
    REQUIRE(std::filesystem::is_regular_file(result.primaryArtifactPath));
    CHECK(InspectFreeBSDAArch64Archive(result.primaryArtifactPath) > 0);
}

TEST_CASE("compiler driver checks FreeBSD AArch64 source libraries and reports missing target dependencies") {
    SUBCASE("a matching FreeBSD dependency is selected without loading a host package") {
        DependencyFixture fixture;
        fixture.ConfigureFreeBSDTargetDependencies();
        fixture.SetApplicationType(ManifestPackageType::SourceLibrary);
        std::vector<Diagnostic> diagnostics;
        auto options = fixture.Options(true, diagnostics);
        options.target = *Target::TargetTriple::Parse("freebsd-aarch64");

        const auto result = CompilerDriver(std::move(options)).Compile();

        CAPTURE(diagnostics.empty() ? std::string{} : diagnostics.front().message);
        CHECK(result.ok);
        CHECK(diagnostics.empty());
        CHECK(result.stats.dependencyFiles == 1);
        CHECK(result.primaryArtifactPath.empty());
    }

    SUBCASE("an unavailable dependency names the requested target") {
        DependencyFixture fixture;
        fixture.SetDependencyTargets({Target::OS::Windows});
        std::vector<Diagnostic> diagnostics;
        auto options = fixture.Options(true, diagnostics);
        options.target = *Target::TargetTriple::Parse("freebsd-aarch64");

        const auto result = CompilerDriver(std::move(options)).Compile();

        CHECK_FALSE(result.ok);
        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics.front().message.contains("freebsd-aarch64"));
        CHECK(diagnostics.front().message.contains("TargetOS"));
    }
}

TEST_CASE("compiler driver links a SharedLibrary package as a native shared library") {
    DependencyFixture fixture;
    fixture.SetApplicationType(ManifestPackageType::SharedLibrary);
    std::vector<Diagnostic> diagnostics;

    const auto result = CompilerDriver(fixture.Options(false, diagnostics)).Compile();

    CHECK(result.ok);
    CHECK(diagnostics.empty());
    CHECK(result.primaryArtifactPath.filename().string() == SharedLibraryFileName("App"));
    CHECK(std::filesystem::is_regular_file(result.primaryArtifactPath));
    if constexpr (Target::HostOS == Target::OS::Windows) {
        REQUIRE(result.secondaryArtifactPaths.size() == 1);
        CHECK(result.secondaryArtifactPaths.front().filename().string() == StaticLibraryFileName("App"));
        CHECK(std::filesystem::is_regular_file(result.secondaryArtifactPaths.front()));
    }
}

TEST_CASE("compiler driver builds a StaticLibrary package as a native archive") {
    DependencyFixture fixture;
    fixture.SetApplicationType(ManifestPackageType::StaticLibrary);
    std::vector<Diagnostic> diagnostics;

    const auto result = CompilerDriver(fixture.Options(false, diagnostics)).Compile();

    CHECK(result.ok);
    CHECK(diagnostics.empty());
    CHECK(result.primaryArtifactPath.filename().string() == StaticLibraryFileName("App"));
    REQUIRE(std::filesystem::is_regular_file(result.primaryArtifactPath));
    std::ifstream archive(result.primaryArtifactPath, std::ios::binary);
    std::array<char, 8> magic{};
    archive.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    CHECK(magic == std::array<char, 8>{'!', '<', 'a', 'r', 'c', 'h', '>', '\n'});
}

TEST_CASE("compiler driver builds a StaticLibrary package for linux-aarch64") {
    DependencyFixture fixture;
    fixture.SetApplicationType(ManifestPackageType::StaticLibrary);
    std::vector<Diagnostic> diagnostics;
    auto options = fixture.Options(false, diagnostics);
    options.target = *Target::TargetTriple::Parse("linux-aarch64");

    const auto result = CompilerDriver(std::move(options)).Compile();

    CHECK(result.ok);
    CHECK(diagnostics.empty());
    CHECK(result.primaryArtifactPath.filename().string() == StaticLibraryFileName("App", Target::OS::Linux));
    REQUIRE(std::filesystem::is_regular_file(result.primaryArtifactPath));

    std::ifstream archive(result.primaryArtifactPath, std::ios::binary);
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(archive)), std::istreambuf_iterator<char>());
    // Walk the archive's members past the symbol index and read each object's
    // ELF header: a cross build must stamp EM_AARCH64 into every one of them,
    // not the host's EM_X86_64.
    std::size_t objects = 0;
    for (std::size_t at = 8; at + 60 <= bytes.size();) {
        std::string size;
        for (std::size_t i = 48; i < 58 && bytes[at + i] != ' '; ++i) {
            size.push_back(static_cast<char>(bytes[at + i]));
        }
        const auto memberSize = static_cast<std::size_t>(std::stoul(size));
        const std::size_t body = at + 60;
        if (memberSize >= 20 && bytes[body] == 0x7F && bytes[body + 1] == 'E') {
            CHECK(bytes[body + 18] == 183); // EM_AARCH64
            CHECK(bytes[body + 19] == 0);
            ++objects;
        }
        at = body + memberSize + (memberSize & 1U);
    }
    CHECK(objects > 0);
}

TEST_CASE("compiler driver checks a SourceLibrary package but refuses to build it") {
    DependencyFixture fixture;
    fixture.SetApplicationType(ManifestPackageType::SourceLibrary);

    std::vector<Diagnostic> checkDiagnostics;
    CHECK(CompilerDriver(fixture.Options(true, checkDiagnostics)).Compile().ok);
    CHECK(checkDiagnostics.empty());

    std::vector<Diagnostic> buildDiagnostics;
    const auto build = CompilerDriver(fixture.Options(false, buildDiagnostics)).Compile();

    CHECK_FALSE(build.ok);
    REQUIRE(buildDiagnostics.size() == 1);
    CHECK(buildDiagnostics[0].IsError());
    CHECK(buildDiagnostics[0].message.contains("Type = \"SourceLibrary\""));
}

TEST_CASE("compiler driver resolves transitive workspace dependencies by normalized package identity") {
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
struct Slice<T> { data: *T; length: uint; }

struct Build {
    timestamp: uint64;
    date: Slice<char8>;
    time: Slice<char8>;
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
    CHECK(std::filesystem::is_regular_file(result.primaryArtifactPath));
}

// Cross-target plumbing
//
// The three questions a cross build asks before it produces anything: which
// machine the option names, whether a back end reaches it from this host, and
// where the artifact lands. None of the cases below run what they build, so
// they hold on every compiler host.

TEST_CASE("a target triple resolves to the machine it names rather than to the host") {
    const TargetContext aarch64 = TargetContextForTriple(*Target::TargetTriple::Parse("linux-aarch64"));
    CHECK(aarch64.arch == Target::Arch::AArch64);
    CHECK(aarch64.os == Target::OS::Linux);
    CHECK(aarch64.object_format == Target::ObjectFormat::ELF);
    CHECK(aarch64.pointer_size == 8);
    CHECK(aarch64.endianness == Target::Endian::Little);

    // An alias resolves to the same description, and each operating system the
    // triple names selects its own object format.
    CHECK(TargetContextForTriple(*Target::TargetTriple::Parse("linux-arm64")).arch == Target::Arch::AArch64);
    CHECK(TargetContextForTriple(*Target::TargetTriple::Parse("windows-aarch64")).object_format ==
          Target::ObjectFormat::COFF);
    CHECK(TargetContextForTriple(*Target::TargetTriple::Parse("macos-aarch64")).object_format ==
          Target::ObjectFormat::MachO);
}

TEST_CASE("every supported triple is one a back end can produce") {
    // Both back ends encode and link in-process, so a triple the driver accepts
    // is one it can always build: four operating systems on two architectures,
    // under either spelling of the architecture.
    for (const std::string_view os : {"freebsd", "linux", "macos", "windows"}) {
        for (const std::string_view arch : {"x86_64", "x64", "amd64", "aarch64", "arm64"}) {
            CHECK(IsSupportedTargetTriple(std::string(os) + "-" + std::string(arch)));
        }
    }

    // Narrowing to those four did not leave the other System V targets, or an
    // architecture with no back end, quietly accepted.
    for (const std::string_view target : {"openbsd-x86_64", "netbsd-x86_64", "dragonfly-x86_64", "illumos-x86_64",
                                          "openbsd-aarch64", "linux-riscv64", "linux-arm32", "windows-x86"}) {
        CHECK_FALSE(IsSupportedTargetTriple(target));
    }
}

TEST_CASE("compiler driver builds an executable for linux-aarch64") {
    DependencyFixture fixture;
    std::vector<Diagnostic> diagnostics;
    auto options = fixture.Options(false, diagnostics);
    options.target = *Target::TargetTriple::Parse("linux-aarch64");

    const auto result = CompilerDriver(std::move(options)).Compile();

    CHECK(result.ok);
    CHECK(diagnostics.empty());
    // The artifact is named for the target operating system and, when the
    // target is foreign, sits in the target's own directory one level below the
    // host build. On an AArch64 Linux machine this is the host's own build, and
    // the host keeps its historical path.
    CHECK(result.primaryArtifactPath.filename().string() == ExecutableFileName("App", Target::OS::Linux));
    CHECK(ArtifactTargetPath(result.primaryArtifactPath) ==
          TargetOutputPath(*Target::TargetTriple::Parse("linux-aarch64")));
    REQUIRE(std::filesystem::is_regular_file(result.primaryArtifactPath));

    std::ifstream executable(result.primaryArtifactPath, std::ios::binary);
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(executable)),
                                           std::istreambuf_iterator<char>());
    REQUIRE(bytes.size() > 64);
    CHECK(bytes[0] == 0x7F);
    CHECK(bytes[1] == 'E');
    CHECK(bytes[4] == 2);                        // ELFCLASS64
    CHECK((bytes[16] | bytes[17] << 8U) == 2);   // ET_EXEC
    CHECK((bytes[18] | bytes[19] << 8U) == 183); // EM_AARCH64

    // A whole program was linked, not just assembled: the header names an entry
    // point, and the code it points at is AArch64 rather than the host's.
    std::uint64_t entry = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        entry |= static_cast<std::uint64_t>(bytes[24 + i]) << (i * 8U);
    }
    CHECK(entry != 0);
    CHECK(entry % 4 == 0);
}
