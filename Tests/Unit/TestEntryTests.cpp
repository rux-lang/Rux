// A test build gathers `#Test()` functions into a generated `Main`; it refuses the packages where it cannot.

#include "CompilerDriverTestSupport.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace Rux;
using namespace Rux::Testing::CompilerDriverTestSupport;

namespace {
bool Reports(const std::vector<Diagnostic> &diagnostics, const std::string &message) {
    return std::ranges::any_of(diagnostics, [&](const Diagnostic &diagnostic) {
        return diagnostic.IsError() && diagnostic.message.contains(message);
    });
}

CompileResult BuildForTests(DependencyFixture &fixture, std::vector<Diagnostic> &diagnostics) {
    auto options = fixture.Options(false, diagnostics);
    options.isTest = true;
    return CompilerDriver(std::move(options)).Compile();
}
} // namespace

TEST_CASE("a test build refuses a package that declares both Main and tests") {
    DependencyFixture fixture;
    fixture.SetApplicationSource(R"(
#Test()
func Checks() {}

func Main() -> int {
    return 0;
}
)");
    std::vector<Diagnostic> diagnostics;
    CHECK_FALSE(BuildForTests(fixture, diagnostics).ok);
    CHECK(Reports(diagnostics, "declares 'Main' and '#Test()' functions"));
}

TEST_CASE("a test build refuses tests without a Test dependency") {
    DependencyFixture fixture;
    fixture.SetApplicationSource(R"(
#Test()
func Checks() {}
)");
    std::vector<Diagnostic> diagnostics;
    CHECK_FALSE(BuildForTests(fixture, diagnostics).ok);
    CHECK(Reports(diagnostics, "has '#Test()' functions but does not depend on 'Test'"));
}

TEST_CASE("a test build of a library needs tests to run") {
    DependencyFixture fixture;
    fixture.SetApplicationType(ManifestPackageType::SourceLibrary);
    fixture.SetApplicationSource(R"(
pub func Helper() -> int32 {
    return 1;
}
)");
    std::vector<Diagnostic> diagnostics;
    CHECK_FALSE(BuildForTests(fixture, diagnostics).ok);
    CHECK(Reports(diagnostics, "has no '#Test()' functions to run"));
}

TEST_CASE("a test build refuses tests it cannot call") {
    DependencyFixture fixture;
    fixture.SetApplicationSource(R"(
#Test()
func Counts() -> int32 {
    return 1;
}

when true {
    #Test()
    func Conditional() {}
}
)");
    std::vector<Diagnostic> diagnostics;
    CHECK_FALSE(BuildForTests(fixture, diagnostics).ok);
    CHECK(Reports(diagnostics, "'#Test' function 'Counts' must return nothing or '! TestFailure'"));
    CHECK(Reports(diagnostics, "a '#Test' function cannot be declared inside 'when'"));
}

TEST_CASE("an ordinary build keeps test functions as plain functions") {
    DependencyFixture fixture;
    fixture.SetApplicationSource(R"(
#Test()
func Checks() {}

func Main() -> int {
    Checks();
    return 0;
}
)");
    std::vector<Diagnostic> diagnostics;
    const auto result = CompilerDriver(fixture.Options(false, diagnostics)).Compile();
    CHECK(result.ok);
    CHECK(diagnostics.empty());
}
