#include "Ir/Hir/Hir.h"
#include "Ir/Hir/Passes/PassManager.h"

#include <doctest.h>

#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

using namespace Rux;

static HirPackage CompileAndOptimize(const std::string &source) {
    Lexer lexer(source, "test.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());

    Parser parser(std::move(lexed.tokens), "test.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    std::vector<Module *> modules = {&parsed.module};
    SemanticAnalyzer analyzer(modules, {}, "test", "windows");
    auto semaModel = analyzer.Analyze();
    REQUIRE_FALSE(semaModel.HasErrors());

    AstToHirLowering hirLowering(semaModel);
    auto package = hirLowering.Generate();

    HirPassManager::Run(package);
    return package;
}

TEST_CASE("optimizer eliminates dead code after return") {
    std::string source = R"(
        func Main() -> int {
            return 42;
            let x = 10;
        }
    )";

    auto package = CompileAndOptimize(source);
    REQUIRE(package.modules.size() == 1);
    auto &mod = package.modules[0];
    REQUIRE(mod.funcs.size() == 1);
    auto &func = mod.funcs[0];
    REQUIRE(func.body.has_value());

    // Le corps de la fonction ne doit contenir que l'instruction return.
    // L'instruction "let x = 10;" doit être éliminée.

    CHECK(func.body->stmts.size() == 1);
    CHECK(dynamic_cast<HirReturnStmt *>(func.body->stmts[0].get()) != nullptr);
}

TEST_CASE("optimizer folds constant true condition branch") {
    std::string source = R"(
        func Main() -> int {
            if true {
                return 1;
            } else {
                return 2;
            }
        }
    )";

    auto package = CompileAndOptimize(source);
    REQUIRE(package.modules.size() == 1);
    auto &mod = package.modules[0];
    REQUIRE(mod.funcs.size() == 1);
    auto &func = mod.funcs[0];
    REQUIRE(func.body.has_value());

    // Le bloc conditionnel if true doit être remplacé par les instructions de sa branche true.
    // "return 2;" dans la branche false ne doit pas être présent.
    REQUIRE(func.body->stmts.size() == 1);
    auto *ret = dynamic_cast<HirReturnStmt *>(func.body->stmts[0].get());
    REQUIRE(ret != nullptr);

    // Le retour doit être une constante litérale avec la valeur "1"
    REQUIRE(ret->value.has_value());
    auto *lit = dynamic_cast<HirLiteralExpr *>(ret->value->get());
    REQUIRE(lit != nullptr);
    CHECK(lit->value == "1");
}

TEST_CASE("optimizer folds constant false condition branch") {
    std::string source = R"(
        func Main() -> int {
            if false {
                return 1;
            } else {
                return 2;
            }
        }
    )";

    auto package = CompileAndOptimize(source);
    REQUIRE(package.modules.size() == 1);
    auto &mod = package.modules[0];
    REQUIRE(mod.funcs.size() == 1);
    auto &func = mod.funcs[0];
    REQUIRE(func.body.has_value());

    // Le bloc conditionnel if false doit être remplacé par la branche else.
    REQUIRE(func.body->stmts.size() == 1);
    auto *ret = dynamic_cast<HirReturnStmt *>(func.body->stmts[0].get());
    REQUIRE(ret != nullptr);

    REQUIRE(ret->value.has_value());
    auto *lit = dynamic_cast<HirLiteralExpr *>(ret->value->get());
    REQUIRE(lit != nullptr);
    CHECK(lit->value == "2");
}

TEST_CASE("optimizer combines constant propagation, condition folding and dead code elimination") {
    std::string source = R"(
        func Main() -> int {
            let a = 1;
            if a == 1 {
                return 10;
            }
            return 20;
        }
    )";

    auto package = CompileAndOptimize(source);
    REQUIRE(package.modules.size() == 1);
    auto &mod = package.modules[0];
    REQUIRE(mod.funcs.size() == 1);
    auto &func = mod.funcs[0];
    REQUIRE(func.body.has_value());

    // 1. "let a = 1;" est conservé
    // 2. "if a == 1" est plié en "if true", donc aplati en "return 10;"
    // 3. "return 20;" après "return 10;" est éliminé en tant que code mort
    REQUIRE(func.body->stmts.size() == 2);

    auto *let = dynamic_cast<HirLetStmt *>(func.body->stmts[0].get());
    REQUIRE(let != nullptr);
    CHECK(let->name == "a");

    auto *ret = dynamic_cast<HirReturnStmt *>(func.body->stmts[1].get());
    REQUIRE(ret != nullptr);
    REQUIRE(ret->value.has_value());
    auto *lit = dynamic_cast<HirLiteralExpr *>(ret->value->get());
    REQUIRE(lit != nullptr);
    CHECK(lit->value == "10");
}

TEST_CASE("optimizer folds integer wrapping and parses hex/binary literals") {
    std::string source = R"(
        func Main() -> int {
            let a: int32 = -2147483648i32;
            let b: int32 = 2147483647i32 + 1i32;
            let c: int64 = 0x00000000FFFFFFFFi64;
            if b == a && c == 4294967295i64 {
                return 100;
            }
            return 200;
        }
    )";

    auto package = CompileAndOptimize(source);
    REQUIRE(package.modules.size() == 1);
    auto &mod = package.modules[0];
    REQUIRE(mod.funcs.size() == 1);
    auto &func = mod.funcs[0];
    REQUIRE(func.body.has_value());

    REQUIRE(func.body->stmts.size() == 4);
    auto *ret = dynamic_cast<HirReturnStmt *>(func.body->stmts[3].get());
    REQUIRE(ret != nullptr);
    REQUIRE(ret->value.has_value());
    auto *lit = dynamic_cast<HirLiteralExpr *>(ret->value->get());
    REQUIRE(lit != nullptr);
    CHECK(lit->value == "100");
}
