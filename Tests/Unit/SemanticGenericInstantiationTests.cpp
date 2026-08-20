#include "Lexer/Lexer.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <doctest.h>
#include <string>
#include <utility>
#include <vector>

using namespace Rux;

namespace {
std::vector<SemanticDiagnostic> AnalyzeSource(const std::string &source) {
    Lexer lexer(source, "instantiation.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "instantiation.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    return analyzer.Analyze().diagnostics;
}
} // namespace

TEST_CASE("a field reached through a pointer to a generic instantiation substitutes its arguments") {
    // The arguments are spelled in the name of the type that carries them, and a pointer has no name of its own.
    // Reading them off the pointer found none, so the fields were still spelled in the type parameters -- which
    // resolve to nothing outside the declaration that named them.
    const auto diagnostics = AnalyzeSource(R"(
        struct Node<T> {
            value: T;
            next: *var Node<T>;
        }

        struct Pair<T, U> {
            left: T;
            right: U;
        }

        func Total(node: *var Node<int32>) -> int32 {
            node.value = node.value + 1i32;
            return node.next == null ? node.value : node.value + Total(node.next);
        }

        func Combine(pair: *Pair<int32, int64>) -> int64 {
            return (pair.left as int64) + pair.right;
        }
    )");

    CHECK(diagnostics.empty());
}

TEST_CASE("an instantiation passed as a type argument waits for the parameter it was built from") {
    // `Node<T>` keeps its argument in its name, with nothing structural to walk. Reading only structure called it
    // concrete, so the call was queued as an instantiation of a type that does not exist yet and never re-queued
    // once the enclosing generic said what `T` was.
    const auto diagnostics = AnalyzeSource(R"(
        struct Node<T> {
            value: T;
            tag: int32;
        }

        func Width<T>() -> uint {
            return sizeof(T);
        }

        struct Holder<T> {
            seed: T;
        }

        extend Holder<T> {
            func Size(self: *Holder<T>) -> uint {
                return Width<Node<T>>();
            }
        }

        func Main() -> int {
            var holder = Holder<int32> { seed: 1 };
            return holder.Size() == 8u ? 0 : 1;
        }
    )");

    CHECK(diagnostics.empty());
}
