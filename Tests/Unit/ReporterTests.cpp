#include "Cli/Reporter.h"
#include "Cli/TerminalStyle.h"

#include <array>
#include <cstdio>
#include <doctest.h>
#include <memory>
#include <string>

using namespace Rux;
using namespace Rux::CliSupport;

namespace {
struct FileCloser {
    void operator()(std::FILE *file) const {
        if (file != nullptr) {
            std::fclose(file);
        }
    }
};

using CapturedFile = std::unique_ptr<std::FILE, FileCloser>;

CapturedFile Capture() {
    std::FILE *raw = nullptr;
#if defined(_WIN32)
    REQUIRE(::tmpfile_s(&raw) == 0);
#else
    raw = std::tmpfile();
#endif
    CapturedFile file(raw);
    REQUIRE(file != nullptr);
    return file;
}

std::string Read(std::FILE *file) {
    REQUIRE(std::fflush(file) == 0);
    REQUIRE(std::fseek(file, 0, SEEK_SET) == 0);
    std::string result;
    std::array<char, 256> buffer{};
    while (const auto read = std::fread(buffer.data(), 1, buffer.size(), file)) {
        result.append(buffer.data(), read);
    }
    return result;
}
} // namespace

TEST_CASE("CLI reporter renders every semantic message family consistently") {
    auto output = Capture();
    Reporter reporter(output.get(), {.color = ColorMode::Off});
    reporter.Error("the build failed");
    reporter.Warning("the cache is stale");
    reporter.Note("the manifest selected release mode");
    reporter.Help("run 'rux update'");
    reporter.Link("https://rux-lang.dev/cli/update");
    reporter.Progress("Compiling", "App v0.4.0");
    reporter.Success("Built", "App in 25 ms");
    reporter.Detail("Output: Bin/App");
    constexpr std::array table = {TableRow{"Passed", "2"}, TableRow{"Failed", "0"}};
    reporter.Table(table);
    reporter.Summary("Summary", table);

    CHECK(Read(output.get()) == "error: the build failed\n"
                                "warning: the cache is stale\n"
                                "  note: the manifest selected release mode\n"
                                "  help: run 'rux update'\n"
                                "  docs: https://rux-lang.dev/cli/update\n"
                                "Compiling App v0.4.0\n"
                                "Built App in 25 ms\n"
                                "  Output: Bin/App\n"
                                "  Passed: 2\n"
                                "  Failed: 0\n"
                                "Summary:\n"
                                "  Passed: 2\n"
                                "  Failed: 0\n");
}

TEST_CASE("CLI reporter applies quiet and verbose policy by message visibility") {
    SUBCASE("quiet retains diagnostics and corrective context") {
        auto output = Capture();
        Reporter reporter(output.get(), {.color = ColorMode::Off, .quiet = true, .verbose = true});
        reporter.Error("failed");
        reporter.Warning("unsafe");
        reporter.Note("context");
        reporter.Help("fix it");
        reporter.Link("https://rux-lang.dev/cli/");
        reporter.Progress("Checking", "App");
        reporter.Success("Checked", "App");
        reporter.Detail("ordinary detail");
        reporter.Verbose("phase detail");
        constexpr std::array rows = {TableRow{"Total", "1"}};
        reporter.Table(rows);
        reporter.Summary("Summary", rows);

        CHECK(Read(output.get()) == "error: failed\n"
                                    "warning: unsafe\n"
                                    "  note: context\n"
                                    "  help: fix it\n"
                                    "  docs: https://rux-lang.dev/cli/\n");
    }

    SUBCASE("normal output omits verbose-only detail") {
        auto output = Capture();
        Reporter reporter(output.get(), {.color = ColorMode::Off});
        reporter.Progress("Checking", "App");
        reporter.Detail("output path");
        reporter.Verbose("loaded manifest");
        CHECK(Read(output.get()) == "Checking App\n  output path\n");
    }

    SUBCASE("verbose output adds phase detail") {
        auto output = Capture();
        Reporter reporter(output.get(), {.color = ColorMode::Off, .verbose = true});
        reporter.Verbose("loaded manifest");
        CHECK(Read(output.get()) == "  loaded manifest\n");
    }
}

TEST_CASE("CLI reporter writes only to its explicit capture stream") {
    auto standardOutput = Capture();
    auto standardError = Capture();
    Reporter out(standardOutput.get(), {.color = ColorMode::Off});
    Reporter err(standardError.get(), {.color = ColorMode::Off});

    out.Success("Built", "App");
    err.Error("cannot build App");

    CHECK(Read(standardOutput.get()) == "Built App\n");
    CHECK(Read(standardError.get()) == "error: cannot build App\n");
}

TEST_CASE("CLI reporter color policy honors environment and redirection inputs") {
    // These model NO_COLOR and TERM=dumb independently of the unit-test
    // runner's own redirected stdout.
    CHECK(ResolveColor(ColorMode::Auto, true, false, false));
    CHECK_FALSE(ResolveColor(ColorMode::Auto, true, true, false));
    CHECK_FALSE(ResolveColor(ColorMode::Auto, true, false, true));
    CHECK_FALSE(ResolveColor(ColorMode::Auto, false, false, false));
    CHECK(ResolveColor(ColorMode::On, false, true, true));
    CHECK_FALSE(ResolveColor(ColorMode::Off, true, false, false));

    auto redirected = Capture();
    CHECK_FALSE(ColorEnabled(ColorMode::Auto, redirected.get()));
    CHECK(ColorEnabled(ColorMode::On, redirected.get()));
}

TEST_CASE("CLI reporter emits complete ANSI styles only when forced") {
    auto coloredOutput = Capture();
    Reporter colored(coloredOutput.get(), {.color = ColorMode::On});
    colored.Error("broken");
    colored.Progress("Compiling", "App");
    colored.Link("https://rux-lang.dev/cli/build");
    const auto styled = Read(coloredOutput.get());
    CHECK(styled == "\033[31m\033[1merror:\033[0m broken\n"
                    "\033[36m\033[1mCompiling\033[0m App\n"
                    "  \033[36m\033[1mdocs:\033[0m \033[36mhttps://rux-lang.dev/cli/build\033[0m\n");

    auto plainOutput = Capture();
    Reporter plain(plainOutput.get(), {.color = ColorMode::Off});
    plain.Error("broken");
    plain.Progress("Compiling", "App");
    const auto unstyled = Read(plainOutput.get());
    CHECK(unstyled == "error: broken\nCompiling App\n");
    CHECK_FALSE(unstyled.contains('\033'));
    CHECK_FALSE(unstyled.contains("[0m"));
}
