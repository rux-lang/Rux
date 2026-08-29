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
} // namespace

TEST_CASE("coalescing accepts exact structural Option shapes and generic payloads") {
    const auto diagnostics = AnalyzeCoalescing(R"(
        variant Maybe<T> { Some(T), None }
        func Generic<T>(option: Maybe<T>, fallback: T) -> T {
            return (<-option) ?? <-fallback;
        }
        func Reuse(option: Maybe<int32>) -> int32 {
            let first = option ?? 1i32;
            return option ?? first;
        }
        func Concrete() -> int32 {
            return Generic<int32>(Maybe::None<int32>(), 7i32);
        }
        variant Option<T> { Some(T), None }
        func Contextual(option: Option<uint8>, pointer: Option<*int32>) -> uint8 {
            let address: *int32 = pointer ?? null;
            return option ?? 1;
        }
    )");

    CHECK(diagnostics.empty());
}

TEST_CASE("coalescing diagnoses invalid operands shapes and fallbacks") {
    const auto diagnostics = AnalyzeCoalescing(R"(
        variant Option<T> { Some(T), None }
        variant Result<T, E> { Success(T), Error(E) }
        variant BadOption { Some(int32), None(int32) }
        variant BadGeneric<T> { Some(T), None(T) }
        enum Failure: int32 { Bad }

        func Scalar() -> int32 { return 1i32 ?? 2i32; }
        func Pointer(value: *int32) -> *int32 { return value ?? null; }
        func Borrowed(value: &Option<int32>) -> int32 { return value ?? 0i32; }
        func Error(value: Result<int32, Failure>) -> int32 { return (<-value) ?? 0i32; }
        func Shape(value: BadOption) -> int32 { return (<-value) ?? 0i32; }
        func GenericShape(value: BadGeneric<int32>) -> int32 { return (<-value) ?? 0i32; }
        func Fallback(value: Option<int32>) -> int32 { return (<-value) ?? false; }
    )");

    CHECK(HasCoalescingError(diagnostics, "requires an Option-shaped left operand, but found 'int32'"));
    CHECK(HasCoalescingError(diagnostics, "requires an Option-shaped left operand, but found '*int32'"));
    CHECK(HasCoalescingError(diagnostics, "requires an Option-shaped left operand, but found '&Option<int32>'"));
    CHECK(HasCoalescingError(diagnostics, "cannot be coalesced"));
    CHECK(HasCoalescingError(diagnostics, "requires an Option-shaped left operand, but found 'BadOption'"));
    CHECK(HasCoalescingError(diagnostics, "requires an Option-shaped left operand, but found 'BadGeneric<int32>'"));
    CHECK(HasCoalescingError(diagnostics, "coalescing fallback has type 'bool8'"));
}

TEST_CASE("coalescing uses explicit and branch-sensitive ownership") {
    const auto implicitOperand = AnalyzeCoalescing(R"(
        variant Option<T> { Some(T), None }
        struct Handle { value: int32; }
        extend Handle {
            func =(self: &var Handle, other: &Handle);
            func ~Handle(self: &var Handle) {}
        }
        func Test(option: Option<Handle>, fallback: Handle) {
            let selected = option ?? <-fallback;
        }
    )");
    CHECK(HasCoalescingError(implicitOperand, "requires an explicit '<-' in coalescing operand"));

    const auto implicitFallback = AnalyzeCoalescing(R"(
        variant Option<T> { Some(T), None }
        struct Handle { value: int32; }
        extend Handle {
            func =(self: &var Handle, other: &Handle);
            func ~Handle(self: &var Handle) {}
        }
        func Test(option: Option<Handle>, fallback: Handle) {
            let selected = (<-option) ?? fallback;
        }
    )");
    CHECK(HasCoalescingError(implicitFallback, "requires an explicit '<-' in coalescing fallback"));

    const auto conditionalMove = AnalyzeCoalescing(R"(
        variant Option<T> { Some(T), None }
        struct Handle { value: int32; }
        extend Handle {
            func =(self: &var Handle, other: &Handle);
            func ~Handle(self: &var Handle) {}
        }
        func Take(value: Handle) {}
        func Test(option: Option<Handle>, fallback: Handle) {
            let selected = (<-option) ?? <-fallback;
            Take(<-fallback);
        }
    )");
    CHECK(HasCoalescingError(conditionalMove, "value 'fallback' may have been moved on some control-flow paths"));

    const auto borrowMerge = AnalyzeCoalescing(R"(
        variant Option<T> { Some(T), None }
        struct Item { value: int32; }
        func Read(item: &Item) -> int32 { return item.value; }
        func Write(item: &var Item) { item.value += 1i32; }
        func Test(option: Option<int32>) {
            var item = Item { value: 1i32 };
            let borrowed: &Item = item;
            let selected = option ?? Read(borrowed);
            Write(item);
        }
    )");
    CHECK(borrowMerge.empty());
}

TEST_CASE("coalescing rejects reference payloads after concrete and generic substitution") {
    const auto concrete = AnalyzeCoalescing(R"(
        variant Option<T> { Some(T), None }
        func Take(value: &int32) {}
        func Test(value: &int32, fallback: &int32) {
            Take(Option::Some<&int32>(value) ?? fallback);
        }
    )");
    CHECK(HasCoalescingError(concrete, "cannot extract reference payload type '&int32'"));

    const auto generic = AnalyzeCoalescing(R"(
        variant Option<T> { Some(T), None }
        func Generic<T>(option: Option<T>, fallback: T) -> T {
            return (<-option) ?? <-fallback;
        }
        func Take(value: &int32) {}
        func Test(value: &int32) {
            Take(Generic<&int32>(Option::Some<&int32>(value), value));
        }
    )");
    CHECK(HasCoalescingError(generic, "cannot extract reference payload type '&int32'"));

    const auto prohibited = AnalyzeCoalescing(R"(
        variant Option<T> { Some(T), None }
        struct Pinned { value: int32; }
        extend Pinned {
            func =(self: &var Pinned, other: &Pinned);
            func <-(self: &var Pinned, other: Pinned);
        }
        func Generic<T>(option: Option<T>, fallback: T) -> T {
            return (<-option) ?? <-fallback;
        }
        func Test(option: Option<Pinned>, fallback: Pinned) {
            let direct = (<-option) ?? <-fallback;
            let deferred = Generic<Pinned>(Option::None<Pinned>(), Pinned { value: 1i32 });
        }
    )");
    CHECK(HasCoalescingError(prohibited, "cannot extract payload type 'Pinned' because moving it is prohibited"));
}
