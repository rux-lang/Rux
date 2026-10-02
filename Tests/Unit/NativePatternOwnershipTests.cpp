// Ownership through native patterns and constructors: what a match over a native subject consumes, how bindings of a
// borrowed subject refer to it, the limits on a subset view, guards that must leave their bindings, and constructor
// operands that transfer by value.

#include "SemanticTestSupport.h"

using namespace Rux;
using namespace Rux::Testing::SemanticTestSupport;

namespace {
/// The errors in `source` other than the pending native-type and native-value rejections.
std::vector<std::string> Errors(const std::string &source) {
    std::vector<std::string> errors;
    for (const auto &diagnostic : AnalyzeSource(source)) {
        if (diagnostic.severity == SemanticDiagnostic::Severity::Error &&
            !diagnostic.message.contains("is not supported in compiled code yet") &&
            !diagnostic.message.contains("is not supported yet")) {
            errors.push_back(diagnostic.message);
        }
    }
    return errors;
}

bool AnyContains(const std::vector<std::string> &errors, const std::string_view text) {
    return std::ranges::any_of(errors, [&](const std::string &error) { return error.contains(text); });
}

std::size_t CountContaining(const std::vector<std::string> &errors, const std::string_view text) {
    return static_cast<std::size_t>(
        std::ranges::count_if(errors, [&](const std::string &error) { return error.contains(text); }));
}

constexpr std::string_view kDeclarations = R"(
    struct Options { verbose: bool; }
    struct Defaults {}
    struct Count { value: int32; }
    struct Owned { handle: int32; }
    extend Owned {
        func =(self: &var Owned, other: &Owned);
    }
    struct ParseError {}
    func UseOptions(options: Options) {}
    func Inspect(options: &Options) {}
    func InspectAny(any: &(Options | Defaults)) {}
    func Keep(owned: Owned) {}
    func Look(owned: &Owned) -> bool { return true; }
)";

std::vector<std::string> ErrorsIn(const std::string_view body) {
    return Errors(std::string(kDeclarations) + std::string(body));
}
} // namespace

TEST_CASE("a borrowed subject is inspected in place and stays with its owner") {
    CHECK(ErrorsIn(R"(
        func Shared(value: &(Options | Defaults)) -> bool {
            let verbose = match value {
                options: Options => options.verbose,
                defaults: Defaults => false
            };
            let again = match value {
                options: Options => options.verbose,
                else => false
            };
            return verbose && again;
        }
        func Owner() {
            let value: Owned? = .Some(Owned { handle: 1i32 });
            Read(value);
            Read(value);
        }
        func Read(value: &Owned?) -> bool {
            return match value {
                owned? => Look(owned),
                none => false
            };
        }
        func Passed(value: &(Options | Defaults)) {
            match value {
                options: Options => Inspect(options),
                else => {}
            }
        }
    )")
              .empty());

    const auto written = ErrorsIn(R"(
        func Shared(value: &(Options | Defaults)) {
            match value {
                options: Options => { options.verbose = true; },
                else => {}
            }
        }
    )");
    CHECK(AnyContains(written, "cannot modify immutable variable 'options'"));
}

TEST_CASE("an exclusive subject is written through single-member bindings only") {
    CHECK(ErrorsIn(R"(
        func Write(value: &var (Options | Defaults)) {
            match value {
                options: Options => { options.verbose = true; },
                else => {}
            }
        }
        func WritePresent(value: &var Count?) {
            match value {
                count? => { count.value = 3i32; },
                none => {}
            }
        }
        func WriteChannel(value: &var (Count ! ParseError)) {
            match value {
                .Success(count) => { count.value = 3i32; },
                .Failure(_) => {}
            }
        }
    )")
              .empty());

    const auto subset = ErrorsIn(R"(
        func Write(value: &var (Options | Defaults | Count)) {
            match value {
                any: Options | Defaults => { any = Defaults {}; },
                else => {}
            }
        }
    )");
    CHECK(AnyContains(subset, "cannot modify immutable variable 'any'"));
}

TEST_CASE("a subset view is never borrowed, addressed, stored, or moved") {
    const auto errors = ErrorsIn(R"(
        func Views(value: &(Options | Defaults | Count)) {
            match value {
                any: Options | Defaults => {
                    InspectAny(any);
                    let address = @any;
                    let stored: &(Options | Defaults) = any;
                    let moved = <-any;
                },
                else => {}
            }
        }
    )");
    CHECK(AnyContains(errors, "subset view 'any' cannot be passed to a reference parameter"));
    CHECK(AnyContains(errors, "subset view 'any' cannot have its address taken"));
    CHECK(AnyContains(errors, "subset view 'any' cannot be stored as a reference"));
    CHECK(AnyContains(errors, "subset view 'any' cannot be moved"));

    // Copying a view into an owned destination of its type materializes it, and a view can be matched and tested.
    CHECK(ErrorsIn(R"(
        func Materialize(value: &(Options | Defaults | Count)) {
            match value {
                any: Options | Defaults => {
                    let owned: Options | Defaults = any;
                    let again: Options | Defaults = any;
                    InspectAny(owned);
                    let verbose = match any {
                        options: Options => options.verbose,
                        else => false
                    };
                    let tested = any is Options;
                },
                else => {}
            }
        }
    )")
              .empty());
}

TEST_CASE("an owned subject is consumed once by a match that binds") {
    CHECK(ErrorsIn(R"(
        func Transfer(value: Owned?) {
            match <-value {
                owned? => Keep(<-owned),
                none => {}
            }
        }
        func Copied(value: Options | Defaults) {
            match value {
                options: Options => UseOptions(options),
                else => {}
            }
            match value {
                options: Options => UseOptions(options),
                else => {}
            }
        }
        func Inspects(value: Owned?) -> bool {
            let present = match value {
                _? => true,
                none => false
            };
            let typed = match value {
                _: Owned => true,
                none => false
            };
            match <-value {
                owned? => Keep(<-owned),
                none => {}
            }
            return present && typed;
        }
    )")
              .empty());

    const auto implicit = ErrorsIn(R"(
        func Implicit(value: Owned?) {
            match value {
                owned? => Keep(<-owned),
                none => {}
            }
        }
    )");
    CHECK(AnyContains(implicit, "move-only value 'value' requires an explicit '<-' in match subject"));

    const auto twice = ErrorsIn(R"(
        func Twice(value: Owned?) {
            match <-value {
                owned? => Keep(<-owned),
                none => {}
            }
            match <-value {
                owned? => Keep(<-owned),
                none => {}
            }
        }
    )");
    CHECK(AnyContains(twice, "value"));
    CHECK_FALSE(twice.empty());
}

TEST_CASE("a failed guard leaves its bindings for later arms") {
    const auto errors = ErrorsIn(R"(
        func Consume(owned: Owned) -> bool { return true; }
        func Guarded(value: Owned?) {
            match <-value {
                owned? if Consume(<-owned) => {},
                owned? => Keep(<-owned),
                none => {}
            }
        }
    )");
    CHECK(AnyContains(errors, "pattern guard cannot move 'owned'"));

    CHECK(ErrorsIn(R"(
        func Guarded(value: Owned?) {
            match <-value {
                owned? if Look(owned) => Keep(<-owned),
                owned? => Keep(<-owned),
                none => {}
            }
        }
    )")
              .empty());
}

TEST_CASE("constructor operands transfer by value exactly once") {
    CHECK(ErrorsIn(R"(
        func Construct(owned: Owned, other: Owned, last: Owned) {
            let present: Owned? = .Some(<-owned);
            let success: Owned ! ParseError = .Success(<-other);
            let failure: int32 ! Owned = .Failure(<-last);
        }
        func Copies(options: Options) {
            let first: Options? = .Some(options);
            let second: Options ! ParseError = .Success(options);
        }
    )")
              .empty());

    const auto implicit = ErrorsIn(R"(
        func Construct(owned: Owned) {
            let present: Owned? = .Some(owned);
        }
    )");
    CHECK(AnyContains(implicit, "move-only value 'owned' requires an explicit '<-' in native constructor"));

    const auto twice = ErrorsIn(R"(
        func Construct(owned: Owned) {
            let present: Owned? = .Some(<-owned);
            let again: Owned ! ParseError = .Success(<-owned);
        }
    )");
    CHECK_EQ(CountContaining(twice, "'owned'"), 1);
}

TEST_CASE("return and fail require explicit transfers of move-only values") {
    CHECK(ErrorsIn(R"(
        func Returns(owned: Owned) -> Owned ! ParseError {
            return <-owned;
        }
        func Fails(owned: Owned) -> int32 ! Owned {
            fail <-owned;
        }
        func Wraps(owned: Owned) -> Owned ! ParseError {
            return .Success(<-owned);
        }
    )")
              .empty());

    const auto implicit = ErrorsIn(R"(
        func Returns(owned: Owned) -> Owned ! ParseError {
            return owned;
        }
        func Fails(owned: Owned) -> int32 ! Owned {
            fail owned;
        }
    )");
    CHECK_EQ(CountContaining(implicit, "requires an explicit '<-' in return"), 2);
}

TEST_CASE("arms merge the ownership states they leave") {
    const auto errors = ErrorsIn(R"(
        func Merge(value: int32 | bool, owned: Owned) {
            match value {
                number: int32 => Keep(<-owned),
                flag: bool => {}
            }
            Keep(<-owned);
        }
    )");
    CHECK(AnyContains(errors, "'owned'"));
}
