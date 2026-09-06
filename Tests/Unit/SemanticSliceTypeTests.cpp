// The punctuation spellings of slices and ranges in type position: what each resolves to, how writability travels on
// the element, and the mistakes the spellings leave room for.

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
    Lexer lexer(source, "slices.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "slices.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    return parsed;
}

std::vector<std::string> Messages(const std::string &source) {
    ParseResult parsed = Parse(source);
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    std::vector<std::string> messages;
    for (const SemanticDiagnostic &diagnostic : analyzer.Analyze().diagnostics) {
        messages.push_back(diagnostic.message);
    }
    return messages;
}

/// The resolved types of the parameters of the first function in `source`, spelled the way diagnostics spell them.
std::vector<std::string> ParameterTypes(const std::string &source) {
    ParseResult parsed = Parse(source);
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());
    const auto *function = dynamic_cast<const FuncDecl *>(parsed.module.items[0].get());
    REQUIRE(function != nullptr);
    std::vector<std::string> types;
    for (const Param &parameter : function->params) {
        const TypeRef *type = model.TryGetType(*parameter.type);
        REQUIRE(type != nullptr);
        types.push_back(type->ToString());
    }
    return types;
}
} // namespace

TEST_CASE("slice and range spellings resolve to the slice and range kinds") {
    const auto types = ParameterTypes(R"(
        func Spellings(bytes: uint8[..], text: var char8[..], span: int..int, closed: int..=int, from: int..,
                       to: ..int, upTo: ..=int, whole: .., nested: int[..][..], boxed: *(var int[..])) {}
    )");
    REQUIRE_EQ(types.size(), 10);
    CHECK_EQ(types[0], "uint8[..]");
    CHECK_EQ(types[1], "var char8[..]");
    CHECK_EQ(types[2], "int..int");
    CHECK_EQ(types[3], "int..=int");
    CHECK_EQ(types[4], "int..");
    CHECK_EQ(types[5], "..int");
    CHECK_EQ(types[6], "..=int");
    CHECK_EQ(types[7], "..");
    CHECK_EQ(types[8], "int[..][..]");
    CHECK_EQ(types[9], "*(var int[..])");
}

TEST_CASE("every spelling reads back from an instantiation name unchanged") {
    // A generic instantiation is identified by its printed name and its fields are typed by reading the arguments
    // back out of that name, so a spelling that did not round-trip would give a field a different type from the one
    // the argument was written with.
    ParseResult parsed = Parse(R"(
        struct Box<T> { value: T; }
        func Main(bytes: Box<uint8[..]>, writable: Box<var uint8[..]>, pointers: Box<(*int)[..]>,
                  boxed: Box<*(var int[..])>, span: Box<int..int>, upTo: Box<..=uint>, whole: Box<..>,
                  spans: Box<int[..]..int[..]>, single: Box<(int,)>, pair: Box<(int, uint8[..])>) {
            let a = bytes.value;
            let b = writable.value;
            let c = pointers.value;
            let d = boxed.value;
            let e = span.value;
            let f = upTo.value;
            let g = whole.value;
            let h = spans.value;
            let i = single.value;
            let j = pair.value;
        }
    )");
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());
    const auto *main = dynamic_cast<const FuncDecl *>(parsed.module.items[1].get());
    REQUIRE(main != nullptr);
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
    CHECK_EQ(types, std::vector<std::string>{"uint8[..]", "var uint8[..]", "(*int)[..]", "*(var int[..])", "int..int",
                                             "..=uint", "..", "int[..]..int[..]", "(int,)", "(int, uint8[..])"});
}

TEST_CASE("a view is as writable as the place or pointer it was built from") {
    ParseResult parsed = Parse(R"(
        func Main(read: *int, write: *var int, shared: &int[4], exclusive: &var int[4], fixed: int[4]) {
            var open: int[4] = [1, 2, 3, 4];
            let a = read[..2];
            let b = write[..2];
            let c = shared[..];
            let d = exclusive[..];
            let e = fixed[..];
            let f = open[1..];
            let g = b[1..];
            let h = b.data;
            let i = b[0];
            let j = f.data;
        }
    )");
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());
    const auto *main = dynamic_cast<const FuncDecl *>(parsed.module.items[0].get());
    REQUIRE(main != nullptr);
    std::vector<std::string> types;
    for (const auto &statement : main->body->stmts) {
        const auto *binding = dynamic_cast<const LetStmt *>(statement.get());
        if (binding && binding->init && binding->name != "open") {
            const TypeRef *type = model.TryGetType(*binding->init);
            REQUIRE(type != nullptr);
            types.push_back(type->ToString());
        }
    }
    CHECK_EQ(types, std::vector<std::string>{"int[..]", "var int[..]", "int[..]", "var int[..]", "int[..]",
                                             "var int[..]", "var int[..]", "*var int", "int", "*var int"});
}

TEST_CASE("a pointer is sliced only with an end bound") {
    const auto messages = Messages(R"(
        func Main(pointer: *int) {
            let whole = pointer[..];
            let tail = pointer[1..];
            let bounded = pointer[1..3];
        }
    )");
    REQUIRE_EQ(messages.size(), 4);
    CHECK_EQ(messages[0], "cannot slice pointer '*int' without an end bound");
    CHECK_EQ(messages[2], "cannot slice pointer '*int' without an end bound");
}

TEST_CASE("a literal is never a writable view") {
    const auto messages = Messages(R"(
        func Main() {
            let text: var char8[..] = "abc";
        }
    )");
    REQUIRE_EQ(messages.size(), 1);
    CHECK_EQ(messages[0],
             "cannot assign string literal to 'var char8[..]' because literal text is stored in a read-only section");
}

TEST_CASE("an empty literal is a view of either flavor") {
    const auto messages = Messages(R"(
        func Main() {
            let none: int[..] = [];
            let writable: var int[..] = [];
        }
    )");
    CHECK(messages.empty());
}

TEST_CASE("slice element access is independent of descriptor binding mutability") {
    const auto messages = Messages(R"(
        func Write(values: var int[..]) {
            let view = values;
            view[0] = 1;
            view[1..2][0] = 2;
            var frozen: int[..] = values;
            frozen[0] = 3;
        }
    )");
    REQUIRE_EQ(messages.size(), 1);
    CHECK_EQ(messages[0], "cannot modify elements through read-only slice 'int[..]'");
}

TEST_CASE("all literal encodings reject writable destinations") {
    const auto messages = Messages(R"(
        func Literals() {
            let bytes: var char8[..] = c8"a";
            let words: var char16[..] = c16"a";
            let scalars: var char32[..] = c32"a";
        }
    )");
    REQUIRE_EQ(messages.size(), 3);
    for (const std::string &message : messages) {
        CHECK(message.contains("literal text is stored in a read-only section"));
    }
}

TEST_CASE("a writable slice weakens to a read-only one but not the reverse") {
    const auto messages = Messages(R"(
        func Weaken(view: var uint8[..]) -> uint8[..] { return view; }
        func Strengthen(view: uint8[..]) -> var uint8[..] { return view; }
    )");
    REQUIRE_EQ(messages.size(), 1);
    CHECK(messages[0].contains("'uint8[..]'"));
    CHECK(messages[0].contains("'var uint8[..]'"));
}

TEST_CASE("a two-sided range type spells its element once on each side") {
    const auto messages = Messages(R"(
        func Mismatch() {
            var span: int..uint;
        }
    )");
    REQUIRE_EQ(messages.size(), 1);
    CHECK_EQ(messages[0], "range type bounds must name one element type, but has 'int' and 'uint'");
}

TEST_CASE("range bounds are compiler-owned without any visible provider") {
    CHECK(Messages(R"(
        func Bounds(pair: int..int, closed: int..=int, from: int.., to: ..int, upto: ..=int) -> int {
            return pair.start + pair.end + closed.start + closed.end + from.start + to.end + upto.end;
        }
    )")
              .empty());
}

TEST_CASE("only bounds present in a range are available as members") {
    const auto messages = Messages(R"(
        func Missing(from: int.., to: ..int, full: ..) {
            let first = from.end;
            let second = to.start;
            let third = full.start;
        }
    )");
    std::vector<std::string> errors;
    for (const std::string &message : messages) {
        if (message.starts_with("range type")) {
            errors.push_back(message);
        }
    }
    REQUIRE_EQ(errors.size(), 3);
    CHECK_EQ(errors[0], "range type 'int..' has no member 'end'");
    CHECK_EQ(errors[1], "range type '..int' has no member 'start'");
    CHECK_EQ(errors[2], "range type '..' has no member 'start'");
}

TEST_CASE("a generic slice extension is rejected once and by what it is") {
    const auto messages = Messages(R"(
        extend T[..] {
            func Count(self: T[..]) -> uint64 { return self.length; }
        }
    )");
    REQUIRE_EQ(messages.size(), 1);
    CHECK_EQ(messages[0], "cannot extend slice type 'T[..]' because element type 'T' is not defined");
}

TEST_CASE("a concrete slice extension keys its methods on the element") {
    const auto messages = Messages(R"(
        extend int[..] {
            func Sum(self: int[..]) -> int {
                var total = 0;
                for value in self { total += value; }
                return total;
            }
        }

        extend char8[..] {
            func Count(self: char8[..]) -> uint64 { return self.length; }
        }

        func Main() -> int {
            let values: int[..] = [1, 2, 3];
            let count = "abc".Count();
            return values.Sum();
        }
    )");
    CHECK(messages.empty());
}

TEST_CASE("a range where an array length belongs is answered as the slice spelling") {
    const auto messages = Messages(R"(
        func Sized(values: int[..3]) {}
    )");
    REQUIRE_EQ(messages.size(), 1);
    CHECK_EQ(messages[0], "array length must be a non-negative compile-time integer");
}
