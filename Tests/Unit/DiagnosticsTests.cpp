#include "Diagnostics/Diagnostics.h"

#include <array>
#include <doctest.h>
#include <string>

using namespace Rux;

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
                                            "try 'rux check --target linux-x86_64'", "https://rux-lang.dev/cli/");

    CHECK(RenderDiagnostic(diagnostic) == "error: target 'plan9-x86_64' is not supported\n"
                                          "  note: supported targets are linux-x86_64 and windows-x86_64\n"
          "  help: try 'rux check --target linux-x86_64'\n"
          "  docs: https://rux-lang.dev/cli/\n");
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
}
