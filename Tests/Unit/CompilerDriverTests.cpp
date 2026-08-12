#include "Driver/BuildTarget.h"
#include "Driver/CompilerDriver.h"
#include "System/Os.h"
#include "System/Process.h"
#include "Target/Target.h"

#include <array>
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

// Whether a back end covers this host, which is what a case that builds for the
// host needs. The AArch64 back end writes ELF, so an AArch64 macOS or Windows
// host has none until the Mach-O and PE writers take an architecture
// (BACKLOG.md "Follow-on"). Those cases are skipped there rather than deleted:
// what they assert is about the driver, not about the machine running it.
constexpr bool kHostHasBackend = Target::HostArch != Target::Arch::AArch64 || Target::HostOS == Target::OS::Linux;

} // namespace

TEST_CASE("test output directories omit the build profile") {
    Manifest manifest;
    manifest.build.output = "Artifacts";
    const std::filesystem::path root = "Workspace";

    CHECK(ResolveBuildOutputDir(root, manifest, "Release", HostTargetTriple()) == root / "Artifacts" / "Release");
    CHECK(ResolveBuildOutputDir(root, manifest, "Release", HostTargetTriple(), false) == root / "Artifacts");
}

TEST_CASE("output directories separate a foreign target from the host") {
    Manifest manifest;
    manifest.build.output = "Artifacts";
    const std::filesystem::path root = "Workspace";
    // The host is one triple, so naming a second one leaves a target that is
    // foreign wherever this runs — on an AArch64 machine as much as on x86-64.
    const bool hostIsWindowsArm = HostTargetTriple() == "windows-aarch64";
    const std::string foreign = hostIsWindowsArm ? "linux-aarch64" : "windows-aarch64";
    const std::string alias = hostIsWindowsArm ? "linux-arm64" : "windows-arm64";

    CHECK(ResolveBuildOutputDir(root, manifest, "Release", foreign) == root / "Artifacts" / "Release" / foreign);
    CHECK(ResolveBuildOutputDir(root, manifest, "Release", foreign, false) == root / "Artifacts" / foreign);
    // An alias resolves to the one canonical directory, so `--target
    // windows-arm64` and `--target windows-aarch64` are the same build.
    CHECK(ResolveBuildOutputDir(root, manifest, "Release", alias) ==
          ResolveBuildOutputDir(root, manifest, "Release", foreign));
    // Target-independent output, such as a published `.ruxpkg`, adds nothing.
    CHECK(ResolveBuildOutputDir(root, manifest, {}, {}, false) == root / "Artifacts");
}

TEST_CASE("artifact names follow the target operating system, not the host") {
    CHECK(OutputFileName("App", PackageArtifactKind(ManifestPackageType::Executable),
                         TargetTripleOs("windows-x86_64")) == "App.exe");
    CHECK(OutputFileName("App", PackageArtifactKind(ManifestPackageType::Executable),
                         TargetTripleOs("linux-aarch64")) == "App");
    CHECK(OutputFileName("App", PackageArtifactKind(ManifestPackageType::SharedLibrary),
                         TargetTripleOs("macos-aarch64")) == "libApp.dylib");
    CHECK(OutputFileName("App", PackageArtifactKind(ManifestPackageType::SharedLibrary),
                         TargetTripleOs("windows-x86_64")) == "App.dll");
    CHECK(OutputFileName("App", PackageArtifactKind(ManifestPackageType::StaticLibrary),
                         TargetTripleOs("freebsd-x86_64")) == "libApp.a");
    CHECK(OutputFileName("App", PackageArtifactKind(ManifestPackageType::StaticLibrary),
                         TargetTripleOs("windows-x86_64")) == "App.lib");
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

TEST_CASE("compiler driver loads path dependencies when building" * doctest::skip(!kHostHasBackend)) {
    DependencyFixture fixture;
    std::vector<Diagnostic> diagnostics;

    const auto result = CompilerDriver(fixture.Options(false, diagnostics)).Compile();

    CHECK(result.ok);
    CHECK(diagnostics.empty());
    CHECK(result.stats.dependencyFiles == 1);
    CHECK(std::filesystem::is_regular_file(result.primaryArtifactPath));
}

TEST_CASE("compiler driver builds one architecture for a foreign operating system") {
    // x86-64 objects and executables are produced in-process, so every
    // supported operating system is reachable from every host.
    constexpr std::string_view foreign = Target::HostOS == Target::OS::Windows ? "linux-x86_64" : "windows-x86_64";
    DependencyFixture fixture;
    std::vector<Diagnostic> diagnostics;
    auto options = fixture.Options(false, diagnostics);
    options.targetName = foreign;

    const auto result = CompilerDriver(std::move(options)).Compile();

    CHECK(result.ok);
    CHECK(diagnostics.empty());
    CHECK(result.primaryArtifactPath.filename().string() == ExecutableFileName("App", TargetTripleOs(foreign)));
    // A foreign target gets its own directory, so it cannot overwrite the host
    // build sitting one level up.
    CHECK(result.primaryArtifactPath.parent_path().filename() == foreign);
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
    options.targetName = "windows-x86_64";
    options.profileName = "Release";

    const auto build = CompilerDriver(std::move(options)).Compile();

    REQUIRE(build.ok);
    REQUIRE(diagnostics.empty());
    REQUIRE(std::filesystem::is_regular_file(build.primaryArtifactPath));
    const auto run = RunCaptured(build.primaryArtifactPath);
    REQUIRE(run.has_value());
    CHECK_MESSAGE(run->exitCode == 73, "Factorial terminated before Main returned normally: exit ", run->exitCode,
                  ", output: ", run->output);
}

TEST_CASE("compiler driver refuses a target no back end covers") {
    // The AArch64 back end writes ELF, so an AArch64 target on any other
    // operating system is refused before code generation starts — on a host of
    // that same kind too, which is what makes this a property of the target.
    DependencyFixture fixture;
    std::vector<Diagnostic> diagnostics;
    auto options = fixture.Options(false, diagnostics);
    options.targetName = "windows-aarch64";

    const auto result = CompilerDriver(std::move(options)).Compile();

    CHECK_FALSE(result.ok);
    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics.front().IsError());
    CHECK(diagnostics.front().message.contains("windows-aarch64"));
    CHECK(diagnostics.front().message.contains("not implemented yet"));
}

TEST_CASE("compiler driver links a SharedLibrary package as a native shared library" *
          doctest::skip(!kHostHasBackend)) {
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

TEST_CASE("compiler driver builds a StaticLibrary package as a native archive" * doctest::skip(!kHostHasBackend)) {
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
    options.targetName = "linux-aarch64";

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

TEST_CASE("compiler driver supplies manifest and command-line build context" * doctest::skip(!kHostHasBackend)) {
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
// they hold on a host with no emulator installed.

TEST_CASE("a target triple resolves to the machine it names rather than to the host") {
    const TargetContext aarch64 = TargetContextForTriple("linux-aarch64");
    CHECK(aarch64.arch == Target::Arch::AArch64);
    CHECK(aarch64.os == Target::OS::Linux);
    CHECK(aarch64.object_format == Target::ObjectFormat::ELF);
    CHECK(aarch64.pointer_size == 8);
    CHECK(aarch64.endianness == Target::Endian::Little);

    // An alias resolves to the same description, and an operating system the
    // triple names is carried even where no back end reaches it yet.
    CHECK(TargetContextForTriple("linux-arm64").arch == Target::Arch::AArch64);
    CHECK(TargetContextForTriple("windows-aarch64").object_format == Target::ObjectFormat::COFF);
    CHECK(TargetContextForTriple("macos-aarch64").object_format == Target::ObjectFormat::MachO);
}

TEST_CASE("the unsupported-target diagnostic follows the back end that would produce the artifact") {
    // Both back ends encode and link in-process, so the answer is a property of
    // the target alone. x86-64 reaches every supported operating system.
    CHECK(UnsupportedBackendReason("linux-x86_64").empty());
    CHECK(UnsupportedBackendReason("windows-x86_64").empty());
    CHECK(UnsupportedBackendReason("macos-x86_64").empty());

    // AArch64 reaches Linux, under either spelling of the triple, from a host
    // of either architecture.
    CHECK(UnsupportedBackendReason("linux-aarch64").empty());
    CHECK(UnsupportedBackendReason("linux-arm64").empty());

    // The other AArch64 systems are refused rather than lowered by an external
    // toolchain, and they are refused on a host of their own kind too: the
    // Mach-O and PE writers take no architecture yet.
    for (const std::string_view triple : {"macos-aarch64", "windows-aarch64"}) {
        const std::string refused = UnsupportedBackendReason(triple);
        CHECK(refused.contains(triple));
        CHECK(refused.contains("not implemented yet"));
        CHECK(refused.contains("Linux only"));
    }

    // An architecture with no back end at all names itself rather than a host.
    const std::string riscv = UnsupportedBackendReason("linux-riscv64");
    CHECK(riscv.contains("riscv64"));
    CHECK(riscv.contains("not implemented yet"));
}

TEST_CASE("compiler driver builds an executable for linux-aarch64") {
    DependencyFixture fixture;
    std::vector<Diagnostic> diagnostics;
    auto options = fixture.Options(false, diagnostics);
    options.targetName = "linux-aarch64";

    const auto result = CompilerDriver(std::move(options)).Compile();

    CHECK(result.ok);
    CHECK(diagnostics.empty());
    // The artifact is named for the target operating system and, when the
    // target is foreign, sits in the target's own directory one level below the
    // host build. On an AArch64 Linux machine this is the host's own build, and
    // the host keeps its historical path.
    CHECK(result.primaryArtifactPath.filename().string() == ExecutableFileName("App", Target::OS::Linux));
    if (HostTargetTriple() != "linux-aarch64") {
        CHECK(result.primaryArtifactPath.parent_path().filename() == "linux-aarch64");
    }
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
