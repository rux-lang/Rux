#include "Diagnostics/Diagnostics.h"

#include <array>
#include <doctest.h>
#include <optional>
#include <string>
#include <string_view>

using namespace Rux;

namespace {
SourceLineLookup LookupFor(std::string sourceName, std::string source) {
    return [sourceName = std::move(sourceName), source = std::move(source)](
               const std::string_view requestedName, const std::size_t lineNumber) -> std::optional<std::string_view> {
        if (requestedName != sourceName) {
            return std::nullopt;
        }
        return FindSourceLine(source, lineNumber);
    };
}
} // namespace

TEST_CASE("diagnostics retain four-field aggregate initialization") {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
    const Diagnostic diagnostic{Diagnostic::Severity::Warning, "Src/Main.rux", {.line = 7, .column = 3}, "careful"};
#pragma clang diagnostic pop

    CHECK(diagnostic.severity == Diagnostic::Severity::Warning);
    CHECK(diagnostic.sourceName == "Src/Main.rux");
    CHECK(diagnostic.location.line == 7);
    CHECK(diagnostic.location.column == 3);
    CHECK(diagnostic.message == "careful");
    CHECK(diagnostic.notes.empty());
    CHECK_FALSE(diagnostic.help.has_value());
    CHECK_FALSE(diagnostic.documentationUrl.has_value());
}

TEST_CASE("plain diagnostics render structured supplemental context") {
    const auto diagnostic = ErrorDiagnostic("target 'plan9-x86_64' is not supported",
                                            {"supported targets are linux-x86_64 and windows-x86_64", ""},
                                            "try 'rux check --target linux-x86_64'", "https://rux-lang.dev/docs/cli");

    CHECK(RenderDiagnostic(diagnostic) == "error: target 'plan9-x86_64' is not supported\n"
                                          "  note: supported targets are linux-x86_64 and windows-x86_64\n"
                                          "  help: try 'rux check --target linux-x86_64'\n"
                                          "  docs: https://rux-lang.dev/docs/cli\n");
    CHECK(RenderDiagnostic(ErrorDiagnostic("failed"), true) == "\033[31m\033[1merror:\033[0m failed\n");
}

TEST_CASE("diagnostic rendering preserves the canonical source prefix and colors semantic labels") {
    Diagnostic diagnostic{Diagnostic::Severity::Warning,
                          "Src/Main.rux",
                          {.line = 12, .column = 9},
                          "cannot assign a string to 'count'",
                          {},
                          {},
                          {}};
    diagnostic.notes = {"'count' was declared on line 4", "a second note"};
    diagnostic.help = "convert the value to 'int'";
    diagnostic.documentationUrl = "https://rux-lang.dev/docs/variables/";

    CHECK(RenderDiagnostic(diagnostic, true) ==
          "Src/Main.rux:12:9: \033[33m\033[1mwarning:\033[0m cannot assign a string to 'count'\n"
          "  \033[2m\033[1mnote:\033[0m 'count' was declared on line 4\n"
          "  \033[2m\033[1mnote:\033[0m a second note\n"
          "  \033[1mhelp:\033[0m convert the value to 'int'\n"
          "  \033[36m\033[1mdocs:\033[0m \033[36mhttps://rux-lang.dev/docs/variables/\033[0m\n");
}

TEST_CASE("empty optional diagnostic fields are omitted") {
    Diagnostic diagnostic{Diagnostic::Severity::Error, {}, {}, "primary", {}, {}, {}};
    diagnostic.notes = {"", "context", ""};
    diagnostic.help = "";
    diagnostic.documentationUrl = "";

    CHECK(RenderDiagnostic(diagnostic) == "error: primary\n  note: context\n");
}

TEST_CASE("human diagnostic rendering escapes embedded control characters") {
    Diagnostic diagnostic{Diagnostic::Severity::Error,
                          std::string("Src\x1b.rux"),
                          {.line = 2, .column = 4},
                          std::string("bad\n\t\x01"),
                          {},
                          {},
                          {}};
    diagnostic.notes = {"detail\rnext"};
    diagnostic.help = std::string("try\0this", 8);
    diagnostic.documentationUrl = std::string("https://example.invalid/\x7f");

    CHECK(RenderDiagnostic(diagnostic) == "Src\\x1b.rux:2:4: error: bad\\n\\t\\x01\n"
                                          "  note: detail\\rnext\n"
                                          "  help: try\\0this\n"
                                          "  docs: https://example.invalid/\\x7f\n");
}

TEST_CASE("source line lookup handles LF, CRLF, empty lines, and missing lines") {
    constexpr std::string_view source = "first\r\n\r\nthird\n";

    REQUIRE(FindSourceLine(source, 1));
    CHECK(*FindSourceLine(source, 1) == "first");
    REQUIRE(FindSourceLine(source, 2));
    CHECK(FindSourceLine(source, 2)->empty());
    REQUIRE(FindSourceLine(source, 3));
    CHECK(*FindSourceLine(source, 3) == "third");
    REQUIRE(FindSourceLine(source, 4));
    CHECK(FindSourceLine(source, 4)->empty());
    CHECK_FALSE(FindSourceLine(source, 0));
    CHECK_FALSE(FindSourceLine(source, 5));
}

TEST_CASE("source frames expand tabs and place one caret at the diagnostic byte column") {
    Diagnostic diagnostic{Diagnostic::Severity::Error,
                          "Src/Main.rux",
                          {.line = 8, .column = 6},
                          "name 'coutn' is not defined",
                          {},
                          {},
                          {}};
    diagnostic.help = "did you mean 'count'?";
    const auto lookup = LookupFor("Src/Main.rux", "one\ntwo\nthree\nfour\nfive\nsix\nseven\n\tlet coutn = 1;\n");

    CHECK(RenderDiagnostic(diagnostic, false, lookup) == "Src/Main.rux:8:6: error: name 'coutn' is not defined\n"
                                                         "  8 |     let coutn = 1;\n"
                                                         "    |         ^\n"
                                                         "  help: did you mean 'count'?\n");
}

TEST_CASE("source frames retain UTF-8 while translating byte columns to display columns") {
    Diagnostic diagnostic{
        Diagnostic::Severity::Warning, "Memory.rux", {.line = 1, .column = 13}, "suspicious name", {}, {}, {}};
    const auto lookup = LookupFor("Memory.rux", "let café = coutn");

    CHECK(RenderDiagnostic(diagnostic, false, lookup) == "Memory.rux:1:13: warning: suspicious name\n"
                                                         "  1 | let café = coutn\n"
                                                         "    |            ^\n");
}

TEST_CASE("source frames support empty lines and columns beyond the line end") {
    Diagnostic empty{Diagnostic::Severity::Error, "Empty.rux", {.line = 2, .column = 1}, "empty", {}, {}, {}};
    const auto emptyLookup = LookupFor("Empty.rux", "first\n\nthird");
    CHECK(RenderDiagnostic(empty, false, emptyLookup) == "Empty.rux:2:1: error: empty\n"
                                                         "  2 | \n"
                                                         "    | ^\n");

    Diagnostic pastEnd{Diagnostic::Severity::Error, "Short.rux", {.line = 1, .column = 99}, "past end", {}, {}, {}};
    const auto shortLookup = LookupFor("Short.rux", "abc");
    CHECK(RenderDiagnostic(pastEnd, false, shortLookup) == "Short.rux:1:99: error: past end\n"
                                                           "  1 | abc\n"
                                                           "    |    ^\n");
}

TEST_CASE("source frames clip long lines around the caret") {
    std::string source(200, 'a');
    source[150] = 'X';
    Diagnostic diagnostic{Diagnostic::Severity::Error, "Long.rux", {.line = 1, .column = 151}, "long line", {}, {}, {}};
    const auto rendered = RenderDiagnostic(diagnostic, false, LookupFor("Long.rux", source));
    const auto frameBegin = rendered.find("  1 | ");
    const auto frameEnd = rendered.find('\n', frameBegin);

    REQUIRE(frameBegin != std::string::npos);
    REQUIRE(frameEnd != std::string::npos);
    const auto frame = rendered.substr(frameBegin, frameEnd - frameBegin);
    CHECK(frame.starts_with("  1 | ..."));
    CHECK(frame.contains('X'));
    CHECK(frame.size() <= 6 + 120);
    CHECK(rendered.find('^', frameEnd) != std::string::npos);
}

TEST_CASE("source frames escape control bytes and fall back when text is unavailable") {
    Diagnostic diagnostic{Diagnostic::Severity::Error, "Unsafe.rux", {.line = 1, .column = 3}, "unsafe", {}, {}, {}};
    const auto escaped = RenderDiagnostic(diagnostic, false, LookupFor("Unsafe.rux", "a\x01z"));
    CHECK(escaped.contains("  1 | a\\x01z\n"));
    CHECK(escaped.contains("    |      ^\n"));

    std::size_t requests = 0;
    const SourceLineLookup missing = [&](std::string_view, std::size_t) -> std::optional<std::string_view> {
        ++requests;
        return std::nullopt;
    };
    CHECK(RenderDiagnostic(diagnostic, false, missing) == "Unsafe.rux:1:3: error: unsafe\n");
    CHECK(requests == 1);
    CHECK(RenderDiagnostic(diagnostic) == "Unsafe.rux:1:3: error: unsafe\n");
}

TEST_CASE("colored source frames style gutters and carets without changing source text") {
    Diagnostic diagnostic{Diagnostic::Severity::Warning, "Color.rux", {.line = 12, .column = 5}, "warning", {}, {}, {}};
    const auto rendered =
        RenderDiagnostic(diagnostic, true, LookupFor("Color.rux", "a\nb\nc\nd\ne\nf\ng\nh\ni\nj\nk\nlet value"));
    CAPTURE(rendered);

    CHECK(rendered.contains("\033[2m  12 |\033[0m let value\n"));
    CHECK(rendered.contains("\033[2m     |\033[0m     \033[33m^\033[0m\n"));
}

TEST_CASE("JSON diagnostics retain the five-key schema and omit supplemental fields") {
    Diagnostic diagnostic{Diagnostic::Severity::Warning,
                          "Src/Quoted\"Name.rux",
                          {.line = 3, .column = 5},
                          "line one\nline two",
                          {},
                          {},
                          {}};
    diagnostic.notes = {"not serialized"};
    diagnostic.help = "also not serialized";
    diagnostic.documentationUrl = "https://example.invalid/not-serialized";

    const std::array diagnostics{diagnostic};
    CHECK(RenderDiagnosticsJson(diagnostics, false) ==
          "{\n"
          "  \"success\": false,\n"
          "  \"diagnostics\": [\n"
          "    {\"file\":\"Src/Quoted\\\"Name.rux\",\"line\":3,\"column\":5,\"severity\":\"warning\","
          "\"message\":\"line one\\nline two\"}\n"
          "  ]\n"
          "}\n");
    CHECK_FALSE(RenderDiagnosticsJson(diagnostics, false).contains("notes"));
    CHECK_FALSE(RenderDiagnosticsJson(diagnostics, false).contains("help"));
    CHECK_FALSE(RenderDiagnosticsJson(diagnostics, false).contains("documentationUrl"));
    CHECK_FALSE(RenderDiagnosticsJson(diagnostics, false).contains("  3 |"));
    CHECK_FALSE(RenderDiagnosticsJson(diagnostics, false).contains('^'));
}
