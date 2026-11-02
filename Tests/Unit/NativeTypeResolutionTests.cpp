// Resolution of the native sum, optional, fallible, and unit types in signatures, aliases, fields, and generic
// arguments, with the existing visibility, alias-cycle, stored-reference, and extension rules applied through them.

#include "SemanticTestSupport.h"

using namespace Rux;
using namespace Rux::Testing::SemanticTestSupport;

namespace {
std::size_t CountContaining(const std::vector<SemanticDiagnostic> &diagnostics, const std::string_view text) {
    return static_cast<std::size_t>(std::ranges::count_if(
        diagnostics, [&](const SemanticDiagnostic &diagnostic) { return diagnostic.message.contains(text); }));
}

} // namespace

TEST_CASE("an unresolved member makes the whole native type unresolved") {
    const auto diagnostics = AnalyzeSource(R"(
        struct A {}
        func F(value: A | Missing, optional: Missing?) {}
    )");
    CHECK(CountContaining(diagnostics, "type 'Missing' is not defined") > 0);
}

TEST_CASE("a structural native type cannot refer to itself") {
    const auto diagnostics = AnalyzeSource(R"(
        type Leaf = int32;
        type Tree = Leaf | (Tree?);
        type PointerTree = Leaf | (*PointerTree);
        func F(tree: Tree, pointer: PointerTree) {}
    )");
    CHECK(CountContaining(diagnostics, "type alias 'Tree' has a cyclic definition") > 0);
    CHECK(CountContaining(diagnostics, "type alias 'PointerTree' has a cyclic definition") > 0);
}

TEST_CASE("unit is an ordinary type without recognizing Core::Unit") {
    const auto diagnostics = AnalyzeSource(R"(
        struct Unit {}
        struct Holder<T> { value: T; }
        struct Done { marker: (); }
        func Same(value: ()) -> () { return value; }
        func Wrapped(holder: Holder<()>) -> Holder<()> { return holder; }
        func Distinct(value: Unit) -> Unit { return value; }
        func Main() -> int {
            let unit: () = Same(());
            return 0;
        }
    )");
    CHECK(diagnostics.empty());
}
