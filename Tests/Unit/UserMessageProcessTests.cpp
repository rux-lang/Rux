// Final process-level user-message contracts that span several CLI commands.

#include "System/Json.h"
#include "System/Os.h"
#include "System/Process.h"

#include <array>
#include <doctest.h>
#include <filesystem>
#include <string>
#include <string_view>

using namespace Rux;

namespace {
std::filesystem::path RuxExecutable() {
    return std::filesystem::path(RUX_ROOT_DIR) / "Bin" / System::ExecutableFileName("rux");
}

std::string ArithmeticManifest() {
    return (std::filesystem::path(RUX_ROOT_DIR) / "Tests" / "Language" / "Arithmetic" / "Rux.toml").string();
}

template <std::size_t N>
System::RunResult Run(const std::array<std::string_view, N> &arguments) {
    const auto result = System::RunCaptured(RuxExecutable(), arguments);
    REQUIRE(result.has_value());
    return *result;
}
} // namespace

TEST_CASE("every CLI JSON mode stays parseable and ANSI-free across presentation flags") {
    const auto manifest = ArithmeticManifest();
    const auto help = Run(std::array<std::string_view, 4>{"--color=always", "--verbose", "help", "--json"});
    const auto check =
        Run(std::array<std::string_view, 6>{"--color=always", "--quiet", "--manifest", manifest, "check", "--json"});
    const auto info =
        Run(std::array<std::string_view, 6>{"--color=always", "--verbose", "--manifest", manifest, "info", "--json"});
    const auto failed = Run(std::array<std::string_view, 8>{"--color=always", "--verbose", "--manifest", manifest,
                                                            "check", "--json", "--target", "plan9-x86_64"});

    for (const auto *result : {&help, &check, &info, &failed}) {
        CAPTURE(result->output);
        CHECK(System::ParseJson(result->output).has_value());
        CHECK_FALSE(result->output.contains('\033'));
    }
    CHECK(help.exitCode == 0);
    CHECK(check.exitCode == 0);
    CHECK(info.exitCode == 0);
    CHECK(failed.exitCode == 1);
}
