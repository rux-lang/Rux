#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <doctest.h>
#include <string>
#include <utility>
#include <vector>

using namespace Rux;

namespace {
std::vector<SemanticDiagnostic> AnalyzeConsumptionDiagnostics(const std::string &source) {
    Lexer lexer(source, "move_consumption.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "move_consumption.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    return analyzer.Analyze().diagnostics;
}
} // namespace

TEST_CASE("move-only values are consumed in every by-value context") {
    const std::vector<SemanticDiagnostic> diagnostics = AnalyzeConsumptionDiagnostics(R"(
        interface Drop {}

        struct Handle { value: int32; }
        extend Handle : Drop {}
        extend Handle {
            func Consume(self: Handle) {}
        }
        struct Owner { handle: Handle; }

        func NewHandle(value: int32) -> Handle {
            return Handle { value: value };
        }

        func Take(value: Handle) {}

        func Main(flag: bool) {
            let initialized = NewHandle(1);
            let movedByInitialization = initialized;
            initialized;

            var assigned = NewHandle(2);
            var destination = NewHandle(3);
            destination = assigned;
            assigned;

            let argument = NewHandle(4);
            Take(argument);
            argument;

            let field = NewHandle(5);
            let owner = Owner { handle: field };
            field;

            let arrayValue = NewHandle(6);
            let values = [arrayValue];
            arrayValue;

            let tupleValue = NewHandle(7);
            let tuple = (tupleValue, 1);
            tupleValue;

            let receiver = NewHandle(8);
            receiver.Consume();
            receiver;

            var ternaryLeft = NewHandle(9);
            var ternaryRight = NewHandle(10);
            let selected = flag ? ternaryLeft : ternaryRight;
            ternaryLeft;
            ternaryRight;

            var reusable = NewHandle(11);
            let consumed = reusable;
            reusable = NewHandle(12);
            Take(reusable);
            reusable;

            let copy = 12;
            let copied = copy;
            copy;
        }
    )");

    const std::vector<std::string> expected = {
        "value 'initialized' is used after it was moved",
        "value 'assigned' is used after it was moved",
        "value 'argument' is used after it was moved",
        "value 'field' is used after it was moved",
        "value 'arrayValue' is used after it was moved",
        "value 'tupleValue' is used after it was moved",
        "value 'receiver' is used after it was moved",
        "value 'ternaryLeft' may have been moved on some control-flow paths",
        "value 'ternaryRight' may have been moved on some control-flow paths",
        "value 'reusable' is used after it was moved",
    };
    REQUIRE_EQ(diagnostics.size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        CHECK_EQ(diagnostics[index].message, expected[index]);
    }
}

TEST_CASE("accepted ownership transfers are retained as semantic and HIR facts") {
    Lexer lexer(R"(
        interface Drop {}
        struct Handle { value: int32; }
        extend Handle : Drop {}

        func ReturnHandle(value: Handle) -> Handle {
            return value;
        }

        func Main() {
            let first = Handle { value: 1 };
            let second = first;
            ReturnHandle(second);
        }
    )",
                "move_consumption_facts.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "move_consumption_facts.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());

    const auto *returnFunction = dynamic_cast<const FuncDecl *>(parsed.module.items[3].get());
    const auto *returnStatement = dynamic_cast<const ReturnStmt *>(returnFunction->body->stmts[0].get());
    REQUIRE(returnStatement != nullptr);
    const Expr &returned = **returnStatement->value;
    const ValueConsumption *returnFact = model.TryGetConsumption(returned);
    REQUIRE(returnFact != nullptr);
    CHECK_EQ(returnFact->kind, ValueConsumptionKind::Return);
    CHECK_EQ(returnFact->type, TypeRef::MakeNamed("Handle"));

    const auto *mainFunction = dynamic_cast<const FuncDecl *>(parsed.module.items[4].get());
    const auto *second = dynamic_cast<const LetStmt *>(mainFunction->body->stmts[1].get());
    REQUIRE(second != nullptr);
    const ValueConsumption *initializationFact = model.TryGetConsumption(*second->init);
    REQUIRE(initializationFact != nullptr);
    CHECK_EQ(initializationFact->kind, ValueConsumptionKind::Initialization);

    const HirPackage package = AstToHirLowering(model).Generate();
    REQUIRE_EQ(package.modules.size(), 1);
    REQUIRE_EQ(package.modules[0].funcs.size(), 2);
    const HirFunc &loweredMain = package.modules[0].funcs[1];
    REQUIRE(loweredMain.body.has_value());
    const auto *loweredSecond = dynamic_cast<const HirLetStmt *>(loweredMain.body->stmts[1].get());
    REQUIRE(loweredSecond != nullptr);
    REQUIRE(loweredSecond->init->consumption.has_value());
    CHECK_EQ(*loweredSecond->init->consumption, ValueConsumptionKind::Initialization);
}

TEST_CASE("rejected by-value contexts do not move their operands") {
    const std::vector<SemanticDiagnostic> diagnostics = AnalyzeConsumptionDiagnostics(R"(
        interface Drop {}
        struct Handle { value: int32; }
        extend Handle : Drop {}
        struct Number { value: int32; }

        func Take(value: int32) {}

        func Main() {
            let assignment = Handle { value: 1 };
            let invalid: int32 = assignment;
            assignment;

            let argument = Handle { value: 2 };
            Take(argument);
            argument;

            let field = Handle { value: 3 };
            let invalidOwner = Number { value: field };
            field;
        }
    )");

    REQUIRE_EQ(diagnostics.size(), 3);
    for (const SemanticDiagnostic &diagnostic : diagnostics) {
        CHECK(diagnostic.message.find("used after it was moved") == std::string::npos);
    }
}
