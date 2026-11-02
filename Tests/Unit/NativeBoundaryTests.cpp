// Native values at the boundaries of a program: across packages, in constants, and in the pointer and slice spellings
// whose grouping decides what a type is.

#include "SemanticTestSupport.h"

using namespace Rux;
using namespace Rux::Testing::SemanticTestSupport;

namespace {
std::vector<std::string> Errors(const std::vector<SemanticDiagnostic> &diagnostics) {
    std::vector<std::string> errors;
    for (const auto &diagnostic : diagnostics) {
        if (diagnostic.severity == SemanticDiagnostic::Severity::Error) {
            errors.push_back(diagnostic.message);
        }
    }
    return errors;
}

bool AnyContains(const std::vector<std::string> &errors, const std::string_view text) {
    return std::ranges::any_of(errors, [&](const std::string &error) { return error.contains(text); });
}
} // namespace

TEST_CASE("a public error alias exposes its members across packages, a named wrapper does not") {
    const std::string dependency = R"(
        pub struct ParseError {}
        pub struct IoError {}
        pub type ReadError = ParseError | IoError;
        pub variant LoadError {
            Unreadable(ReadError)
        }
        pub func Read() -> int32 throws ReadError { return 1i32; }
        pub func Load() -> int32 throws LoadError { return 1i32; }
    )";
    CHECK(Errors(AnalyzeWithDep(R"(
        import Store::{ IoError, Load, LoadError, ParseError, Read };
        func Use() -> int32 {
            let read = try Read() catch { _: ParseError => 1i32, _: IoError => 2i32 };
            let load = try Load() catch { LoadError::Unreadable(cause) => 3i32 };
            return read + load;
        }
    )",
                                "Store", dependency))
              .empty());

    // The alias names exactly its members, so a caller covering only one of them is told which one it missed.
    const auto partial = Errors(AnalyzeWithDep(R"(
        import Store::{ ParseError, Read };
        func Use() -> int32 {
            return try Read() catch { _: ParseError => 1i32 };
        }
    )",
                                               "Store", dependency));
    CHECK(AnyContains(partial, "missing _: IoError"));
}

TEST_CASE("pointer and slice spellings keep their grouping") {
    const auto analyzed = AnalyzeSource(R"(
        func Use(pointerToOptional: *int32?, optionalPointer: (*int32)?, slice: int32?[..], optionalSlice: int32[..]?,
                 writable: (var char8[..])?) {
            let first: *(int32?) = pointerToOptional;
            let second: (*int32)? = optionalPointer;
            let third: (int32?)[..] = slice;
            let fourth: (int32[..])? = optionalSlice;
        }
        func Confused(pointerToOptional: *int32?) {
            let wrong: (*int32)? = pointerToOptional;
        }
    )");
    const auto errors = Errors(analyzed);
    REQUIRE_EQ(errors.size(), 1);
    CHECK(errors.front().contains("cannot assign"));
}
