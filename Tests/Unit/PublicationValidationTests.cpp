#include "Package/PublicationValidation.h"

#include <doctest.h>
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
