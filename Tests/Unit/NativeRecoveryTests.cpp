// `outcome catch { arms }` removes one outer fallible level: the success passes through unchanged, the arms see only
// the error payload and must cover all of it, and each arm either recovers a value of the success type or leaves.

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
    struct ParseError {}
    struct IoError { code: int32; }
    variant DecodeError {
        Missing,
        InvalidDigit(int32)
    }
    struct Owned { handle: int32; }
    extend Owned {
        func =(self: &var Owned, other: &Owned);
    }
    struct Pinned { handle: int32; }
    extend Pinned {
        func =(self: &var Pinned, other: &Pinned);
        func <-(self: &var Pinned, other: Pinned);
    }
    intrinsic func Panic(message: char8[..]);
    func Read() -> (Options | Defaults) ! (ParseError | IoError) { return Defaults {}; }
    func ReadConfig() -> Options ! (DecodeError | IoError) { return Options { verbose: false }; }
    func Close() -> ! IoError {}
    func Log(error: IoError) {}
    func Count() -> int32 ! IoError { return 1i32; }
    func Recover(position: int32) -> Options { return Options { verbose: true }; }
)";

std::vector<std::string> ErrorsIn(const std::string_view body) {
    return Errors(std::string(kDeclarations) + std::string(body));
}
} // namespace

TEST_CASE("catch arms match the error alone and recover the success type") {
    CHECK(ErrorsIn(R"(
        func LoadWithDefaults() -> (Options | Defaults) ! IoError {
            return Read() catch {
                _: ParseError => Defaults {},
                e: IoError => fail e
            };
        }
        func LoadConfig() -> Options ! IoError {
            let options = ReadConfig() catch {
                DecodeError::Missing => Options { verbose: false },
                DecodeError::InvalidDigit(position) => Recover(position),
                e: IoError => fail e
            };
            return options;
        }
        func Fallback() -> int32 {
            return Count() catch { else => 0i32 };
        }
    )")
              .empty());
}

TEST_CASE("a unit success recovers with blocks, a non-unit success needs a value") {
    CHECK(ErrorsIn(R"(
        func Discard() {
            Close() catch { else => {} };
            Close() catch { e: IoError => { Log(e); } };
            let done: () = Close() catch { else => () };
        }
    )")
              .empty());

    const auto errors = ErrorsIn(R"(
        func Invented() -> int32 {
            return Count() catch { else => {} };
        }
    )");
    CHECK(AnyContains(errors, "a block arm completes with '()', but 'catch' must recover a value of type 'int32'"));
}

TEST_CASE("diverging arms never decide the recovered type") {
    CHECK(ErrorsIn(R"(
        #NoReturn()
        func Stop() {
            Panic("stopped");
        }
        func Diverging() -> int32 ! IoError {
            let first = Count() catch { e => fail e };
            let second = Count() catch { e => Panic("unexpected") };
            let third = Count() catch { e => Stop() };
            let fourth = Count() catch { e => return 4i32 };
            let fifth = Count() catch { e => { return 5i32; } };
            return first + second + third + fourth + fifth;
        }
        func Looping() -> int32 {
            var total = 0i32;
            while total < 10i32 {
                let first = Count() catch { e => break };
                let second = Count() catch {
                    e => {
                        total = total + e.code;
                        break;
                    }
                };
                let third = Count() catch { else => { continue; } };
                total = total + first + second + third;
            }
            return total;
        }
    )")
              .empty());
}

TEST_CASE("the success passes through with every inner level") {
    CHECK(ErrorsIn(R"(
        func Layer() -> (int32 ! ParseError) ! IoError { return .Success(.Success(1i32)); }
        func Keep() -> int32 {
            let inner: int32 ! ParseError = Layer() catch { e => .Failure(ParseError {}) };
            return match inner {
                .Success(value) => value,
                .Failure(_) => 0i32
            };
        }
        func OnMatch(flag: bool) -> int32 {
            let value = match flag {
                true => Count(),
                false => Count()
            } catch { else => 0i32 };
            return value;
        }
    )")
              .empty());
}

TEST_CASE("catch consumes its subject and moves the error it binds") {
    CHECK(ErrorsIn(R"(
        func Transfer(outcome: Owned ! IoError) -> int32 {
            let owned = (<-outcome) catch { e => Owned { handle: 0i32 } };
            return owned.handle;
        }
        func Copies(outcome: int32 ! IoError) -> int32 {
            let first = outcome catch { else => 0i32 };
            let second = outcome catch { else => 0i32 };
            return first + second;
        }
    )")
              .empty());

    const auto implicit = ErrorsIn(R"(
        func Implicit(outcome: Owned ! IoError) -> int32 {
            let owned = outcome catch { e => Owned { handle: 0i32 } };
            return owned.handle;
        }
    )");
    CHECK(AnyContains(implicit, "requires an explicit '<-' in catch subject"));

    const auto pinned = ErrorsIn(R"(
        func MakePinned() -> int32 ! Pinned { return 1i32; }
        func Binds() -> int32 {
            return MakePinned() catch { e => 0i32 };
        }
    )");
    CHECK(AnyContains(pinned, "'catch' cannot bind error payload 'Pinned' by value because moving it is prohibited"));
}

TEST_CASE("catch rejects borrowed and non-fallible subjects and partial coverage") {
    const auto errors = ErrorsIn(R"(
        func Borrowed(outcome: &(int32 ! IoError)) -> int32 {
            return outcome catch { else => 0i32 };
        }
        func Plain(value: int32) -> int32 {
            return value catch { else => 0i32 };
        }
        func Partial() -> Options | Defaults {
            return Read() catch {
                _: ParseError => Defaults {}
            };
        }
        func PartialStruct() -> int32 {
            return Count() catch {
                IoError { code: 1i32 } => 0i32
            };
        }
        func Wrong() -> int32 {
            return Count() catch { else => true };
        }
    )");
    CHECK(AnyContains(errors, "'catch' cannot consume the borrowed value '&(int32 ! IoError)'"));
    CHECK(AnyContains(errors, "'catch' recovers a native fallible, but the subject has type 'int32'"));
    CHECK(AnyContains(errors, "match on 'IoError | ParseError' is not exhaustive; missing _: IoError"));
    CHECK(AnyContains(errors, "'catch' arms do not cover every error of type 'IoError'"));
    CHECK(AnyContains(errors, "'catch' arm produces 'bool8', but the recovered value has type 'int32'"));
}

TEST_CASE("a recovered value converts unambiguously or not at all") {
    const auto errors = ErrorsIn(R"(
        type R = int32 ! ParseError;
        func Nested() -> ((int32 | R) ! ParseError) ! IoError { return .Success(.Success(1i32)); }
        func Ambiguous(value: R) -> int32 {
            let recovered = Nested() catch { e => value };
            return 0i32;
        }
    )");
    CHECK(AnyContains(errors, "ambiguous"));
}
