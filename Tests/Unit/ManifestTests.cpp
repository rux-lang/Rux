#include "Package/Manifest.h"
#include "System/Os.h"

#include <chrono>
#include <doctest.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace Rux;
using namespace Rux::System;

TEST_CASE("Manifest preserves author arrays and uses canonical assignment spacing") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root = TempDirectory() / ("rux-manifest-test-" + std::to_string(nonce));
    const std::filesystem::path path = root / "Rux.toml";
    std::filesystem::create_directories(root);

    {
        std::ofstream input(path);
        input << R"([Package]
Name = "App"
Version = "0.1.0"
Type = "bin"
Authors = ["Rux Contributors <info@rux-lang.dev>"]

[Build]
Output = "Bin"
)";
        REQUIRE(input.good());
    }

    const auto manifest = Manifest::Load(path);
    REQUIRE(manifest.has_value());
    CHECK(manifest->package.authors == std::vector<std::string>{"Rux Contributors <info@rux-lang.dev>"});

    Manifest updated = *manifest;
    REQUIRE(updated.AddDependency("Io", ""));
    REQUIRE(updated.Save(path));

    std::ifstream output(path);
    std::ostringstream contents;
    contents << output.rdbuf();
    const std::string expected = R"([Package]
Name = "App"
Version = "0.1.0"
Type = "bin"
Authors = ["Rux Contributors <info@rux-lang.dev>"]

[Dependencies]
Io = "*"

[Build]
Output = "Bin"
)";
    CHECK(contents.str() == expected);

    std::error_code error;
    std::filesystem::remove_all(root, error);
}
