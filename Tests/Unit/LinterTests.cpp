#include <doctest.h>

#include "Linter/Linter.h"

TEST_CASE("linter accepts syntactically valid source") {
    auto result = Rux::Linting::Lint("func Main() -> int { return 0; }", "valid.rux");
    CHECK_FALSE(result.HasErrors());
}

TEST_CASE("linter reports syntax errors") {
    auto result = Rux::Linting::Lint("func Main() -> int { let value = 1 return value; }", "invalid.rux");
    CHECK(result.HasErrors());
}
