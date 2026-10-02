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

TypeRef Named(const char *name) {
    return TypeRef::MakeNamed(name);
}
} // namespace

TEST_CASE("native types resolve to their normalized identities") {
    Lexer lexer(R"(
        struct A {}
        struct B {}
        struct E {}
        struct Box<T> { value: T; }
        type R = int32 ! E;
        type OptionalError = E?;
        func F(sum: B | A | A, nested: (int32 ! E) ! E, aliased: int32 ! OptionalError, unit: ! E,
               optional: int32??, member: R | A, generic: Box<int32?>, grouped: (A | B)?) {}
    )",
                "native_resolution.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "native_resolution.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    const SemanticModel model = SemanticAnalyzer({&parsed.module}, {}, "native", "Windows").Analyze();

    const FuncDecl *function = nullptr;
    for (const auto &item : parsed.module.items) {
        if (const auto *candidate = dynamic_cast<const FuncDecl *>(item.get()); candidate && candidate->name == "F") {
            function = candidate;
        }
    }
    REQUIRE(function != nullptr);
    REQUIRE_EQ(function->params.size(), 8);
    const auto parameter = [&](const std::size_t index) { return ResolvedType(model, *function->params[index].type); };

    const TypeRef e = Named("E");
    CHECK_EQ(parameter(0), TypeRef::MakeSum({Named("A"), Named("B")}));
    CHECK_EQ(parameter(1), TypeRef::MakeFallible(TypeRef::MakeFallible(TypeRef::MakeInt32(), e), e));
    CHECK_EQ(parameter(2), TypeRef::MakeFallible(TypeRef::MakeInt32(), TypeRef::MakeOptional(e)));
    CHECK_EQ(parameter(3), TypeRef::MakeFallible(TypeRef::MakeUnit(), e));
    CHECK_EQ(parameter(4), TypeRef::MakeOptional(TypeRef::MakeOptional(TypeRef::MakeInt32())));
    CHECK_EQ(parameter(5), TypeRef::MakeSum({TypeRef::MakeFallible(TypeRef::MakeInt32(), e), Named("A")}));
    CHECK_EQ(parameter(6).ToString(), "Box<int32?>");
    CHECK_EQ(parameter(7), TypeRef::MakeOptional(TypeRef::MakeSum({Named("A"), Named("B")})));
    CHECK_FALSE(model.HasErrors());
}

TEST_CASE("an unresolved member makes the whole native type unresolved") {
    const auto diagnostics = AnalyzeSource(R"(
        struct A {}
        func F(value: A | Missing, optional: Missing?) {}
    )");
    CHECK(CountContaining(diagnostics, "type 'Missing' is not defined") > 0);
}

TEST_CASE("native types keep the existing visibility rule") {
    const auto diagnostics = AnalyzeSource(R"(
        struct Hidden {}
        pub func Leak(value: Hidden?, other: int32 ! Hidden) {}
    )");
    CHECK(CountContaining(diagnostics, "public function 'Leak' exposes private type 'Hidden'") > 0);
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

TEST_CASE("a native type holding a reference stores that reference") {
    const auto diagnostics = AnalyzeSource(R"(
        struct A {}
        struct E {}
        func F(value: int32) {
            let optional: (&int32)? = none;
            let sum: (&A) | int32 = value;
            let failure: int32 ! (&E) = value;
        }
    )");
    CHECK(CountContaining(diagnostics, "cannot store reference type '(&int32)?'") > 0);
    CHECK(CountContaining(diagnostics, "cannot store reference type '&A | int32'") > 0);
    CHECK(CountContaining(diagnostics, "cannot store reference type 'int32 ! &E'") > 0);
}

TEST_CASE("native and unit types are never extension targets") {
    const auto diagnostics = AnalyzeSource(R"(
        struct A {}
        struct B {}
        struct E {}
        extend int32? {}
        extend A | B {}
        extend int32 ! E {}
        extend () {}
    )");
    CHECK_EQ(CountContaining(diagnostics, "cannot extend native type 'int32?'"), 1);
    CHECK_EQ(CountContaining(diagnostics, "cannot extend native type 'A | B'"), 1);
    CHECK_EQ(CountContaining(diagnostics, "cannot extend native type 'int32 ! E'"), 1);
    CHECK_EQ(CountContaining(diagnostics, "cannot extend native type '()'"), 1);
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
