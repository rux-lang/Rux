// How native payloads are destroyed: each native level's drop glue destroys only its active payload, selected by the
// level's native tag; an instantiation composing a native level from its parameters still gets glue; and an arm over a
// consumed subject destroys whatever its pattern leaves unbound, so a discard match neither leaks nor double-drops.

#include "Ir/Lir/Lir.h"
#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"
#include "Types/NativeLayout.h"

#include <algorithm>
#include <doctest.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

using namespace Rux;

namespace {
struct Analyzed {
    explicit Analyzed(ParseResult input)
        : parsed(std::move(input))
        , model(SemanticAnalyzer({&parsed.module}, {}, "cleanup", "Windows").Analyze()) {
    }

    ParseResult parsed;
    SemanticModel model;
};

std::unique_ptr<Analyzed> Analyze(const std::string &source) {
    Lexer lexer(source, "cleanup.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "cleanup.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    auto analyzed = std::make_unique<Analyzed>(std::move(parsed));
    for (const auto &diagnostic : analyzed->model.diagnostics) {
        INFO(diagnostic.message);
        CHECK(diagnostic.severity != SemanticDiagnostic::Severity::Error);
    }
    return analyzed;
}

const std::string kTracked = R"(
    struct Counter { count: int32; }
    struct Tracked { counter: *var Counter; }
    extend Tracked {
        func ~Tracked(&var self) {
            // Safety: every `Tracked` the tests build points `counter` at a `Counter` that outlives it
            unsafe { self.counter.count = self.counter.count + 1; }
        }
    }
    struct E {}
)";

} // namespace

TEST_CASE("an instantiation composing a native level from its parameters gets glue") {
    const auto analyzed = Analyze(kTracked + R"(
        func Hold<T>(value: T) {
            let wrapped: T? = .Some(move value);
        }
        func Use(counter: *var Counter) {
            Hold<Tracked>(Tracked { counter: counter });
        }
    )");
    CHECK(analyzed->model.TryGetDropGlue(TypeRef::MakeOptional(TypeRef::MakeNamed("Tracked"))) != nullptr);
}
