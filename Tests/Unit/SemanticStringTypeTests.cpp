#include "IntrinsicTestDeclarations.h"
// Semantic contracts of text: what a literal is typed as, what a text slice exposes, and the uses the language
// deliberately does not define for a view over characters.

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
    Lexer lexer(source + std::string(Rux::Testing::StringDeclarations), "strings.rux");
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

    REQUIRE_EQ(parsed.module.items.size(), 5);
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

TEST_CASE("a string prefix names the encoding the literal is a slice of") {
    const auto types = LetInitializerTypes(R"(
        let eight = c8"text";
        let sixteen = c16"text";
        let thirtyTwo = c32"text";
    )");
    REQUIRE_EQ(types.size(), 3);
    CHECK_EQ(types[0], "char8[..]");
    CHECK_EQ(types[1], "char16[..]");
    CHECK_EQ(types[2], "char32[..]");
}

TEST_CASE("an unprefixed literal is UTF-8 text") {
    // The bare form is UTF-8, which is what an unprefixed literal in a UTF-8 source file already is.
    const auto types = LetInitializerTypes(R"(
        let bare = "text";
        let eight = c8"text";
    )");
    REQUIRE_EQ(types.size(), 2);
    CHECK_EQ(types[0], "char8[..]");
    CHECK_EQ(types[1], "char8[..]");
}

TEST_CASE("the text names are spellings of the character slices") {
    const auto types = LetInitializerTypes(R"(
        let text: string = c8"text";
        let eight: string8 = c8"text";
        let wide: string16 = c16"text";
    )");
    REQUIRE_EQ(types.size(), 3);
    CHECK_EQ(types[0], "char8[..]");
    CHECK_EQ(types[1], "char8[..]");
    CHECK_EQ(types[2], "char16[..]");
}

TEST_CASE("text exposes its code units through data and its length in them") {
    const auto types = LetInitializerTypes(R"(
        let eight = c8"text";
        let sixteen = c16"text";
        let thirtyTwo = c32"text";
        let eightData = eight.data;
        let sixteenData = sixteen.data;
        let thirtyTwoData = thirtyTwo.data;
        let length = eight.length;
    )");
    REQUIRE_EQ(types.size(), 7);
    CHECK_EQ(types[3], "*char8");
    CHECK_EQ(types[4], "*char16");
    CHECK_EQ(types[5], "*char32");
    CHECK_EQ(types[6], "uint64");
}

TEST_CASE("indexing text yields one code unit of its own encoding") {
    const auto types = LetInitializerTypes(R"(
        let eight = c8"text";
        let sixteen = c16"text";
        let thirtyTwo = c32"text";
        let first = eight[0];
        let second = sixteen[0];
        let third = thirtyTwo[0];
    )");
    REQUIRE_EQ(types.size(), 6);
    CHECK_EQ(types[3], "char8");
    CHECK_EQ(types[4], "char16");
    CHECK_EQ(types[5], "char32");
}

TEST_CASE("a range of text is a slice of the same code units") {
    const auto types = LetInitializerTypes(R"(
        let text = c8"text";
        let part = text[0..2];
        let tail = text[2..];
    )");
    REQUIRE_EQ(types.size(), 3);
    CHECK_EQ(types[1], "char8[..]");
    CHECK_EQ(types[2], "char8[..]");
}

TEST_CASE("text has no member other than data and length") {
    const auto messages = Messages(AnalyzeSource(R"(
        func Main() {
            let text = c8"text";
            let size = text.size;
        }
    )"));
    REQUIRE_EQ(messages.size(), 2);
    CHECK_EQ(messages[0], "slice type 'char8[..]' has no member 'size'");
    CHECK_EQ(messages[1], "cannot infer type of 'size'");
}

TEST_CASE("a literal's code units cannot be written through the view") {
    const auto messages = Messages(AnalyzeSource(R"(
        func Main() {
            var text = c8"text";
            text[0] = c8'x';
        }
    )"));
    REQUIRE_EQ(messages.size(), 1);
    CHECK_EQ(messages[0], "cannot modify elements through read-only slice 'char8[..]'");
}

TEST_CASE("the encodings are separate types with no conversion between them") {
    const auto messages = Messages(AnalyzeSource(R"(
        func Eight(text: string8) {}

        func Main() {
            let wide: string16 = c8"text";
            Eight(c16"text");
        }
    )"));
    REQUIRE_EQ(messages.size(), 2);
    CHECK_EQ(messages[0], "cannot assign 'char8[..]' to 'char16[..]'");
    CHECK_EQ(messages[1], "argument 1 to 'Eight' has type 'char16[..]', but parameter 'text' requires 'char8[..]'");
}

TEST_CASE("text is neither compared nor ordered in this version") {
    const auto messages = Messages(AnalyzeSource(R"(
        func Main() {
            let left = c8"one";
            let right = c8"two";
            let same = left == right;
            let ordered = left < right;
        }
    )"));
    REQUIRE_EQ(messages.size(), 2);
    CHECK_EQ(messages[0], "operator '==' is not defined for slice type 'char8[..]'");
    CHECK_EQ(messages[1], "operator '<' is not defined for slice type 'char8[..]'");
}

TEST_CASE("text is not a value any cast converts") {
    const auto messages = Messages(AnalyzeSource(R"(
        func Main() {
            let text = c8"text";
            let number = text as uint64;
            let wider = text as string16;
        }
    )"));
    REQUIRE_GE(messages.size(), 2);
    CHECK_EQ(messages[0], "cannot cast value of type 'char8[..]' to 'uint64'");
    CHECK_EQ(messages[1], "cannot cast value of type 'char8[..]' to 'char16[..]'");
}

TEST_CASE("an ordinary struct named Slice is built from a literal's own members") {
    // A user's `Slice` is a name like any other, and text still reaches it through the two members every view has.
    const auto messages = Messages(AnalyzeSource(R"(
        struct Slice<T> {
            data: *T;
            length: uint;
        }

        func Main() {
            let text = c8"text";
            let units = Slice<char8> { data: text.data, length: text.length };
        }
    )"));
    CHECK(messages.empty());
}
