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
        if (diagnostic.severity == SemanticDiagnostic::Severity::Error) {
            errors.push_back(diagnostic.message);
        }
    }
    return errors;
}

} // namespace

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

TEST_CASE("a native value without a written native type is an ordinary value") {
    const auto analyzed = Analyze(R"(
        func Main() -> int {
            let value = .Some(1i32);
            return 0;
        }
    )");
    CHECK_FALSE(analyzed->model.HasErrors());
}
