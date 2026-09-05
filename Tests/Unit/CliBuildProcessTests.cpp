#include "CliProcessTestSupport.h"

using namespace Rux;
using namespace Rux::Testing::CliProcessTestSupport;

TEST_CASE("build help publishes the all-target matrix option") {
    const auto textHelp = Run(std::array<std::string_view, 2>{"help", "build"});
    const auto jsonHelp = Run(std::array<std::string_view, 3>{"help", "build", "--json"});

    CHECK(textHelp.exitCode == 0);
    CHECK(textHelp.output.contains("--all"));
    CHECK(textHelp.output.contains("all 16 target/profile cells"));
    CHECK(textHelp.output.contains("--release --target linux-x86_64"));
    CHECK(jsonHelp.exitCode == 0);
    CHECK(jsonHelp.output.contains("\"flags\":\"--all\""));
    CHECK(jsonHelp.output.contains("all 16 target/profile cells"));
    CHECK(jsonHelp.output.contains("--release --target linux-x86_64"));
}

TEST_CASE("build --all reports every cell, continues after failures, and honors quiet") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = System::TempDirectory() / ("rux-build-matrix-" + std::to_string(nonce));
    const auto manifestPath = root / "Rux.toml";
    const auto sourcePath = root / "Src" / "Main.rux";
    std::filesystem::create_directories(sourcePath.parent_path());
    std::ofstream manifestFile(manifestPath, std::ios::binary);
    manifestFile << R"([Manifest]
Version = 1

[Package]
Name = "MatrixFixture"
Version = "0.1.0"
Type = "Executable"

[Build]
Output = "Artifacts"
)";
    manifestFile.close();
    std::ofstream source(sourcePath, std::ios::binary);
    source << R"(asm func Triple(x: int64) -> int64 {
    mov rax, rdi
    imul rax, 3
    ret
}

func Main() -> int {
    return 0;
}
)";
    source.close();
    REQUIRE(manifestFile);
    REQUIRE(source);

    const auto manifest = manifestPath.string();
    const auto reported = Run(std::array<std::string_view, 8>{"--manifest", manifest, "--color=never", "build", "--all",
                                                              "--stats", "--define", "Matrix=enabled"});

    CHECK(reported.exitCode == 1);
    CHECK(CountOccurrences(reported.output, "'imul' is an x86-64 instruction") == 8);
    CHECK(reported.output.contains("Build matrix"));
    CHECK(reported.output.contains("Status  Profile  Target              Time"));
    CHECK(reported.output.contains("Built   Debug    FreeBSD x86-64"));
    CHECK(reported.output.contains("Failed  Debug    FreeBSD AArch64"));
    CHECK(reported.output.contains("Built   Release  Windows x86-64"));
    CHECK(reported.output.contains("Failed  Release  Windows AArch64"));
    CHECK(reported.output.contains("Failed 16 cells in "));
    CHECK(reported.output.contains("(8 succeeded, 8 failed)"));
    CHECK(reported.output.contains("Aggregate statistics:"));
    CHECK_FALSE(reported.output.contains("\033["));
    // Artifact paths are reported relative to the package root.
    CHECK(reported.output.contains(
        (std::filesystem::path("Artifacts") / "Debug" / "FreeBSD" / "x86-64" / "MatrixFixture").string()));

    for (const std::string_view profile : {"Debug", "Release"}) {
        for (const std::string_view os : {"FreeBSD", "Linux", "macOS", "Windows"}) {
            const auto fileName = os == "Windows" ? "MatrixFixture.exe" : "MatrixFixture";
            CHECK(std::filesystem::is_regular_file(root / "Artifacts" / profile / os / "x86-64" / fileName));
        }
    }

    std::ofstream invalidSource(sourcePath, std::ios::binary | std::ios::trunc);
    invalidSource << "func Main() -> int { return Missing; }\n";
    invalidSource.close();
    REQUIRE(invalidSource);
    const auto quiet = Run(std::array<std::string_view, 5>{"--manifest", manifest, "build", "--all", "--quiet"});
    CHECK(quiet.exitCode == 1);
    CHECK(CountOccurrences(quiet.output, "name 'Missing' is not defined in this scope") == 16);
    CHECK_FALSE(quiet.output.contains("Build matrix"));
    CHECK_FALSE(quiet.output.contains("Aggregate statistics:"));
    CHECK_FALSE(quiet.output.contains("Compiling"));

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

TEST_CASE("build --stats reports each section once and stays silent when quiet") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = System::TempDirectory() / ("rux-build-stats-" + std::to_string(nonce));
    const auto manifestPath = root / "Rux.toml";
    const auto sourcePath = root / "Src" / "Main.rux";
    std::filesystem::create_directories(sourcePath.parent_path());
    std::ofstream manifestFile(manifestPath, std::ios::binary);
    manifestFile << R"([Manifest]
Version = 1

[Package]
Name = "StatsFixture"
Version = "0.2.0"
Type = "Executable"
)";
    manifestFile.close();
    std::ofstream source(sourcePath, std::ios::binary);
    source << "func Main() -> int { return 0; }\n";
    source.close();
    REQUIRE(manifestFile);
    REQUIRE(source);

    const auto manifest = manifestPath.string();
    const auto reported =
        Run(std::array<std::string_view, 5>{"--manifest", manifest, "--color=never", "build", "--stats"});

    CAPTURE(reported.output);
    CHECK(reported.exitCode == 0);
    // The report opens with the same progress and status lines a plain build
    // prints: --stats adds sections, it does not replace the summary.
    const auto host = Driver::TargetDisplayName(Rux::Target::TargetTriple::Host());
    CHECK(reported.output.contains("Compiling StatsFixture v0.2.0 (Debug, " + host + ")"));
    CHECK(reported.output.contains("Built StatsFixture (Debug, " + host + ") in "));
    CHECK(reported.output.contains("  Package:  StatsFixture v0.2.0"));
    CHECK(reported.output.contains("Time:"));
    CHECK(reported.output.contains("Source:"));
    CHECK(reported.output.contains("Optimization:"));
    CHECK(reported.output.contains("Performance:"));
    // Machine-facing IDs stay out of report prose.
    CHECK_FALSE(reported.output.contains(Driver::HostTargetTriple()));

    const auto quiet =
        Run(std::array<std::string_view, 6>{"--manifest", manifest, "--color=never", "build", "--stats", "--quiet"});
    CHECK(quiet.exitCode == 0);
    CHECK(quiet.output.empty());

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

TEST_CASE("clean removes only the configured output root and Temp tree") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = System::TempDirectory() / ("rux-clean-test-" + std::to_string(nonce));
    const auto manifestPath = root / "Rux.toml";
    std::filesystem::create_directories(root / "Artifacts" / "Release" /
                                        Driver::TargetOutputPath(Target::TargetTriple::Host()));
    std::filesystem::create_directories(root / "Temp" / "Obj");
    std::filesystem::create_directories(root / "Keep");
    std::ofstream manifestFile(manifestPath, std::ios::binary);
    manifestFile << R"([Manifest]
Version = 1

[Package]
Name = "CleanTest"
Version = "0.1.0"
Type = "Executable"

[Build]
Output = "Artifacts"
)";
    manifestFile.close();
    std::ofstream sentinel(root / "Keep" / "sentinel", std::ios::binary);
    sentinel << "keep";
    sentinel.close();
    REQUIRE(manifestFile);
    REQUIRE(sentinel);

    const auto manifest = manifestPath.string();
    const auto tempOnly =
        Run(std::array<std::string_view, 5>{"--manifest", manifest, "--color=never", "clean", "--temp"});
    CHECK(tempOnly.exitCode == 0);
    CHECK(tempOnly.output.contains("Removed 1 directory in "));
    CHECK(tempOnly.output.contains("Path: Temp"));
    CHECK(std::filesystem::exists(root / "Artifacts"));
    CHECK_FALSE(std::filesystem::exists(root / "Temp"));

    const auto result = Run(std::array<std::string_view, 4>{"--manifest", manifest, "clean", "--quiet"});

    CHECK(result.exitCode == 0);
    CHECK(result.output.empty());
    CHECK_FALSE(std::filesystem::exists(root / "Artifacts"));
    CHECK_FALSE(std::filesystem::exists(root / "Temp"));
    CHECK(std::filesystem::is_regular_file(root / "Keep" / "sentinel"));
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

TEST_CASE("run is host-only while build and test accept a target") {
    const auto runHelp = Run(std::array<std::string_view, 3>{"help", "run", "--json"});
    CHECK(runHelp.exitCode == 0);
    CHECK_FALSE(runHelp.output.contains("--target <triple>"));
    const auto testHelp = Run(std::array<std::string_view, 3>{"help", "test", "--json"});
    CHECK(testHelp.exitCode == 0);
    CHECK(testHelp.output.contains("--target <triple>"));

    CHECK(Run(std::array<std::string_view, 2>{"run", "--target"}).exitCode == 2);
    const auto rejected = Run(std::array<std::string_view, 3>{"run", "--target", "linux-aarch64"});
    CHECK(rejected.exitCode == 2);
    CHECK(rejected.output.contains("command 'run' cannot execute target 'linux-aarch64'"));
    CHECK(rejected.output.contains("rux build --target linux-aarch64"));

    // An unknown triple is rejected by commands that still select targets.
    const auto manifest = ArithmeticManifest();
    for (const std::string_view command : {"build", "test"}) {
        const auto unknown =
            Run(std::array<std::string_view, 5>{"--manifest", manifest, command, "--target", "plan9-x86_64"});
        CHECK(unknown.exitCode == 1);
        CHECK(unknown.output.contains("target 'plan9-x86_64'"));
    }
}

TEST_CASE("invalid target input is rejected before manifest discovery") {
    const auto missing = (std::filesystem::path(RUX_ROOT_DIR) / "Tests" / "missing-Rux.toml").string();
    const auto result =
        Run(std::array<std::string_view, 5>{"--manifest", missing, "build", "--target", "plan9-x86_64"});

    CHECK(result.exitCode == 1);
    CHECK(result.output.contains("target 'plan9-x86_64' is not supported"));
    CHECK_FALSE(result.output.contains("manifest"));
}

TEST_CASE("check renders unsupported targets as structured diagnostics without changing JSON schema") {
    const auto missing = (std::filesystem::path(RUX_ROOT_DIR) / "Tests" / "missing-Rux.toml").string();
    const auto human = Run(std::array<std::string_view, 5>{"--manifest", missing, "check", "--target", "plan9-x86_64"});

    CHECK(human.exitCode == 1);
    CHECK(human.output.contains("error: target 'plan9-x86_64' is not supported"));
    CHECK(human.output.contains("  note: supported targets are "));
    CHECK(human.output.contains("  help: try 'rux check --target linux-x86_64'"));
    CHECK(human.output.contains("  docs: https://rux-lang.dev/docs/cli"));
    CHECK_FALSE(human.output.contains("manifest"));

    const auto json =
        Run(std::array<std::string_view, 6>{"--manifest", missing, "check", "--json", "--target", "plan9-x86_64"});
    CHECK(json.exitCode == 1);
    CHECK(json.output.contains("\"message\":\"target 'plan9-x86_64' is not supported\""));
    CHECK_FALSE(json.output.contains("supported targets are"));
    CHECK_FALSE(json.output.contains("documentationUrl"));
    CHECK_FALSE(json.output.contains("rux-lang.dev"));
}

TEST_CASE("check and lint render source frames while check JSON remains frame-free") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = System::TempDirectory() / ("rux-source-frame-test-" + std::to_string(nonce));
    const auto sourceDir = root / "Src";
    const auto manifestPath = root / "Rux.toml";
    const auto sourcePath = sourceDir / "Main.rux";
    std::error_code error;
    std::filesystem::create_directories(sourceDir, error);
    REQUIRE(!error);
    {
        std::ofstream manifest(manifestPath, std::ios::binary);
        manifest << "[Manifest]\nVersion = 1\n\n[Package]\nName = \"FrameTest\"\nVersion = \"0.1.0\"\n"
                    "Type = \"Executable\"\n";
        REQUIRE(manifest.good());
    }
    {
        std::ofstream source(sourcePath, std::ios::binary);
        source << "func Main() -> int {\n    return Missing;\n}\n";
        REQUIRE(source.good());
    }

    const auto manifest = manifestPath.string();
    const auto check =
        Run(std::array<std::string_view, 5>{"--manifest", manifest, "check", "--quiet", "--color=never"});
    CAPTURE(check.output);
    CHECK(check.exitCode == 1);
    CHECK(check.output.contains("  2 |     return Missing;"));
    CHECK(check.output.contains("    |            ^"));

    const auto json = Run(std::array<std::string_view, 5>{"--manifest", manifest, "check", "--quiet", "--json"});
    CHECK(json.exitCode == 1);
    CHECK(json.output.contains("\"line\":2"));
    CHECK_FALSE(json.output.contains("return Missing"));
    CHECK_FALSE(json.output.contains('|'));
    CHECK_FALSE(json.output.contains('^'));

    {
        std::ofstream source(sourcePath, std::ios::binary | std::ios::trunc);
        source << "func bad_name() {}\n";
        REQUIRE(source.good());
    }
    const auto lint = Run(std::array<std::string_view, 5>{"--manifest", manifest, "lint", "--quiet", "--color=never"});
    CAPTURE(lint.output);
    CHECK(lint.exitCode == 0);
    CHECK(lint.output.contains("  1 | func bad_name() {}"));
    CHECK(lint.output.contains("    | ^"));

    std::filesystem::remove_all(root, error);
    CHECK(!error);
}

TEST_CASE("CLI checks and builds the canonical macOS AArch64 target through its ARM64 alias") {
    const auto manifest = ArithmeticManifest();
    const auto check =
        Run(std::array<std::string_view, 6>{"--manifest", manifest, "check", "--target", "macos-arm64", "--quiet"});
    REQUIRE(check.exitCode == 0);

    const auto result = Run(std::array<std::string_view, 7>{"--manifest", manifest, "build", "--release", "--target",
                                                            "macos-arm64", "--quiet"});

    REQUIRE(result.exitCode == 0);
    CHECK_FALSE(result.output.contains("cannot run"));

    const auto output = std::filesystem::path(RUX_ROOT_DIR) / "Bin" / "Tests" / "Language" / "Release" / "macOS" /
                        "AArch64" / "Arithmetic";
    REQUIRE(std::filesystem::is_regular_file(output));

    std::ifstream input(output, std::ios::binary);
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    Testing::MachOImage image;
    std::string error;
    const bool parsed = Testing::ReadMachO64(bytes, image, error);
    CAPTURE(error);
    REQUIRE(parsed);
    CHECK(image.Architecture() == Testing::MachOArchitecture::AArch64);
    CHECK(image.fileType == 2); // MH_EXECUTE
    REQUIRE(image.codeSignature);
    CHECK(image.codeSignature->offset + image.codeSignature->size == bytes.size());

    const auto rejected = Run(std::array<std::string_view, 3>{"run", "--target", "macos-aarch64"});
    CHECK(rejected.exitCode == 2);
    CHECK(rejected.output.contains("command 'run' cannot execute target 'macos-aarch64'"));
}

TEST_CASE("CLI checks and builds the canonical FreeBSD AArch64 target through its ARM64 alias") {
    const auto manifest = ArithmeticManifest();
    const auto check =
        Run(std::array<std::string_view, 6>{"--manifest", manifest, "check", "--target", "freebsd-arm64", "--quiet"});
    REQUIRE(check.exitCode == 0);

    const auto result = Run(std::array<std::string_view, 7>{"--manifest", manifest, "build", "--release", "--target",
                                                            "freebsd-arm64", "--quiet"});

    REQUIRE(result.exitCode == 0);
    CHECK_FALSE(result.output.contains("not implemented"));
    CHECK_FALSE(result.output.contains("cannot run"));

    const auto output = std::filesystem::path(RUX_ROOT_DIR) / "Bin" / "Tests" / "Language" / "Release" / "FreeBSD" /
                        "AArch64" / "Arithmetic";
    REQUIRE(std::filesystem::is_regular_file(output));

    Testing::ElfImage image{ReadBinaryFile(output)};
    REQUIRE(image.bytes.size() >= 64);
    CHECK(image.OsAbi() == 9);     // ELFOSABI_FREEBSD
    CHECK(image.Type() == 2);      // ET_EXEC
    CHECK(image.Machine() == 183); // EM_AARCH64
    CHECK(image.Entry() != 0);
    CHECK(image.Interpreter().empty());
}

TEST_CASE("target tests reject a foreign operating system before discovering packages") {
    const std::string foreignTarget =
        Driver::HostTargetTriple().starts_with("windows-") ? "linux-x86_64" : "windows-x86_64";
    const auto result = Run(std::array<std::string_view, 3>{"test", "--target", foreignTarget});

    CHECK(result.exitCode == 1);
    CHECK(result.output.contains("target '" + foreignTarget + "' cannot be executed on host"));
    CHECK(result.output.contains("can build and check target '" + foreignTarget + "'"));
    CHECK(result.output.contains("rux build --target " + foreignTarget));
    CHECK(result.output.contains("rux check --target " + foreignTarget));
    CHECK(result.output.contains("rux test --target " + foreignTarget));
    CHECK(result.output.contains("on a native '" + foreignTarget + "' host"));
    CHECK_FALSE(result.output.contains("Running "));
}

TEST_CASE("CLI cross-builds a Windows AArch64 executable without trying to run it") {
    const auto manifest = ArithmeticManifest();
    const auto result = Run(std::array<std::string_view, 7>{"--manifest", manifest, "build", "--release", "--target",
                                                            "windows-aarch64", "--quiet"});

    REQUIRE(result.exitCode == 0);
    CHECK_FALSE(result.output.contains("cannot run"));

    const auto output = std::filesystem::path(RUX_ROOT_DIR) / "Bin" / "Tests" / "Language" / "Release" / "Windows" /
                        "AArch64" / "Arithmetic.exe";
    REQUIRE(std::filesystem::is_regular_file(output));

    std::ifstream input(output, std::ios::binary);
    const std::vector<unsigned char> image((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    REQUIRE(image.size() >= 0x40);
    REQUIRE(image[0] == 'M');
    REQUIRE(image[1] == 'Z');
    const std::size_t peOffset = Read32(image, 0x3C);
    REQUIRE(peOffset + 6 <= image.size());
    CHECK(Read32(image, peOffset) == 0x00004550); // PE\0\0
    CHECK(Read16(image, peOffset + 4) == 0xAA64); // IMAGE_FILE_MACHINE_ARM64
}
