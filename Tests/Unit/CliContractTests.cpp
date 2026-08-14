#include "Cli/CliHelpRenderer.h"
#include "Cli/CliSpec.h"

#include <algorithm>
#include <doctest.h>
#include <format>
#include <ranges>
#include <string>
#include <string_view>

using namespace Rux;
using namespace Rux::CliContract;

namespace {
const OptionSpec *FindOption(const CommandSpec &command, const std::string_view spelling) {
    const auto it = std::ranges::find_if(command.options,
                                         [&](const OptionSpec &option) { return OptionMatches(option, spelling); });
    return it == command.options.end() ? nullptr : &*it;
}
} // namespace

TEST_CASE("CLI contract owns positional limits and value-taking options") {
    const auto *add = FindCommand("add");
    const auto *build = FindCommand("build");
    const auto *run = FindCommand("run");
    REQUIRE(add != nullptr);
    REQUIRE(build != nullptr);
    REQUIRE(run != nullptr);

    CHECK(PositionalsFor(add->name).minimum == 1);
    CHECK(PositionalsFor(add->name).maximum == 1);
    CHECK(PositionalsFor(build->name).maximum == 0);
    CHECK(PositionalsFor(run->name).acceptsPassthrough);

    const auto *target = FindOption(*build, "--target");
    const auto *release = FindOption(*build, "--release");
    REQUIRE(target != nullptr);
    REQUIRE(release != nullptr);
    CHECK(OptionTakesValue(*target));
    CHECK_FALSE(OptionTakesValue(*release));
    CHECK(PreferredOptionName(*target) == "--target");
}

TEST_CASE("CLI contract is the source of build option conflicts") {
    const auto *build = FindCommand("build");
    REQUIRE(build != nullptr);

    std::size_t allConflicts = 0;
    for (const auto &[left, right] : build->conflicts) {
        REQUIRE(FindOption(*build, left) != nullptr);
        REQUIRE(FindOption(*build, right) != nullptr);
        if (left == "--all") {
            ++allConflicts;
            CHECK((right == "--target" || right == "--debug" || right == "--release" || right == "--emit"));
        }
    }
    CHECK(allConflicts == 4);
}

TEST_CASE("CLI help rendering independently controls wrapping color and examples") {
    const auto *check = FindCommand("check");
    const auto *build = FindCommand("build");
    REQUIRE(check != nullptr);
    REQUIRE(build != nullptr);

    CHECK(CliHelp::NormalizeTerminalWidth(0) == 80);
    CHECK(CliHelp::NormalizeTerminalWidth(20) == 48);
    const auto wrapped = CliHelp::RenderCommand(*check, 48, false);
    CHECK_FALSE(wrapped.contains("safety flaws without emitting compilation binaries."));
    CHECK(wrapped.contains("emitting compilation binaries.\n"));
    CHECK_FALSE(wrapped.contains("\033["));

    const auto colored = CliHelp::RenderCommand(*build, 80, true);
    CHECK(colored.contains("\033[1mUsage:\033[0m"));
    CHECK(colored.contains("\033[36m--all"));
    for (const auto example : build->examples) {
        const auto commandLine = std::format("rux build{}{}", example.empty() ? "" : " ", example);
        CHECK(colored.contains(commandLine));
    }
}

TEST_CASE("CLI JSON rendering consumes the same command and option data") {
    const auto *build = FindCommand("build");
    REQUIRE(build != nullptr);
    const auto json = CliHelp::RenderJson("test-version", "build");

    CHECK(json.starts_with("{\"schemaVersion\":1"));
    CHECK(json.contains("\"version\":\"test-version\""));
    CHECK(json.contains("\"name\":\"build\""));
    for (const auto &option : build->options) {
        CHECK(json.contains("\"flags\":\"" + std::string(option.flags) + "\""));
    }
    CHECK(json.contains("\"flags\":\"--target <triple>\",\"description\":"));
    CHECK(json.contains("\"takesValue\":true"));
}
