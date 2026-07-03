#include <doctest.h>

#include "Formatter/Formatter.h"

TEST_CASE("formatter normalizes line endings and trailing whitespace") {
    const auto result = Rux::Formatting::Format("let value = 1;  \r\nreturn value;\t");
    CHECK(result.changed);
    CHECK(result.text == "let value = 1;\nreturn value;\n");
}

TEST_CASE("formatter leaves canonical source unchanged") {
    const auto result = Rux::Formatting::Format("return 0;\n");
    CHECK_FALSE(result.changed);
}
