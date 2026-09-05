#include "CompilerDriverTestSupport.h"

using namespace Rux;
using namespace Rux::Testing::CompilerDriverTestSupport;

TEST_CASE("compiler driver runs inferred literals and arithmetic without Core") {
    DependencyFixture fixture;
    fixture.SetApplicationSource(R"(
func Main() -> int {
    let text = "no library required";
    var total = 0;
    for value in 0..4 { total += value; }
    return total - 6;
}
)");
    std::vector<Diagnostic> diagnostics;
    const auto result = CompilerDriver(fixture.Options(false, diagnostics)).Compile();
    REQUIRE(result.ok);
    const auto executed = RunCaptured(result.primaryArtifactPath);
    REQUIRE(executed);
    CHECK_EQ(executed->exitCode, 0);
}

TEST_CASE("compiler driver obtains intrinsic declarations from an ordinary dependency") {
    DependencyFixture fixture;
    fixture.SetDependencySource(R"(
pub intrinsic type int8;
extend int8 { pub const Min: int8 = -128i8; }
pub intrinsic struct string8 { pub data: *char8; pub length: uint; }
pub type string = string8;
pub enum OperatingSystem { Unknown, FreeBSD, Linux, macOS, Windows }
pub struct Target { pub os: OperatingSystem; }
pub intrinsic #target: Target;
)");
    fixture.SetApplicationSource(R"(
import Dependency::{ int8, string, #target };
when int8::Min == -128i8 {
    func Main() -> int {
        let text: string = "hello";
        return (text.length as int) - 5;
    }
} else { import Missing::NotSelected; }
)");
    std::vector<Diagnostic> diagnostics;
    const auto result = CompilerDriver(fixture.Options(false, diagnostics)).Compile();
    REQUIRE(result.ok);
    const auto executed = RunCaptured(result.primaryArtifactPath);
    REQUIRE(executed);
    CHECK_EQ(executed->exitCode, 0);
}

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
    CHECK(std::ranges::any_of(diagnostics.front().notes,
                              [](const std::string &note) { return note.contains("TargetOS"); }));
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

TEST_CASE("compiler driver qualifies same-named public generics from different packages") {
    DependencyFixture fixture;
    fixture.ConfigureSameNamedGenericDependencies();
    std::vector<Diagnostic> diagnostics;

    const auto build = CompilerDriver(fixture.Options(false, diagnostics)).Compile();

    CAPTURE(diagnostics.empty() ? std::string{} : diagnostics.front().message);
    REQUIRE(build.ok);
    REQUIRE(diagnostics.empty());
    REQUIRE(std::filesystem::is_regular_file(build.primaryArtifactPath));
    const auto run = RunCaptured(build.primaryArtifactPath);
    REQUIRE(run.has_value());
    CHECK_MESSAGE(run->exitCode == 42, "qualified generic call returned ", run->exitCode, ", output: ", run->output);
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
    REQUIRE(build.diagnostics.size() == buildDiagnostics.size());
    CHECK(build.diagnostics[0].message == buildDiagnostics[0].message);
    REQUIRE(buildDiagnostics[0].notes.size() == 1);
    CHECK(buildDiagnostics[0].notes[0] == "compiler phase: Configuring");
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
