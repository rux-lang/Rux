#include "Lexer/Lexer.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <algorithm>
#include <doctest.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Rux;

namespace {
std::vector<SemanticDiagnostic> AnalyzeCoalescing(const std::string &source) {
    Lexer lexer(source, "coalescing.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "coalescing.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    return analyzer.Analyze().diagnostics;
}

bool HasCoalescingError(const std::vector<SemanticDiagnostic> &diagnostics, const std::string_view text) {
    return std::ranges::any_of(diagnostics, [text](const SemanticDiagnostic &diagnostic) {
        return diagnostic.severity == SemanticDiagnostic::Severity::Error && diagnostic.message.contains(text);
    });
}

bool HasCoalescingNote(const std::vector<SemanticDiagnostic> &diagnostics, const std::string_view text) {
    return std::ranges::any_of(diagnostics, [text](const SemanticDiagnostic &diagnostic) {
        return std::ranges::any_of(diagnostic.notes, [text](const std::string &note) { return note.contains(text); });
    });
}
} // namespace

TEST_CASE("coalescing accepts native optionals and generic payloads") {
    const auto diagnostics = AnalyzeCoalescing(R"(
        func Generic<T>(option: T?, fallback: T) -> T {
            return (move option) ?? move fallback;
        }
        func Reuse(option: int32?) -> int32 {
            let first = option ?? 1i32;
            return option ?? first;
        }
        func Concrete() -> int32 {
            return Generic<int32>(none, 7i32);
        }
        func Contextual(option: uint8?, pointer: (*int32)?) -> uint8 {
            let address: *int32 = pointer ?? null;
            return option ?? 1;
        }
    )");

    CHECK(diagnostics.empty());
}

TEST_CASE("coalescing diagnoses invalid operands and fallbacks") {
    const auto diagnostics = AnalyzeCoalescing(R"(
        variant Option<T> { Some(T), None }
        variant Result<T, E> { Success(T), Error(E) }
        enum Failure: int32 { Bad }

        func Scalar() -> int32 { return 1i32 ?? 2i32; }
        func Pointer(value: *int32) -> *int32 { return value ?? null; }
        func Legacy(value: Option<int32>) -> int32 { return (move value) ?? 0i32; }
        func Error(value: Result<int32, Failure>) -> int32 { return (move value) ?? 0i32; }
        func Fallback(value: int32?) -> int32 { return (move value) ?? false; }
    )");

    CHECK(HasCoalescingError(diagnostics, "requires an optional left operand, but found 'int32'"));
    CHECK(HasCoalescingError(diagnostics, "requires an optional left operand, but found '*int32'"));
    CHECK(HasCoalescingError(diagnostics, "requires an optional left operand, but found 'Option<int32>'"));
    CHECK(HasCoalescingNote(diagnostics, "a variant with 'Some' and 'None' cases is an ordinary variant; only a native "
                                         "optional is coalesced"));
    CHECK(HasCoalescingError(diagnostics, "requires an optional left operand, but found 'Result<int32, Failure>'"));
    CHECK(HasCoalescingError(diagnostics, "coalescing fallback has type 'bool8'"));
}

TEST_CASE("coalescing uses explicit and branch-sensitive ownership") {
    const auto implicitOperand = AnalyzeCoalescing(R"(
        struct Handle { value: int32; }
        extend Handle {
            func ~Handle(&var self) {}
        }
        func Test(option: Handle?, fallback: Handle) {
            let selected = option ?? move fallback;
        }
    )");
    CHECK(HasCoalescingError(implicitOperand, "cannot copy 'Handle?', which owns resources"));

    const auto implicitFallback = AnalyzeCoalescing(R"(
        struct Handle { value: int32; }
        extend Handle {
            func ~Handle(&var self) {}
        }
        func Test(option: Handle?, fallback: Handle) {
            let selected = (move option) ?? fallback;
        }
    )");
    CHECK(HasCoalescingError(implicitFallback, "cannot copy 'Handle', which owns resources"));

    const auto conditionalMove = AnalyzeCoalescing(R"(
        struct Handle { value: int32; }
        extend Handle {
            func ~Handle(&var self) {}
        }
        func Take(value: Handle) {}
        func Test(option: Handle?, fallback: Handle) {
            let selected = (move option) ?? move fallback;
            Take(move fallback);
        }
    )");
    CHECK(HasCoalescingError(conditionalMove, "value 'fallback' may have been moved on some control-flow paths"));

    const auto borrowMerge = AnalyzeCoalescing(R"(
        struct Item { value: int32; }
        func Read(item: &Item) -> int32 { return item.value; }
        func Write(item: &var Item) { item.value += 1i32; }
        func Test(option: int32?) {
            var item = Item { value: 1i32 };
            let borrowed: &Item = item;
            let selected = option ?? Read(borrowed);
            Write(item);
        }
    )");
    CHECK(borrowMerge.empty());
}

TEST_CASE("coalescing an optional reference borrows, and a generic payload is never a reference") {
    const auto concrete = AnalyzeCoalescing(R"(
        struct Item { value: int32; }
        extend Item {
            func Find(&self, present: bool) -> &int32? {
                if present { return self.value; }
                return none;
            }
        }
        func Take(value: &int32) {}
        func Test(item: &Item, fallback: &int32) {
            Take(item.Find(true) ?? fallback);
            let held = item.Find(false) ?? fallback;
            Take(held);
        }
    )");
    CHECK(concrete.empty());

    const auto temporary = AnalyzeCoalescing(R"(
        struct Item { value: int32; }
        extend Item {
            func Find(&self) -> &int32? { return self.value; }
        }
        func Test(item: &Item) -> int32 {
            let held = item.Find() ?? 3i32;
            return held;
        }
    )");
    CHECK(HasCoalescingError(temporary, "coalescing fallback has type 'int32', but the optional payload is '&int32'"));

    const auto generic = AnalyzeCoalescing(R"(
        func Generic<T>(option: T?, fallback: T) -> T {
            return (move option) ?? move fallback;
        }
        func Take(value: &int32) {}
        func Test(value: &int32) {
            Take(Generic<&int32>(none, value));
        }
    )");
    CHECK(HasCoalescingError(generic, "cannot be or hold a reference"));

    const auto prohibited = AnalyzeCoalescing(R"(
        #NoCopy
        #NoMove
        struct Pinned { value: int32; }
        extend Pinned {
        }
        func Generic<T>(option: T?, fallback: T) -> T {
            return (move option) ?? move fallback;
        }
        func Test(option: Pinned?, fallback: Pinned) {
            let direct = (move option) ?? move fallback;
            let deferred = Generic<Pinned>(none, Pinned { value: 1i32 });
        }
    )");
    CHECK(HasCoalescingError(prohibited, "cannot extract payload type 'Pinned' because moving it is prohibited"));
}
