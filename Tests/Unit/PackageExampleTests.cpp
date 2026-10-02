// Every fenced Rux block in a first-party package README, compiled by the real compiler.
//
// Documentation examples are the one part of a package nothing else checks. `rux check` never sees a README, the
// documentation-style suite reads declarations rather than prose, and a reader is otherwise the first to discover
// that a snippet does not build. Five did not: an `import Core::#target;` that does not parse, repeated in all four
// platform READMEs; two Algorithms examples written against a pointer-and-length API that slices replaced; a C
// example passing a literal where a `*char8` is wanted; an Io example omitting a required type argument and passing
// a concrete stream where an interface is expected; and a Collections example whose `Debug` bound cannot be
// satisfied without importing `Rux/Format`. Nothing in the suite would have caught any of them.
//
// A fenced block is one of four things, and only the first three are the compiler's business.
//
//   * A whole program, recognized by its own `func Main`.
//   * A run of statements, which goes inside a generated `Main`.
//   * Declarations with nothing to run, which get an empty `Main`.
//   * A fragment leaning on names the surrounding prose supplies, which cannot compile alone and never claimed to.
//
// A fragment cannot be recognized from what the compiler says. Any block that leans on a name would report an
// undefined one, so a rule keyed on that diagnostic excuses exactly the regression worth catching: an example that
// comes to name something the package no longer has. The fragments are therefore named in PackageExampleSupport.cpp,
// and the list may only shrink — a named block that starts compiling fails, because it has become an example and must
// be checked as one.

#include "PackageExampleSupport.h"
#include "System/Os.h"
#include "System/Process.h"

#include <algorithm>
#include <doctest.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using namespace Rux;
using namespace Rux::Testing::PackageExamples;

TEST_CASE("every package README example compiles") {
    const auto compiler = std::filesystem::path(RUX_ROOT_DIR) / "Bin" / System::ExecutableFileName("rux");
    REQUIRE_MESSAGE(std::filesystem::exists(compiler), "the compiler has to be built before its examples are checked");

    const auto root = std::filesystem::path(RUX_TEST_BIN_DIR) / "rux-readme-examples";
    std::size_t compilable = 0;
    std::size_t signatures = 0;
    std::vector<std::string> seenFragments;
    for (const ReadmeExample &example : MaterializeReadmeExamples(root, signatures)) {
        const auto manifest = example.manifest.string();
        std::vector<std::string_view> arguments{"--manifest", manifest, "--color=never", "check"};
        if (!example.target.empty()) {
            arguments.emplace_back("--target");
            arguments.push_back(example.target);
        }
        const auto result = System::RunCaptured(compiler, arguments);
        REQUIRE(result.has_value());
        INFO("Packages/", example.package, "/README.md, block ", example.block, " read as a ", Describe(example.kind),
             "\n", example.source, "\n", result->output);

        if (example.fragment) {
            seenFragments.push_back(example.name);
            CHECK_MESSAGE(result->exitCode != 0, example.name,
                          " compiles on its own, so it is an example rather than prose context. Remove it from "
                          "Fragments so that it is checked as one.");
            continue;
        }
        ++compilable;
        CHECK(result->exitCode == 0);
    }

    // A block count is reported rather than asserted: a new example must not have to be registered anywhere, and a
    // README that loses one must not fail for having done so. The fragment names are the exception, because each
    // one excuses a failure and an entry excusing nothing hides the next one.
    MESSAGE("compiled ", compilable, " blocks; ", seenFragments.size(), " fragments need prose context; ", signatures,
            " illustrate a signature");
    CHECK(compilable > 0);
    std::ranges::sort(seenFragments);
    for (const std::string_view name : Fragments()) {
        CHECK_MESSAGE(std::ranges::binary_search(seenFragments, name), name,
                      " names no README block. Remove it from Fragments.");
    }
    std::filesystem::remove_all(root);
}
