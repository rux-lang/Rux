// What typed throws lower to, in HIR forms the later stages already know: a throw site is a match on the outcome its
// call returns whose failure arm either returns the outer failure or stores the error in its handler's outcome slot
// and breaks out of the handler's hidden loop; the handler's arms then match the slot. A `break` inside a handler
// still leaves the source loop around it, which a label then names.

#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <doctest.h>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>

using namespace Rux;

namespace {
HirPackage LowerSource(const std::string &source) {
    Lexer lexer(source, "throws.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "throws.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    for (const auto &diagnostic : model.diagnostics) {
        CAPTURE(diagnostic.message);
    }
    REQUIRE_FALSE(model.HasErrors());
    AstToHirLowering lowering(model);
    HirPackage package = lowering.Generate();
    REQUIRE(lowering.Diagnostics().empty());
    return package;
}

const HirFunc &RequireFunction(const HirPackage &package, const std::string &name) {
    for (const HirModule &module : package.modules) {
        for (const HirFunc &function : module.funcs) {
            if (function.name == name) {
                return function;
            }
        }
    }
    FAIL("missing lowered function " << name);
    throw std::runtime_error("missing lowered function");
}

/// Every statement under `block`, nested blocks and expression blocks included, in order.
void VisitStatements(const HirBlock &block, const std::function<void(const HirStmt &)> &visit);

void VisitExpression(const HirExpr *expression, const std::function<void(const HirStmt &)> &visit) {
    if (!expression) {
        return;
    }
    if (const auto *block = dynamic_cast<const HirBlockExpr *>(expression)) {
        VisitStatements(block->block, visit);
        VisitExpression(block->value.get(), visit);
    }
    else if (const auto *match = dynamic_cast<const HirMatchExpr *>(expression)) {
        VisitExpression(match->subject.get(), visit);
        for (const HirMatchArm &arm : match->arms) {
            VisitExpression(arm.body.get(), visit);
        }
    }
    else if (const auto *assignment = dynamic_cast<const HirAssignExpr *>(expression)) {
        VisitExpression(assignment->value.get(), visit);
    }
    else if (const auto *construct = dynamic_cast<const HirEnumConstructExpr *>(expression)) {
        for (const auto &payload : construct->payloads) {
            VisitExpression(payload.get(), visit);
        }
    }
}

void VisitStatements(const HirBlock &block, const std::function<void(const HirStmt &)> &visit) {
    for (const auto &statement : block.stmts) {
        visit(*statement);
        if (const auto *scope = dynamic_cast<const HirScopeStmt *>(statement.get())) {
            VisitStatements(scope->block, visit);
        }
        else if (const auto *loop = dynamic_cast<const HirLoopStmt *>(statement.get())) {
            VisitStatements(loop->body, visit);
        }
        else if (const auto *whileLoop = dynamic_cast<const HirWhileStmt *>(statement.get())) {
            VisitStatements(whileLoop->body, visit);
        }
        else if (const auto *conditional = dynamic_cast<const HirIfStmt *>(statement.get())) {
            VisitStatements(conditional->thenBlock, visit);
            if (conditional->elseBlock) {
                VisitStatements(*conditional->elseBlock, visit);
            }
        }
        else if (const auto *match = dynamic_cast<const HirMatchStmt *>(statement.get())) {
            for (const HirMatchArm &arm : match->arms) {
                VisitExpression(arm.body.get(), visit);
            }
        }
        else if (const auto *expression = dynamic_cast<const HirExprStmt *>(statement.get())) {
            VisitExpression(expression->expr.get(), visit);
        }
        else if (const auto *let = dynamic_cast<const HirLetStmt *>(statement.get())) {
            VisitExpression(let->init.get(), visit);
        }
        else if (const auto *returned = dynamic_cast<const HirReturnStmt *>(statement.get())) {
            VisitExpression(returned->value ? returned->value->get() : nullptr, visit);
        }
    }
}

template <typename Statement>
std::vector<const Statement *> StatementsOf(const HirFunc &function) {
    std::vector<const Statement *> found;
    REQUIRE(function.body.has_value());
    VisitStatements(*function.body, [&](const HirStmt &statement) {
        if (const auto *typed = dynamic_cast<const Statement *>(&statement)) {
            found.push_back(typed);
        }
    });
    return found;
}
} // namespace

TEST_CASE("a throw site to the function exit lowers like propagation") {
    const HirPackage package = LowerSource(R"(
        struct Fault {}
        func Read() -> int32 ! Fault { return 1i32; }
        func Forward() -> int32 ! Fault {
            let value = Read()!;
            return value;
        }
    )");
    const HirFunc &forward = RequireFunction(package, "Forward");
    const auto *let = dynamic_cast<const HirLetStmt *>(forward.body->stmts.front().get());
    REQUIRE(let != nullptr);
    const auto *match = dynamic_cast<const HirMatchExpr *>(let->init.get());
    REQUIRE(match != nullptr);
    CHECK(match->type == TypeRef::MakeInt32());
    CHECK(match->subject->type.ToString() == "int32 ! Fault");
    REQUIRE(match->arms.size() == 2);
    const auto *failing = dynamic_cast<const HirBlockExpr *>(match->arms[1].body.get());
    REQUIRE(failing != nullptr);
    REQUIRE(failing->block.stmts.size() == 1);
    CHECK(dynamic_cast<const HirReturnStmt *>(failing->block.stmts.front().get()) != nullptr);
}

TEST_CASE("a handler is a match on its operand's outcome") {
    const HirPackage package = LowerSource(R"(
        struct Fault {}
        func Read() -> int32 ! Fault { return 1i32; }
        func Optional() -> int32? { return Read() !! none; }
        func Recovered() -> int32 { return Read() catch { _ => 0i32 }; }
    )");
    for (const auto &[name, type] :
         {std::pair{"Optional", std::string("int32?")}, std::pair{"Recovered", std::string("int32")}}) {
        CAPTURE(name);
        const HirFunc &function = RequireFunction(package, name);
        const auto *returned = dynamic_cast<const HirReturnStmt *>(function.body->stmts.front().get());
        REQUIRE(returned != nullptr);
        const auto *value = dynamic_cast<const HirMatchExpr *>(returned->value->get());
        REQUIRE(value != nullptr);
        CHECK(value->type.ToString() == type);
        CHECK(value->subject->type.ToString() == "int32 ! Fault");
        REQUIRE(value->arms.size() == 2);
        CHECK(StatementsOf<HirLoopStmt>(function).empty());
    }
}

TEST_CASE("an unlabeled break in an arm leaves the source loop around it") {
    const HirPackage package = LowerSource(R"(
        struct Fault {}
        func Read(index: int32) -> int32 ! Fault { return index; }
        func Scan() -> int32 {
            var total = 0i32;
            while total < 10i32 {
                let value = Read(total) catch { _ => break };
                total = total + value + 1i32;
            }
            return total;
        }
    )");
    const HirFunc &scan = RequireFunction(package, "Scan");
    const auto loops = StatementsOf<HirWhileStmt>(scan);
    REQUIRE(loops.size() == 1);
    // No hidden loop stands between the arm and the source loop, so the jump needs no label.
    const std::string label = loops.front()->label;
    const auto breaks = StatementsOf<HirBreakStmt>(scan);
    CHECK(std::ranges::count_if(breaks, [&](const HirBreakStmt *leave) { return leave->label == label; }) == 1);
}
