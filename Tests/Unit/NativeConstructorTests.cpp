// The native constructors `.Success`, `.Failure`, and `.Some` in every value position: the expected type selects the
// channel each operand is checked against, wrappers the context supplies are recorded, and a constructor that cannot
// know a channel, or that meets a nominal variant or another form, is refused with a message that says so.

#include "SemanticTestSupport.h"

#include <memory>

using namespace Rux;
using namespace Rux::Testing::SemanticTestSupport;

namespace {
using Kind = NativeConversionStep::Kind;

struct Analyzed {
    explicit Analyzed(ParseResult result)
        : parsed(std::move(result))
        , model(SemanticAnalyzer({&parsed.module}, {}, "constructors", "Windows").Analyze()) {
    }

    ParseResult parsed;
    SemanticModel model;
};

std::unique_ptr<Analyzed> Analyze(const std::string &source) {
    Lexer lexer(source, "native_constructors.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "native_constructors.rux");
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

std::vector<Kind> RouteOf(const Analyzed &analyzed, const Expr &expr) {
    std::vector<Kind> kinds;
    if (const auto *route = analyzed.model.TryGetNativeConversion(expr)) {
        for (const NativeConversionStep &step : *route) {
            kinds.push_back(step.kind);
        }
    }
    return kinds;
}

std::vector<std::string> Errors(const Analyzed &analyzed) {
    std::vector<std::string> errors;
    for (const auto &diagnostic : analyzed.model.diagnostics) {
        if (diagnostic.severity == SemanticDiagnostic::Severity::Error &&
            !diagnostic.message.contains("is not supported in compiled code yet") &&
            !diagnostic.message.contains("is not supported yet")) {
            errors.push_back(diagnostic.message);
        }
    }
    return errors;
}

bool Reports(const Analyzed &analyzed, const std::string_view text) {
    return std::ranges::any_of(analyzed.model.diagnostics,
                               [&](const SemanticDiagnostic &diagnostic) { return diagnostic.message.contains(text); });
}
} // namespace

TEST_CASE("constructors build values in every value position") {
    const auto analyzed = Analyze(R"(
        struct E {}
        struct Holder { result: int32 ! E; }
        func Store(result: int32 ! E) {}
        func Use(error: E) {
            let accepted: int32 ! E = .Success(1i32);
            let rejected: int32 ! E = .Failure(error);
            let present: int32? = .Some(3i32);
            let holder = Holder { result: .Failure(error) };
            let items: (int32 ! E)[2] = [.Success(1i32), .Failure(error)];
            Store(.Failure(error));
        }
    )");
    CHECK(Errors(*analyzed).empty());
    // A constructor of the expected form records no wrapper.
    CHECK(RouteOf(*analyzed, Initializer(Function(*analyzed, "Use"), 0)).empty());
}

TEST_CASE("the channel a constructor selects types its operand") {
    const auto analyzed = Analyze(R"(
        struct E {}
        func Use(error: E) {
            let same: int32 ! int32 = .Failure(2i32);
            let other: int32 ! int32 = .Success(1i32);
            let optionalError: int32 ! (E?) = .Failure(none);
            let nested: int32 ! (int32 ! E) = .Failure(.Failure(error));
            let wrongPayload: int32? = .Some(true);
            let wrongChannel: int32 ! E = .Failure(1i32);
        }
    )");
    const std::vector<std::string> errors = Errors(*analyzed);
    REQUIRE_EQ(errors.size(), 2);
    CHECK(errors[0].contains("cannot assign"));
    CHECK(errors[1].contains("cannot assign"));
}

TEST_CASE("every nested optional state is constructible") {
    const auto analyzed = Analyze(R"(
        func Use() {
            let missing: int32?? = none;
            let storedAbsence: int32?? = .Some(none);
            let storedValue: int32?? = .Some(.Some(7i32));
        }
    )");
    CHECK(Errors(*analyzed).empty());
    const FuncDecl &use = Function(*analyzed, "Use");
    CHECK_EQ(RouteOf(*analyzed, Initializer(use, 0)), std::vector{Kind::Absent});
    const auto *storedAbsence = dynamic_cast<const NativeConstructExpr *>(&Initializer(use, 1));
    REQUIRE(storedAbsence != nullptr);
    CHECK_EQ(RouteOf(*analyzed, *storedAbsence->operand), std::vector{Kind::Absent});
}

TEST_CASE("the context may wrap a constructor of the other form") {
    const auto analyzed = Analyze(R"(
        struct E {}
        func Use() {
            let present: int32? ! E = .Some(1i32);
            let succeeded: (int32 ! E)? = .Success(1i32);
        }
    )");
    CHECK(Errors(*analyzed).empty());
    const FuncDecl &use = Function(*analyzed, "Use");
    CHECK_EQ(RouteOf(*analyzed, Initializer(use, 0)), std::vector{Kind::Success});
    CHECK_EQ(RouteOf(*analyzed, Initializer(use, 1)), std::vector{Kind::Presence});
}

TEST_CASE("a fallible constructor needs its other channel from the context") {
    const auto analyzed = Analyze(R"(
        func Use() {
            let unknown = .Success(1i32);
            let inferred = .Some(1i32);
        }
    )");
    CHECK(Reports(*analyzed, "cannot infer the type of 'unknown' from a native constructor with an unknown channel"));
    CHECK_FALSE(Reports(*analyzed, "cannot infer the type of 'inferred'"));
    CHECK_EQ(ResolvedType(analyzed->model, Initializer(Function(*analyzed, "Use"), 1)),
             TypeRef::MakeOptional(TypeRef::MakeInt32()));
}

TEST_CASE("a constructor meeting another form or a nominal variant is refused") {
    const auto analyzed = Analyze(R"(
        variant Option<T> {
            Some(T),
            None
        }
        variant Result<T, E> {
            Success(T),
            Error(E)
        }
        struct E {}
        func Use() {
            let legacy: Option<int32> = .Some(1i32);
            let legacyResult: Result<int32, E> = .Success(1i32);
            let scalar: int32 = .Some(1i32);
            let optional: int32? = .Success(1i32);
        }
    )");
    CHECK(Reports(*analyzed,
                  "'.Some(...)' constructs a native optional; write 'Option::Some(...)' for variant 'Option'"));
    CHECK(Reports(*analyzed,
                  "'.Success(...)' constructs a native fallible; write 'Result::Success(...)' for variant 'Result'"));
    CHECK(Reports(*analyzed, "'.Some(...)' constructs a native optional, but the expected type is 'int32'"));
    CHECK(Reports(*analyzed, "'.Success(...)' constructs a native fallible, but the expected type is 'int32?'"));
}

TEST_CASE("a native value without a written native type cannot reach lowering") {
    const auto analyzed = Analyze(R"(
        func Main() -> int {
            let value = .Some(1i32);
            return 0;
        }
    )");
    CHECK(Reports(*analyzed, "native value of type 'int32?' is not supported in compiled code yet"));
}
