// `value? else (e => mapper)`: on success the value continues and the mapper never runs; on failure `e` owns the
// complete error, the mapper may use local context, and its value fails the enclosing function. The mapper's path
// always leaves, so what it moves stays owned on the continuing path.

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
    intrinsic func Panic(message: char8[..]);
    struct Options {}
    struct ParseError {}
    struct IoError {}
    type ReadError = ParseError | IoError;
    struct ConfigError { kind: int32; }
    struct Owned { handle: int32; }
    extend Owned {
        func =(self: &var Owned, other: &Owned);
    }
    struct Pinned { handle: int32; }
    extend Pinned {
        func =(self: &var Pinned, other: &Pinned);
        func <-(self: &var Pinned, other: Pinned);
    }
    func ReadAt(path: char8[..]) -> Options ! ReadError { return Options {}; }
    func ToConfigError(e: ReadError, path: char8[..]) -> ConfigError { return ConfigError { kind: 1i32 }; }
    func Wrap(owned: Owned) -> ConfigError { return ConfigError { kind: owned.handle }; }
    func Keep(owned: Owned) {}
)";

std::vector<std::string> ErrorsIn(const std::string_view body) {
    return Errors(std::string(kDeclarations) + std::string(body));
}
} // namespace

TEST_CASE("a mapper binds the complete error and may use local context") {
    CHECK(ErrorsIn(R"(
        func LoadConfig(path: char8[..]) -> Options ! ConfigError {
            let value = ReadAt(path)? else (e => ToConfigError(e, path));
            return value;
        }
        func ByMember(path: char8[..]) -> Options ! ConfigError {
            return ReadAt(path)? else (e => match e {
                _: ParseError => ConfigError { kind: 1i32 },
                _: IoError => ConfigError { kind: 2i32 }
            });
        }
        func Discarded(path: char8[..]) -> Options ! ConfigError {
            return ReadAt(path)? else (_ => ConfigError { kind: 0i32 });
        }
        func Leaves(path: char8[..]) -> Options ! ConfigError {
            let first = ReadAt(path)? else (e => fail ConfigError { kind: 3i32 });
            let second = ReadAt(path)? else (e => Panic("unreadable"));
            return ReadAt(path)? else (e => return Options {});
        }
        func OptionalData(outcome: int32 ! (ParseError?)) -> int32 ! ConfigError {
            return outcome? else (e => ConfigError { kind: 4i32 });
        }
    )")
              .empty());
}

TEST_CASE("a move-only error is bound and transferred, never discarded") {
    CHECK(ErrorsIn(R"(
        func Read() -> int32 ! Owned { return 1i32; }
        func Transfer() -> int32 ! ConfigError {
            return Read()? else (e => Wrap(<-e));
        }
    )")
              .empty());
    const auto discarded = ErrorsIn(R"(
        func Read() -> int32 ! Owned { return 1i32; }
        func Discard() -> int32 ! ConfigError {
            return Read()? else (_ => ConfigError { kind: 0i32 });
        }
    )");
    CHECK(AnyContains(discarded, "the error 'Owned' cannot be discarded with '_' because it is move-only"));
    const auto pinned = ErrorsIn(R"(
        func Read() -> int32 ! Pinned { return 1i32; }
        func Map() -> int32 ! ConfigError {
            return Read()? else (e => ConfigError { kind: 0i32 });
        }
    )");
    CHECK(AnyContains(pinned, "'?' cannot extract payload type 'Pinned' because moving it is prohibited"));
}

TEST_CASE("a local moved inside the mapper stays owned on the continuing path") {
    CHECK(ErrorsIn(R"(
        func Load(path: char8[..], owned: Owned) -> Options ! ConfigError {
            let value = ReadAt(path)? else (e => Wrap(<-owned));
            Keep(<-owned);
            return value;
        }
    )")
              .empty());
}

TEST_CASE("mapping needs a native fallible operand and a fallible function") {
    const auto errors = ErrorsIn(R"(
        func Present(count: int32?) -> int32 ! ConfigError {
            return count? else (e => ConfigError { kind: 0i32 });
        }
        func NoChannel(path: char8[..]) -> Options {
            return ReadAt(path)? else (e => ConfigError { kind: 0i32 });
        }
        func WrongType(path: char8[..]) -> Options ! ConfigError {
            return ReadAt(path)? else (e => 5i32);
        }
        func Fallible(path: char8[..]) -> Options ! ConfigError {
            return ReadAt(path)? else (e => Nested());
        }
        func Nested() -> ConfigError ! IoError { return ConfigError { kind: 0i32 }; }
    )");
    CHECK(AnyContains(errors, "'? else' maps the error of a native fallible, but the operand has type 'int32?'"));
    CHECK(AnyContains(errors, "'? else' fails the enclosing function, but it returns 'Options'"));
    CHECK(AnyContains(errors, "the mapped error has type 'int32', but the enclosing function fails with "
                              "'ConfigError'"));
    // A mapper returning a fallible never propagates it implicitly.
    CHECK(AnyContains(errors, "the mapped error has type 'ConfigError ! IoError'"));
}
