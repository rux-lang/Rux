#include "Reporting/Reporting.h"

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

TEST_CASE("reporting remains a narrow dependency in the CMake component graph") {
    const auto cmakePath = std::filesystem::path(RUX_ROOT_DIR) / "Compiler" / "CMakeLists.txt";
    std::ifstream input(cmakePath);
    REQUIRE(input);
    const std::string cmake{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};

    const auto reporting = cmake.find("rux_add_component(RuxReporting");
    const auto target = cmake.find("rux_add_component(RuxTarget", reporting);
    REQUIRE(reporting != std::string::npos);
    REQUIRE(target != std::string::npos);
    const auto reportingBlock = cmake.substr(reporting, target - reporting);
    CHECK_FALSE(reportingBlock.contains("target_link_libraries"));

    CHECK(cmake.contains("PRIVATE RuxReporting"));
    CHECK(cmake.contains("RuxCliHelp PUBLIC RuxCliContract PRIVATE RuxDiagnostics RuxReporting"));
    CHECK(cmake.contains("RuxCliContract RuxCliHelp RuxCliReporting RuxReporting"));
    CHECK(cmake.contains("RuxCliReporting PUBLIC RuxReporting PRIVATE RuxSystem"));
}
