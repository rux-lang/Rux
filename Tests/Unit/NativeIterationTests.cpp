// The iterator convention with native optionals: a `Next` returning `Item?` drives a `for` loop, presence continues
// it with the payload, and only outer absence ends it, so an item may itself be optional, fallible, a sum, or the unit.

#include "SemanticTestSupport.h"

using namespace Rux;
using namespace Rux::Testing::SemanticTestSupport;

namespace {
/// The errors in `source` other than the pending rejection of a loop awaiting its lowering.
std::vector<std::string> Errors(const std::string &source) {
    std::vector<std::string> errors;
    for (const auto &diagnostic : AnalyzeSource(source)) {
        if (diagnostic.severity == SemanticDiagnostic::Severity::Error &&
            !diagnostic.message.contains("is not supported yet")) {
            errors.push_back(diagnostic.message);
        }
    }
    return errors;
}

bool AnyContains(const std::vector<std::string> &errors, const std::string_view text) {
    return std::ranges::any_of(errors, [&](const std::string &error) { return error.contains(text); });
}
} // namespace

TEST_CASE("a Next returning an optional drives a loop with its payload") {
    CHECK(Errors(R"(
        struct E {}
        struct Counter { next: int32; }
        extend Counter {
            func Next(self: &var Counter) -> int32? {
                if self.next > 3i32 {
                    return none;
                }
                self.next = self.next + 1i32;
                return self.next;
            }
        }
        struct Bag { start: int32; }
        extend Bag {
            func Iterate(self: &Bag) -> Counter {
                return Counter { next: self.start };
            }
        }
        struct Lookups { index: int32; }
        extend Lookups {
            func Next(self: &var Lookups) -> int32?? { return none; }
        }
        struct Outcomes { index: int32; }
        extend Outcomes {
            func Next(self: &var Outcomes) -> (int32 ! E)? { return none; }
        }
        struct Units { index: int32; }
        extend Units {
            func Next(self: &var Units) -> ()? { return none; }
        }
        struct Members { index: int32; }
        extend Members {
            func Next(self: &var Members) -> (int32 | bool)? { return none; }
        }
        func Use(counter: Counter, bag: Bag, lookups: Lookups, outcomes: Outcomes, units: Units, members: Members) {
            var total = 0i32;
            for value in counter {
                total = total + value;
            }
            for value in bag {
                total = total + value;
            }
            for item in lookups {
                let optional: int32? = item;
            }
            for item in outcomes {
                let fallible: int32 ! E = item;
            }
            for item in units {
                let unit: () = item;
            }
            for item in members {
                let member: int32 | bool = item;
            }
        }
    )")
              .empty());
}

TEST_CASE("only an optional or an Option-shaped variant reports the end") {
    const auto errors = Errors(R"(
        struct E {}
        struct Fallible { index: int32; }
        extend Fallible {
            func Next(self: &var Fallible) -> int32 ! E { return 1i32; }
        }
        struct Shared { index: int32; }
        extend Shared {
            func Next(self: &Shared) -> int32? { return none; }
        }
        func Use(fallible: Fallible, shared: Shared) {
            for value in fallible {}
            for value in shared {}
        }
    )");
    CHECK(AnyContains(errors, "cannot iterate over 'Fallible'"));
    CHECK(AnyContains(errors, "iterator method 'Next' on 'Shared' must take a mutable receiver"));
}
