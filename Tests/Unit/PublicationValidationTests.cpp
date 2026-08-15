#include "Cli/PublicationProblem.h"
#include "Package/PublicationValidation.h"

#include <doctest.h>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

using namespace Rux;

namespace {
Manifest Parsed(const std::string_view text) {
    auto result = Manifest::Parse(text, "Rux.toml");
    if (!result.Ok()) {
        REQUIRE_MESSAGE(result.Ok(), "expected the manifest to parse: ", result.diagnostics.front().message);
    }
    return std::move(*result.manifest);
}
} // namespace

TEST_CASE("Publication validation accepts a complete source package") {
    const Manifest manifest = Parsed(R"([Manifest]
Version = 1
MinRux = "0.4.0"

[Package]
Namespace = "Rux"
Name = "App"
Version = "1.2.3"
Type = "SourceLibrary"

[Dependencies]
Io = { Namespace = "Rux", Version = "^1.0.0" }
)");

    CHECK(ValidateForPublication(manifest).empty());
}

TEST_CASE("Publication validation preserves every package rejection message") {
    const Manifest manifest = Parsed(R"([Manifest]
Version = 1

[Package]
Name = "App"
Version = "1.2.3"
Type = "Executable"

[Dependencies]
Util = { Path = "../Util" }
)");

    CHECK(ValidateForPublication(manifest) ==
          std::vector<std::string>{"[Package].Type = \"Executable\" cannot be published by Rux 0.4.0; "
                                   "this release publishes only Type = \"SourceLibrary\"",
                                   "publication requires [Package].Namespace; a namespace-free package "
                                   "is local-only",
                                   "publication requires [Manifest].MinRux, the oldest compiler release "
                                   "that can build the package; it must be at least 0.4.0",
                                   "dependency 'Util' uses Path = \"../Util\"; publication requires "
                                   "registry dependencies"});
}

TEST_CASE("Publication validation preserves the minimum compiler rejection message") {
    const Manifest manifest = Parsed(R"([Manifest]
Version = 1
MinRux = "0.3.9"

[Package]
Namespace = "Rux"
Name = "App"
Version = "1.2.3"
Type = "SourceLibrary"
)");

    CHECK(ValidateForPublication(manifest) ==
          std::vector<std::string>{"[Manifest].MinRux is '0.3.9' but publication requires at least 0.4.0"});
}

TEST_CASE("Publication validation preserves the workspace rejection message") {
    const Manifest manifest = Parsed(R"([Manifest]
Version = 1

[Workspace]
Packages = ["Packages/Core"]
)");

    CHECK(ValidateForPublication(manifest) ==
          std::vector<std::string>{"a workspace cannot be published; publish a member package instead"});
}

TEST_CASE("Publication problem documents become labeled context and actionable guidance") {
    struct Case {
        std::string_view code;
        std::string_view expectedNote;
        std::string_view expectedHelp;
    };

    constexpr Case cases[] = {
        {"version_conflict", "published versions are immutable", "increment [Package].Version"},
        {"namespace_not_found", "namespace 'Rux' is not registered", "claim namespace 'Rux'"},
        {"publication_forbidden", "does not own or maintain namespace 'Rux'", "owner or maintainer token"},
        {"insufficient_scope", "lacks the 'publish' scope", "token with the 'publish' scope"},
        {"authentication_required", "was not accepted", "rux login --registry"},
        {"rate_limited", "rate-limiting publication requests", "wait briefly"},
    };

    for (const auto &entry : cases) {
        const auto problem = CliSupport::DescribePublicationProblem(
            entry.code == "rate_limited" ? 429U : 422U,
            std::format(R"({{"code":"{}","detail":"registry explanation"}})", entry.code), "Rux/Json", "1.2.0", "Rux",
            "https://registry.example", "RUX_TOKEN");
        CAPTURE(entry.code);
        CHECK(problem.error == "the registry rejected Rux/Json 1.2.0");
        CHECK(std::ranges::find(problem.notes, "registry: 'https://registry.example'") != problem.notes.end());
        CHECK(std::ranges::any_of(
            problem.notes, [](const std::string &note) { return note.starts_with("registry response status: "); }));
        CHECK(std::ranges::any_of(problem.notes,
                                  [&](const std::string &note) { return note.contains(entry.expectedNote); }));
        CHECK(problem.help.contains(entry.expectedHelp));
    }
}

TEST_CASE("Publication problem fallback retains unknown details as notes") {
    const auto problem = CliSupport::DescribePublicationProblem(
        503, R"({"detail":"temporarily unavailable","errors":[{"code":"archive","detail":"too large"},
             {"code":"policy"}]})",
        "Rux/Json", "1.2.0", "Rux", "https://registry.example", "credentials.toml");

    CHECK(problem.error == "the registry rejected Rux/Json 1.2.0");
    CHECK(std::ranges::find(problem.notes, "registry: 'https://registry.example'") != problem.notes.end());
    CHECK(std::ranges::find(problem.notes, "registry response status: 503") != problem.notes.end());
    CHECK(std::ranges::find(problem.notes, "registry detail: temporarily unavailable") != problem.notes.end());
    CHECK(std::ranges::find(problem.notes, "registry detail [archive]: too large") != problem.notes.end());
    CHECK(std::ranges::find(problem.notes, "registry detail: policy") != problem.notes.end());
    CHECK(problem.help.contains("review the registry details"));

    const auto empty = CliSupport::DescribePublicationProblem(500, {}, "Rux/Json", "1.2.0", "Rux",
                                                              "https://registry.example", "RUX_TOKEN");
    CHECK(empty.notes ==
          std::vector<std::string>{"registry: 'https://registry.example'", "registry response status: 500"});
    CHECK(empty.help.contains("retry publication"));
}
