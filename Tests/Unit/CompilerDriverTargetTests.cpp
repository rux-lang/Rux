#include "Cli/BuildReport.h"
#include "CompilerDriverTestSupport.h"

using namespace Rux;
using namespace Rux::CliSupport;
using namespace Rux::Testing::CompilerDriverTestSupport;

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
    CHECK(phases == std::vector{CompilePhase::Configuring, CompilePhase::LoadingSources, CompilePhase::Lexing,
                                CompilePhase::Parsing, CompilePhase::LoadingDependency, CompilePhase::LoadingDependency,
                                CompilePhase::Analyzing, CompilePhase::LoweringToHir, CompilePhase::OptimizingHir,
                                CompilePhase::LoweringToLir, CompilePhase::OptimizingLir, CompilePhase::EmittingObjects,
                                CompilePhase::Linking});
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

    const BuildReportInfo info{.packageName = "App",
                               .packageVersion = "0.1.0",
                               .artifactPath = result.primaryArtifactPath,
                               .packageRoot = {},
                               .profile = BuildProfile::Release,
                               .targetTriple = "macos-arm64"};
    // The alias normalizes to AArch64, which the report states in its display
    // spelling rather than repeating the canonical ID.
    CHECK(FormatBuildStats(info, result.stats, false).contains("Built App (Release, macOS AArch64) in"));
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
    CHECK(std::ranges::any_of(diagnostics.front().notes,
                              [](const std::string &note) { return note.contains("TargetOS"); }));
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
    const BuildReportInfo info{.packageName = "App",
                               .packageVersion = "0.1.0",
                               .artifactPath = result.primaryArtifactPath,
                               .packageRoot = {},
                               .profile = BuildProfile::Release,
                               .targetTriple = "freebsd-arm64"};
    CHECK(FormatBuildStats(info, result.stats, false).contains("Built App (Release, FreeBSD AArch64) in"));
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
        CHECK(std::ranges::any_of(diagnostics.front().notes,
                                  [](const std::string &note) { return note.contains("TargetOS"); }));
    }
}

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
