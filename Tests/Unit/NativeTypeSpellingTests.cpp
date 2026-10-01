// Canonical spellings and symbol identity of native types: instantiation names that embed them read back unchanged,
// linker spellings keep distinct native forms apart, and same-spelled nominal members from different modules stay
// distinct members.

#include "SemanticTestSupport.h"

#include <memory>

using namespace Rux;
using namespace Rux::Testing::SemanticTestSupport;

namespace {
struct Analyzed {
    explicit Analyzed(ParseResult result)
        : parsed(std::move(result))
        , model(SemanticAnalyzer({&parsed.module}, {}, "spellings", "Windows").Analyze()) {
    }

    ParseResult parsed;
    SemanticModel model;
};

std::unique_ptr<Analyzed> Analyze(const std::string &source) {
    Lexer lexer(source, "native_spellings.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "native_spellings.rux");
    ParseResult parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    return std::make_unique<Analyzed>(std::move(parsed));
}

const FuncDecl &Function(const Analyzed &analyzed, const std::string_view name) {
    for (const auto &item : analyzed.parsed.module.items) {
        if (const auto *function = dynamic_cast<const FuncDecl *>(item.get()); function && function->name == name) {
            return *function;
        }
    }
    FAIL("no function named ", name);
    throw;
}
} // namespace

TEST_CASE("the native spelling split finds only the loosest operator outside brackets") {
    const auto fallible = TypeRef::SplitNativeSpelling("A | B ! E | F");
    REQUIRE(fallible.has_value());
    CHECK(fallible->form == TypeRef::NativeSpelling::Form::Fallible);
    CHECK_EQ(fallible->parts, std::vector<std::string>{"A | B", "E | F"});

    const auto unitSuccess = TypeRef::SplitNativeSpelling("! IoError");
    REQUIRE(unitSuccess.has_value());
    CHECK_EQ(unitSuccess->parts, std::vector<std::string>{"", "IoError"});

    const auto sum = TypeRef::SplitNativeSpelling("A | Box<B | C> | (D ! E)");
    REQUIRE(sum.has_value());
    CHECK(sum->form == TypeRef::NativeSpelling::Form::Sum);
    CHECK_EQ(sum->parts, std::vector<std::string>{"A", "Box<B | C>", "(D ! E)"});

    CHECK_FALSE(TypeRef::SplitNativeSpelling("Box<A | B>").has_value());
    CHECK_FALSE(TypeRef::SplitNativeSpelling("(A ! E)?").has_value());
    CHECK_FALSE(TypeRef::SplitNativeSpelling("func(A) -> B").has_value());
}

TEST_CASE("distinct native forms never share a linker spelling") {
    const TypeRef a = TypeRef::MakeNamed("A");
    const TypeRef b = TypeRef::MakeNamed("B");
    const std::vector<TypeRef> types = {
        TypeRef::MakeSum({a, b}),
        TypeRef::MakeFallible(a, b),
        TypeRef::MakeOptional(a),
        TypeRef::MakeOptional(TypeRef::MakeOptional(a)),
        TypeRef::MakeFallible(TypeRef::MakeUnit(), a),
        TypeRef::MakeArray(a),
        TypeRef::MakeSlice(a),
        a,
    };
    for (std::size_t first = 0; first < types.size(); ++first) {
        for (std::size_t second = first + 1; second < types.size(); ++second) {
            CHECK_MESSAGE(types[first].MangledSpelling() != types[second].MangledSpelling(), types[first].ToString(),
                          " and ", types[second].ToString(), " share a linker spelling");
        }
    }
    CHECK_EQ(TypeRef::MakeOptional(TypeRef::MakeInt32()).MangledSpelling(), "int32_O");
}

TEST_CASE("every native spelling reads back from an instantiation name unchanged") {
    // A generic instantiation is identified by its printed name and its fields are typed by reading the arguments
    // back out of that name, so a native argument that did not round-trip would give the field another type.
    const auto analyzed = Analyze(R"(
        struct A {}
        struct B {}
        struct E {}
        struct Box<T> { value: T; }
        func Main(sum: Box<A | B>, optional: Box<int32?>, nested: Box<int32??>, fallible: Box<A ! E>,
                  unit: Box<! E>, grouped: Box<(A ! E) ! B>, optionalError: Box<A ! (E?)>,
                  optionalSum: Box<(A | B)?>, slices: Box<int32?[..]>, optionalSlice: Box<int32[..]?>,
                  pointer: Box<*(int32?)>, optionalPointer: Box<(*int32)?>, empty: Box<()>,
                  member: Box<A | (B?)>) {
            let a = sum.value;
            let b = optional.value;
            let c = nested.value;
            let d = fallible.value;
            let e = unit.value;
            let f = grouped.value;
            let g = optionalError.value;
            let h = optionalSum.value;
            let i = slices.value;
            let j = optionalSlice.value;
            let k = pointer.value;
            let l = optionalPointer.value;
            let m = empty.value;
            let n = member.value;
        }
    )");
    const FuncDecl &main = Function(*analyzed, "Main");
    std::vector<std::string> types;
    for (const auto &statement : main.body->stmts) {
        const auto *binding = dynamic_cast<const LetStmt *>(statement.get());
        REQUIRE(binding != nullptr);
        types.push_back(ResolvedType(analyzed->model, *binding->init).ToString());
    }
    CHECK_EQ(types, std::vector<std::string>{"A | B", "int32?", "int32??", "A ! E", "! E", "(A ! E) ! B", "A ! (E?)",
                                             "(A | B)?", "int32?[..]", "int32[..]?", "*(int32?)", "(*int32)?", "()",
                                             "A | (B?)"});
}

TEST_CASE("reordered and aliased sums are one instantiation") {
    const auto analyzed = Analyze(R"(
        struct A {}
        struct B {}
        struct Box<T> { value: T; }
        type Either = B | A;
        func Main(first: Box<A | B>, second: Box<B | A>, third: Box<Either>, fourth: Box<A | B | A>) {
            let a = first.value;
            let b = second.value;
            let c = third.value;
            let d = fourth.value;
        }
    )");
    const FuncDecl &main = Function(*analyzed, "Main");
    std::vector<std::string> parameters;
    for (const Param &parameter : main.params) {
        parameters.push_back(ResolvedType(analyzed->model, *parameter.type).ToString());
    }
    CHECK_EQ(parameters, std::vector<std::string>(4, "Box<A | B>"));
}

TEST_CASE("same-spelled nominal members from different modules stay distinct") {
    const auto analyzed = Analyze(R"(
        module Left {
            pub struct Error {}
        }
        module Right {
            pub struct Error {}
        }
        func Main(both: Left::Error | Right::Error, same: Left::Error | Left::Error) {}
    )");
    const FuncDecl &main = Function(*analyzed, "Main");
    const TypeRef &both = ResolvedType(analyzed->model, *main.params[0].type);
    const TypeRef &same = ResolvedType(analyzed->model, *main.params[1].type);
    REQUIRE(both.IsSum());
    CHECK_EQ(both.inner.size(), 2);
    CHECK_NE(both.inner[0], both.inner[1]);
    CHECK_FALSE(same.IsSum());
}
