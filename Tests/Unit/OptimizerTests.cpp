#include "Ir/Hir/Hir.h"
#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Optimization/Pipeline.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <algorithm>
#include <doctest.h>
#include <memory>
#include <string_view>

using namespace Rux;

static HirPackage CompileToHir(const std::string &source) {
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
    return hirLowering.Generate();
}

static HirPackage CompileAndOptimize(const std::string &source) {
    auto package = CompileToHir(source);
    auto pipeline = Optimization::OptimizationPipeline::ForProfile(BuildProfile::Release);
    REQUIRE(pipeline.RunHir(package).reachedFixedPoint);
    return package;
}

static LirPackage CompileToLir(const std::string &source, const BuildProfile profile) {
    auto package = CompileToHir(source);
    auto pipeline = Optimization::OptimizationPipeline::ForProfile(profile);
    REQUIRE(pipeline.RunHir(package).reachedFixedPoint);
    return HirToLirLowering(std::move(package), TargetContext::CreateNative()).Generate();
}

namespace {
class RecordingHirPass final : public Optimization::HirPass {
public:
    RecordingHirPass(std::string_view name, std::vector<std::string_view> &runs, std::size_t changesRemaining)
        : name_(name)
        , runs_(runs)
        , changesRemaining_(changesRemaining) {
    }

    [[nodiscard]] std::string_view Name() const noexcept override {
        return name_;
    }

    Optimization::PassChange Run(HirPackage &, const Optimization::PassContext &context) override {
        runs_.push_back(name_);
        contexts.push_back(context);
        if (changesRemaining_ == 0) {
            return Optimization::PassChange::None;
        }
        --changesRemaining_;
        return Optimization::PassChange::Changed;
    }

    std::vector<Optimization::PassContext> contexts;

private:
    std::string_view name_;
    std::vector<std::string_view> &runs_;
    std::size_t changesRemaining_;
};
} // namespace

TEST_CASE("optimization pipelines are selected explicitly by profile") {
    auto debug = Optimization::OptimizationPipeline::ForProfile(BuildProfile::Debug);
    auto release = Optimization::OptimizationPipeline::ForProfile(BuildProfile::Release);

    CHECK(debug.HirPassNames().empty());
    CHECK(debug.LirPassNames().empty());
    CHECK(release.HirPassNames() == std::vector<std::string_view>{"hir-constant-folder"});
    CHECK(release.LirPassNames().empty());
}

TEST_CASE("pass pipeline reports changes and preserves explicit order") {
    HirPackage package;
    std::vector<std::string_view> runs;
    Optimization::HirPassPipeline pipeline(BuildProfile::Release, 4);
    auto first = std::make_unique<RecordingHirPass>("first", runs, 1);
    auto *firstObserver = first.get();
    pipeline.Add(std::move(first));
    pipeline.Add(std::make_unique<RecordingHirPass>("second", runs, 0));

    const auto report = pipeline.Run(package);

    CHECK(report.change == Optimization::PassChange::Changed);
    CHECK(report.reachedFixedPoint);
    CHECK(report.iterations == 2);
    CHECK(runs == std::vector<std::string_view>{"first", "second", "first", "second"});
    REQUIRE(firstObserver->contexts.size() == 2);
    CHECK(firstObserver->contexts[0].iteration == 0);
    CHECK(firstObserver->contexts[1].iteration == 1);
    CHECK(firstObserver->contexts[0].fixedPointLimit == 4);
}

TEST_CASE("pass pipeline stops at its fixed-point limit") {
    HirPackage package;
    std::vector<std::string_view> runs;
    Optimization::HirPassPipeline pipeline(BuildProfile::Release, 3);
    pipeline.Add(std::make_unique<RecordingHirPass>("never-settles", runs, 10));

    const auto report = pipeline.Run(package);

    CHECK(report.change == Optimization::PassChange::Changed);
    CHECK_FALSE(report.reachedFixedPoint);
    CHECK(report.iterations == 3);
    CHECK(runs.size() == 3);
}

TEST_CASE("independent optimization pipelines do not share constant-folding analysis state") {
    HirPackage firstPackage;
    HirModule firstModule;
    HirFunc firstFunction;
    HirBlock firstBody;
    auto binding = std::make_unique<HirLetStmt>();
    binding->name = "private-value";
    binding->type = TypeRef::MakeInt32();
    auto literal = std::make_unique<HirLiteralExpr>();
    literal->type = TypeRef::MakeInt32();
    literal->value = "41";
    binding->init = std::move(literal);
    firstBody.stmts.push_back(std::move(binding));
    firstFunction.body = std::move(firstBody);
    firstModule.funcs.push_back(std::move(firstFunction));
    firstPackage.modules.push_back(std::move(firstModule));

    HirPackage secondPackage;
    HirModule secondModule;
    HirConst independentConstant;
    auto independentValue = std::make_unique<HirVarExpr>();
    independentValue->name = "private-value";
    independentValue->type = TypeRef::MakeInt32();
    independentConstant.value = std::move(independentValue);
    secondModule.consts.push_back(std::move(independentConstant));
    secondPackage.modules.push_back(std::move(secondModule));

    auto firstPipeline = Optimization::OptimizationPipeline::ForProfile(BuildProfile::Release);
    auto secondPipeline = Optimization::OptimizationPipeline::ForProfile(BuildProfile::Release);
    CHECK(firstPipeline.RunHir(firstPackage).reachedFixedPoint);
    CHECK(secondPipeline.RunHir(secondPackage).reachedFixedPoint);

    CHECK(dynamic_cast<HirVarExpr *>(secondPackage.modules[0].consts[0].value.get()) != nullptr);
}

TEST_CASE("HIR-to-LIR optimization is gated by the build profile") {
    const std::string source = R"(
        func Main() -> int {
            if true {
                return 1;
            } else {
                return 2;
            }
        }
    )";

    const auto debug = CompileToLir(source, BuildProfile::Debug);
    const auto release = CompileToLir(source, BuildProfile::Release);
    REQUIRE(debug.modules.size() == 1);
    REQUIRE(release.modules.size() == 1);
    REQUIRE(debug.modules[0].funcs.size() == 1);
    REQUIRE(release.modules[0].funcs.size() == 1);

    const auto &debugBlocks = debug.modules[0].funcs[0].blocks;
    const auto &releaseBlocks = release.modules[0].funcs[0].blocks;
    CHECK(debugBlocks.size() > releaseBlocks.size());
    CHECK(std::ranges::any_of(
        debugBlocks, [](const LirBlock &block) { return block.term && block.term->kind == LirTermKind::Branch; }));
    CHECK_FALSE(std::ranges::any_of(
        releaseBlocks, [](const LirBlock &block) { return block.term && block.term->kind == LirTermKind::Branch; }));
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

TEST_CASE("optimizer does not discard calls or potentially trapping expressions") {
    const std::string source = R"(
        func Observe() -> int {
            return 7;
        }

        func Main(value: int) -> int {
            let product = 0 * Observe();
            let remainder = Observe() % 1;
            let trap = 0 * (10 / value);
            return product + remainder + trap;
        }
    )";

    auto package = CompileAndOptimize(source);
    REQUIRE(package.modules.size() == 1);
    REQUIRE(package.modules[0].funcs.size() == 2);
    auto &body = *package.modules[0].funcs[1].body;
    REQUIRE(body.stmts.size() == 4);

    const auto *product = dynamic_cast<const HirLetStmt *>(body.stmts[0].get());
    const auto *remainder = dynamic_cast<const HirLetStmt *>(body.stmts[1].get());
    const auto *trap = dynamic_cast<const HirLetStmt *>(body.stmts[2].get());
    REQUIRE(product != nullptr);
    REQUIRE(remainder != nullptr);
    REQUIRE(trap != nullptr);

    const auto *productExpr = dynamic_cast<const HirBinaryExpr *>(product->init.get());
    const auto *remainderExpr = dynamic_cast<const HirBinaryExpr *>(remainder->init.get());
    const auto *trapExpr = dynamic_cast<const HirBinaryExpr *>(trap->init.get());
    REQUIRE(productExpr != nullptr);
    REQUIRE(remainderExpr != nullptr);
    REQUIRE(trapExpr != nullptr);
    CHECK(dynamic_cast<const HirCallExpr *>(productExpr->right.get()) != nullptr);
    CHECK(dynamic_cast<const HirCallExpr *>(remainderExpr->left.get()) != nullptr);
    CHECK(dynamic_cast<const HirBinaryExpr *>(trapExpr->right.get()) != nullptr);
}

TEST_CASE("optimizer keeps lexical shadowing isolated") {
    const std::string source = R"(
        func Main(condition: bool) -> int {
            let value = 1;
            if condition {
                let value = 2;
                if condition {
                    return value;
                }
            }
            return value;
        }
    )";

    auto package = CompileAndOptimize(source);
    auto &body = *package.modules[0].funcs[0].body;
    REQUIRE(body.stmts.size() == 3);
    const auto *returned = dynamic_cast<const HirReturnStmt *>(body.stmts[2].get());
    REQUIRE(returned != nullptr);
    REQUIRE(returned->value.has_value());
    CHECK(dynamic_cast<const HirVarExpr *>(returned->value->get()) != nullptr);
}

TEST_CASE("optimizer never substitutes an address operand") {
    const std::string source = R"(
        func Main() -> int {
            let value: int = 41;
            let pointer = @value;
            return value;
        }
    )";

    auto package = CompileAndOptimize(source);
    auto &body = *package.modules[0].funcs[0].body;
    REQUIRE(body.stmts.size() == 3);
    const auto *pointer = dynamic_cast<const HirLetStmt *>(body.stmts[1].get());
    REQUIRE(pointer != nullptr);
    const auto *address = dynamic_cast<const HirUnaryExpr *>(pointer->init.get());
    REQUIRE(address != nullptr);
    CHECK(address->op == TokenKind::At);
    CHECK(dynamic_cast<const HirVarExpr *>(address->operand.get()) != nullptr);

    const auto *returned = dynamic_cast<const HirReturnStmt *>(body.stmts[2].get());
    REQUIRE(returned != nullptr);
    REQUIRE(returned->value.has_value());
    CHECK(dynamic_cast<const HirLiteralExpr *>(returned->value->get()) != nullptr);
}

TEST_CASE("optimizer invalidates facts across loops and mutation") {
    const std::string source = R"(
        func Main(condition: bool) -> int {
            let constant = 3;
            var mutable = 1;
            while condition {
                let inside = constant;
                mutable = inside;
            }
            mutable = constant;
            return constant + mutable;
        }
    )";

    auto package = CompileAndOptimize(source);
    auto &body = *package.modules[0].funcs[0].body;
    REQUIRE(body.stmts.size() == 5);
    const auto *loop = dynamic_cast<const HirWhileStmt *>(body.stmts[2].get());
    REQUIRE(loop != nullptr);
    const auto *inside = dynamic_cast<const HirLetStmt *>(loop->body.stmts[0].get());
    REQUIRE(inside != nullptr);
    CHECK(dynamic_cast<const HirVarExpr *>(inside->init.get()) != nullptr);

    const auto *returned = dynamic_cast<const HirReturnStmt *>(body.stmts[4].get());
    REQUIRE(returned != nullptr);
    REQUIRE(returned->value.has_value());
    const auto *sum = dynamic_cast<const HirBinaryExpr *>(returned->value->get());
    REQUIRE(sum != nullptr);
    CHECK(dynamic_cast<const HirVarExpr *>(sum->left.get()) != nullptr);
    CHECK(dynamic_cast<const HirVarExpr *>(sum->right.get()) != nullptr);
}
