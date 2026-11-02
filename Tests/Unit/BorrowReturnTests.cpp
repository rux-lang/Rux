// Functions that return references: what the signature says the result borrows, the fact analysis records for it,
// and the borrow a call's result keeps on its source argument. The diagnostic text of each rejection is pinned by the
// BorrowReturn* goldens; these cases cover the recorded facts and the accepted forms.

#include "SemanticTestSupport.h"

#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Rux;
using namespace Rux::Testing::SemanticTestSupport;

namespace {
ParseResult ParseClean(const std::string_view source) {
    Lexer lexer(std::string(source), "borrow-return.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "borrow-return.rux");
    ParseResult result = parser.Parse();
    for (const auto &diagnostic : result.diagnostics) {
        CAPTURE(diagnostic.message);
    }
    REQUIRE_FALSE(result.HasErrors());
    return result;
}

const FuncDecl &Function(const ParseResult &result, const std::string_view name) {
    for (const auto &item : result.module.items) {
        if (const auto *function = dynamic_cast<const FuncDecl *>(item.get()); function && function->name == name) {
            return *function;
        }
        if (const auto *extension = dynamic_cast<const ImplDecl *>(item.get())) {
            for (const auto &method : extension->methods) {
                if (method->name == name) {
                    return *method;
                }
            }
        }
    }
    FAIL("missing function " << name);
    throw std::runtime_error("missing function");
}

std::vector<std::string> Errors(const std::vector<SemanticDiagnostic> &diagnostics) {
    std::vector<std::string> messages;
    for (const auto &diagnostic : diagnostics) {
        if (diagnostic.severity == SemanticDiagnostic::Severity::Error) {
            messages.push_back(diagnostic.message);
        }
    }
    return messages;
}

bool AnyContains(const std::vector<std::string> &messages, const std::string_view text) {
    return std::ranges::any_of(messages, [&](const std::string &message) { return message.contains(text); });
}
} // namespace

TEST_CASE("a returned reference records the parameter it borrows from") {
    ParseResult parsed = ParseClean(R"(
        struct Pair { first: int32; second: int32; }
        extend Pair {
            func First(&self) -> &int32 { return self.first; }
            func Second(&var self, other: &Pair) -> &var int32 { return self.second; }
            func Find(&self, present: bool) -> &int32? {
                if present { return self.first; }
                return none;
            }
            func Other(self, other: &Pair) -> &int32 { return other.first; }
        }
        func Larger(count: int32, pair: &Pair) -> &int32 { return pair.second; }
        func Element(index: uint, items: int32[..]) -> &int32 { return items[index]; }
        func Owned(pair: &Pair) -> int32 { return pair.first; }
    )");
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE(Errors(model.diagnostics).empty());

    const ResolvedBorrowReturn *first = model.TryGetBorrowReturn(Function(parsed, "First"));
    REQUIRE(first != nullptr);
    CHECK_EQ(first->parameterIndex, 0);
    CHECK(first->source->IsReceiver());
    CHECK_FALSE(first->exclusive);
    CHECK_FALSE(first->optional);

    const ResolvedBorrowReturn *second = model.TryGetBorrowReturn(Function(parsed, "Second"));
    REQUIRE(second != nullptr);
    CHECK(second->source->IsReceiver());
    CHECK(second->exclusive);

    const ResolvedBorrowReturn *found = model.TryGetBorrowReturn(Function(parsed, "Find"));
    REQUIRE(found != nullptr);
    CHECK(found->optional);

    // A receiver taken by value lends nothing, so the one reference parameter is the source.
    const ResolvedBorrowReturn *other = model.TryGetBorrowReturn(Function(parsed, "Other"));
    REQUIRE(other != nullptr);
    CHECK_EQ(other->source->name, "other");
    CHECK_EQ(other->parameterIndex, 1);

    const ResolvedBorrowReturn *larger = model.TryGetBorrowReturn(Function(parsed, "Larger"));
    REQUIRE(larger != nullptr);
    CHECK_EQ(larger->source->name, "pair");
    CHECK_EQ(larger->parameterIndex, 1);

    const ResolvedBorrowReturn *element = model.TryGetBorrowReturn(Function(parsed, "Element"));
    REQUIRE(element != nullptr);
    CHECK_EQ(element->source->name, "items");

    CHECK(model.TryGetBorrowReturn(Function(parsed, "Owned")) == nullptr);
}

TEST_CASE("an optional reference is written '&T?' and a reference to an optional '&(T?)'") {
    ParseResult parsed = ParseClean(R"(
        struct Pair { first: int32; second: int32; }
        extend Pair {
            func Find(&self) -> &int32? { return self.first; }
            func Holds(&self, value: &(int32?)) -> bool { return value is none; }
        }
    )");
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE(Errors(model.diagnostics).empty());
    const FuncDecl &find = Function(parsed, "Find");
    CHECK_EQ(ResolvedType(model, **find.returnType).ToString(), "&int32?");
    const FuncDecl &option = Function(parsed, "Holds");
    CHECK_EQ(ResolvedType(model, *option.params[1].type).ToString(), "&(int32?)");
}

TEST_CASE("a call's result keeps its source argument borrowed until its last use") {
    const auto accepted = Errors(AnalyzeSource(R"(
        struct Pair { first: int32; second: int32; }
        extend Pair {
            func First(&self) -> &int32 { return self.first; }
            func SecondMutable(&var self) -> &var int32 { return self.second; }
        }
        func Use(pair: &var Pair) -> int32 {
            let first = pair.First();
            let other = pair.First();
            let total: int32 = first + other;
            pair.first = 3i32;
            let second = pair.SecondMutable();
            second = total;
            return pair.second;
        }
    )"));
    CHECK(accepted.empty());

    const auto shared = Errors(AnalyzeSource(R"(
        struct Pair { first: int32; second: int32; }
        extend Pair {
            func First(&self) -> &int32 { return self.first; }
        }
        func Use(pair: &var Pair) -> int32 {
            let first = pair.First();
            pair.second = 3i32;
            return first;
        }
    )"));
    CHECK(AnyContains(shared, "cannot modify 'pair.second' while it is immutably borrowed"));

    const auto exclusive = Errors(AnalyzeSource(R"(
        struct Pair { first: int32; second: int32; }
        extend Pair {
            func SecondMutable(&var self) -> &var int32 { return self.second; }
        }
        func Use(pair: &var Pair) -> int32 {
            let second = pair.SecondMutable();
            let first: int32 = pair.first;
            second = first;
            return 0i32;
        }
    )"));
    CHECK(AnyContains(exclusive, "cannot read 'pair.first' while 'second' holds an exclusive borrow"));
}

TEST_CASE("the reference an optional reference holds borrows what the optional borrowed") {
    const auto coalesced = Errors(AnalyzeSource(R"(
        struct Pair { first: int32; second: int32; }
        extend Pair {
            func Find(&self, present: bool) -> &int32? {
                if present { return self.first; }
                return none;
            }
        }
        func Use(pair: &var Pair, fallback: &int32) -> int32 {
            let found = pair.Find(true) ?? fallback;
            pair.first = 2i32;
            return found;
        }
    )"));
    CHECK(AnyContains(coalesced, "cannot modify 'pair.first' while it is"));

    const auto bound = Errors(AnalyzeSource(R"(
        struct Pair { first: int32; second: int32; }
        extend Pair {
            func Find(&self, present: bool) -> &int32? {
                if present { return self.first; }
                return none;
            }
        }
        func Use(pair: &var Pair) -> int32 {
            if pair.Find(true) is .Some(value) {
                pair.first = 2i32;
                return value;
            }
            return 0i32;
        }
    )"));
    CHECK(AnyContains(bound, "cannot modify 'pair.first' while it is immutably borrowed"));

    const auto matched = Errors(AnalyzeSource(R"(
        struct Pair { first: int32; second: int32; }
        extend Pair {
            func Find(&self, present: bool) -> &int32? {
                if present { return self.first; }
                return none;
            }
        }
        func Use(pair: &var Pair) -> int32 {
            let found = pair.Find(true);
            match found {
                .Some(value) => {
                    pair.second = 2i32;
                    let copy: int32 = value;
                },
                none => {}
            }
            return 0i32;
        }
    )"));
    CHECK(AnyContains(matched, "cannot modify 'pair.second' while it is immutably borrowed"));
}

TEST_CASE("a returned reference comes from a function called by name, on storage that outlives the statement") {
    const auto errors = Errors(AnalyzeSource(R"(
        struct Pair { first: int32; second: int32; }
        extend Pair {
            func First(&self) -> &int32 { return self.first; }
        }
        func Make() -> Pair { return Pair { first: 1i32, second: 2i32 }; }
        func Indirect(pair: &Pair, read: func(&Pair) -> &int32) -> int32 {
            return read(pair);
        }
        func Temporary() -> int32 {
            return Make().First();
        }
    )"));
    CHECK(AnyContains(errors, "cannot call a function value that returns reference '&int32'"));
    CHECK(AnyContains(errors, "the returned reference would borrow from a temporary value"));
}
