#include "Driver/BuildReport.h"

#include <doctest.h>

using namespace Rux::Misc;

TEST_CASE("CountLines counts newline-terminated and trailing lines") {
    CHECK(CountLines("") == 0);
    CHECK(CountLines("one line") == 1);
    CHECK(CountLines("a\nb") == 2);
    CHECK(CountLines("a\nb\n") == 2);
    CHECK(CountLines("\n\n") == 2);
}

TEST_CASE("FormatNumber groups digits with commas") {
    CHECK(FormatNumber(0) == "0");
    CHECK(FormatNumber(999) == "999");
    CHECK(FormatNumber(1000) == "1,000");
    CHECK(FormatNumber(1234567) == "1,234,567");
}

TEST_CASE("FormatDecimal trims trailing zeros") {
    CHECK(FormatDecimal(2.0, 2) == "2");
    CHECK(FormatDecimal(1.5, 2) == "1.5");
    CHECK(FormatDecimal(1.25, 2) == "1.25");
    CHECK(FormatDecimal(1.204, 2) == "1.2");
}

TEST_CASE("FormatCompactNumber abbreviates thousands and millions") {
    CHECK(FormatCompactNumber(950.0) == "950");
    CHECK(FormatCompactNumber(1500.0) == "1.5K");
    CHECK(FormatCompactNumber(2'000'000.0) == "2M");
}

TEST_CASE("FormatTokenThroughput picks a unit per magnitude") {
    CHECK(FormatTokenThroughput(500.0) == "500 tok/s");
    CHECK(FormatTokenThroughput(1500.0) == "1.5 K tok/s");
    CHECK(FormatTokenThroughput(2'000'000.0) == "2 M tok/s");
}

TEST_CASE("FormatSize reports KB below one MB and MB above") {
    CHECK(FormatSize(512) == "1 KB");
    CHECK(FormatSize(10 * 1024) == "10 KB");
    CHECK(FormatSize(2 * 1024 * 1024) == "2 MB");
    CHECK(FormatSize(1536 * 1024) == "1.5 MB");
}
