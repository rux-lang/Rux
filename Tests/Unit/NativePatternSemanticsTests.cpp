// What native, typed, and presence patterns select, whether a match over a native subject covers every value without
// an arm that can never run, the free-name rule for binding patterns, and the modes of `is`.

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

/// Every error diagnostic in `source` with its notes, for the cases that check a note.
std::vector<SemanticDiagnostic> ErrorDiagnostics(const std::string &source) {
    std::vector<SemanticDiagnostic> errors;
    for (auto &diagnostic : AnalyzeSource(source)) {
        if (diagnostic.severity == SemanticDiagnostic::Severity::Error) {
            errors.push_back(std::move(diagnostic));
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
    struct ParseError {}
    struct IoError {}
    func Use(value: int32) {}
)";

std::vector<std::string> ErrorsIn(const std::string_view body) {
    return Errors(std::string(kDeclarations) + std::string(body));
}
} // namespace

TEST_CASE("fallible matches cover both channels at every level") {
    CHECK(ErrorsIn(R"(
        func Channels(outcome: Options ! ParseError) -> int32 {
            return match outcome {
                .Success(options) => 1i32,
                .Failure(error) => 2i32
            };
        }
        func Nested(outcome: (int32 ! ParseError) ! IoError) -> int32 {
            return match outcome {
                .Success(.Success(value)) => value,
                .Success(.Failure(error)) => 2i32,
                .Failure(error) => 3i32
            };
        }
        func SameType(outcome: int32 ! int32) -> int32 {
            return match outcome {
                .Success(value) => value,
                .Failure(code) => code
            };
        }
        func Discard(outcome: Options ! ParseError) {
            match outcome {
                .Success(_) => {},
                .Failure(_) => {}
            }
        }
    )")
              .empty());

    const auto missing = ErrorsIn(R"(
        func Channels(outcome: Options ! ParseError) -> int32 {
            return match outcome {
                .Success(options) => 1i32
            };
        }
        func Nested(outcome: (int32 ! ParseError) ! IoError) -> int32 {
            return match outcome {
                .Success(.Success(value)) => value,
                .Failure(error) => 3i32
            };
        }
    )");
    CHECK(AnyContains(missing, "match on 'Options ! ParseError' is not exhaustive; missing .Failure(_)"));
    CHECK(AnyContains(missing, "match on '(int32 ! ParseError) ! IoError' is not exhaustive; missing "
                               ".Success(.Failure(_))"));
}

TEST_CASE("every nested optional state is a separate case") {
    CHECK(ErrorsIn(R"(
        func Lookup(found: int32??) -> int32 {
            return match found {
                .Some(.Some(value)) => value,
                .Some(none) => 1i32,
                none => 2i32
            };
        }
    )")
              .empty());

    const auto missing = ErrorsIn(R"(
        func Lookup(found: int32??) -> int32 {
            return match found {
                .Some(.Some(value)) => value,
                none => 2i32
            };
        }
    )");
    CHECK(AnyContains(missing, "match on 'int32?\?' is not exhaustive; missing .Some(none)"));
}

TEST_CASE("the presence suffix covers exactly what its constructor spelling does") {
    CHECK(ErrorsIn(R"(
        func Suffix(found: int32??) -> int32 {
            return match found {
                value?? => value,
                none? => 1i32,
                none => 2i32
            };
        }
        func Mixed(found: int32??) -> int32 {
            return match found {
                .Some(value?) => value,
                none? => 1i32,
                none => 2i32
            };
        }
        func Guarded(count: int32?) -> int32 {
            return match count {
                value? if value > 0i32 => value,
                _? => 0i32,
                none => 1i32
            };
        }
    )")
              .empty());

    // A suffix after its constructor spelling adds nothing, and the reverse holds too.
    const auto repeated = ErrorsIn(R"(
        func Repeated(count: int32?) -> int32 {
            return match count {
                .Some(value) => value,
                other? => other,
                none => 0i32
            };
        }
        func Reversed(count: int32?) -> int32 {
            return match count {
                value? => value,
                .Some(other) => other,
                none => 0i32
            };
        }
    )");
    CHECK_EQ(CountContaining(repeated, "match arm is unreachable"), 2);

    const auto missing = ErrorsIn(R"(
        func Missing(found: int32??) -> int32 {
            return match found {
                value?? => value,
                none => 2i32
            };
        }
    )");
    CHECK(AnyContains(missing, "missing .Some(none)"));

    const auto wrongSubject = ErrorsIn(R"(
        func OnSum(value: int32 | bool) -> int32 {
            return match value {
                v? => 1i32,
                else => 2i32
            };
        }
        func OnFallible(value: int32 ! ParseError) -> int32 {
            return match value {
                v? => 1i32,
                else => 2i32
            };
        }
    )");
    CHECK(AnyContains(wrongSubject,
                      "a presence suffix needs an optional subject, but the matched value has type 'bool8 | int32'"));
    CHECK(AnyContains(
        wrongSubject,
        "a presence suffix needs an optional subject, but the matched value has type 'int32 ! ParseError'"));
}

TEST_CASE("typed patterns select members, one presence level, or the whole subject") {
    CHECK(ErrorsIn(R"(
        func Members(value: Options | Defaults) -> int32 {
            return match value {
                options: Options => 1i32,
                defaults: Defaults => 2i32
            };
        }
        func Subset(value: Options | Defaults | int32) -> int32 {
            return match value {
                any: Options | Defaults => 1i32,
                number: int32 => number
            };
        }
        func Wildcard(value: Options | Defaults) -> int32 {
            return match value {
                _: Options => 1i32,
                _: Defaults => 2i32
            };
        }
        func Presence(count: int32?) -> int32 {
            return match count {
                value: int32 => value,
                none => 0i32
            };
        }
        func OuterPresence(found: int32??) -> int32 {
            return match found {
                stored: int32? => 1i32,
                none => 0i32
            };
        }
        func PresentMember(value: (Options | Defaults)?) -> int32 {
            return match value {
                options: Options => 1i32,
                defaults: Defaults => 2i32,
                none => 0i32
            };
        }
        func Whole(count: int32?) -> int32 {
            return match count {
                whole: int32? => 1i32
            };
        }
        func WholeNested(found: int32??) -> int32 {
            return match found {
                whole: int32?? => 1i32
            };
        }
    )")
              .empty());

    const auto rejected = ErrorsIn(R"(
        func SkipsLevel(found: int32??) -> int32 {
            return match found {
                value: int32 => value,
                else => 0i32
            };
        }
        func Channel(outcome: Options ! ParseError) -> int32 {
            return match outcome {
                options: Options => 1i32,
                else => 0i32
            };
        }
        func NotMember(value: Options | Defaults) -> int32 {
            return match value {
                number: int32 => number,
                else => 0i32
            };
        }
        func Plain(value: int32) -> int32 {
            return match value {
                flag: bool => 1i32,
                else => 0i32
            };
        }
    )");
    CHECK(AnyContains(rejected, "typed pattern 'int32' cannot select from optional 'int32?\?'"));
    CHECK(AnyContains(rejected, "typed pattern 'Options' cannot select from fallible 'Options ! ParseError'"));
    CHECK(AnyContains(rejected, "type 'int32' is not a member or subset of sum 'Defaults | Options'"));
    CHECK(AnyContains(rejected, "typed pattern 'bool8' cannot match a value of type 'int32'"));

    // A typed pattern binds the type its annotation names, so a selected member is not the whole sum.
    const auto bound = ErrorsIn(R"(
        func UseOptions(options: Options) {}
        func Bound(value: Options | Defaults) {
            match value {
                options: Options => UseOptions(options),
                any: Options | Defaults => UseOptions(any)
            }
        }
    )");
    CHECK(AnyContains(bound, "Defaults | Options"));
}

TEST_CASE("the help for a typed pattern on a fallible names both channel patterns") {
    const auto errors = ErrorDiagnostics(std::string(kDeclarations) + R"(
        func Channel(outcome: Options ! ParseError) -> int32 {
            return match outcome {
                options: Options => 1i32,
                else => 0i32
            };
        }
    )");
    REQUIRE_EQ(errors.size(), 1);
    REQUIRE(errors.front().help.has_value());
    CHECK_EQ(*errors.front().help, "match a channel with '.Success(v: Options)' or '.Failure(e: ParseError)'");
}

TEST_CASE("guards never cover and only wholly covered arms are unreachable") {
    const auto guarded = ErrorsIn(R"(
        func Guarded(value: Options | Defaults) -> int32 {
            return match value {
                options: Options if options.verbose => 1i32,
                defaults: Defaults => 2i32
            };
        }
    )");
    CHECK(AnyContains(guarded, "match on 'Defaults | Options' is not exhaustive; missing _: Options"));

    CHECK(ErrorsIn(R"(
        func Overlap(value: Options | Defaults | int32) -> int32 {
            return match value {
                first: Options | Defaults => 1i32,
                second: Defaults | int32 => 2i32
            };
        }
    )")
              .empty());

    const auto covered = ErrorsIn(R"(
        func Covered(value: Options | Defaults | int32) -> int32 {
            return match value {
                first: Options | Defaults => 1i32,
                _: Options => 2i32,
                number: int32 => number
            };
        }
    )");
    CHECK_EQ(CountContaining(covered, "match arm is unreachable because earlier arms already match every value it "
                                      "matches"),
             1);
}

TEST_CASE("an else arm is never unreachable") {
    CHECK(ErrorsIn(R"(
        func Covered(value: Options | Defaults) -> int32 {
            return match value {
                options: Options => 1i32,
                defaults: Defaults => 2i32,
                else => 3i32
            };
        }
        func Whole(value: Options | Defaults) -> int32 {
            return match value {
                any => 1i32,
                else => 3i32
            };
        }
        func Pick<T, U>(value: T | U) -> int32 {
            return match value {
                x: T => 1i32,
                else => 2i32
            };
        }
        func Use() {
            let same = Pick<int32, int32>(1i32);
            let different = Pick<int32, int64>(1i32);
        }
    )")
              .empty());
}

TEST_CASE("arms that collapse together are reported where they are instantiated") {
    const auto errors = ErrorDiagnostics(R"(
        func Pair<T, U>(value: T | U) -> int32 {
            return match value {
                x: T => 1i32,
                y: U => 2i32
            };
        }
        func Use() {
            let different = Pair<int32, int64>(1i32);
            let same = Pair<int32, int32>(1i32);
        }
    )");
    REQUIRE_EQ(errors.size(), 1);
    CHECK(errors.front().message.contains("match arm is unreachable"));
    REQUIRE_EQ(errors.front().notes.size(), 1);
    CHECK_EQ(errors.front().notes.front(), "in 'Pair' instantiated with T = int32, U = int32");

    // An annotation that is no member of the written subject can become one after substitution, and the reverse.
    CHECK(Errors(R"(
        func Select<T>(value: int32 | bool) -> int32 {
            return match value {
                x: T => 1i32,
                else => 2i32
            };
        }
        func Use() {
            let valid = Select<int32>(true);
        }
    )")
              .empty());
    CHECK(AnyContains(Errors(R"(
        func Select<T>(value: int32 | bool) -> int32 {
            return match value {
                x: T => 1i32,
                else => 2i32
            };
        }
        func Use() {
            let invalid = Select<int64>(true);
        }
    )"),
                      "type 'int64' is not a member or subset of sum 'bool8 | int32'"));
}

TEST_CASE("a typed pattern on the whole subject is portable across instantiations") {
    CHECK(ErrorsIn(R"(
        func Size<T>(value: T) -> int32 {
            return 1i32;
        }
        func Describe<T>(value: T) -> int32 {
            return match value {
                v: T => Size(v)
            };
        }
        func Use(optional: int32?, outcome: int32 ! ParseError) {
            let plain = Describe(1i32);
            let present = Describe(optional);
            let fallible = Describe(outcome);
        }
    )")
              .empty());
}

TEST_CASE("qualified case patterns select a variant member of a sum") {
    const std::string declarations = std::string(kDeclarations) + R"(
        variant DecodeError {
            Missing,
            InvalidDigit(int32)
        }
        variant Outcome<T> {
            Done(T),
            Pending
        }
    )";
    CHECK(Errors(declarations + R"(
        func Recover(error: DecodeError | IoError) -> int32 {
            return match error {
                DecodeError::Missing => 1i32,
                DecodeError::InvalidDigit(position) => position,
                e: IoError => 3i32
            };
        }
        func Channel(outcome: Options ! DecodeError) -> int32 {
            return match outcome {
                .Success(_) => 0i32,
                .Failure(.Missing) => 1i32,
                .Failure(.InvalidDigit(position)) => position
            };
        }
    )")
              .empty());

    const auto missing = Errors(declarations + R"(
        func Recover(error: DecodeError | IoError) -> int32 {
            return match error {
                DecodeError::Missing => 1i32,
                e: IoError => 3i32
            };
        }
    )");
    CHECK(AnyContains(missing, "missing DecodeError::InvalidDigit(_)"));

    const auto ambiguous = Errors(declarations + R"(
        func Pick(value: Outcome<int32> | Outcome<bool>) -> int32 {
            return match value {
                Outcome::Pending => 1i32,
                else => 2i32
            };
        }
    )");
    CHECK(AnyContains(ambiguous, "case pattern on 'Outcome' is ambiguous in 'Outcome<bool8> | Outcome<int32>'"));

    const auto unqualified = ErrorDiagnostics(declarations + R"(
        func Recover(error: DecodeError | IoError) -> int32 {
            return match error {
                .Missing => 1i32,
                else => 2i32
            };
        }
    )");
    REQUIRE_EQ(unqualified.size(), 1);
    CHECK_EQ(unqualified.front().message, "case pattern '.Missing' cannot select from sum 'DecodeError | IoError'");
    REQUIRE(unqualified.front().help.has_value());
    CHECK_EQ(*unqualified.front().help, "write 'DecodeError::Missing' to select the member and its case");
}

TEST_CASE("a binding pattern must introduce a free name") {
    const std::string declarations = std::string(kDeclarations) + R"(
        type Settings = Options;
        const Limit: int32 = 4i32;
        func Helper() {}
        variant DecodeError {
            Missing,
            InvalidDigit(int32)
        }
    )";
    const auto errors = ErrorDiagnostics(declarations + R"(
        func First(value: Options | Defaults) -> int32 {
            return match value {
                Options => 1i32,
                else => 2i32
            };
        }
        func Middle(value: int32) -> int32 {
            return match value {
                1i32 => 1i32,
                Settings => 2i32,
                else => 3i32
            };
        }
        func Last(value: int32) -> int32 {
            return match value {
                1i32 => 1i32,
                Limit => 2i32
            };
        }
        func Function(value: int32) -> int32 {
            return match value {
                Helper => 1i32
            };
        }
        func Parameter<T>(value: int32) -> int32 {
            return match value {
                T => 1i32
            };
        }
        func Case(error: DecodeError) -> int32 {
            return match error {
                Missing => 1i32
            };
        }
        func MemberCase(error: DecodeError | IoError) -> int32 {
            return match error {
                Missing => 1i32
            };
        }
        func Nested(outcome: int32 ! DecodeError) -> int32 {
            return match outcome {
                .Success(value) => value,
                .Failure(Missing) => 1i32
            };
        }
    )");
    const auto messageCount = [&](const std::string_view text) {
        return std::ranges::count_if(errors, [&](const auto &error) { return error.message.contains(text); });
    };
    CHECK_EQ(messageCount("pattern 'Options' cannot bind a new variable because 'Options' already names a type"), 1);
    CHECK_EQ(messageCount("pattern 'Settings' cannot bind a new variable because 'Settings' already names a type"), 1);
    CHECK_EQ(messageCount("pattern 'Limit' cannot bind a new variable because 'Limit' already names a constant"), 1);
    CHECK_EQ(messageCount("pattern 'Helper' cannot bind a new variable because 'Helper' already names a function"), 1);
    CHECK_EQ(messageCount("pattern 'T' cannot bind a new variable because 'T' already names a"), 1);
    CHECK_EQ(messageCount("pattern 'Missing' cannot bind a new variable because 'Missing' is a case of variant "
                          "'DecodeError'"),
             3);

    const auto options =
        std::ranges::find_if(errors, [](const auto &error) { return error.message.starts_with("pattern 'Options'"); });
    REQUIRE(options != errors.end());
    REQUIRE(options->help.has_value());
    CHECK_EQ(*options->help, "write 'options: Options' to select the member, 'Options { ... }' to destructure it, or "
                             "'Type::Case' to select a case");
    REQUIRE_EQ(options->notes.size(), 1);
    CHECK(options->notes.front().contains("'Options' was declared as a type"));

    // Shadowing a variable or a parameter stays legal, and so does a free lowercase binding.
    CHECK(Errors(declarations + R"(
        func Shadow(value: int32, limit: int32) -> int32 {
            let count = 3i32;
            return match value {
                1i32 => limit,
                count => count
            };
        }
        func Rebind(limit: int32) -> int32 {
            return match limit {
                limit => limit
            };
        }
    )")
              .empty());
}

TEST_CASE("native and nominal absence patterns are not interchangeable") {
    const auto errors = ErrorDiagnostics(std::string(kDeclarations) + R"(
        variant Option<T> {
            Some(T),
            None
        }
        func Nominal(value: Option<int32>) -> int32 {
            return match value {
                none => 0i32,
                else => 1i32
            };
        }
        func Native(value: int32?) -> int32 {
            return match value {
                .None => 0i32,
                else => 1i32
            };
        }
    )");
    REQUIRE_EQ(errors.size(), 2);
    CHECK_EQ(errors[0].message, "'none' matches a native optional; use '.None' for variant 'Option'");
    CHECK_EQ(errors[1].message, "'.None' matches a variant case; use 'none' for native optional 'int32?'");
    REQUIRE(errors[1].help.has_value());
    CHECK_EQ(*errors[1].help, "write 'none'");
}

TEST_CASE("native levels open only through their own patterns") {
    const auto errors = ErrorsIn(R"(
        func Literal(count: int32?) -> int32 {
            return match count {
                7i32 => 1i32,
                else => 0i32
            };
        }
        func Crossed(count: int32?) -> int32 {
            return match count {
                .Success(value) => value,
                else => 0i32
            };
        }
        func Arity(outcome: int32 ! ParseError) -> int32 {
            return match outcome {
                .Success(a, b) => 1i32,
                else => 0i32
            };
        }
    )");
    CHECK(AnyContains(errors, "this pattern cannot match native optional 'int32?'"));
    CHECK(AnyContains(errors, "'.Success(...)' matches a native fallible, but the matched value has type 'int32?'"));
    CHECK(AnyContains(errors, "pattern '.Success' expects 1 field, but found 2"));
}

TEST_CASE("a unit subject is covered by the unit pattern") {
    CHECK(ErrorsIn(R"(
        func Unit(value: ()) -> int32 {
            return match value {
                () => 1i32
            };
        }
        func Completion(outcome: ! ParseError) -> int32 {
            return match outcome {
                .Success(()) => 1i32,
                .Failure(_) => 0i32
            };
        }
    )")
              .empty());
}

TEST_CASE("is selects membership by the subject's form") {
    CHECK(ErrorsIn(R"(
        func Member(value: Options | Defaults | int32) -> bool {
            return value is Defaults && value is (Options | Defaults);
        }
        func Present(count: int32?, any: (Options | Defaults)?) -> bool {
            return count is int32 && any is Options && any is (Options | Defaults);
        }
        func Exact(count: int32) -> bool {
            return count is int32;
        }
        func Borrowed(value: &(Options | Defaults)) -> bool {
            return value is Options;
        }
        struct Box<T> { value: T; }
        func Generic(value: Box<int32?> | Defaults) -> bool {
            return value is Box<int32?>;
        }
        func Collapsed<T, U>(value: T | U) -> bool {
            return value is T;
        }
        func Use() {
            let same = Collapsed<int32, int32>(1i32);
            let different = Collapsed<int32, bool>(true);
        }
    )")
              .empty());

    const auto rejected = ErrorsIn(R"(
        func NotMember(value: Options | Defaults) -> bool {
            return value is int32;
        }
        func Deeper(found: int32??) -> bool {
            return found is int32;
        }
        func Channel(outcome: int32 ! ParseError) -> bool {
            return outcome is int32;
        }
        func Plain(count: int32) -> bool {
            return count is int64;
        }
    )");
    CHECK(AnyContains(rejected, "type 'int32' is not a member or subset of sum 'Defaults | Options'"));
    CHECK(AnyContains(rejected, "type 'int32' is neither the payload of optional 'int32?\?' nor a member of it"));
    CHECK(AnyContains(rejected, "'is' cannot test the channel of fallible 'int32 ! ParseError'"));
    CHECK(AnyContains(rejected, "'is int64' can never be true for a value of type 'int32'"));

    const auto instantiated = Errors(R"(
        func Test<T>(value: int32 | bool) -> bool {
            return value is T;
        }
        func Use() {
            let invalid = Test<int64>(true);
        }
    )");
    CHECK(AnyContains(instantiated, "type 'int64' is not a member or subset of sum 'bool8 | int32'"));
}

TEST_CASE("is keeps the interface diagnostic and never narrows") {
    const auto errors = ErrorsIn(R"(
        interface Display {}
        func UseOptions(options: Options) {}
        func Interface(value: Options | Defaults) -> bool {
            return value is Display;
        }
        func NoNarrowing(value: Options | Defaults) {
            if value is Options {
                UseOptions(value);
            }
        }
    )");
    CHECK(AnyContains(errors, "type test 'is Display' is unavailable: interface checks are not implemented"));
    CHECK(AnyContains(errors, "Defaults | Options"));
}

TEST_CASE("an ungrouped sum after is or as names the grouping") {
    const auto errors = ErrorDiagnostics(std::string(kDeclarations) + R"(
        func Test(value: Options | Defaults) -> bool {
            return value is Options | Defaults;
        }
        func Bits(bits: int32, mask: int32) -> int32 {
            return bits as int32 | mask;
        }
    )");
    REQUIRE_EQ(errors.size(), 1);
    CHECK_EQ(errors.front().message, "a sum type after 'is' must be grouped");
    REQUIRE(errors.front().help.has_value());
    CHECK_EQ(*errors.front().help, "write 'value is (Options | Defaults)'");
}

TEST_CASE("the grouping help names a primitive first member as written") {
    const auto errors = ErrorDiagnostics(R"(
        func Test(value: int32 | bool) -> bool {
            return value is int32 | bool;
        }
    )");
    REQUIRE_EQ(errors.size(), 1);
    REQUIRE(errors.front().help.has_value());
    CHECK_EQ(*errors.front().help, "write 'value is (int32 | bool)'");
}
