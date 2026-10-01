// Generic substitution through native sums, optionals, and fallibles: sums normalize again under each instantiation,
// bare parameters and repeated outer constructors are legal members, and arguments deduce through native forms.

#include "SemanticTestSupport.h"

#include <memory>

using namespace Rux;
using namespace Rux::Testing::SemanticTestSupport;

namespace {
/// A parsed module and its model. Facts are keyed by node address, so the module never moves once analyzed.
struct Analyzed {
    explicit Analyzed(ParseResult result)
        : parsed(std::move(result))
        , model(SemanticAnalyzer({&parsed.module}, {}, "generics", "Windows").Analyze()) {
    }

    ParseResult parsed;
    SemanticModel model;
};

/// Analyze `source`, which may still report native types as pending; the facts these tests read are recorded anyway.
std::unique_ptr<Analyzed> Analyze(const std::string &source) {
    Lexer lexer(source, "native_generics.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "native_generics.rux");
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

/// The initializer of the `index`th `let` in `function`.
const Expr &Initializer(const FuncDecl &function, const std::size_t index) {
    REQUIRE(function.body != nullptr);
    REQUIRE(index < function.body->stmts.size());
    const auto *let = dynamic_cast<const LetStmt *>(function.body->stmts[index].get());
    REQUIRE(let != nullptr);
    return *let->init;
}

std::size_t CountContaining(const SemanticModel &model, const std::string_view text) {
    return static_cast<std::size_t>(std::ranges::count_if(
        model.diagnostics, [&](const SemanticDiagnostic &diagnostic) { return diagnostic.message.contains(text); }));
}
} // namespace

TEST_CASE("renormalizing a substituted sum removes members that became equal") {
    TypeRef sum = TypeRef::MakeSum({TypeRef::MakeTypeParam("T"), TypeRef::MakeTypeParam("U")});
    REQUIRE(sum.IsSum());
    sum.inner[0] = TypeRef::MakeInt32();
    sum.inner[1] = TypeRef::MakeInt32();
    CHECK_EQ(TypeRef::Renormalize(sum), TypeRef::MakeInt32());

    TypeRef distinct = TypeRef::MakeSum({TypeRef::MakeTypeParam("T"), TypeRef::MakeTypeParam("U")});
    distinct.inner[0] = TypeRef::MakeInt64();
    distinct.inner[1] = TypeRef::MakeInt32();
    CHECK_EQ(TypeRef::Renormalize(distinct), TypeRef::MakeSum({TypeRef::MakeInt32(), TypeRef::MakeInt64()}));

    const TypeRef optional = TypeRef::MakeOptional(TypeRef::MakeInt32());
    CHECK_EQ(TypeRef::Renormalize(optional), optional);
}

TEST_CASE("bare parameters and repeated outer constructors are legal sum members") {
    const auto analyzedPointer = Analyze(R"(
        struct Timeout {}
        struct Box<T> { value: T; }
        func Pair<T, U>(value: T | U) {}
        func Errors<E>(value: E | Timeout) {}
        func Boxes<T, U>(value: Box<T> | Box<U>) {}
    )");
    const Analyzed &analyzed = *analyzedPointer;
    // Only the pending native-type diagnostics are reported: nothing rejects these members at the declaration.
    CHECK_EQ(analyzed.model.diagnostics.size(),
             CountContaining(analyzed.model, "is not supported in compiled code yet"));
    CHECK_EQ(CountContaining(analyzed.model, "is not supported in compiled code yet"), 3);
}

TEST_CASE("a sum normalizes again for each instantiation") {
    const auto analyzedPointer = Analyze(R"(
        struct Box<T> { value: T; }
        func Choose<T, U>(left: T, right: U) -> T | U { return left; }
        func Boxes<T, U>(left: Box<T>, right: Box<U>) -> Box<T> | Box<U> { return left; }
        func Use(box: Box<int32>, other: Box<int64>) {
            let same = Choose<int32, int32>(1i32, 2i32);
            let different = Choose<int32, int64>(1i32, 2i64);
            let sameBoxes = Boxes<int32, int32>(box, box);
            let differentBoxes = Boxes<int32, int64>(box, other);
        }
    )");
    const Analyzed &analyzed = *analyzedPointer;
    const FuncDecl &use = Function(analyzed, "Use");
    const auto typeOf = [&](const std::size_t index) { return ResolvedType(analyzed.model, Initializer(use, index)); };
    CHECK_EQ(typeOf(0), TypeRef::MakeInt32());
    CHECK_EQ(typeOf(1), TypeRef::MakeSum({TypeRef::MakeInt32(), TypeRef::MakeInt64()}));
    CHECK_EQ(typeOf(2).ToString(), "Box<int32>");
    CHECK_EQ(typeOf(3).ToString(), "Box<int32> | Box<int64>");
}

TEST_CASE("arguments deduce type parameters through optionals and fallibles") {
    const auto analyzedPointer = Analyze(R"(
        struct ParseError {}
        func Payload<T>(value: T?) -> T { return Payload(value); }
        func Channels<T, E>(value: T ! E) -> E { return Channels(value); }
        func Use(optional: int32??, outcome: int64 ! ParseError) {
            let fromOptional = Payload(optional);
            let fromFallible = Channels(outcome);
        }
    )");
    const Analyzed &analyzed = *analyzedPointer;
    const FuncDecl &use = Function(analyzed, "Use");
    const auto bindingAt = [&](const std::size_t index) -> const ResolvedCallableBinding & {
        const auto *call = dynamic_cast<const CallExpr *>(&Initializer(use, index));
        REQUIRE(call != nullptr);
        const ResolvedCallableBinding *binding = analyzed.model.TryGetCallableBinding(*call);
        REQUIRE(binding != nullptr);
        return *binding;
    };
    // One optional level is peeled per optional parameter level, so `int32??` binds `T` to `int32?`.
    CHECK_EQ(bindingAt(0).substitutions.at("T"), TypeRef::MakeOptional(TypeRef::MakeInt32()));
    CHECK_EQ(bindingAt(1).substitutions.at("T"), TypeRef::MakeInt64());
    CHECK_EQ(bindingAt(1).substitutions.at("E"), TypeRef::MakeNamed("ParseError"));
}
