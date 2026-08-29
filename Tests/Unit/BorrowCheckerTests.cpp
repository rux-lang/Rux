#include "Lexer/Lexer.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <doctest.h>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Rux;

namespace {
std::vector<SemanticDiagnostic> AnalyzeBorrows(const std::string &source) {
    Lexer lexer(source, "borrow-checker.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "borrow-checker.rux");
    auto parsed = parser.Parse();
    std::string parserDiagnostics;
    for (const Diagnostic &diagnostic : parsed.diagnostics) {
        parserDiagnostics += diagnostic.message + "\n";
    }
    INFO(parserDiagnostics);
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", CompileTimeContext{});
    return analyzer.Analyze().diagnostics;
}

bool HasErrorContaining(const std::vector<SemanticDiagnostic> &diagnostics, const std::string_view text) {
    return std::ranges::any_of(diagnostics, [&](const SemanticDiagnostic &diagnostic) {
        return diagnostic.severity == Diagnostic::Severity::Error && diagnostic.message.contains(text);
    });
}

void CheckNoErrors(const std::vector<SemanticDiagnostic> &diagnostics) {
    for (const SemanticDiagnostic &diagnostic : diagnostics) {
        INFO(diagnostic.message);
        CHECK_NE(diagnostic.severity, Diagnostic::Severity::Error);
    }
}
} // namespace

TEST_CASE("shared loans coexist and end at their last path use") {
    const auto diagnostics = AnalyzeBorrows(R"(
        struct Item { value: int32; }
        func Read(item: &Item) {}
        func Write(item: &var Item) {}
        func Value(item: &Item) -> int32 { return item.value; }
        func Test(flag: bool) {
            var item = Item { value: 1i32 };
            let first: &Item = item;
            let second: &Item = item;
            if flag {
                Read(first);
            } else {
                Read(first);
            }
            Read(second);
            Write(item);
            let final: &var Item = item;
            if Value(final) != 1i32 || item.value != 1i32 {
                return;
            }
        }
    )");

    CheckNoErrors(diagnostics);
}

TEST_CASE("exclusive loans reject overlapping reads borrows and call arguments") {
    const auto diagnostics = AnalyzeBorrows(R"(
        struct Item { value: int32; }
        func Read(item: &Item) {}
        func Write(item: &var Item) {}
        func Pair(first: &var Item, second: &var Item) {}
        func Value(item: &Item) -> int32 { return item.value; }
        func SharedThenExclusive() {
            var item = Item { value: 1i32 };
            let shared: &Item = item;
            let exclusive: &var Item = item;
            Read(shared);
            Write(exclusive);
        }
        func ExclusiveThenRead() {
            var item = Item { value: 1i32 };
            let exclusive: &var Item = item;
            Read(item);
            Write(exclusive);
        }
        func OverlappingArguments() {
            var item = Item { value: 1i32 };
            Pair(item, item);
        }
        func AliasedArguments() {
            var item = Item { value: 1i32 };
            let exclusive: &var Item = item;
            Pair(exclusive, item);
        }
        func AcrossConditional(flag: bool) {
            var item = Item { value: 1i32 };
            let shared: &Item = item;
            if flag && Value(shared) == 1i32 {
                Write(item);
                Read(shared);
            }
        }
    )");

    CHECK(HasErrorContaining(diagnostics, "while it is immutably borrowed"));
    CHECK(HasErrorContaining(diagnostics, "holds an exclusive borrow"));
    CHECK(HasErrorContaining(diagnostics, "overlapping exclusive borrows"));
}

TEST_CASE("exclusive loans distinguish disjoint fields") {
    const auto diagnostics = AnalyzeBorrows(R"(
        struct Pair { left: int32; right: int32; }
        func Bump(value: &var int32) {}
        func Test() {
            var pair = Pair { left: 1i32, right: 2i32 };
            let left: &var int32 = pair.left;
            let right: &var int32 = pair.right;
            Bump(left);
            Bump(right);
        }
    )");

    CheckNoErrors(diagnostics);
}

TEST_CASE("reference provenance merges through control flow and expires after use") {
    const auto diagnostics = AnalyzeBorrows(R"(
        struct Item { value: int32; }
        func Read(item: &Item) {}
        func Write(item: &var Item) {}
        func Test(flag: bool) {
            var left = Item { value: 1i32 };
            var right = Item { value: 2i32 };
            var selected: &Item = left;
            if flag {
                selected = right;
            }
            Read(selected);
            Write(left);
            Write(right);
        }
    )");

    CheckNoErrors(diagnostics);
}

TEST_CASE("references cannot escape into storage or returns") {
    const auto diagnostics = AnalyzeBorrows(R"(
        struct Item { value: int32; }
        struct Box<T> { value: T; }
        type SharedItem = &Item;
        struct BadStruct { item: &Item; }
        variant BadEnum { Some(&Item) }
        union BadUnion { item: &Item }
        func Escape(item: &Item) -> &Item {
            return item;
        }
        func StoreGeneric(box: Box<SharedItem>) {}
        func Store(item: &Item) {
            let array = [item, item];
            let tuple = (item, item);
        }
    )");

    CHECK(HasErrorContaining(diagnostics, "field 'item' in struct 'BadStruct' cannot store reference"));
    CHECK(HasErrorContaining(diagnostics, "payload in variant case 'BadEnum::Some' cannot store reference"));
    CHECK(HasErrorContaining(diagnostics, "field 'item' in union 'BadUnion' cannot store reference"));
    CHECK(HasErrorContaining(diagnostics, "function return type cannot store reference"));
    CHECK(HasErrorContaining(diagnostics, "function parameter cannot store reference type 'Box<&Item>'"));
    CHECK(HasErrorContaining(diagnostics, "cannot escape through a return"));
    CHECK(HasErrorContaining(diagnostics, "array value cannot store reference"));
    CHECK(HasErrorContaining(diagnostics, "tuple value cannot store reference"));
}
