#include "Semantic/ConditionalCompilation.h"

#include "Target/Target.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace Rux {
namespace {
std::string FilePathToModulePath(const std::string &filePath) {
    std::filesystem::path path(filePath);
    std::vector<std::string> parts;
    for (const auto &part : path) {
        parts.push_back(part.generic_string());
    }
    std::size_t start = parts.size() > 1 ? parts.size() - 1 : 0;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (parts[i] == "Src" || parts[i] == "src") {
            start = i + 1;
        }
    }
    if (!parts.empty()) {
        parts.back() = std::filesystem::path(parts.back()).stem().generic_string();
    }
    std::string result;
    for (std::size_t i = start; i < parts.size(); ++i) {
        if (!result.empty()) {
            result += "::";
        }
        result += parts[i];
    }
    return result;
}

bool EqualsIgnoringCase(const std::string_view left, const std::string_view right) {
    return std::ranges::equal(left, right, [](const char x, const char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

std::optional<Target::OS> ParseTargetSystem(const std::string_view name) {
    if (name.empty()) {
        return Target::HostOS;
    }
    if (EqualsIgnoringCase(name, "FreeBSD")) {
        return Target::OS::FreeBSD;
    }
    if (EqualsIgnoringCase(name, "Linux")) {
        return Target::OS::Linux;
    }
    if (EqualsIgnoringCase(name, "MacOS") || EqualsIgnoringCase(name, "osx") || EqualsIgnoringCase(name, "darwin")) {
        return Target::OS::MacOS;
    }
    if (EqualsIgnoringCase(name, "Windows")) {
        return Target::OS::Windows;
    }
    return std::nullopt;
}

class ConditionalFolder {
public:
    ConditionalFolder(const CompileTimeContext &context, const std::vector<Module *> &modules,
                      std::vector<Diagnostic> &inputDiags)
        : evaluator(context, modules)
        , diags(inputDiags) {
    }

    void Run(const std::vector<Module *> &modules) {
        // Declarations first: a `when` branch can define constants that a
        // condition inside a function body then tests.
        for (auto *module : modules) {
            currentFile = module->name;
            currentModulePath = FilePathToModulePath(module->name);
            currentDeclModulePath.clear();
            currentFunction.clear();
            SyncEvaluatorContext();
            evaluator.SetImports(*module);
            ResolveDecls(module->items);
            ResolveLinkConstants(module->items);
        }
        for (auto *module : modules) {
            currentFile = module->name;
            currentModulePath = FilePathToModulePath(module->name);
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
    ConditionalEvaluator evaluator;
    std::vector<Diagnostic> &diags;
    std::string currentFile;
    std::string currentFunction;
    std::string currentModulePath;
    std::string currentDeclModulePath;

    void SyncEvaluatorContext() {
        evaluator.SetSourceContext(currentFile, currentModulePath, currentFunction);
    }

    void AppendDiagnostics(std::vector<Diagnostic> diagnostics) {
        diags.insert(diags.end(), std::make_move_iterator(diagnostics.begin()),
                     std::make_move_iterator(diagnostics.end()));
    }

    void EmitError(const SourceLocation location, std::string message) {
        diags.push_back({Diagnostic::Severity::Error, currentFile, location, std::move(message)});
    }

    void EmitWarning(const SourceLocation location, std::string message) {
        diags.push_back({Diagnostic::Severity::Warning, currentFile, location, std::move(message)});
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

    // Borrows each branch/arm's pattern expressions into the shape SelectMatchArm
    // expects (an empty inner list marks the `else` arm).
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

    void ResolveLinkConstant(std::string &constantName, std::string &result, const SourceLocation location,
                             const std::string_view argumentName) {
        if (constantName.empty()) {
            return;
        }

        auto evaluation = evaluator.EvaluateConstant(constantName);
        AppendDiagnostics(std::move(evaluation.diagnostics));
        if (!evaluation.value) {
            EmitError(location,
                      std::format("'#Link' {} '{}' is not a compile-time constant", argumentName, constantName));
            return;
        }
        if (const auto *text = std::get_if<std::string>(&*evaluation.value)) {
            result = *text;
            constantName.clear();
            return;
        }
        EmitError(location, std::format("'#Link' {} '{}' must be a string", argumentName, constantName));
    }

    void ResolveLinkConstants(std::vector<DeclPtr> &decls) {
        for (auto &decl : decls) {
            if (!decl) {
                continue;
            }
            if (auto *function = dynamic_cast<ExternFuncDecl *>(decl.get())) {
                ResolveLinkConstant(function->dllConst, function->dll, function->location, "library name");
                ResolveLinkConstant(function->symbolNameConst, function->symbolName, function->location, "symbol name");
            }
            else if (auto *block = dynamic_cast<ExternBlockDecl *>(decl.get())) {
                ResolveLinkConstant(block->dllConst, block->dll, block->location, "library name");
                ResolveLinkConstants(block->items);
            }
            else if (auto *module = dynamic_cast<ModuleDecl *>(decl.get())) {
                ResolveLinkConstants(module->items);
            }
        }
    }

    // Declaration-level `when`

    void ResolveDecls(std::vector<DeclPtr> &decls) {
        std::vector<DeclPtr> resolved;
        resolved.reserve(decls.size());

        for (auto &decl : decls) {
            if (!decl) {
                continue;
            }
            auto *when = dynamic_cast<WhenDecl *>(decl.get());
            if (when && when->matchSubject) {
                const int idx = SelectMatchArm(*when->matchSubject, BorrowArmPatterns(when->branches), when->location);
                if (idx >= 0) {
                    auto &branch = when->branches[static_cast<std::size_t>(idx)];
                    if (branch.directive == WhenDecl::Directive::Error) {
                        EmitError(branch.directiveLocation, branch.directiveMessage);
                    }
                    else if (branch.directive == WhenDecl::Directive::Warn) {
                        EmitWarning(branch.directiveLocation, branch.directiveMessage);
                    }
                    else {
                        ResolveDecls(branch.items);
                        evaluator.RegisterDeclarations(branch.items);
                        for (auto &item : branch.items) {
                            resolved.push_back(std::move(item));
                        }
                    }
                }
                continue;
            }
            if (!when) {
                if (auto *module = dynamic_cast<ModuleDecl *>(decl.get())) {
                    const std::string savedModule = currentModulePath;
                    const std::string savedDeclModule = currentDeclModulePath;
                    currentModulePath =
                        currentModulePath.empty() ? module->name : currentModulePath + "::" + module->name;
                    currentDeclModulePath =
                        currentDeclModulePath.empty() ? module->name : currentDeclModulePath + "::" + module->name;
                    SyncEvaluatorContext();
                    ResolveDecls(module->items);
                    currentModulePath = savedModule;
                    currentDeclModulePath = savedDeclModule;
                    SyncEvaluatorContext();
                }
                else if (auto *impl = dynamic_cast<ImplDecl *>(decl.get())) {
                    ResolveImplConditionals(*impl);
                }
                resolved.push_back(std::move(decl));
                continue;
            }

            for (auto &branch : when->branches) {
                // A branch with no condition is the trailing `else`.
                if (branch.condition && !EvalCondition(branch.condition.get(), when->location)) {
                    continue;
                }
                ResolveDecls(branch.items);
                evaluator.RegisterDeclarations(branch.items);
                for (auto &item : branch.items) {
                    resolved.push_back(std::move(item));
                }
                break;
            }
        }

        decls = std::move(resolved);
    }

    // An `extend` body holds methods, not a declaration list, so the `when`
    // chains written between them are folded separately: the methods of the
    // taken branch join the ones written unconditionally.
    void ResolveImplConditionals(ImplDecl &impl) {
        for (auto &conditional : impl.conditionals) {
            if (!conditional) {
                continue;
            }
            for (auto &branch : conditional->branches) {
                if (branch.condition && !EvalCondition(branch.condition.get(), conditional->location)) {
                    continue;
                }
                ResolveDecls(branch.items); // a nested `when` resolves first
                for (auto &item : branch.items) {
                    if (!item) {
                        continue;
                    }
                    auto *method = dynamic_cast<FuncDecl *>(item.get());
                    if (!method) {
                        EmitError(item->location, "only methods can be declared inside an 'extend' block");
                        continue;
                    }
                    item.release();
                    impl.methods.emplace_back(method);
                }
                break;
            }
        }
        impl.conditionals.clear();
    }

    // Statement-level `when`

    void ResolveDeclBodies(Decl &decl) {
        if (auto *func = dynamic_cast<FuncDecl *>(&decl)) {
            const std::string savedFunction = currentFunction;
            currentFunction = currentDeclModulePath.empty() ? func->name : currentDeclModulePath + "::" + func->name;
            SyncEvaluatorContext();
            if (func->body) {
                ResolveBlock(*func->body);
            }
            currentFunction = savedFunction;
            SyncEvaluatorContext();
        }
        else if (auto *impl = dynamic_cast<ImplDecl *>(&decl)) {
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
        else if (auto *module = dynamic_cast<ModuleDecl *>(&decl)) {
            const std::string savedModule = currentModulePath;
            const std::string savedDeclModule = currentDeclModulePath;
            currentModulePath = currentModulePath.empty() ? module->name : currentModulePath + "::" + module->name;
            currentDeclModulePath =
                currentDeclModulePath.empty() ? module->name : currentDeclModulePath + "::" + module->name;
            SyncEvaluatorContext();
            for (const auto &item : module->items) {
                if (item) {
                    ResolveDeclBodies(*item);
                }
            }
            currentModulePath = savedModule;
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

    // Appends `stmt` to `out`, or — for a `when` — the statements of its taken
    // branch, which are spliced into the enclosing block rather than nested in
    // one, so a `when` introduces no scope of its own.
    void ResolveStmt(StmtPtr stmt, std::vector<StmtPtr> &out) {
        auto *ifStmt = dynamic_cast<IfStmt *>(stmt.get());
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
        if (auto *ifStmt = dynamic_cast<IfStmt *>(&stmt)) {
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
        else if (auto *whileStmt = dynamic_cast<WhileStmt *>(&stmt)) {
            ResolveExpr(whileStmt->condition.get());
            if (whileStmt->body) {
                ResolveBlock(*whileStmt->body);
            }
        }
        else if (auto *doWhileStmt = dynamic_cast<DoWhileStmt *>(&stmt)) {
            if (doWhileStmt->body) {
                ResolveBlock(*doWhileStmt->body);
            }
            ResolveExpr(doWhileStmt->condition.get());
        }
        else if (auto *loopStmt = dynamic_cast<LoopStmt *>(&stmt)) {
            if (loopStmt->body) {
                ResolveBlock(*loopStmt->body);
            }
        }
        else if (auto *forStmt = dynamic_cast<ForStmt *>(&stmt)) {
            ResolveExpr(forStmt->iterable.get());
            if (forStmt->body) {
                ResolveBlock(*forStmt->body);
            }
        }
        else if (auto *matchStmt = dynamic_cast<MatchStmt *>(&stmt)) {
            ResolveExpr(matchStmt->subject.get());
            for (auto &arm : matchStmt->arms) {
                ResolveExpr(arm.body.get());
            }
        }
        else if (auto *letStmt = dynamic_cast<LetStmt *>(&stmt)) {
            ResolveExpr(letStmt->init.get());
        }
        else if (auto *returnStmt = dynamic_cast<ReturnStmt *>(&stmt)) {
            if (returnStmt->value) {
                ResolveExpr(returnStmt->value->get());
            }
        }
        else if (auto *exprStmt = dynamic_cast<ExprStmt *>(&stmt)) {
            ResolveExpr(exprStmt->expr.get());
        }
        else if (auto *declStmt = dynamic_cast<DeclStmt *>(&stmt)) {
            if (declStmt->decl) {
                if (const auto *constDecl = dynamic_cast<const ConstDecl *>(declStmt->decl.get())) {
                    evaluator.RegisterConstant(*constDecl);
                }
                ResolveDeclBodies(*declStmt->decl);
            }
        }
    }

    // Blocks can hide inside expressions (a match arm body), so expressions are
    // walked too.
    void ResolveExpr(Expr *expr) {
        if (!expr) {
            return;
        }
        if (auto *blockExpr = dynamic_cast<BlockExpr *>(expr)) {
            if (blockExpr->block) {
                ResolveBlock(*blockExpr->block);
            }
        }
        else if (auto *matchExpr = dynamic_cast<MatchExpr *>(expr)) {
            ResolveExpr(matchExpr->subject.get());
            for (auto &arm : matchExpr->arms) {
                ResolveExpr(arm.body.get());
            }
        }
        else if (auto *binary = dynamic_cast<BinaryExpr *>(expr)) {
            ResolveExpr(binary->left.get());
            ResolveExpr(binary->right.get());
        }
        else if (auto *assign = dynamic_cast<AssignExpr *>(expr)) {
            ResolveExpr(assign->target.get());
            ResolveExpr(assign->value.get());
        }
        else if (auto *unary = dynamic_cast<UnaryExpr *>(expr)) {
            ResolveExpr(unary->operand.get());
        }
        else if (auto *ternary = dynamic_cast<TernaryExpr *>(expr)) {
            ResolveExpr(ternary->condition.get());
            ResolveExpr(ternary->thenExpr.get());
            ResolveExpr(ternary->elseExpr.get());
        }
        else if (auto *call = dynamic_cast<CallExpr *>(expr)) {
            ResolveExpr(call->callee.get());
            for (auto &arg : call->args) {
                ResolveExpr(arg.get());
            }
        }
        else if (auto *index = dynamic_cast<IndexExpr *>(expr)) {
            ResolveExpr(index->object.get());
            ResolveExpr(index->index.get());
        }
        else if (auto *field = dynamic_cast<FieldExpr *>(expr)) {
            ResolveExpr(field->object.get());
        }
        else if (auto *cast = dynamic_cast<CastExpr *>(expr)) {
            ResolveExpr(cast->operand.get());
        }
        else if (auto *intrinsic = dynamic_cast<IntrinsicExpr *>(expr)) {
            for (auto &arg : intrinsic->args) {
                ResolveExpr(arg.get());
            }
        }
    }
};
} // namespace

void ResolveConditionalCompilation(const std::vector<Module *> &modules, const CompileTimeContext &context,
                                   std::vector<Diagnostic> &diags) {
    ConditionalFolder folder(context, modules, diags);
    folder.Run(modules);
}

void ResolveConditionalCompilation(const std::vector<Module *> &modules, const std::string_view targetSystem,
                                   std::vector<Diagnostic> &diags) {
    CompileTimeContext context;
    if (const auto os = ParseTargetSystem(targetSystem)) {
        context.target.os = *os;
        context.target.object_format = Target::GetObjectFormat(*os);
    }
    ConditionalFolder folder(context, modules, diags);
    folder.Run(modules);
}
} // namespace Rux
