#include "Driver/BuildTarget.h"
#include "Driver/Version.h"
#include "System/Os.h"
#include "System/Process.h"

#include <array>
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
} // namespace

TEST_CASE("help JSON publishes the stable 0.4 command contract") {
    const auto result = Run(std::array<std::string_view, 2>{"help", "--json"});
    CHECK(result.exitCode == 0);
    CHECK(result.output.contains("\"schemaVersion\":1"));
    CHECK(result.output.contains("\"version\":\"" RUX_VERSION "\""));
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
    CHECK(Run(std::array<std::string_view, 2>{"fmt", "extra"}).exitCode == 2);
}

TEST_CASE("command help accepts equals options and reports one command") {
    const auto result = Run(std::array<std::string_view, 3>{"help", "build", "--json"});
    CHECK(result.exitCode == 0);
    CHECK(result.output.contains("\"name\":\"build\""));
    CHECK_FALSE(result.output.contains("\"name\":\"check\""));
    CHECK(result.output.contains("--emit <kind[,kind...]>"));
}

TEST_CASE("build, run and test share one target option") {
    for (const std::string_view command : {"run", "test"}) {
        const auto json = Run(std::array<std::string_view, 3>{"help", command, "--json"});
        CHECK(json.exitCode == 0);
        CHECK(json.output.contains("--target <triple>"));
    }
    // An unknown triple is rejected the same way whichever command names it.
    const auto manifest = ArithmeticManifest();
    for (const std::string_view command : {"build", "run", "test"}) {
        const auto unknown =
            Run(std::array<std::string_view, 5>{"--manifest", manifest, command, "--target", "plan9-x86_64"});
        CHECK(unknown.exitCode == 1);
        CHECK(unknown.output.contains("unsupported target 'plan9-x86_64'"));
    }
}

TEST_CASE("a target without a back end names itself") {
    // AArch64 Mach-O is refused before the build starts, on every host.
    const auto manifest = ArithmeticManifest();
    const auto result =
        Run(std::array<std::string_view, 5>{"--manifest", manifest, "build", "--target", "macos-aarch64"});

    CHECK(result.exitCode == 1);
    CHECK(result.output.contains("macos-aarch64"));
    CHECK(result.output.contains("not implemented yet"));
}

TEST_CASE("CLI cross-builds a Windows AArch64 executable without trying to run it") {
    const auto manifest = ArithmeticManifest();
    const auto result = Run(std::array<std::string_view, 7>{"--manifest", manifest, "build", "--release", "--target",
                                                            "windows-aarch64", "--quiet"});

    REQUIRE(result.exitCode == 0);
    CHECK_FALSE(result.output.contains("cannot run"));

    auto output = std::filesystem::path(RUX_ROOT_DIR) / "Bin" / "Tests" / "Language" / "Release";
    if (Driver::HostTargetTriple() != "windows-aarch64") {
        output /= "windows-aarch64";
    }
    output /= "Arithmetic.exe";
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
