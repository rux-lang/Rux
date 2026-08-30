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

TEST_CASE("formatter canonicalizes line documentation markers") {
    constexpr std::string_view source = R"(///Summary.
//// decorative
///   Deliberately indented prose.
///   - nested item
///
func Read();
)";
    constexpr std::string_view expected = R"(/// Summary.
//// decorative
///   Deliberately indented prose.
///   - nested item
///
func Read();
)";
    const auto result = Rux::Formatting::Format(source);
    CHECK(result.changed);
    CHECK_EQ(result.text, expected);
}

TEST_CASE("formatter canonicalizes one-line block documentation") {
    constexpr std::string_view source = R"(/**Summary.*/
/**  Spaced summary.  */
/***/
/**/
struct Item {}
)";
    constexpr std::string_view expected = R"(/** Summary. */
/** Spaced summary. */
/***/
/**/
struct Item {}
)";
    const auto result = Rux::Formatting::Format(source);
    CHECK(result.changed);
    CHECK_EQ(result.text, expected);
}

TEST_CASE("formatter indents multiline block documentation without a star margin") {
    constexpr std::string_view source = R"(    /**
      Summary.

        Continued paragraph.
      */
    func Read();
)";
    constexpr std::string_view expected = R"(    /**
        Summary.

          Continued paragraph.
    */
    func Read();
)";
    const auto result = Rux::Formatting::Format(source);
    CHECK(result.changed);
    CHECK_EQ(result.text, expected);
}

TEST_CASE("formatter keeps direct line tags compact") {
    constexpr std::string_view source = R"(/// Reads a value.
/// @param   input    Source text.
/// @returns    Parsed value.
///  continuation stays indented
func Read(input: String) -> int;
)";
    constexpr std::string_view expected = R"(/// Reads a value.
/// @param input Source text.
/// @returns Parsed value.
///  continuation stays indented
func Read(input: String) -> int;
)";
    const auto result = Rux::Formatting::Format(source);
    CHECK(result.changed);
    CHECK_EQ(result.text, expected);
}

TEST_CASE("formatter keeps multiline block tags compact") {
    constexpr std::string_view source = R"(/**
 * Reads a value.
 * @typeParam   T   Result type.
 * @param input Input value.
 * @see   Core::Result   Related result.
 */
func Read<T>(input: String) -> T;
)";
    constexpr std::string_view expected = R"(/**
    Reads a value.
    @typeParam T Result type.
    @param input Input value.
    @see Core::Result Related result.
*/
func Read<T>(input: String) -> T;
)";
    const auto result = Rux::Formatting::Format(source);
    CHECK(result.changed);
    CHECK_EQ(result.text, expected);
}

TEST_CASE("formatter keeps direct tags direct") {
    constexpr std::string_view source = R"(///@deprecated   Use NewItem.
///@see  NewItem
struct OldItem {}

/** @returns   A value. */
func Value() -> int;
)";
    constexpr std::string_view expected = R"(/// @deprecated Use NewItem.
/// @see NewItem
struct OldItem {}

/** @returns A value. */
func Value() -> int;
)";
    const auto result = Rux::Formatting::Format(source);
    CHECK(result.changed);
    CHECK_EQ(result.text, expected);
}

TEST_CASE("formatter preserves Markdown-sensitive documentation whitespace") {
    constexpr std::string_view source = R"(/// Summary.
///
///     let text = "@param untouched";
///
/// ```rux
/// @param   not_a_tag   remains spaced
/// ```
///
/// @param   value   Actual tag.
func Read(value: int);
)";
    constexpr std::string_view expected = R"(/// Summary.
///
///     let text = "@param untouched";
///
/// ```rux
/// @param   not_a_tag   remains spaced
/// ```
///
/// @param value Actual tag.
func Read(value: int);
)";
    const auto result = Rux::Formatting::Format(source);
    CHECK(result.changed);
    CHECK_EQ(result.text, expected);
}

TEST_CASE("formatter only edits lexer-recognized comment ranges") {
    constexpr std::string_view source = R"(const line = "///not documentation";
const block = "/** not documentation */";
const character = '/';
///Actual documentation.
func Read();
)";
    constexpr std::string_view expected = R"(const line = "///not documentation";
const block = "/** not documentation */";
const character = '/';
/// Actual documentation.
func Read();
)";
    const auto result = Rux::Formatting::Format(source);
    CHECK(result.changed);
    CHECK_EQ(result.text, expected);
}

TEST_CASE("formatter preserves ordinary and decorative comment content") {
    constexpr std::string_view source =
        "// ordinary   \n//// banner   \n/* block   */\n/*** decorative   */\nlet value = 1;   \n";
    constexpr std::string_view expected =
        "// ordinary   \n//// banner   \n/* block   */\n/*** decorative   */\nlet value = 1;\n";
    const auto result = Rux::Formatting::Format(source);
    CHECK(result.changed);
    CHECK_EQ(result.text, expected);
}

TEST_CASE("formatter preserves unterminated documentation blocks") {
    constexpr std::string_view source = "/** still open   ";
    constexpr std::string_view expected = "/** still open   \n";
    const auto result = Rux::Formatting::Format(source);
    CHECK(result.changed);
    CHECK_EQ(result.text, expected);
}

TEST_CASE("formatter does not merge separated or mixed documentation groups") {
    constexpr std::string_view source = R"(///First group.

///Second group.
/* divider */
///Third group.
func Read();
)";
    constexpr std::string_view expected = R"(/// First group.

/// Second group.
/* divider */
/// Third group.
func Read();
)";
    const auto result = Rux::Formatting::Format(source);
    CHECK(result.changed);
    CHECK_EQ(result.text, expected);
}

TEST_CASE("formatter preserves tag order and malformed tag text") {
    constexpr std::string_view source = R"(/// Summary.
/// @see   Core::Item   First.
/// @param
/// @unknown   keep   exact
/// @deprecated    Last.
func Read();
)";
    constexpr std::string_view expected = R"(/// Summary.
/// @see Core::Item First.
/// @param
/// @unknown   keep   exact
/// @deprecated Last.
func Read();
)";
    const auto result = Rux::Formatting::Format(source);
    CHECK(result.changed);
    CHECK_EQ(result.text, expected);
}

TEST_CASE("formatter is idempotent for documentation forms") {
    constexpr std::string_view inputs[] = {
        "///Summary.\nfunc Read();\n",
        "/** Summary. */\nstruct Item {}\n",
        "/**\n    Summary.\n    @returns Value.\n*/\nfunc Value() -> int;\n",
        "/// Summary.\n///\n/// @param value Text.\nfunc Read(value: int);\n",
    };
    for (const std::string_view input : inputs) {
        const auto first = Rux::Formatting::Format(input);
        const auto second = Rux::Formatting::Format(first.text);
        CAPTURE(input);
        CHECK_FALSE(second.changed);
        CHECK_EQ(second.text, first.text);
    }
}
