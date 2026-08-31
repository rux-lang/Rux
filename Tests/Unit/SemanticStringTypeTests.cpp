// Semantic contracts of the built-in string types: what a literal is typed as, what a string exposes, and the uses
// the language deliberately does not define for a view over validated text.

#include "Lexer/Lexer.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <doctest.h>
#include <string>
#include <utility>
#include <vector>

using namespace Rux;

namespace {
ParseResult Parse(const std::string &source) {
    Lexer lexer(source, "strings.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "strings.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    return parsed;
}

std::vector<SemanticDiagnostic> AnalyzeSource(const std::string &source) {
    ParseResult parsed = Parse(source);
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    return analyzer.Analyze().diagnostics;
}

/// The types of the `let` initializers in `Main`, in declaration order, so a test names the spellings it cares about
/// rather than walking the model itself.
std::vector<std::string> LetInitializerTypes(const std::string &body) {
    ParseResult parsed = Parse("func Main() {\n" + body + "\n}\n");
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());

    REQUIRE_EQ(parsed.module.items.size(), 1);
    const auto *main = dynamic_cast<const FuncDecl *>(parsed.module.items[0].get());
    REQUIRE(main != nullptr);
    REQUIRE(main->body != nullptr);

    std::vector<std::string> types;
    for (const auto &statement : main->body->stmts) {
        const auto *binding = dynamic_cast<const LetStmt *>(statement.get());
        if (!binding || !binding->init) {
            continue;
        }
        const TypeRef *type = model.TryGetType(*binding->init);
        REQUIRE(type != nullptr);
        types.push_back(type->ToString());
    }
    return types;
}

/// The messages of every diagnostic, so a test asserts what was reported rather than how many.
std::vector<std::string> Messages(const std::vector<SemanticDiagnostic> &diagnostics) {
    std::vector<std::string> messages;
    for (const SemanticDiagnostic &diagnostic : diagnostics) {
        messages.push_back(diagnostic.message);
    }
    return messages;
}
} // namespace

TEST_CASE("a string prefix names the encoding the literal is text in") {
    const auto types = LetInitializerTypes(R"(
        let eight = s8"text";
        let sixteen = s16"text";
        let thirtyTwo = s32"text";
    )");
    REQUIRE_EQ(types.size(), 3);
    CHECK_EQ(types[0], "string8");
    CHECK_EQ(types[1], "string16");
    CHECK_EQ(types[2], "string32");
}

TEST_CASE("an unprefixed literal is UTF-8 text") {
    // The bare form is UTF-8, which is what an unprefixed literal in a UTF-8 source file already is. The character
    // prefixes it replaced no longer spell a string at all.
    const auto types = LetInitializerTypes(R"(
        let bare = "text";
        let eight = s8"text";
    )");
    REQUIRE_EQ(types.size(), 2);
    CHECK_EQ(types[0], "string8");
    CHECK_EQ(types[1], "string8");
}

TEST_CASE("string is a spelling of string8") {
    const auto types = LetInitializerTypes(R"(
        let text: string = s8"text";
        let eight: string8 = s8"text";
    )");
    REQUIRE_EQ(types.size(), 2);
    CHECK_EQ(types[0], "string8");
    CHECK_EQ(types[1], "string8");
}

TEST_CASE("a string exposes its code units through data and its length in them") {
    const auto types = LetInitializerTypes(R"(
        let eight = s8"text";
        let sixteen = s16"text";
        let thirtyTwo = s32"text";
        let eightData = eight.data;
        let sixteenData = sixteen.data;
        let thirtyTwoData = thirtyTwo.data;
        let length = eight.length;
    )");
    REQUIRE_EQ(types.size(), 7);
    CHECK_EQ(types[3], "*char8");
    CHECK_EQ(types[4], "*char16");
    CHECK_EQ(types[5], "*char32");
    CHECK_EQ(types[6], "uint");
}

TEST_CASE("indexing a string yields one code unit of its own encoding") {
    const auto types = LetInitializerTypes(R"(
        let eight = s8"text";
        let sixteen = s16"text";
        let thirtyTwo = s32"text";
        let first = eight[0];
        let second = sixteen[0];
        let third = thirtyTwo[0];
    )");
    REQUIRE_EQ(types.size(), 6);
    CHECK_EQ(types[3], "char8");
    CHECK_EQ(types[4], "char16");
    CHECK_EQ(types[5], "char32");
}

TEST_CASE("a string has no member other than data and length") {
    const auto messages = Messages(AnalyzeSource(R"(
        func Main() {
            let text = s8"text";
            let size = text.size;
        }
    )"));
    REQUIRE_EQ(messages.size(), 2);
    CHECK_EQ(messages[0], "string type 'string8' has no member 'size'");
    CHECK_EQ(messages[1], "cannot infer type of 'size'");
}

TEST_CASE("a string is immutable through every place it can be written") {
    const auto messages = Messages(AnalyzeSource(R"(
        func Main() {
            var text = s8"text";
            text.length = 0;
            text[0] = c8'x';
        }
    )"));
    REQUIRE_EQ(messages.size(), 2);
    CHECK_EQ(messages[0], "cannot modify member 'length' of immutable string 'string8'");
    CHECK_EQ(messages[1], "cannot modify code units of immutable string 'string8'");
}

TEST_CASE("a range of a string is rejected rather than splitting a character") {
    const auto messages = Messages(AnalyzeSource(R"(
        func Main() {
            let text = s8"text";
            let part = text[0..2];
        }
    )"));
    REQUIRE_EQ(messages.size(), 2);
    CHECK_EQ(messages[0], "cannot take a range of string type 'string8'");
    CHECK_EQ(messages[1], "cannot infer type of 'part'");
}

TEST_CASE("the string encodings are separate types with no conversion between them") {
    const auto messages = Messages(AnalyzeSource(R"(
        func Eight(text: string8) {}

        func Main() {
            let wide: string16 = s8"text";
            Eight(s16"text");
        }
    )"));
    REQUIRE_EQ(messages.size(), 2);
    CHECK_EQ(messages[0], "cannot assign 'string8' to 'string16'");
    CHECK_EQ(messages[1], "argument 1 to 'Eight' has type 'string16', but parameter 'text' requires 'string8'");
}

TEST_CASE("a string is neither compared nor ordered in this version") {
    const auto messages = Messages(AnalyzeSource(R"(
        func Main() {
            let left = s8"one";
            let right = s8"two";
            let same = left == right;
            let ordered = left < right;
        }
    )"));
    REQUIRE_EQ(messages.size(), 2);
    CHECK_EQ(messages[0], "operator '==' is not defined for string type 'string8'");
    CHECK_EQ(messages[1], "operator '<' is not defined for string type 'string8'");
}

TEST_CASE("a string is not a value any cast converts") {
    const auto messages = Messages(AnalyzeSource(R"(
        func Main() {
            let text = s8"text";
            let number = text as uint64;
            let wider = text as string16;
        }
    )"));
    REQUIRE_GE(messages.size(), 2);
    CHECK_EQ(messages[0], "cannot cast value of type 'string8' to 'uint64'");
    CHECK_EQ(messages[1], "cannot cast value of type 'string8' to 'string16'");
}

TEST_CASE("a slice is built from a string's own members") {
    // The escape hatch: raw code units are still reachable, just never implicitly.
    const auto messages = Messages(AnalyzeSource(R"(
        struct Slice<T> {
            data: *T;
            length: uint;
        }

        func Main() {
            let text = s8"text";
            let units = Slice<char8> { data: text.data, length: text.length };
        }
    )"));
    CHECK(messages.empty());
}
