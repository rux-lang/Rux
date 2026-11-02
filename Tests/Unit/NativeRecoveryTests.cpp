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

constexpr std::string_view kDeclarations = R"(
    struct Options { verbose: bool; }
    struct Defaults {}
    struct ParseError {}
    struct IoError { code: int32; }
    variant DecodeError {
        Missing,
        InvalidDigit(int32)
    }
    #NoCopy
    struct Owned { handle: int32; }
    extend Owned {
    }
    #NoCopy
    #NoMove
    struct Pinned { handle: int32; }
    extend Pinned {
    }
    intrinsic func Panic(message: char8[..]);
    func Read() -> (Options | Defaults) throws (ParseError | IoError) { return Defaults {}; }
    func ReadConfig() -> Options throws (DecodeError | IoError) { return Options { verbose: false }; }
    func Close() throws IoError {}
    func Log(error: IoError) {}
    func Count() -> int32 throws IoError { return 1i32; }
    func Recover(position: int32) -> Options { return Options { verbose: true }; }
)";

std::vector<std::string> ErrorsIn(const std::string_view body) {
    return Errors(std::string(kDeclarations) + std::string(body));
}
} // namespace

TEST_CASE("catch arms match the error alone and recover the success type") {
    CHECK(ErrorsIn(R"(
        func LoadWithDefaults() -> (Options | Defaults) throws IoError {
            return try Read() catch {
                _: ParseError => Defaults {},
                e: IoError => throw e
            };
        }
        func LoadConfig() -> Options throws IoError {
            let options = try ReadConfig() catch {
                DecodeError::Missing => Options { verbose: false },
                DecodeError::InvalidDigit(position) => Recover(position),
                e: IoError => throw e
            };
            return options;
        }
        func Fallback() -> int32 {
            return try Count() catch { _ => 0i32 };
        }
    )")
              .empty());
}

TEST_CASE("diverging arms never decide the recovered type") {
    CHECK(ErrorsIn(R"(
        #NoReturn
        func Stop() {
            Panic("stopped");
        }
        func Diverging() -> int32 throws IoError {
            let first = try Count() catch { e => throw e };
            let second = try Count() catch { e => Panic("unexpected") };
            let third = try Count() catch { e => Stop() };
            let fourth = try Count() catch { e => return 4i32 };
            let fifth = try Count() catch { e => { return 5i32; } };
            return first + second + third + fourth + fifth;
        }
        func Looping() -> int32 {
            var total = 0i32;
            while total < 10i32 {
                let first = try Count() catch { e => break };
                let second = try Count() catch {
                    e => {
                        total = total + e.code;
                        break;
                    }
                };
                let third = try Count() catch { _ => { continue; } };
                total = total + first + second + third;
            }
            return total;
        }
    )")
              .empty());
}
