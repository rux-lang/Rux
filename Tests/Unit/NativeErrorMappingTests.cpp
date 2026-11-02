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

constexpr std::string_view kDeclarations = R"(
    intrinsic func Panic(message: char8[..]);
    struct Options {}
    struct ParseError {}
    struct IoError {}
    type ReadError = ParseError | IoError;
    struct ConfigError { kind: int32; }
    #NoCopy
    struct Owned { handle: int32; }
    extend Owned {
    }
    #NoCopy
    #NoMove
    struct Pinned { handle: int32; }
    extend Pinned {
    }
    func ReadAt(path: char8[..]) -> Options throws ReadError { return Options {}; }
    func ToConfigError(e: ReadError, path: char8[..]) -> ConfigError { return ConfigError { kind: 1i32 }; }
    func Wrap(owned: Owned) -> ConfigError { return ConfigError { kind: owned.handle }; }
    func Keep(owned: Owned) {}
)";

std::vector<std::string> ErrorsIn(const std::string_view body) {
    return Errors(std::string(kDeclarations) + std::string(body));
}
} // namespace

TEST_CASE("a local moved inside the mapper stays owned on the continuing path") {
    CHECK(ErrorsIn(R"(
        func Load(path: char8[..], owned: Owned) -> Options throws ConfigError {
            let value = try ReadAt(path) catch { e => throw Wrap(move owned) };
            Keep(move owned);
            return value;
        }
    )")
              .empty());
}
