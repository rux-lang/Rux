// Native conversions in analyzed programs: each accepted implicit conversion records its one route on the converted
// expression, literals target sums by kind, contextual `none` becomes absence, and ambiguous or narrowing conversions
// are refused with a message that names the choice to make.

#include "SemanticTestSupport.h"

#include <memory>

using namespace Rux;
using namespace Rux::Testing::SemanticTestSupport;

namespace {
using Kind = NativeConversionStep::Kind;

struct Analyzed {
    explicit Analyzed(ParseResult result)
        : parsed(std::move(result))
        , model(SemanticAnalyzer({&parsed.module}, {}, "conversions", "Windows").Analyze()) {
    }

    ParseResult parsed;
    SemanticModel model;
};

std::unique_ptr<Analyzed> Analyze(const std::string &source) {
    Lexer lexer(source, "native_conversions.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "native_conversions.rux");
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

const Expr &Initializer(const FuncDecl &function, const std::size_t index) {
    const auto *let = dynamic_cast<const LetStmt *>(function.body->stmts.at(index).get());
    REQUIRE(let != nullptr);
    return *let->init;
}

/// The recorded route of `expr` as step kinds, or an empty list when no native conversion was recorded.
std::vector<Kind> RouteOf(const Analyzed &analyzed, const Expr &expr) {
    std::vector<Kind> kinds;
    if (const auto *route = analyzed.model.TryGetNativeConversion(expr)) {
        for (const NativeConversionStep &step : *route) {
            kinds.push_back(step.kind);
        }
    }
    return kinds;
}

bool Reports(const Analyzed &analyzed, const std::string_view text) {
    return std::ranges::any_of(analyzed.model.diagnostics,
                               [&](const SemanticDiagnostic &diagnostic) { return diagnostic.message.contains(text); });
}
} // namespace

TEST_CASE("implicit conversions record their one route") {
    const auto analyzed = Analyze(R"(
        struct A {}
        struct B {}
        struct E {}
        func Use(a: A, optionalA: A?, sum: A | B) {
            let present: int32? = 5;
            let injected: A | B = a;
            let succeeded: int32 ! E = 7i32;
            let nested: int32? ! E = 7i32;
            let widened: (A | B)? = optionalA;
            let same: A | B = sum;
            let absent: int32? ! E = none;
        }
    )");
    const FuncDecl &use = Function(*analyzed, "Use");
    CHECK_EQ(RouteOf(*analyzed, Initializer(use, 0)), std::vector{Kind::Presence, Kind::Identity});
    CHECK_EQ(RouteOf(*analyzed, Initializer(use, 1)), std::vector{Kind::Inject});
    CHECK_EQ(RouteOf(*analyzed, Initializer(use, 2)), std::vector{Kind::Success, Kind::Identity});
    CHECK_EQ(RouteOf(*analyzed, Initializer(use, 3)), (std::vector{Kind::Success, Kind::Presence, Kind::Identity}));
    CHECK_EQ(RouteOf(*analyzed, Initializer(use, 4)), std::vector{Kind::WidenOptional, Kind::Inject});
    // Identity records nothing.
    CHECK(RouteOf(*analyzed, Initializer(use, 5)).empty());
    CHECK_EQ(RouteOf(*analyzed, Initializer(use, 6)), std::vector{Kind::Success, Kind::Absent});
}

TEST_CASE("arguments and returns convert through the same rules") {
    const auto analyzed = Analyze(R"(
        struct E {}
        func Take(value: int32?) {}
        func Give(value: int32) -> int32 ! E { return value; }
        func Payload<T>(value: T?) -> T { return Payload(value); }
        func Use() {
            Take(3i32);
            let deduced = Payload(7i32);
        }
    )");
    const FuncDecl &use = Function(*analyzed, "Use");
    const auto *call = dynamic_cast<const ExprStmt *>(use.body->stmts.at(0).get());
    REQUIRE(call != nullptr);
    const auto *take = dynamic_cast<const CallExpr *>(call->expr.get());
    REQUIRE(take != nullptr);
    CHECK_EQ(RouteOf(*analyzed, *take->args.front()), std::vector{Kind::Presence, Kind::Identity});

    const FuncDecl &give = Function(*analyzed, "Give");
    const auto *returned = dynamic_cast<const ReturnStmt *>(give.body->stmts.front().get());
    REQUIRE(returned != nullptr);
    CHECK_EQ(RouteOf(*analyzed, **returned->value), std::vector{Kind::Success, Kind::Identity});

    // A plain argument to a `T?` parameter deduces `T` and converts by presence construction.
    const auto *deduced = dynamic_cast<const CallExpr *>(&Initializer(use, 1));
    REQUIRE(deduced != nullptr);
    const ResolvedCallableBinding *binding = analyzed->model.TryGetCallableBinding(*deduced);
    REQUIRE(binding != nullptr);
    CHECK_EQ(binding->substitutions.at("T"), TypeRef::MakeInt32());
    CHECK_EQ(RouteOf(*analyzed, *deduced->args.front()), std::vector{Kind::Presence, Kind::Identity});
}

TEST_CASE("an unsuffixed literal targets a sum by its kind, never by its value") {
    const auto analyzed = Analyze(R"(
        func Use() {
            let small: uint8 | int32 = 5;
            let large: uint8 | int32 = 300;
            let single: int32 | bool = 5;
            let fractional: float32 | int32 = 1.5;
        }
    )");
    const FuncDecl &use = Function(*analyzed, "Use");
    CHECK(Reports(*analyzed, "integer literal is ambiguous for 'int32 | uint8'"));
    const auto *single = analyzed->model.TryGetNativeConversion(Initializer(use, 2));
    REQUIRE(single != nullptr);
    REQUIRE_EQ(single->size(), 1);
    CHECK(single->front().kind == Kind::Inject);
    CHECK_EQ(single->front().member, 1);
    const auto *fractional = analyzed->model.TryGetNativeConversion(Initializer(use, 3));
    REQUIRE(fractional != nullptr);
    CHECK_EQ(fractional->front().member, 0);
}

TEST_CASE("narrowing, extraction, and absence outside an optional are refused") {
    const auto analyzed = Analyze(R"(
        struct A {}
        struct B {}
        func Use(sum: A | B, optional: int32?) {
            let narrowed: A = sum;
            let extracted: int32 = optional;
            let nowhere: int32 = none;
            let member: A | (B?) = none;
        }
    )");
    CHECK(Reports(*analyzed, "cannot assign 'A | B' to 'A'"));
    CHECK(Reports(*analyzed, "cannot assign 'int32?' to 'int32'"));
    CHECK(Reports(*analyzed, "'none' needs an expected optional type, but found 'int32'"));
    // `none` never chooses a sum member, even an optional one.
    CHECK(Reports(*analyzed, "'none' needs an expected optional type, but found 'A | (B?)'"));
}

TEST_CASE("forwarding versus nesting is an ambiguity the source must resolve") {
    const auto analyzed = Analyze(R"(
        struct E {}
        struct F {}
        type R = int32 ! E;
        func Keep(value: R) -> (int32 | R) ! E { return value; }
        func Widen(value: R) -> R ! (E | F) { return value; }
        func Nest(value: R) -> R ! F { return value; }
        func Forward(value: R) -> int32 ! (E | F) { return value; }
    )");
    CHECK(Reports(*analyzed, "conversion from 'int32 ! E' to 'int32 | (int32 ! E) ! E' is ambiguous"));
    CHECK(Reports(*analyzed, "conversion from 'int32 ! E' to '(int32 ! E) ! E | F' is ambiguous"));
    const auto routeOfReturn = [&](const std::string_view name) {
        const auto *returned = dynamic_cast<const ReturnStmt *>(Function(*analyzed, name).body->stmts.front().get());
        REQUIRE(returned != nullptr);
        return RouteOf(*analyzed, **returned->value);
    };
    CHECK_EQ(routeOfReturn("Nest"), std::vector{Kind::Success, Kind::Identity});
    CHECK_EQ(routeOfReturn("Forward"), std::vector{Kind::WidenFallible});
}

TEST_CASE("a match takes a native expected type arm by arm") {
    const auto analyzed = Analyze(R"(
        struct E {}
        func Take(value: int32?) {}
        func Returned(value: int32) -> int32? {
            return match value { 0 => none, else => .Some(value) };
        }
        func Failed(value: int32, error: E) -> int32 ! E {
            return match value { 0 => .Success(value), else => .Failure(error) };
        }
        func Use(value: int32) {
            let annotated: int32? = match value { 0 => none, else => .Some(value) };
            let joined = match value { 0 => none, else => .Some(value) };
            let narrow: int8? = match value { 0 => none, 1 => .Some(7), else => 9 };
            Take(match value { 0 => none, else => .Some(value) });
        }
    )");
    CHECK(analyzed->model.diagnostics.empty());
    const auto typeOf = [&](const Expr &expr) {
        const TypeRef *type = analyzed->model.TryGetType(expr);
        REQUIRE(type != nullptr);
        return type->ToString();
    };
    const auto returned = [&](const std::string_view name) -> const Expr & {
        const auto *statement = dynamic_cast<const ReturnStmt *>(Function(*analyzed, name).body->stmts.front().get());
        REQUIRE(statement != nullptr);
        return **statement->value;
    };
    const FuncDecl &use = Function(*analyzed, "Use");
    const auto *call = dynamic_cast<const ExprStmt *>(use.body->stmts.at(3).get());
    REQUIRE(call != nullptr);
    const auto *take = dynamic_cast<const CallExpr *>(call->expr.get());
    REQUIRE(take != nullptr);

    // The match holds the expected type, and no conversion of the match as a whole is recorded: converting an open
    // `opaque?` would make every arm absent.
    const std::vector<const Expr *> matches{&returned("Returned"), &Initializer(use, 0), &Initializer(use, 1),
                                            take->args.front().get()};
    for (const Expr *match : matches) {
        CHECK_EQ(typeOf(*match), "int32?");
        CHECK(RouteOf(*analyzed, *match).empty());
    }
    CHECK_EQ(typeOf(returned("Failed")), "int32 ! E");
    CHECK(RouteOf(*analyzed, returned("Failed")).empty());
    CHECK_EQ(typeOf(Initializer(use, 2)), "int8?");

    // A plain arm still converts on its own route.
    const auto *narrow = dynamic_cast<const MatchExpr *>(&Initializer(use, 2));
    REQUIRE(narrow != nullptr);
    CHECK_EQ(RouteOf(*analyzed, *narrow->arms.at(2).body), std::vector{Kind::Presence, Kind::Identity});
}

TEST_CASE("a match arm its expected type refuses is reported") {
    const auto analyzed = Analyze(R"(
        func Mismatch(value: int32) -> int32? {
            return match value { 0 => .Some(1i32), else => .Some(true) };
        }
        func Late(value: int32) -> int32? {
            return match value { 0 => .Failure(true), 1 => none, else => .Some(1i32) };
        }
        func Refused(value: int32) -> int32? {
            return match value { 0 => none, else => .Failure(true) };
        }
        func Open(value: int32) {
            let open = match value { 0 => .Success(1i32), else => .Failure(true) };
        }
    )");
    CHECK(Reports(*analyzed, "match arm type mismatch: expected 'int32?', found 'bool8?'"));
    CHECK(Reports(*analyzed, "'.Failure(...)' constructs a native fallible, but the expected type is 'int32?'"));
    CHECK(Reports(*analyzed, "'return' value must have type 'int32?', but found 'opaque?'"));
    CHECK(Reports(*analyzed, "cannot infer the type of 'open' from a native constructor with an unknown channel"));
}
