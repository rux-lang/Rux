// `?` on a native fallible: the success continues, inner levels intact, and the error leaves as the enclosing
// function's outer failure by identity, sum injection, or subset widening only. Legacy and native outcomes never mix,
// a borrowed operand is refused, and the operand is consumed like any other by-value use.

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
    struct ParseError {}
    struct IoError {}
    struct Timeout {}
    #NoCopy
    struct Owned { handle: int32; }
    extend Owned {
    }
    #NoCopy
    #NoMove
    struct Pinned { handle: int32; }
    extend Pinned {
    }
    func UseValue(value: int32) {}
)";

std::vector<std::string> ErrorsIn(const std::string_view body) {
    return Errors(std::string(kDeclarations) + std::string(body));
}
} // namespace

TEST_CASE("'?' on an optional removes one presence level") {
    CHECK(ErrorsIn(R"(
        func Nested(found: int32??) -> int32? {
            let inner: int32? = found?;
            return inner;
        }
        func SuccessfulAbsence(count: int32?) -> int32? throws IoError {
            let value = count?;
            return value;
        }
        func First<T>(value: T?) -> T? {
            let present = value?;
            return .Some(present);
        }
        func Use(nested: int32??) {
            let first = First<int32?>(nested);
        }
        func Transfer(owned: Owned?) -> int32? {
            let value = (move owned)?;
            return value.handle;
        }
    )")
              .empty());
}

TEST_CASE("coalescing takes the payload or a fallback that converts or leaves") {
    CHECK(ErrorsIn(R"(
        intrinsic func Panic(message: char8[..]);
        func Fallbacks(count: int32?, nested: int32??, unit: ()?) -> int32 throws ParseError {
            let a = count ?? 0i32;
            let b = count ?? throw ParseError {};
            let c = count ?? Panic("absent");
            let d: int32? = nested ?? none;
            let e = unit ?? ();
            for index in 0i32..3i32 {
                let f = count ?? continue;
                let g = count ?? break;
            }
            return count ?? return 1i32;
        }
    )")
              .empty());

    const auto errors = ErrorsIn(R"(
        func Wrong(count: int32?) -> int32 {
            return count ?? true;
        }
        func Borrowed(count: &(int32?)) -> int32 {
            return count ?? 0i32;
        }
        func Pinned(value: Pinned?, fallback: Pinned) -> int32 {
            let pinned = (move value) ?? move fallback;
            return 0i32;
        }
    )");
    CHECK(AnyContains(errors, "coalescing fallback has type 'bool8', but the optional payload is 'int32'"));
    CHECK(AnyContains(errors, "cannot take '&"));
    CHECK(AnyContains(errors, "cannot extract payload type 'Pinned' because moving it is prohibited"));
}

TEST_CASE("coalescing moves its operand and only possibly its fallback") {
    CHECK(ErrorsIn(R"(
        func Take(value: Owned?, fallback: Owned) -> int32 {
            let owned = (move value) ?? move fallback;
            return owned.handle;
        }
    )")
              .empty());
    const auto errors = ErrorsIn(R"(
        func Keep(owned: Owned) {}
        func Reuse(value: Owned?, fallback: Owned) {
            let owned = (move value) ?? move fallback;
            Keep(move fallback);
        }
    )");
    CHECK(AnyContains(errors, "'fallback'"));
}
