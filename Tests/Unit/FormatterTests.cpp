#include "Formatter/Formatter.h"

#include <doctest.h>

TEST_CASE("formatter normalizes line endings and trailing whitespace") {
    const auto result = Rux::Formatting::Format("let value = 1;  \r\nreturn value;\t");
    CHECK(result.changed);
    CHECK(result.text == "let value = 1;\nreturn value;\n");
}

TEST_CASE("formatter leaves canonical source unchanged") {
    const auto result = Rux::Formatting::Format("return 0;\n");
    CHECK_FALSE(result.changed);
}

TEST_CASE("formatter preserves ownership and lifecycle syntax") {
    constexpr std::string_view source = R"(extend Cell {
    func =(self: &var Cell, other: &Cell);
    func <-(self: &var Cell, other: Cell) {}
    func ~Cell(self: &var Cell) {}
}
func Transfer(source: Cell) -> Cell {
    let destination <- source;
    return <- destination;
}
)";
    const auto result = Rux::Formatting::Format(source);
    CHECK_FALSE(result.changed);
    CHECK_EQ(result.text, source);
}
