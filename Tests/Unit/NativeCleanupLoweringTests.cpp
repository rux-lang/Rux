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

LirPackage Lower(const Analyzed &analyzed) {
    HirToLirLowering lowering(AstToHirLowering(analyzed.model).Generate(), TargetContext::CreateNative());
    LirPackage package = lowering.Generate();
    REQUIRE(lowering.Diagnostics().empty());
    return package;
}

const LirFunc &RequireFunction(const LirPackage &package, const std::string &name) {
    for (const LirModule &module : package.modules) {
        for (const LirFunc &function : module.funcs) {
            if (function.name == name) {
                return function;
            }
        }
    }
    FAIL("missing lowered function " << name);
    throw std::runtime_error("missing lowered function");
}

/// Whether the block of a match arm calls `symbol` directly, before anything the arm's body adds.
bool ArmCalls(const LirFunc &function, const std::string &arm, const std::string &symbol) {
    for (const LirBlock &block : function.blocks) {
        if (block.label != arm) {
            continue;
        }
        return std::ranges::any_of(block.instrs, [&](const LirInstr &instruction) {
            return instruction.op == LirOpcode::Call && instruction.strArg == symbol;
        });
    }
    FAIL("missing block " << arm);
    return false;
}

const std::string kTracked = R"(
    struct Counter { count: int32; }
    struct Tracked { counter: *var Counter; }
    extend Tracked {
        func =(self: &var Tracked, other: &Tracked);
        func ~Tracked(self: &var Tracked) {
            self.counter.count = self.counter.count + 1;
        }
    }
    struct E {}
)";

/// The tags of the variant steps a native level's drop glue destroys, in step order.
std::vector<std::string> DestroyedTags(const SemanticModel &model, const TypeRef &type) {
    const DropGluePlan *plan = model.TryGetDropGlue(type);
    REQUIRE(plan != nullptr);
    std::vector<std::string> tags;
    for (const DropGlueStep &step : plan->steps) {
        CHECK_EQ(step.kind, DropGlueStep::Kind::EnumVariant);
        CHECK_EQ(step.form, CaseTypeForm::Variant);
        REQUIRE_EQ(step.payloadTypes.size(), 1);
        tags.push_back(step.discriminant);
    }
    return tags;
}
} // namespace

TEST_CASE("native drop glue destroys only the active payload by its native tag") {
    const auto analyzed = Analyze(kTracked + R"(
        func Use(optional: Tracked?, success: Tracked ! E, failure: int32 ! Tracked, both: Tracked ! Tracked,
                 sum: int32 | Tracked, nested: (Tracked ! E)?, plain: int32?) {}
    )");
    const SemanticModel &model = analyzed->model;
    const TypeRef tracked = TypeRef::MakeNamed("Tracked");
    const TypeRef error = TypeRef::MakeNamed("E");

    CHECK_EQ(DestroyedTags(model, TypeRef::MakeOptional(tracked)), std::vector<std::string>{"1"});
    CHECK_EQ(DestroyedTags(model, TypeRef::MakeFallible(tracked, error)), std::vector<std::string>{"0"});
    CHECK_EQ(DestroyedTags(model, TypeRef::MakeFallible(TypeRef::MakeInt32(), tracked)), std::vector<std::string>{"1"});
    CHECK_EQ(DestroyedTags(model, TypeRef::MakeFallible(tracked, tracked)), (std::vector<std::string>{"0", "1"}));
    // Canonical order spells `Tracked | int32`, so the tracked member is member zero.
    CHECK_EQ(DestroyedTags(model, TypeRef::MakeSum({TypeRef::MakeInt32(), tracked})), std::vector<std::string>{"0"});
    CHECK_EQ(DestroyedTags(model, TypeRef::MakeOptional(TypeRef::MakeFallible(tracked, error))),
             std::vector<std::string>{"1"});
    // Nothing to destroy, so no glue at all.
    CHECK(model.TryGetDropGlue(TypeRef::MakeOptional(TypeRef::MakeInt32())) == nullptr);
}

TEST_CASE("an instantiation composing a native level from its parameters gets glue") {
    const auto analyzed = Analyze(kTracked + R"(
        func Hold<T>(value: T) {
            let wrapped: T? = .Some(<-value);
        }
        func Use(counter: *var Counter) {
            Hold<Tracked>(Tracked { counter: counter });
        }
    )");
    CHECK(analyzed->model.TryGetDropGlue(TypeRef::MakeOptional(TypeRef::MakeNamed("Tracked"))) != nullptr);
}

TEST_CASE("an arm over a consumed subject destroys what it leaves unbound") {
    const auto analyzed = Analyze(kTracked + R"(
        func Produce(counter: *var Counter) -> Tracked ! E {
            return .Success(Tracked { counter: counter });
        }
        func DiscardNamed(counter: *var Counter) {
            let held: Tracked ! E = .Success(Tracked { counter: counter });
            match <-held {
                .Success(_) => {},
                .Failure(e) => {}
            }
        }
        func DiscardTemporary(counter: *var Counter) {
            match Produce(counter) {
                .Success(_) => {},
                .Failure(_) => {}
            }
        }
        func Bind(counter: *var Counter) {
            let held: Tracked ! E = .Success(Tracked { counter: counter });
            match <-held {
                .Success(value) => {},
                .Failure(e) => {}
            }
        }
    )");
    const LirPackage package = Lower(*analyzed);
    const std::string fallibleGlue =
        analyzed->model.TryGetDropGlue(TypeRef::MakeFallible(TypeRef::MakeNamed("Tracked"), TypeRef::MakeNamed("E")))
            ->symbol;
    const std::string trackedGlue = analyzed->model.TryGetDropGlue(TypeRef::MakeNamed("Tracked"))->symbol;

    // An arm that binds nothing destroys the subject it was handed, whose glue destroys only the active payload; an
    // arm that binds leaves its payload to the binding's own cleanup.
    const LirFunc &discardNamed = RequireFunction(package, "DiscardNamed");
    CHECK(ArmCalls(discardNamed, "match.arm0", fallibleGlue));
    CHECK_FALSE(ArmCalls(discardNamed, "match.arm1", fallibleGlue));
    // A temporary is taken by its match, so every arm that binds nothing destroys it.
    const LirFunc &discardTemporary = RequireFunction(package, "DiscardTemporary");
    CHECK(ArmCalls(discardTemporary, "match.arm0", fallibleGlue));
    CHECK(ArmCalls(discardTemporary, "match.arm1", fallibleGlue));
    const LirFunc &bind = RequireFunction(package, "Bind");
    CHECK_FALSE(ArmCalls(bind, "match.arm0", fallibleGlue));
    CHECK_FALSE(ArmCalls(bind, "match.arm0", trackedGlue));
}
