#include "BuildInfo/CompilerMetadata.h"
#include "Driver/BuildTarget.h"
#include "ElfReader.h"
#include "MachOReader.h"
#include "System/Os.h"
#include "System/Process.h"

#include <array>
#include <chrono>
#include <doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using namespace Rux;

namespace {
std::filesystem::path RuxExecutable() {
    return std::filesystem::path(RUX_ROOT_DIR) / "Bin" / System::ExecutableFileName("rux");
}

// A checked-in package to point option-handling checks at, so they do not
// depend on the directory the test binary happens to run from.
std::string ArithmeticManifest() {
    return (std::filesystem::path(RUX_ROOT_DIR) / "Tests" / "Language" / "Arithmetic" / "Rux.toml").string();
}

template <std::size_t N>
System::RunResult Run(const std::array<std::string_view, N> &arguments) {
    const auto result = System::RunCaptured(RuxExecutable(), arguments);
    REQUIRE(result.has_value());
    return *result;
}

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

std::size_t CountOccurrences(const std::string_view text, const std::string_view needle) {
    std::size_t count = 0;
    std::size_t offset = 0;
    while ((offset = text.find(needle, offset)) != std::string_view::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}
} // namespace

TEST_CASE("help JSON publishes the stable 0.4 command contract") {
    const auto result = Run(std::array<std::string_view, 2>{"help", "--json"});
    CHECK(result.exitCode == 0);
    CHECK(result.output.contains("\"schemaVersion\":1"));
    CHECK(result.output.contains("\"version\":\"" + std::string(CompilerBuild::compilerVersion) + "\""));
    constexpr std::array commands = {"add",     "build",   "check", "clean", "doc",       "fmt",    "help",   "info",
                                     "init",    "install", "lint",  "list",  "login",     "logout", "new",    "pack",
                                     "publish", "remove",  "run",   "test",  "uninstall", "update", "version"};
    for (const std::string_view command : commands) {
        CHECK(result.output.contains("\"name\":\"" + std::string(command) + "\""));
    }
    CHECK_FALSE(result.output.contains("buildDate"));
    CHECK_FALSE(result.output.contains("buildTime"));
}

TEST_CASE("CLI usage failures return 2 and suggest close matches") {
    const auto unknown = Run(std::array<std::string_view, 1>{"bild"});
    CHECK(unknown.exitCode == 2);
    CHECK(unknown.output.contains("Did you mean 'build'"));

    CHECK(Run(std::array<std::string_view, 2>{"build", "--target"}).exitCode == 2);
    CHECK(Run(std::array<std::string_view, 2>{"build", "--color=sometimes"}).exitCode == 2);
    CHECK(Run(std::array<std::string_view, 3>{"build", "--quiet", "--verbose"}).exitCode == 2);
    const auto profileConflict = Run(std::array<std::string_view, 3>{"build", "--debug", "--release"});
    CHECK(profileConflict.exitCode == 2);
    CHECK(profileConflict.output.contains("options '--debug' and '--release' cannot be used together"));
    CHECK(Run(std::array<std::string_view, 2>{"fmt", "extra"}).exitCode == 2);
}

TEST_CASE("build --all conflicts are usage errors before manifest loading") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto missing =
        (System::TempDirectory() / ("rux-build-matrix-missing-" + std::to_string(nonce)) / "Rux.toml").string();

    const auto target =
        Run(std::array<std::string_view, 6>{"--manifest", missing, "build", "--all", "--target", "linux-aarch64"});
    CHECK(target.exitCode == 2);
    CHECK(target.output.contains("options '--all' and '--target' cannot be used together"));
    CHECK_FALSE(target.output.contains("specified manifest"));

    const auto debug = Run(std::array<std::string_view, 5>{"--manifest", missing, "build", "--all", "--debug"});
    CHECK(debug.exitCode == 2);
    CHECK(debug.output.contains("options '--all' and '--debug' cannot be used together"));
    CHECK_FALSE(debug.output.contains("specified manifest"));

    const auto release = Run(std::array<std::string_view, 5>{"--manifest", missing, "build", "--all", "--release"});
    CHECK(release.exitCode == 2);
    CHECK(release.output.contains("options '--all' and '--release' cannot be used together"));
    CHECK_FALSE(release.output.contains("specified manifest"));

    const auto emit = Run(std::array<std::string_view, 6>{"--manifest", missing, "build", "--all", "--emit", "lir"});
    CHECK(emit.exitCode == 2);
    CHECK(emit.output.contains("options '--all' and '--emit' cannot be used together"));
    CHECK_FALSE(emit.output.contains("specified manifest"));
}

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
    CHECK(reported.output.contains("Built   Debug    freebsd-x86_64"));
    CHECK(reported.output.contains("Failed  Debug    freebsd-aarch64"));
    CHECK(reported.output.contains("Built   Release  windows-x86_64"));
    CHECK(reported.output.contains("Failed  Release  windows-aarch64"));
    CHECK(reported.output.contains("16 cells: 8 succeeded, 8 failed"));
    CHECK(reported.output.contains("Aggregate statistics:"));
    CHECK_FALSE(reported.output.contains("\033["));

    for (const std::string_view profile : {"Debug", "Release"}) {
        for (const std::string_view target : {"freebsd-x86_64", "linux-x86_64", "macos-x86_64", "windows-x86_64"}) {
            const auto fileName = target.starts_with("windows-") ? "MatrixFixture.exe" : "MatrixFixture";
            CHECK(std::filesystem::is_regular_file(root / "Artifacts" / profile / target / fileName));
        }
    }

    std::ofstream invalidSource(sourcePath, std::ios::binary | std::ios::trunc);
    invalidSource << "func Main() -> int { return Missing; }\n";
    invalidSource.close();
    REQUIRE(invalidSource);
    const auto quiet = Run(std::array<std::string_view, 5>{"--manifest", manifest, "build", "--all", "--quiet"});
    CHECK(quiet.exitCode == 1);
    CHECK(CountOccurrences(quiet.output, "undefined name 'Missing'") == 16);
    CHECK_FALSE(quiet.output.contains("Build matrix"));
    CHECK_FALSE(quiet.output.contains("Aggregate statistics:"));
    CHECK_FALSE(quiet.output.contains("Compiling"));

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

TEST_CASE("clean removes only the configured output root and Temp tree") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = System::TempDirectory() / ("rux-clean-test-" + std::to_string(nonce));
    const auto manifestPath = root / "Rux.toml";
    std::filesystem::create_directories(root / "Artifacts" / "Release" / Driver::HostTargetTriple());
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
    const auto result = Run(std::array<std::string_view, 4>{"--manifest", manifest, "clean", "--quiet"});

    CHECK(result.exitCode == 0);
    CHECK_FALSE(std::filesystem::exists(root / "Artifacts"));
    CHECK_FALSE(std::filesystem::exists(root / "Temp"));
    CHECK(std::filesystem::is_regular_file(root / "Keep" / "sentinel"));
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

TEST_CASE("documentation and source archives use the configured raw output root") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = System::TempDirectory() / ("rux-raw-output-test-" + std::to_string(nonce));
    const auto manifestPath = root / "Rux.toml";
    std::filesystem::create_directories(root / "Src");
    std::ofstream manifestFile(manifestPath, std::ios::binary);
    manifestFile << R"([Manifest]
Version = 1
MinRux = "0.4.0"

[Package]
Namespace = "Acme"
Name = "Widget"
Version = "1.2.3"
Type = "SourceLibrary"

[Build]
Output = "Artifacts"
)";
    manifestFile.close();
    std::ofstream source(root / "Src" / "Widget.rux", std::ios::binary);
    source << "module Widget {\n    pub func Answer() -> int { return 42; }\n}\n";
    source.close();
    REQUIRE(manifestFile);
    REQUIRE(source);

    const auto manifest = manifestPath.string();
    const auto pack = Run(std::array<std::string_view, 4>{"--manifest", manifest, "--quiet", "pack"});
    const auto doc = Run(std::array<std::string_view, 4>{"--manifest", manifest, "--quiet", "doc"});

    CHECK(pack.exitCode == 0);
    CHECK(doc.exitCode == 0);
    CHECK(std::filesystem::is_regular_file(root / "Artifacts" / "Widget-1.2.3.ruxpkg"));
    CHECK(std::filesystem::is_regular_file(root / "Artifacts" / "Docs" / "index.html"));
    CHECK_FALSE(std::filesystem::exists(root / "Artifacts" / "Debug"));
    CHECK_FALSE(std::filesystem::exists(root / "Artifacts" / "Release"));
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

TEST_CASE("command help accepts equals options and reports one command") {
    const auto result = Run(std::array<std::string_view, 3>{"help", "build", "--json"});
    CHECK(result.exitCode == 0);
    CHECK(result.output.contains("\"name\":\"build\""));
    CHECK_FALSE(result.output.contains("\"name\":\"check\""));
    CHECK(result.output.contains("--emit <kind[,kind...]>"));
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
    CHECK(rejected.output.contains("unknown option '--target' for command 'run'"));

    // An unknown triple is rejected by commands that still select targets.
    const auto manifest = ArithmeticManifest();
    for (const std::string_view command : {"build", "test"}) {
        const auto unknown =
            Run(std::array<std::string_view, 5>{"--manifest", manifest, command, "--target", "plan9-x86_64"});
        CHECK(unknown.exitCode == 1);
        CHECK(unknown.output.contains("unsupported target 'plan9-x86_64'"));
    }
}

TEST_CASE("invalid target input is rejected before manifest discovery") {
    const auto missing = (std::filesystem::path(RUX_ROOT_DIR) / "Tests" / "missing-Rux.toml").string();
    const auto result =
        Run(std::array<std::string_view, 5>{"--manifest", missing, "build", "--target", "plan9-x86_64"});

    CHECK(result.exitCode == 1);
    CHECK(result.output.contains("unsupported target 'plan9-x86_64'"));
    CHECK_FALSE(result.output.contains("manifest"));
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

    const auto output =
        std::filesystem::path(RUX_ROOT_DIR) / "Bin" / "Tests" / "Language" / "Release" / "macos-aarch64" / "Arithmetic";
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
    CHECK(rejected.output.contains("unknown option '--target' for command 'run'"));
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

    const auto output = std::filesystem::path(RUX_ROOT_DIR) / "Bin" / "Tests" / "Language" / "Release" /
                        "freebsd-aarch64" / "Arithmetic";
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
    CHECK(result.output.contains("test suite cannot execute on this host"));
    CHECK(result.output.contains("rux build --target " + foreignTarget));
    CHECK(result.output.contains("then test on a native"));
    CHECK_FALSE(result.output.contains("Running "));
}

TEST_CASE("CLI cross-builds a Windows AArch64 executable without trying to run it") {
    const auto manifest = ArithmeticManifest();
    const auto result = Run(std::array<std::string_view, 7>{"--manifest", manifest, "build", "--release", "--target",
                                                            "windows-aarch64", "--quiet"});

    REQUIRE(result.exitCode == 0);
    CHECK_FALSE(result.output.contains("cannot run"));

    const auto output = std::filesystem::path(RUX_ROOT_DIR) / "Bin" / "Tests" / "Language" / "Release" /
                        "windows-aarch64" / "Arithmetic.exe";
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

TEST_CASE("structured commands emit a complete JSON document on operational failure") {
    const auto missing = (std::filesystem::path(RUX_ROOT_DIR) / "Tests" / "missing-Rux.toml").string();
    const auto check = Run(std::array<std::string_view, 4>{"check", "--json", "--manifest", missing});
    CHECK(check.exitCode == 1);
    CHECK(check.output.contains("\"success\": false"));
    CHECK(check.output.contains("\"diagnostics\": ["));

    const auto info = Run(std::array<std::string_view, 4>{"info", "--json", "--manifest", missing});
    CHECK(info.exitCode == 1);
    CHECK(info.output.contains("{\"success\":false,\"error\":"));
}

TEST_CASE("human help honors color mode while JSON stays unstyled") {
    const auto colored = Run(std::array<std::string_view, 2>{"help", "--color=always"});
    CHECK(colored.exitCode == 0);
    CHECK(colored.output.contains("\033[1m\033[36mRux\033[0m"));
    CHECK(colored.output.contains("\033[36mbuild"));

    const auto plain = Run(std::array<std::string_view, 2>{"help", "--color=never"});
    CHECK(plain.exitCode == 0);
    CHECK_FALSE(plain.output.contains("\033["));

    const auto command = Run(std::array<std::string_view, 3>{"help", "build", "--color=always"});
    CHECK(command.exitCode == 0);
    CHECK(command.output.contains("\033[1mUsage:\033[0m"));
    CHECK(command.output.contains("\033[36m--emit <kind[,kind...]>"));

    const auto json = Run(std::array<std::string_view, 3>{"help", "--json", "--color=always"});
    CHECK(json.exitCode == 0);
    CHECK_FALSE(json.output.contains("\033["));
    CHECK(json.output.starts_with("{\"schemaVersion\":1"));
}
