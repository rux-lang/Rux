#include "Linter/Linter.h"

#include <doctest.h>
#include <string>
#include <vector>

using namespace Rux;

TEST_CASE("linter accepts syntactically valid source") {
    auto result = Rux::Linting::Lint("func Main() -> int { return 0; }", "valid.rux");
    CHECK_FALSE(result.HasErrors());
}

TEST_CASE("linter reports syntax errors") {
    auto result = Rux::Linting::Lint("func Main() -> int { let value = 1 return value; }", "invalid.rux");
    CHECK(result.HasErrors());
}

TEST_CASE("linter accepts correct naming conventions") {
    const std::string source = R"(
        module ValidModule {
            const MyConstant = 42;

            struct MyStruct {
                someField: int32;
            }

            enum MyEnum {
                FirstVariant,
                SecondVariant{innerValue: int32;}
            }

            interface MyInterface {
                func SomeMethod(someParam: int32);
            }

            func SomeFunction(firstParam: int32, secondParam: float64) -> int32 {
                let localVariable = 10;
                return localVariable;
            }
        }
    )";

    auto result = Rux::Linting::Lint(source, "naming_valid.rux");
    CHECK_FALSE(result.HasErrors());
    CHECK(result.diagnostics.empty());
}

TEST_CASE("linter warns on bad function name") {
    auto result = Rux::Linting::Lint("func bad_name() {}", "func_bad.rux");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].severity == Diagnostic::Severity::Warning);
    CHECK(result.diagnostics[0].message == "function name 'bad_name' should be PascalCase");
    CHECK(result.diagnostics[0].help == "rename it to 'BadName'");
}

TEST_CASE("linter accepts symbolic operator function names") {
    auto result = Rux::Linting::Lint(R"(
        extend Number {
            func +(self: *self, other: Number) -> Number;
            func ==(self: *self, other: Number) -> bool;
        }
    )",
                                     "operators.rux");
    CHECK_FALSE(result.HasErrors());
    CHECK(result.diagnostics.empty());
}

TEST_CASE("linter warns on bad function parameter name") {
    auto result = Rux::Linting::Lint("func Test(BadParam: int) {}", "param_bad.rux");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].severity == Diagnostic::Severity::Warning);
    CHECK(result.diagnostics[0].message == "parameter name 'BadParam' should be camelCase");
    CHECK(result.diagnostics[0].help == "rename it to 'badParam'");
}

TEST_CASE("linter warns on bad struct name") {
    auto result = Rux::Linting::Lint("struct badStruct {}", "struct_bad.rux");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].severity == Diagnostic::Severity::Warning);
    CHECK(result.diagnostics[0].message == "struct name 'badStruct' should be PascalCase");
    CHECK(result.diagnostics[0].help == "rename it to 'BadStruct'");
}

TEST_CASE("linter warns on bad struct field name") {
    auto result = Rux::Linting::Lint("struct Test { BadField: int; }", "field_bad.rux");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].severity == Diagnostic::Severity::Warning);
    CHECK(result.diagnostics[0].message == "struct field name 'BadField' should be camelCase");
    CHECK(result.diagnostics[0].help == "rename it to 'badField'");
}

TEST_CASE("linter warns on bad enum name") {
    auto result = Rux::Linting::Lint("enum badEnum { Variant }", "enum_bad.rux");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].severity == Diagnostic::Severity::Warning);
    CHECK(result.diagnostics[0].message == "enum name 'badEnum' should be PascalCase");
    CHECK(result.diagnostics[0].help == "rename it to 'BadEnum'");
}

TEST_CASE("linter warns on bad enum variant name") {
    auto result = Rux::Linting::Lint("enum Test { badVariant }", "variant_bad.rux");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].severity == Diagnostic::Severity::Warning);
    CHECK(result.diagnostics[0].message == "enum variant name 'badVariant' should be PascalCase");
    CHECK(result.diagnostics[0].help == "rename it to 'BadVariant'");
}

TEST_CASE("linter warns on bad enum variant field name") {
    auto result = Rux::Linting::Lint("enum Test { Variant{BadField: int;} }", "var_field_bad.rux");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].severity == Diagnostic::Severity::Warning);
    CHECK(result.diagnostics[0].message == "enum variant field name 'BadField' should be camelCase");
    CHECK(result.diagnostics[0].help == "rename it to 'badField'");
}

TEST_CASE("linter warns on bad union name") {
    auto result = Rux::Linting::Lint("union badUnion { x: int }", "union_bad.rux");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].severity == Diagnostic::Severity::Warning);
    CHECK(result.diagnostics[0].message == "union name 'badUnion' should be PascalCase");
    CHECK(result.diagnostics[0].help == "rename it to 'BadUnion'");
}

TEST_CASE("linter warns on bad union field name") {
    auto result = Rux::Linting::Lint("union Test { BadField: int }", "union_field_bad.rux");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].severity == Diagnostic::Severity::Warning);
    CHECK(result.diagnostics[0].message == "union field name 'BadField' should be camelCase");
    CHECK(result.diagnostics[0].help == "rename it to 'badField'");
}

TEST_CASE("linter warns on bad interface name") {
    auto result = Rux::Linting::Lint("interface badInterface {}", "interface_bad.rux");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].severity == Diagnostic::Severity::Warning);
    CHECK(result.diagnostics[0].message == "interface name 'badInterface' should be PascalCase");
    CHECK(result.diagnostics[0].help == "rename it to 'BadInterface'");
}

TEST_CASE("linter warns on bad module name") {
    auto result = Rux::Linting::Lint("module bad_module {}", "module_bad.rux");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].severity == Diagnostic::Severity::Warning);
    CHECK(result.diagnostics[0].message == "module name 'bad_module' should be PascalCase");
    CHECK(result.diagnostics[0].help == "rename it to 'BadModule'");
}

TEST_CASE("linter warns on bad constant name") {
    auto result = Rux::Linting::Lint("const badConst = 42;", "const_bad.rux");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].severity == Diagnostic::Severity::Warning);
    CHECK(result.diagnostics[0].message == "constant name 'badConst' should be PascalCase");
    CHECK(result.diagnostics[0].help == "rename it to 'BadConst'");
}

TEST_CASE("linter warns on bad type alias name") {
    auto result = Rux::Linting::Lint("type badAlias = int;", "alias_bad.rux");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].severity == Diagnostic::Severity::Warning);
    CHECK(result.diagnostics[0].message == "type alias name 'badAlias' should be PascalCase");
    CHECK(result.diagnostics[0].help == "rename it to 'BadAlias'");
}

TEST_CASE("naming.type allows foreign type and member names") {
    auto result = Rux::Linting::Lint(R"(
        #Allow("naming.type")
        type time_t = int64;

        #Allow("naming.type")
        struct timespec {
            tv_sec: time_t;
            tv_nsec: int64;
        }

        #Allow("naming.type")
        enum OperatingSystem {
            macOS
        }
    )",
                                     "foreign_types.rux");
    CHECK_FALSE(result.HasErrors());
    CHECK(result.diagnostics.empty());
}

TEST_CASE("naming.type rejects unknown rules and non-type declarations") {
    auto unknown = Rux::Linting::Lint(R"(
        #Allow("naming.unknown")
        type time_t = int64;
    )",
                                      "unknown_allow.rux");
    CHECK(unknown.HasErrors());

    auto nonType = Rux::Linting::Lint(R"(
        #Allow("naming.type")
        func bad_name() {}
    )",
                                      "invalid_allow_target.rux");
    CHECK(nonType.HasErrors());
}

TEST_CASE("linter warns on bad local variable name") {
    auto result = Rux::Linting::Lint("func Test() { let BadVar = 10; }", "local_bad.rux");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].severity == Diagnostic::Severity::Warning);
    CHECK(result.diagnostics[0].message == "local variable name 'BadVar' should be camelCase");
    CHECK(result.diagnostics[0].help == "rename it to 'badVar'");
}

TEST_CASE("linter warns on bad loop variable name") {
    auto result = Rux::Linting::Lint("func Test() { for BadVar in 0..10 {} }", "loop_bad.rux");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].severity == Diagnostic::Severity::Warning);
    CHECK(result.diagnostics[0].message == "loop variable name 'BadVar' should be camelCase");
    CHECK(result.diagnostics[0].help == "rename it to 'badVar'");
}

TEST_CASE("linter warns on bad pattern binding name") {
    auto result = Rux::Linting::Lint("func Test() { let (x, BadVar) = (1, 2); }", "pattern_bad.rux");
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].severity == Diagnostic::Severity::Warning);
    CHECK(result.diagnostics[0].message == "pattern binding name 'BadVar' should be camelCase");
    CHECK(result.diagnostics[0].help == "rename it to 'badVar'");
}

TEST_CASE("linter normalizes acronyms digits and leading underscores") {
    auto result = Rux::Linting::Lint("func _HTTP_2_handler(HTTP_2_VALUE: int) {}", "normalization.rux");
    REQUIRE(result.diagnostics.size() == 2);
    CHECK(result.diagnostics[0].help == "rename it to 'Http2Handler'");
    CHECK(result.diagnostics[1].help == "rename it to 'http2Value'");
}

TEST_CASE("linter accepts valid names containing acronyms and digits") {
    auto result = Rux::Linting::Lint("func HTTP2Server(http2Value: int) {}", "acronyms.rux");
    CHECK_FALSE(result.HasErrors());
    CHECK(result.diagnostics.empty());
}

TEST_CASE("linter omits naming help when the normalized name collides") {
    auto declarations = Rux::Linting::Lint(R"(
        func bad_name() {}
        func BadName() {}
    )",
                                           "declaration_collision.rux");
    REQUIRE(declarations.diagnostics.size() == 1);
    CHECK_FALSE(declarations.diagnostics[0].help.has_value());

    auto members = Rux::Linting::Lint(R"(
        struct Example {
            BadField: int;
            badField: int;
        }
    )",
                                      "member_collision.rux");
    REQUIRE(members.diagnostics.size() == 1);
    CHECK_FALSE(members.diagnostics[0].help.has_value());

    auto locals = Rux::Linting::Lint("func Example(badVar: int) { let BadVar = 1; }", "local_collision.rux");
    REQUIRE(locals.diagnostics.size() == 1);
    CHECK_FALSE(locals.diagnostics[0].help.has_value());
}

TEST_CASE("linter preserves source locations and warning counts for naming diagnostics") {
    auto result = Rux::Linting::Lint("func bad_name(BadParam: int) {}", "warning_count.rux");
    REQUIRE(result.diagnostics.size() == 2);
    CHECK(result.diagnostics[0].severity == Diagnostic::Severity::Warning);
    CHECK(result.diagnostics[0].location.line == 1);
    CHECK(result.diagnostics[0].location.column == 1);
    CHECK(result.diagnostics[1].severity == Diagnostic::Severity::Warning);
    CHECK(result.diagnostics[1].location.line == 1);
    CHECK(result.diagnostics[1].location.column == 15);
}

namespace {
/// The messages one lint run produced, which is what every documentation case below asserts against.
std::vector<std::string> Messages(const Rux::Linting::LintResult &result) {
    std::vector<std::string> messages;
    messages.reserve(result.diagnostics.size());
    for (const Diagnostic &diagnostic : result.diagnostics) {
        messages.push_back(diagnostic.message);
    }
    return messages;
}
} // namespace

TEST_CASE("a public declaration without documentation is reported") {
    const std::string source = R"(
pub struct Handle {
    value: int32;
}

pub func Open() -> int32 {
    return 0i32;
}

pub const Limit = 8;

pub interface Reader {
    func Read() -> int32;
}

pub type Count = int32;
)";

    const auto messages = Messages(Rux::Linting::Lint(source, "docs.rux"));
    REQUIRE_EQ(messages.size(), 5);
    CHECK_EQ(messages[0], "public struct 'Handle' has no documentation comment");
    CHECK_EQ(messages[1], "public function 'Open' has no documentation comment");
    CHECK_EQ(messages[2], "public constant 'Limit' has no documentation comment");
    CHECK_EQ(messages[3], "public interface 'Reader' has no documentation comment");
    CHECK_EQ(messages[4], "public type alias 'Count' has no documentation comment");
}

TEST_CASE("a declaration that is not published owes no documentation") {
    const std::string source = R"(
struct Handle {
    value: int32;
}

func Open() -> int32 {
    return 0i32;
}
)";

    CHECK(Messages(Rux::Linting::Lint(source, "docs.rux")).empty());
}

TEST_CASE("documentation without an API page is reported") {
    const std::string source = R"(
/// Opens the handle.
pub func Open() -> int32 {
    return 0i32;
}
)";

    const auto messages = Messages(Rux::Linting::Lint(source, "docs.rux"));
    REQUIRE_EQ(messages.size(), 1);
    CHECK_EQ(messages[0], "documentation for public function 'Open' names no API page");
}

TEST_CASE("documentation naming its API page is accepted") {
    const std::string source = R"(
/// Opens the handle.
///
/// https://rux-lang.dev/docs/api/filesystem/open
pub func Open() -> int32 {
    return 0i32;
}
)";

    CHECK(Messages(Rux::Linting::Lint(source, "docs.rux")).empty());
}

TEST_CASE("an undocumented public declaration can allow the rule deliberately") {
    const std::string source = R"(
#Allow("docs.missing")
pub func Open() -> int32 {
    return 0i32;
}

/// Closes the handle.
#Allow("docs.api-url")
pub func Close() -> int32 {
    return 0i32;
}
)";

    CHECK(Messages(Rux::Linting::Lint(source, "docs.rux")).empty());
}

TEST_CASE("a comment line wider than the limit is reported once, with its width") {
    // 121 columns of comment: one past the limit, which is the boundary worth pinning.
    const std::string wide = "// " + std::string(118, 'x') + "\nfunc Main() -> int { return 0; }\n";

    const auto messages = Messages(Rux::Linting::Lint(wide, "width.rux"));
    REQUIRE_EQ(messages.size(), 1);
    CHECK_EQ(messages[0], "comment line is 121 columns wide, over the limit of 120");

    const std::string exact = "// " + std::string(117, 'x') + "\nfunc Main() -> int { return 0; }\n";
    CHECK(Messages(Rux::Linting::Lint(exact, "width.rux")).empty());
}

TEST_CASE("comment width is counted in columns rather than bytes") {
    // Each character is two bytes, so a byte count would report 240 for a line that reads as 120 columns.
    const std::string source = "// " + [] {
        std::string text;
        for (int index = 0; index < 117; ++index) {
            text += "é";
        }
        return text;
    }() + "\nfunc Main() -> int { return 0; }\n";

    CHECK(Messages(Rux::Linting::Lint(source, "width.rux")).empty());
}

TEST_CASE("a wide line inside a block comment is reported too") {
    const std::string source = "/*\n" + std::string(121, 'x') + "\n*/\nfunc Main() -> int { return 0; }\n";

    const auto messages = Messages(Rux::Linting::Lint(source, "width.rux"));
    REQUIRE_EQ(messages.size(), 1);
    CHECK_EQ(messages[0], "comment line is 121 columns wide, over the limit of 120");
}

TEST_CASE("a wide line of code is left to the formatter") {
    const std::string source = "func Main() -> int { let value = 0; " + std::string(100, ' ') + "return value; }\n";

    CHECK(Messages(Rux::Linting::Lint(source, "width.rux")).empty());
}

TEST_CASE("an unknown lint rule is rejected where it is written") {
    const std::string source = R"(
#Allow("docs.mising")
pub func Open() -> int32 { return 0i32; }
)";
    const auto result = Rux::Linting::Lint(source, "allow.rux");

    REQUIRE(result.HasErrors());
    CHECK_EQ(result.diagnostics.front().message,
             "unknown lint rule 'docs.mising'; valid rules are: naming.type, docs.missing, docs.api-url");
}
