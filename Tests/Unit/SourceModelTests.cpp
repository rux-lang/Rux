#include "Diagnostics/Diagnostics.h"
#include "SourceModel/SourceFile.h"
#include "SourceModel/SourceLocation.h"
#include "SourceModel/SourceText.h"

#include <cstddef>
#include <cstdint>
#include <doctest.h>
#include <filesystem>
#include <string>
#include <type_traits>
#include <utility>

using namespace Rux;

TEST_CASE("source text indexes diagnostic lines without changing line boundaries") {
    for (const std::string contents : {"", "\n", "a\nb", "a\r\n\r\nb\r\n", "a\rb"}) {
        CAPTURE(contents);
        SourceText text(contents);
        CHECK(text.Text() == contents);
        for (std::size_t line = 0; line != 8; ++line) {
            CHECK(text.Line(line) == FindSourceLine(contents, line));
        }
        SourceText moved(std::move(text));
        for (std::size_t line = 0; line != 8; ++line) {
            CHECK(moved.Line(line) == FindSourceLine(contents, line));
        }
    }
}

TEST_CASE("source locations retain their diagnostic representation") {
    static_assert(std::is_aggregate_v<SourceLocation>);
    static_assert(std::is_standard_layout_v<SourceLocation>);
    static_assert(sizeof(SourceLocation) == 3 * sizeof(std::uint32_t));
    static_assert(offsetof(SourceLocation, line) == 0);
    static_assert(offsetof(SourceLocation, column) == sizeof(std::uint32_t));
    static_assert(offsetof(SourceLocation, offset) == 2 * sizeof(std::uint32_t));

    const SourceLocation defaultLocation;
    CHECK_EQ(defaultLocation.line, 1);
    CHECK_EQ(defaultLocation.column, 1);
    CHECK_EQ(defaultLocation.offset, 0);

    const SourceLocation diagnosticLocation{.line = 17, .column = 23, .offset = 101};
    CHECK_EQ(diagnosticLocation.line, 17);
    CHECK_EQ(diagnosticLocation.column, 23);
    CHECK_EQ(diagnosticLocation.offset, 101);
}

TEST_CASE("source files carry identity and text without loading policy") {
    const SourceFile file{.path = std::filesystem::path("Example.rux"), .source = "func Main() -> int { return 0; }\n"};
    CHECK_EQ(file.path, std::filesystem::path("Example.rux"));
    CHECK_EQ(file.source, "func Main() -> int { return 0; }\n");
}
