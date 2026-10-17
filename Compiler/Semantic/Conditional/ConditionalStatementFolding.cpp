// Folding `#if` chains inside function bodies, the statement-level counterpart
// to declaration folding.

#include "Semantic/Conditional/ConditionalFolding.h"

#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace Rux::ConditionalFolding {
namespace {
class StatementFolder {
public:
    StatementFolder(ConditionalEvaluator &inputEvaluator, std::vector<Diagnostic> &inputDiags)
        : evaluator(inputEvaluator)
        , diags(inputDiags) {
    }

    void Run(const std::vector<Module *> &modules) {
        for (auto *module : modules) {
            // A module with no `when` statement and no nested declaration in any block has nothing to fold or register
            // there, and walking every expression of every body is most of what folding costs.
            if (!module->bodiesNeedConditionalFolding)
                continue;
            currentFile = module->name;
            currentDeclModulePath.clear();
            currentFunction.clear();
            SyncEvaluatorContext();
            evaluator.SetImports(*module);
            for (const auto &decl : module->items) {
                ResolveDeclBodies(*decl);
            }
        }
    }

private:
    ConditionalEvaluator &evaluator;
    std::vector<Diagnostic> &diags;
    std::string currentFile;
    std::string currentFunction;
    std::string currentDeclModulePath;

    void SyncEvaluatorContext() {
        evaluator.SetSourceContext(currentFile, currentDeclModulePath, currentFunction);
    }

    void AppendDiagnostics(std::vector<Diagnostic> diagnostics) {
        diags.insert(diags.end(), std::make_move_iterator(diagnostics.begin()),
                     std::make_move_iterator(diagnostics.end()));
    }

    bool EvalCondition(const Expr *condition, const SourceLocation location) {
        auto result = evaluator.EvaluateCondition(condition, location);
        AppendDiagnostics(std::move(result.diagnostics));
        return result.value;
    }

    int SelectMatchArm(const Expr &subject, const std::vector<std::vector<const Expr *>> &arms,
                       const SourceLocation location) {
        auto result = evaluator.SelectMatchArm(subject, arms, location);
        AppendDiagnostics(std::move(result.diagnostics));
        return result.arm;
    }

    template <typename Arms>
    static std::vector<std::vector<const Expr *>> BorrowArmPatterns(const Arms &arms) {
        std::vector<std::vector<const Expr *>> out;
        out.reserve(arms.size());
        for (const auto &arm : arms) {
            std::vector<const Expr *> patterns;
            patterns.reserve(arm.patterns.size());
            for (const auto &pattern : arm.patterns) {
                patterns.push_back(pattern.get());
            }
            out.push_back(std::move(patterns));
        }
        return out;
    }

    void ResolveDeclBodies(Decl &decl) {
        if (auto *func = AstCast<FuncDecl>(&decl)) {
            const std::string savedFunction = currentFunction;
            currentFunction = currentDeclModulePath.empty() ? func->name : currentDeclModulePath + "::" + func->name;
            SyncEvaluatorContext();
            if (func->body) {
                ResolveBlock(*func->body);
            }
            currentFunction = savedFunction;
            SyncEvaluatorContext();
        }
        else if (auto *impl = AstCast<ImplDecl>(&decl)) {
            for (const auto &method : impl->methods) {
                if (method && method->body) {
                    const std::string savedFunction = currentFunction;
                    currentFunction = impl->typeName + "::" + method->name;
                    SyncEvaluatorContext();
                    ResolveBlock(*method->body);
                    currentFunction = savedFunction;
                    SyncEvaluatorContext();
                }
            }
        }
        else if (auto *module = AstCast<ModuleDecl>(&decl)) {
            const std::string savedDeclModule = currentDeclModulePath;
            currentDeclModulePath =
                currentDeclModulePath.empty() ? module->name : currentDeclModulePath + "::" + module->name;
            SyncEvaluatorContext();
            for (const auto &item : module->items) {
                if (item) {
                    ResolveDeclBodies(*item);
                }
            }
            currentDeclModulePath = savedDeclModule;
            SyncEvaluatorContext();
        }
    }

    void ResolveBlock(Block &block) {
        std::vector<StmtPtr> resolved;
        resolved.reserve(block.stmts.size());
        for (auto &stmt : block.stmts) {
            if (stmt) {
                ResolveStmt(std::move(stmt), resolved);
            }
        }
        block.stmts = std::move(resolved);
    }

    void ResolveStmt(StmtPtr stmt, std::vector<StmtPtr> &out) {
        auto *ifStmt = AstCast<IfStmt>(stmt.get());
        if (ifStmt && ifStmt->isCompileTime) {
            Block *taken = nullptr;
            if (ifStmt->matchSubject) {
                const int idx =
                    SelectMatchArm(*ifStmt->matchSubject, BorrowArmPatterns(ifStmt->matchArms), ifStmt->location);
                if (idx >= 0) {
                    taken = ifStmt->matchArms[static_cast<std::size_t>(idx)].block.get();
                }
            }
            else if (EvalCondition(ifStmt->condition.get(), ifStmt->location)) {
                taken = ifStmt->thenBlock.get();
            }
            else {
                for (auto &elseIf : ifStmt->elseIfs) {
                    if (EvalCondition(elseIf.condition.get(), elseIf.location)) {
                        taken = elseIf.block.get();
                        break;
                    }
                }
                if (!taken) {
                    taken = ifStmt->elseBlock.get();
                }
            }
            if (taken) {
                for (auto &inner : taken->stmts) {
                    if (inner) {
                        ResolveStmt(std::move(inner), out);
                    }
                }
            }
            return;
        }

        ResolveNestedBlocks(*stmt);
        out.push_back(std::move(stmt));
    }

    void ResolveNestedBlocks(Stmt &stmt) {
        if (auto *ifStmt = AstCast<IfStmt>(&stmt)) {
            ResolveExpr(ifStmt->condition.get());
            if (ifStmt->thenBlock) {
                ResolveBlock(*ifStmt->thenBlock);
            }
            for (auto &elseIf : ifStmt->elseIfs) {
                ResolveExpr(elseIf.condition.get());
                if (elseIf.block) {
                    ResolveBlock(*elseIf.block);
                }
            }
            if (ifStmt->elseBlock) {
                ResolveBlock(*ifStmt->elseBlock);
            }
        }
        else if (auto *whileStmt = AstCast<WhileStmt>(&stmt)) {
            ResolveExpr(whileStmt->condition.get());
            if (whileStmt->body) {
                ResolveBlock(*whileStmt->body);
            }
        }
        else if (auto *doWhileStmt = AstCast<DoWhileStmt>(&stmt)) {
            if (doWhileStmt->body) {
                ResolveBlock(*doWhileStmt->body);
            }
            ResolveExpr(doWhileStmt->condition.get());
        }
        else if (auto *loopStmt = AstCast<LoopStmt>(&stmt)) {
            if (loopStmt->body) {
                ResolveBlock(*loopStmt->body);
            }
        }
        else if (auto *forStmt = AstCast<ForStmt>(&stmt)) {
            ResolveExpr(forStmt->iterable.get());
            if (forStmt->body) {
                ResolveBlock(*forStmt->body);
            }
        }
        else if (auto *matchStmt = AstCast<MatchStmt>(&stmt)) {
            ResolveExpr(matchStmt->subject.get());
            for (auto &arm : matchStmt->arms) {
                ResolveExpr(arm.body.get());
            }
        }
        else if (auto *letStmt = AstCast<LetStmt>(&stmt)) {
            ResolveExpr(letStmt->init.get());
        }
        else if (auto *returnStmt = AstCast<ReturnStmt>(&stmt)) {
            if (returnStmt->value) {
                ResolveExpr(returnStmt->value->get());
            }
        }
        else if (auto *exprStmt = AstCast<ExprStmt>(&stmt)) {
            ResolveExpr(exprStmt->expr.get());
        }
        else if (auto *declStmt = AstCast<DeclStmt>(&stmt)) {
            if (declStmt->decl) {
                if (const auto *constDecl = AstCast<const ConstDecl>(declStmt->decl.get())) {
                    evaluator.RegisterConstant(*constDecl);
                }
                ResolveDeclBodies(*declStmt->decl);
            }
        }
    }

    void ResolveExpr(Expr *expr) {
        if (!expr) {
            return;
        }
        if (auto *blockExpr = AstCast<BlockExpr>(expr)) {
            if (blockExpr->block) {
                ResolveBlock(*blockExpr->block);
            }
        }
        else if (auto *matchExpr = AstCast<MatchExpr>(expr)) {
            ResolveExpr(matchExpr->subject.get());
            for (auto &arm : matchExpr->arms) {
                ResolveExpr(arm.body.get());
            }
        }
        else if (auto *binary = AstCast<BinaryExpr>(expr)) {
            ResolveExpr(binary->left.get());
            ResolveExpr(binary->right.get());
        }
        else if (auto *assign = AstCast<AssignExpr>(expr)) {
            ResolveExpr(assign->target.get());
            ResolveExpr(assign->value.get());
        }
        else if (auto *unary = AstCast<UnaryExpr>(expr)) {
            ResolveExpr(unary->operand.get());
        }
        else if (auto *ternary = AstCast<TernaryExpr>(expr)) {
            ResolveExpr(ternary->condition.get());
            ResolveExpr(ternary->thenExpr.get());
            ResolveExpr(ternary->elseExpr.get());
        }
        else if (auto *call = AstCast<CallExpr>(expr)) {
            ResolveExpr(call->callee.get());
            for (auto &arg : call->args) {
                ResolveExpr(arg.get());
            }
        }
        else if (auto *index = AstCast<IndexExpr>(expr)) {
            ResolveExpr(index->object.get());
            ResolveExpr(index->index.get());
        }
        else if (auto *field = AstCast<FieldExpr>(expr)) {
            ResolveExpr(field->object.get());
        }
        else if (auto *cast = AstCast<CastExpr>(expr)) {
            ResolveExpr(cast->operand.get());
        }
        else if (auto *intrinsic = AstCast<IntrinsicExpr>(expr)) {
            for (auto &arg : intrinsic->args) {
                ResolveExpr(arg.get());
            }
        }
    }
};
} // namespace

void FoldStatements(const std::vector<Module *> &modules, ConditionalEvaluator &evaluator,
                    std::vector<Diagnostic> &diags) {
    StatementFolder(evaluator, diags).Run(modules);
}
} // namespace Rux::ConditionalFolding
