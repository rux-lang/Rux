// Folding `#if` chains between declarations, rewriting each module in place.

#include "Semantic/Conditional/ConditionalFolding.h"

#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace Rux::ConditionalFolding {
namespace {
class DeclarationFolder {
public:
    DeclarationFolder(ConditionalEvaluator &inputEvaluator, std::vector<Diagnostic> &inputDiags)
        : evaluator(inputEvaluator)
        , diags(inputDiags) {
    }

    void Run(const std::vector<Module *> &modules) {
        for (auto *module : modules) {
            currentFile = module->name;
            currentModulePath = FilePathToModulePath(module->name);
            SyncEvaluatorContext();
            evaluator.SetImports(*module);
            ResolveDecls(module->items);
            ResolveLinkConstants(module->items);
        }
    }

private:
    ConditionalEvaluator &evaluator;
    std::vector<Diagnostic> &diags;
    std::string currentFile;
    std::string currentModulePath;

    void SyncEvaluatorContext() {
        evaluator.SetSourceContext(currentFile, currentModulePath, {});
    }

    void AppendDiagnostics(std::vector<Diagnostic> diagnostics) {
        diags.insert(diags.end(), std::make_move_iterator(diagnostics.begin()),
                     std::make_move_iterator(diagnostics.end()));
    }

    void EmitError(const SourceLocation location, std::string message) {
        diags.push_back({Diagnostic::Severity::Error, currentFile, location, std::move(message), {}, {}, {}});
    }

    void EmitWarning(const SourceLocation location, std::string message) {
        diags.push_back({Diagnostic::Severity::Warning, currentFile, location, std::move(message), {}, {}, {}});
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
                    currentModulePath =
                        currentModulePath.empty() ? module->name : currentModulePath + "::" + module->name;
                    SyncEvaluatorContext();
                    ResolveDecls(module->items);
                    currentModulePath = savedModule;
                    SyncEvaluatorContext();
                }
                else if (auto *impl = dynamic_cast<ImplDecl *>(decl.get())) {
                    ResolveImplConditionals(*impl);
                }
                resolved.push_back(std::move(decl));
                continue;
            }

            for (auto &branch : when->branches) {
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

    void ResolveImplConditionals(ImplDecl &impl) {
        for (auto &conditional : impl.conditionals) {
            if (!conditional) {
                continue;
            }
            for (auto &branch : conditional->branches) {
                if (branch.condition && !EvalCondition(branch.condition.get(), conditional->location)) {
                    continue;
                }
                ResolveDecls(branch.items);
                for (auto &item : branch.items) {
                    if (!item) {
                        continue;
                    }
                    if (auto *constant = dynamic_cast<ConstDecl *>(item.get())) {
                        item.release();
                        impl.constants.emplace_back(constant);
                        continue;
                    }
                    auto *method = dynamic_cast<FuncDecl *>(item.get());
                    if (!method) {
                        EmitError(item->location,
                                  "only methods and constants can be declared inside an 'extend' block");
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
};
} // namespace

void FoldDeclarations(const std::vector<Module *> &modules, ConditionalEvaluator &evaluator,
                      std::vector<Diagnostic> &diags) {
    DeclarationFolder(evaluator, diags).Run(modules);
}
} // namespace Rux::ConditionalFolding
