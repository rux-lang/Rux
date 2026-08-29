#include "Linter/Linter.h"

#include <doctest.h>
#include <string>
#include <string_view>
#include <vector>

using namespace Rux;

namespace {
std::vector<std::string> DiagnosticMessages(const Linting::LintResult &result) {
    std::vector<std::string> messages;
    messages.reserve(result.diagnostics.size());
    for (const auto &diagnostic : result.diagnostics) {
        messages.push_back(diagnostic.message);
    }
    return messages;
}
} // namespace

TEST_CASE("Valid structured documentation tags produce no lint warnings") {
    const std::string source = R"(
/// Parses a value.
/// @typeParam T Produced value type.
/// @param input Source text.
/// @returns The parsed value.
/// @see Core::Result Related result.
func Parse<T>(input: String) -> T;

/// Stores a value.
/// @typeParam T Stored value type.
/// @deprecated Use Storage.
struct Box<T> {}

/// Optional value.
/// @typeParam T Carried value type.
variant Option<T> { None, Some(T) }

/// Reads bytes.
/// @param buffer Destination bytes.
/// @returns Bytes read.
#Link("system")
extern func Read(buffer: *uint8) -> int;
)";
    const auto result = Linting::Lint(source, "valid-doc-tags.rux");
    CHECK_FALSE(result.HasErrors());
    CHECK(result.diagnostics.empty());
}

TEST_CASE("Param tags must name non-receiver callable parameters") {
    const std::string source = R"(
/// Reads a value.
/// @param missing Not a parameter.
func Read(value: int);

struct Item {}
extend Item {
    /// Reads the receiver.
    /// @param self The receiver.
    func Inspect(self: &Item);
}

/// A type, not a callable.
/// @param value Invalid here.
struct Record { value: int; }

/// Variadic call.
/// @param args The unnamed variadic tail.
#Link("system")
extern func Call(value: int, ...);
)";
    const auto result = Linting::Lint(source, "param-doc-tags.rux");
    CHECK_FALSE(result.HasErrors());
    const auto messages = DiagnosticMessages(result);
    REQUIRE_EQ(messages.size(), 4);
    CHECK_EQ(messages[0], "@param 'missing' does not name a documentable parameter of 'Read'");
    CHECK_EQ(messages[1], "@param cannot document the self receiver");
    CHECK_EQ(messages[2], "@param is not valid on 'Record'");
    CHECK_EQ(messages[3], "@param 'args' does not name a documentable parameter of 'Call'");
    REQUIRE(result.diagnostics[1].help.has_value());
    CHECK(result.diagnostics[1].help->contains("Safety"));
}

TEST_CASE("Type parameter tags match parameters introduced by that declaration") {
    const std::string source = R"(
/// Generic function.
/// @typeParam U Missing type parameter.
func Convert<T>() -> T;

/// Generic structure.
/// @typeParam T Stored type.
struct Box<T> {}

/// Generic variant.
/// @typeParam T Carried type.
variant Maybe<T> { None, Some(T) }

/// Interface placeholder is not generic.
/// @typeParam Self Not a declared parameter.
interface Reader {}

extend Box<int> {
    /// Extension reuses rather than introduces T.
    /// @typeParam T Not introduced here.
    func Read();
}
)";
    const auto result = Linting::Lint(source, "type-param-doc-tags.rux");
    CHECK_FALSE(result.HasErrors());
    const auto messages = DiagnosticMessages(result);
    REQUIRE_EQ(messages.size(), 3);
    CHECK_EQ(messages[0], "@typeParam 'U' does not name a type parameter introduced by 'Convert'");
    CHECK_EQ(messages[1], "@typeParam 'Self' does not name a type parameter introduced by 'Reader'");
    CHECK_EQ(messages[2], "@typeParam 'T' does not name a type parameter introduced by 'Read'");
}

TEST_CASE("Returns tags require an ordinary value-returning callable") {
    const std::string source = R"(
/// Returns a value.
/// @returns The value.
func Value() -> int;

/// Has no result.
/// @returns Invalid result.
func NoValue();

/// Does not return.
/// @returns Invalid result.
#NoReturn()
func Exit();

struct Item {}
extend Item {
    /// Destroys an item.
    /// @returns Invalid result.
    func ~Item(self: &var Item) {}
}

/// Not callable.
/// @returns Invalid result.
const Limit = 1;
)";
    const auto result = Linting::Lint(source, "returns-doc-tags.rux");
    CHECK_FALSE(result.HasErrors());
    const auto messages = DiagnosticMessages(result);
    REQUIRE_EQ(messages.size(), 4);
    CHECK_EQ(messages[0], "@returns is not valid because 'NoValue' returns no value");
    CHECK_EQ(messages[1], "@returns is not valid because 'Exit' returns no value");
    CHECK_EQ(messages[2], "@returns is not valid because '~Item' returns no value");
    CHECK_EQ(messages[3], "@returns is not valid on 'Limit'");
}

TEST_CASE("Parser-recorded tag issues become precise lint warnings") {
    const std::string source = R"(
/// Summary.
/// @return Unknown alias.
/// @param
/// @returns First.
/// @returns Duplicate.
/// @see javascript:alert(1) Unsafe.
/// Resumed prose.
func Broken();
)";
    const auto result = Linting::Lint(source, "syntax-doc-tags.rux");
    CHECK_FALSE(result.HasErrors());
    const auto messages = DiagnosticMessages(result);
    REQUIRE_EQ(messages.size(), 6);
    CHECK(messages[0].contains("unknown documentation tag '@return'"));
    CHECK(messages[1].contains("@param requires"));
    CHECK(messages[2].contains("duplicate documentation tag '@returns'"));
    CHECK(messages[3].contains("unsafe or unsupported scheme"));
    CHECK(messages[4].contains("unindented prose cannot follow documentation tags"));
    CHECK(messages[5].contains("@returns is not valid because 'Broken' returns no value"));

    static constexpr std::uint32_t expectedLines[] = {3, 4, 6, 7, 8, 5};
    for (std::size_t index = 0; index < std::size(expectedLines); ++index) {
        CAPTURE(index);
        CHECK(result.diagnostics[index].severity == Diagnostic::Severity::Warning);
        CHECK_EQ(result.diagnostics[index].location.line, expectedLines[index]);
    }
}

TEST_CASE("Malformed continuation warnings retain the tag recovery text") {
    const std::string source = R"(
/// Summary.
/// @param value First.
///  One leading space.
func Read(value: int);
)";
    const auto result = Linting::Lint(source, "continuation-doc-tags.rux");
    CHECK_FALSE(result.HasErrors());
    REQUIRE_EQ(result.diagnostics.size(), 1);
    CHECK_EQ(result.diagnostics[0].message, "documentation tag continuation must begin with two spaces");
    CHECK_EQ(result.diagnostics[0].location.line, 4);
    CHECK_EQ(result.diagnostics[0].location.column, 5);
}

TEST_CASE("Structured callable tags are invalid on fields cases and named fields") {
    const std::string source = R"(
struct Record {
    /// @param value Invalid field tag.
    value: int;
}

enum State {
    /// @returns Invalid case tag.
    Ready
}

variant Message {
    Record {
        /// @typeParam T Invalid named-field tag.
        value: int;
    }
}
)";
    const auto result = Linting::Lint(source, "member-doc-tags.rux");
    CHECK_FALSE(result.HasErrors());
    const auto messages = DiagnosticMessages(result);
    REQUIRE_EQ(messages.size(), 3);
    CHECK_EQ(messages[0], "structured tag is not valid on struct field 'value'");
    CHECK_EQ(messages[1], "structured tag is not valid on enum member 'Ready'");
    CHECK_EQ(messages[2], "structured tag is not valid on variant case field 'value'");
}

TEST_CASE("See and deprecated tags are valid on documentable members") {
    const std::string source = R"(
struct Record {
    /// Field docs.
    /// @deprecated Use next.
    /// @see Record::next
    value: int;
}

enum State {
    /// Case docs.
    /// @deprecated Use Running.
    Ready
}

variant Message {
    Record {
        /// Payload docs.
        /// @see Message::Record
        value: int;
    }
}
)";
    const auto result = Linting::Lint(source, "valid-member-doc-tags.rux");
    CHECK_FALSE(result.HasErrors());
    CHECK(result.diagnostics.empty());
}

TEST_CASE("Detached and trailing documentation groups are lint warnings") {
    const std::string source = R"(
/// Blank-separated.

func Blank();

/// Ordinary-separated.
/* divider */
func Ordinary();

func First(); /// Trailing.
func Next();

/// End of file.
)";
    const auto result = Linting::Lint(source, "orphan-docs.rux");
    CHECK_FALSE(result.HasErrors());
    const auto messages = DiagnosticMessages(result);
    REQUIRE_EQ(messages.size(), 4);
    CHECK_EQ(messages[0], "documentation comment is not attached to an item");
    CHECK_EQ(messages[1], "documentation comment is not attached to an item");
    CHECK_EQ(messages[2], "trailing documentation comment is not attached to an item");
    CHECK_EQ(messages[3], "documentation comment is not attached to an item");
    CHECK_EQ(result.diagnostics[0].location.line, 2);
    CHECK_EQ(result.diagnostics[1].location.line, 6);
    CHECK_EQ(result.diagnostics[2].location.line, 10);
    CHECK_EQ(result.diagnostics[3].location.line, 13);
}

TEST_CASE("Missing public documentation help names both supported forms") {
    const auto result = Linting::Lint("pub func Public();\n", "missing-docs.rux");
    CHECK_FALSE(result.HasErrors());
    REQUIRE_EQ(result.diagnostics.size(), 1);
    REQUIRE(result.diagnostics[0].help.has_value());
    CHECK(result.diagnostics[0].help->contains("///"));
    CHECK(result.diagnostics[0].help->contains("/** ... */"));
}

TEST_CASE("Documentation tags remain optional") {
    const std::string source = R"(
/// Plain documentation without tags.
pub func Plain(value: int) -> int;

/** Block documentation without tags. */
pub struct Block {}
)";
    const auto result = Linting::Lint(source, "optional-doc-tags.rux");
    CHECK_FALSE(result.HasErrors());
    CHECK(result.diagnostics.empty());
}
