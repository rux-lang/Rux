#include "CliProcessTestSupport.h"

#include <regex>

using namespace Rux::Testing::CliProcessTestSupport;

TEST_CASE("test rejects invalid worker counts and documents the serial default") {
    for (const std::string_view value : {"0", "-1", "1x", "", "99999999999999999999999999"}) {
        const auto result = Run(std::array<std::string_view, 3>{"test", "--jobs", value});
        CHECK(result.exitCode != 0);
        CHECK(result.output.contains("requires a positive integer"));
    }
    CHECK(Run(std::array<std::string_view, 2>{"test", "--jobs"}).exitCode == 2);
    const auto help = Run(std::array<std::string_view, 2>{"test", "--help"});
    CHECK(help.output.contains("--jobs <N>"));
    CHECK(help.output.contains("default: 1"));
}

TEST_CASE("parallel package tests match serial reports when artifacts collide and tests fail") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::path(RUX_TEST_BIN_DIR) / ("rux-parallel-tests-" + std::to_string(nonce));
    const std::string package = "[Manifest]\nVersion = 1\n[Package]\nName = \"Same\"\n"
                                "Version = \"0.1.0\"\nType = \"Executable\"\n";
    WriteTextFile(root / "Rux.toml", package);
    for (unsigned index = 0; index < 4; ++index) {
        const auto test = root / "Tests" / std::to_string(index);
        // Every package writes the same executable. Its compile-and-run operation must remain indivisible.
        WriteTextFile(test / "Rux.toml", package + "[Build]\nOutput = \"../../Bin\"\n");
        WriteTextFile(test / "Src" / "Main.rux", std::format("func Main() -> int {{ return {}; }}\n", index % 2));
    }
    const std::string manifest = (root / "Rux.toml").string();
    auto RunJobs = [&](const std::string_view jobs) {
        return Run(std::array<std::string_view, 7>{"--manifest", manifest, "--color=never", "test", "--release",
                                                   "--jobs", jobs});
    };
    const auto serial = RunJobs("1");
    const auto parallel = RunJobs("4");
    CAPTURE(serial.output);
    CAPTURE(parallel.output);
    CHECK(serial.exitCode == 1);
    CHECK(parallel.exitCode == serial.exitCode);
    CHECK(parallel.output.contains("(2 passed, 2 failed)"));
    const std::regex durations(R"(in [0-9]+(?:\.[0-9]+)? (?:ms|s|min)(?: [0-9]+(?:\.[0-9]+)? s)?)");
    CHECK(std::regex_replace(NormalizeNewlines(serial.output), durations, "in <duration>") ==
          std::regex_replace(NormalizeNewlines(parallel.output), durations, "in <duration>"));
    const auto defaults =
        Run(std::array<std::string_view, 5>{"--manifest", manifest, "--color=never", "test", "--release"});
    CHECK(std::regex_replace(NormalizeNewlines(defaults.output), durations, "in <duration>") ==
          std::regex_replace(NormalizeNewlines(serial.output), durations, "in <duration>"));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    CHECK(!error);
}
