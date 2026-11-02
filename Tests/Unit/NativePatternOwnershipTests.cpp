// Ownership through native patterns and constructors: what a match over a native subject consumes, how bindings of a
// borrowed subject refer to it, the limits on a subset view, guards that must leave their bindings, and constructor
// operands that transfer by value.

#include "SemanticTestSupport.h"

using namespace Rux;
using namespace Rux::Testing::SemanticTestSupport;

namespace {
/// The errors in `source`.
std::vector<std::string> Errors(const std::string &source) {
    std::vector<std::string> errors;
    for (const auto &diagnostic : AnalyzeSource(source)) {
        if (diagnostic.severity == SemanticDiagnostic::Severity::Error) {
            errors.push_back(diagnostic.message);
        }
    }
    return errors;
}

bool AnyContains(const std::vector<std::string> &errors, const std::string_view text) {
    return std::ranges::any_of(errors, [&](const std::string &error) { return error.contains(text); });
}

constexpr std::string_view kDeclarations = R"(
    struct Options { verbose: bool; }
    struct Defaults {}
    struct Count { value: int32; }
    #NoCopy
    struct Owned { handle: int32; }
    extend Owned {
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
                _ => false
            };
            return verbose && again;
        }
        func Owner() {
            let value: Owned? = .Some(Owned { handle: 1i32 });
            Read(value);
            Read(value);
        }
        func Read(value: &(Owned?)) -> bool {
            return match value {
                .Some(owned) => Look(owned),
                none => false
            };
        }
        func Passed(value: &(Options | Defaults)) {
            match value {
                options: Options => Inspect(options),
                _ => {}
            }
        }
    )")
              .empty());

    const auto written = ErrorsIn(R"(
        func Shared(value: &(Options | Defaults)) {
            match value {
                options: Options => { options.verbose = true; },
                _ => {}
            }
        }
    )");
    CHECK(AnyContains(written, "cannot modify immutable variable 'options'"));
}

TEST_CASE("a subset view is never borrowed, addressed, stored, or moved") {
    const auto errors = ErrorsIn(R"(
        func Views(value: &(Options | Defaults | Count)) {
            match value {
                any: Options | Defaults => {
                    InspectAny(any);
                    let address = @any;
                    let stored: &(Options | Defaults) = any;
                    let moved = move any;
                },
                _ => {}
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
                        _ => false
                    };
                    let tested = any is Options;
                },
                _ => {}
            }
        }
    )")
              .empty());
}

TEST_CASE("an owned subject is consumed once by a match that binds") {
    CHECK(ErrorsIn(R"(
        func Transfer(value: Owned?) {
            match move value {
                .Some(owned) => Keep(move owned),
                none => {}
            }
        }
        func Copied(value: Options | Defaults) {
            match value {
                options: Options => UseOptions(options),
                _ => {}
            }
            match value {
                options: Options => UseOptions(options),
                _ => {}
            }
        }
        func Inspects(value: Owned?) -> bool {
            let present = match value {
                .Some(_) => true,
                none => false
            };
            let typed = match value {
                _: Owned => true,
                none => false
            };
            match move value {
                .Some(owned) => Keep(move owned),
                none => {}
            }
            return present && typed;
        }
    )")
              .empty());

    const auto implicit = ErrorsIn(R"(
        func Implicit(value: Owned?) {
            match value {
                .Some(owned) => Keep(move owned),
                none => {}
            }
        }
    )");
    CHECK(AnyContains(implicit, "cannot copy 'Owned?', which prohibits copying"));

    const auto twice = ErrorsIn(R"(
        func Twice(value: Owned?) {
            match move value {
                .Some(owned) => Keep(move owned),
                none => {}
            }
            match move value {
                .Some(owned) => Keep(move owned),
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
            match move value {
                .Some(owned) if Consume(move owned) => {},
                .Some(owned) => Keep(move owned),
                none => {}
            }
        }
    )");
    CHECK(AnyContains(errors, "pattern guard cannot move 'owned'"));

    CHECK(ErrorsIn(R"(
        func Guarded(value: Owned?) {
            match move value {
                .Some(owned) if Look(owned) => Keep(move owned),
                .Some(owned) => Keep(move owned),
                none => {}
            }
        }
    )")
              .empty());
}

TEST_CASE("arms merge the ownership states they leave") {
    const auto errors = ErrorsIn(R"(
        func Merge(value: int32 | bool, owned: Owned) {
            match value {
                number: int32 => Keep(move owned),
                flag: bool => {}
            }
            Keep(move owned);
        }
    )");
    CHECK(AnyContains(errors, "'owned'"));
}
