#include "Reporting/Reporting.h"

#include <array>
#include <chrono>
#include <doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace Rux::Reporting;

TEST_CASE("reporting durations use canonical units at every boundary") {
    using namespace std::chrono_literals;
    CHECK(FormatDuration(0ms) == "0 ms");
    CHECK(FormatDuration(1ms) == "1 ms");
    CHECK(FormatDuration(999ms) == "999 ms");
    CHECK(FormatDuration(1000ms) == "1 s");
    CHECK(FormatDuration(59'999ms) == "60 s");
    CHECK(FormatDuration(60'000ms) == "1 min 0 s");
    CHECK(FormatDuration(65'200ms) == "1 min 5.2 s");
}

TEST_CASE("reporting styles render semantic statuses in color or plain text") {
    const Style plain{false};
    CHECK(RenderStatus(StatusVerb::Compiling, plain) == "Compiling");
    CHECK(RenderStatus(StatusVerb::Built, plain) == "Built");
    CHECK(RenderStatus(StatusVerb::Failed, plain) == "Failed");

    const Style colored{true};
    CHECK(RenderStatus(StatusVerb::Compiling, colored) == "\033[36m\033[1mCompiling\033[0m");
    CHECK(RenderStatus(StatusVerb::Built, colored) == "\033[32m\033[1mBuilt\033[0m");
    CHECK(RenderStatus(StatusVerb::Failed, colored) == "\033[31m\033[1mFailed\033[0m");
    CHECK(colored.Color(StatusKind::Warning) == Ansi::yellow);
    CHECK(colored.Color(StatusKind::Error) == Ansi::red);
}

TEST_CASE("reporting status vocabulary distinguishes progress and outcomes") {
    CHECK(StatusText(StatusVerb::Checking) == "Checking");
    CHECK(StatusText(StatusVerb::Downloading) == "Downloading");
    CHECK(StatusText(StatusVerb::Checked) == "Checked");
    CHECK(StatusText(StatusVerb::Installed) == "Installed");
    CHECK(StatusText(StatusVerb::Removed) == "Removed");
    CHECK(KindOf(StatusVerb::Downloading) == StatusKind::Progress);
    CHECK(KindOf(StatusVerb::Installed) == StatusKind::Success);
}

TEST_CASE("reporting helpers indent detail and pluralize labels") {
    CHECK(Indent("Output: Bin/App") == "  Output: Bin/App");
    CHECK(Indent("one\ntwo", 2) == "    one\n    two");
    CHECK(Indent("already aligned", 0) == "already aligned");
    CHECK(Pluralize(0, "cell") == "cells");
    CHECK(Pluralize(1, "cell") == "cell");
    CHECK(Pluralize(2, "cell") == "cells");
    CHECK(Pluralize(2, "diagnostic", "diagnoses") == "diagnoses");
    CHECK(FormatCount(1, "file") == "1 file");
    CHECK(FormatCount(3, "file") == "3 files");
}

TEST_CASE("reporting rows align their label column to the block that holds them") {
    constexpr std::array rows = {TableRow{"Lexing", "0 ms"}, TableRow{"Emitting RCU objects", "1 ms"},
                                 TableRow{"Total", "22 ms"}};

    // The colon stays attached to its label and the padding follows it, so
    // what lines up is the value column rather than a column of colons.
    CHECK(RenderRows(rows) == "  Lexing:               0 ms\n"
                              "  Emitting RCU objects: 1 ms\n"
                              "  Total:                22 ms\n");

    // Right alignment squares up the far edge so magnitudes compare.
    CHECK(RenderRows(rows, ValueAlign::Right) == "  Lexing:                0 ms\n"
                                                 "  Emitting RCU objects:  1 ms\n"
                                                 "  Total:                22 ms\n");

    CHECK(RenderRows({}).empty());
}

TEST_CASE("reporting sections title a block and style only the title") {
    constexpr std::array rows = {TableRow{"Constants", "1"}};

    CHECK(RenderSection("Optimization", rows, Style{false}) == "Optimization:\n"
                                                               "  Constants: 1\n");
    CHECK(RenderSection("Optimization", rows, Style{true}) == "\033[1mOptimization\033[0m:\n"
                                                              "  Constants: 1\n");
    // A section with nothing to report is its own heading and nothing else,
    // so callers can decide to skip it rather than print an empty block.
    CHECK(RenderSection("Optimization", {}, Style{false}) == "Optimization:\n");
}

TEST_CASE("reporting remains a narrow dependency in the CMake component graph") {
    const auto component = [](const std::string_view name) {
        const auto path = std::filesystem::path(RUX_ROOT_DIR) / "Compiler" / name / "CMakeLists.txt";
        std::ifstream input(path);
        REQUIRE(input);
        return std::string{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    };
    const auto reporting = component("Reporting");
    CHECK(reporting.contains("rux_add_component(RuxReporting"));
    CHECK_FALSE(reporting.contains("target_link_libraries"));
    CHECK(component("Diagnostics").contains("PRIVATE RuxReporting"));
    const auto cli = component("Cli");
    CHECK(cli.contains("RuxCliHelp PUBLIC RuxCliContract PRIVATE RuxDiagnostics RuxReporting"));
    CHECK(cli.contains("RuxCliReporting PUBLIC RuxReporting RuxDriverModel PRIVATE RuxSystem"));
}
