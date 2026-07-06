#include "Linter/Linter.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <iterator>
#include <string_view>

#include "Lexer/Lexer.h"
#include "Syntax/Parser/Parser.h"

namespace Rux::Linting {

namespace {

bool IsPascalCase(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    if (!std::isupper(static_cast<unsigned char>(name[0]))) {
        return false;
    }
    for (char c : name) {
        if (c == '_') {
            return false;
        }
    }
    return true;
}

bool IsCamelCase(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    if (!std::islower(static_cast<unsigned char>(name[0]))) {
        return false;
    }
    for (char c : name) {
        if (c == '_') {
            return false;
        }
    }
    return true;
}

class LinterVisitor {
public:
    explicit LinterVisitor(std::string sourceName)
        : sourceName(std::move(sourceName)) {
    }

    std::vector<Diagnostic> diagnostics;

    void VisitModule(const Module &module) {
        for (const auto &item : module.items) {
            if (item) {
                VisitDecl(*item);
            }
        }
    }

private:
    std::string sourceName;

    void Warn(SourceLocation loc, std::string message) {
        Diagnostic d;
        d.severity = Diagnostic::Severity::Warning;
        d.sourceName = sourceName;
        d.location = loc;
        d.message = std::move(message);
        diagnostics.push_back(std::move(d));
    }

    void VisitDecl(const Decl &decl) {
        if (const auto *fn = dynamic_cast<const FuncDecl *>(&decl)) {
            if (!IsPascalCase(fn->name)) {
                Warn(fn->location, std::format("function name '{}' should be PascalCase", fn->name));
            }
            for (const auto &p : fn->params) {
                if (!IsCamelCase(p.name)) {
                    Warn(p.location, std::format("parameter name '{}' should be camelCase", p.name));
                }
            }
            if (fn->body) {
                VisitBlock(*fn->body);
            }
        }
        else if (const auto *st = dynamic_cast<const StructDecl *>(&decl)) {
            if (!IsPascalCase(st->name)) {
                Warn(st->location, std::format("struct name '{}' should be PascalCase", st->name));
            }
            for (const auto &f : st->fields) {
                if (!IsCamelCase(f.name)) {
                    Warn(f.location, std::format("struct field name '{}' should be camelCase", f.name));
                }
            }
        }
        else if (const auto *en = dynamic_cast<const EnumDecl *>(&decl)) {
            if (!IsPascalCase(en->name)) {
                Warn(en->location, std::format("enum name '{}' should be PascalCase", en->name));
            }
            for (const auto &v : en->variants) {
                if (!IsPascalCase(v.name)) {
                    Warn(v.location, std::format("enum variant name '{}' should be PascalCase", v.name));
                }
                for (const auto &nf : v.namedFields) {
                    if (!IsCamelCase(nf.name)) {
                        Warn(nf.location, std::format("enum variant field name '{}' should be camelCase", nf.name));
                    }
                }
            }
        }
        else if (const auto *un = dynamic_cast<const UnionDecl *>(&decl)) {
            if (!IsPascalCase(un->name)) {
                Warn(un->location, std::format("union name '{}' should be PascalCase", un->name));
            }
            for (const auto &f : un->fields) {
                if (!IsCamelCase(f.name)) {
                    Warn(f.location, std::format("union field name '{}' should be camelCase", f.name));
                }
            }
        }
        else if (const auto *iface = dynamic_cast<const InterfaceDecl *>(&decl)) {
            if (!IsPascalCase(iface->name)) {
                Warn(iface->location, std::format("interface name '{}' should be PascalCase", iface->name));
            }
            for (const auto &m : iface->methods) {
                if (m) {
                    VisitDecl(*m);
                }
            }
        }
        else if (const auto *impl = dynamic_cast<const ImplDecl *>(&decl)) {
            for (const auto &m : impl->methods) {
                if (m) {
                    VisitDecl(*m);
                }
            }
        }
        else if (const auto *mod = dynamic_cast<const ModuleDecl *>(&decl)) {
            if (!IsPascalCase(mod->name)) {
                Warn(mod->location, std::format("module name '{}' should be PascalCase", mod->name));
            }
            for (const auto &item : mod->items) {
                if (item) {
                    VisitDecl(*item);
                }
            }
        }
        else if (const auto *cnst = dynamic_cast<const ConstDecl *>(&decl)) {
            if (!IsPascalCase(cnst->name)) {
                Warn(cnst->location, std::format("constant name '{}' should be PascalCase", cnst->name));
            }
            if (cnst->value) {
                VisitExpr(*cnst->value);
            }
        }
        else if (const auto *alias = dynamic_cast<const TypeAliasDecl *>(&decl)) {
            if (!IsPascalCase(alias->name)) {
                Warn(alias->location, std::format("type alias name '{}' should be PascalCase", alias->name));
            }
        }
        else if (const auto *extBlock = dynamic_cast<const ExternBlockDecl *>(&decl)) {
            for (const auto &item : extBlock->items) {
                if (item) {
                    VisitDecl(*item);
                }
            }
        }
    }

    void VisitBlock(const Block &block) {
        for (const auto &stmt : block.stmts) {
            if (stmt) {
                VisitStmt(*stmt);
            }
        }
    }

    void VisitStmt(const Stmt &stmt) {
        if (const auto *exprStmt = dynamic_cast<const ExprStmt *>(&stmt)) {
            if (exprStmt->expr) {
                VisitExpr(*exprStmt->expr);
            }
        }
        else if (const auto *letStmt = dynamic_cast<const LetStmt *>(&stmt)) {
            if (!letStmt->name.empty() && !IsCamelCase(letStmt->name)) {
                Warn(letStmt->location, std::format("variable name '{}' should be camelCase", letStmt->name));
            }
            if (letStmt->pattern) {
                VisitPattern(*letStmt->pattern);
            }
            if (letStmt->init) {
                VisitExpr(*letStmt->init);
            }
        }
        else if (const auto *ifStmt = dynamic_cast<const IfStmt *>(&stmt)) {
            if (ifStmt->condition) {
                VisitExpr(*ifStmt->condition);
            }
            if (ifStmt->thenBlock) {
                VisitBlock(*ifStmt->thenBlock);
            }
            for (const auto &elif : ifStmt->elseIfs) {
                if (elif.condition) {
                    VisitExpr(*elif.condition);
                }
                if (elif.block) {
                    VisitBlock(*elif.block);
                }
            }
            if (ifStmt->elseBlock) {
                VisitBlock(*ifStmt->elseBlock);
            }
        }
        else if (const auto *whileStmt = dynamic_cast<const WhileStmt *>(&stmt)) {
            if (whileStmt->condition) {
                VisitExpr(*whileStmt->condition);
            }
            if (whileStmt->body) {
                VisitBlock(*whileStmt->body);
            }
        }
        else if (const auto *doWhileStmt = dynamic_cast<const DoWhileStmt *>(&stmt)) {
            if (doWhileStmt->body) {
                VisitBlock(*doWhileStmt->body);
            }
            if (doWhileStmt->condition) {
                VisitExpr(*doWhileStmt->condition);
            }
        }
        else if (const auto *loopStmt = dynamic_cast<const LoopStmt *>(&stmt)) {
            if (loopStmt->body) {
                VisitBlock(*loopStmt->body);
            }
        }
        else if (const auto *forStmt = dynamic_cast<const ForStmt *>(&stmt)) {
            if (!forStmt->variable.empty() && !IsCamelCase(forStmt->variable)) {
                Warn(forStmt->location, std::format("loop variable name '{}' should be camelCase", forStmt->variable));
            }
            if (forStmt->iterable) {
                VisitExpr(*forStmt->iterable);
            }
            if (forStmt->body) {
                VisitBlock(*forStmt->body);
            }
        }
        else if (const auto *matchStmt = dynamic_cast<const MatchStmt *>(&stmt)) {
            if (matchStmt->subject) {
                VisitExpr(*matchStmt->subject);
            }
            for (const auto &arm : matchStmt->arms) {
                if (arm.pattern) {
                    VisitPattern(*arm.pattern);
                }
                if (arm.body) {
                    VisitExpr(*arm.body);
                }
            }
        }
        else if (const auto *retStmt = dynamic_cast<const ReturnStmt *>(&stmt)) {
            if (retStmt->value && *retStmt->value) {
                VisitExpr(**retStmt->value);
            }
        }
        else if (const auto *declStmt = dynamic_cast<const DeclStmt *>(&stmt)) {
            if (declStmt->decl) {
                VisitDecl(*declStmt->decl);
            }
        }
    }

    void VisitPattern(const Pattern &pattern) {
        if (const auto *idPat = dynamic_cast<const IdentPattern *>(&pattern)) {
            if (!idPat->name.empty() && !IsCamelCase(idPat->name)) {
                Warn(idPat->location, std::format("variable binding '{}' should be camelCase", idPat->name));
            }
        }
        else if (const auto *rangePat = dynamic_cast<const RangePattern *>(&pattern)) {
            if (rangePat->lo) {
                VisitPattern(*rangePat->lo);
            }
            if (rangePat->hi) {
                VisitPattern(*rangePat->hi);
            }
        }
        else if (const auto *enumPat = dynamic_cast<const EnumPattern *>(&pattern)) {
            for (const auto &arg : enumPat->args) {
                if (arg) {
                    VisitPattern(*arg);
                }
            }
            for (const auto &narg : enumPat->namedArgs) {
                if (narg.pattern) {
                    VisitPattern(*narg.pattern);
                }
            }
        }
        else if (const auto *structPat = dynamic_cast<const StructPattern *>(&pattern)) {
            for (const auto &f : structPat->fields) {
                if (f.pattern) {
                    VisitPattern(*f.pattern);
                }
            }
        }
        else if (const auto *tuplePat = dynamic_cast<const TuplePattern *>(&pattern)) {
            for (const auto &el : tuplePat->elements) {
                if (el) {
                    VisitPattern(*el);
                }
            }
        }
        else if (const auto *guardedPat = dynamic_cast<const GuardedPattern *>(&pattern)) {
            if (guardedPat->inner) {
                VisitPattern(*guardedPat->inner);
            }
            if (guardedPat->guard) {
                VisitExpr(*guardedPat->guard);
            }
        }
    }

    void VisitExpr(const Expr &expr) {
        if (const auto *unExpr = dynamic_cast<const UnaryExpr *>(&expr)) {
            if (unExpr->operand) {
                VisitExpr(*unExpr->operand);
            }
        }
        else if (const auto *postExpr = dynamic_cast<const PostfixExpr *>(&expr)) {
            if (postExpr->operand) {
                VisitExpr(*postExpr->operand);
            }
        }
        else if (const auto *binExpr = dynamic_cast<const BinaryExpr *>(&expr)) {
            if (binExpr->left) {
                VisitExpr(*binExpr->left);
            }
            if (binExpr->right) {
                VisitExpr(*binExpr->right);
            }
        }
        else if (const auto *assignExpr = dynamic_cast<const AssignExpr *>(&expr)) {
            if (assignExpr->target) {
                VisitExpr(*assignExpr->target);
            }
            if (assignExpr->value) {
                VisitExpr(*assignExpr->value);
            }
        }
        else if (const auto *ternExpr = dynamic_cast<const TernaryExpr *>(&expr)) {
            if (ternExpr->condition) {
                VisitExpr(*ternExpr->condition);
            }
            if (ternExpr->thenExpr) {
                VisitExpr(*ternExpr->thenExpr);
            }
            if (ternExpr->elseExpr) {
                VisitExpr(*ternExpr->elseExpr);
            }
        }
        else if (const auto *rangeExpr = dynamic_cast<const RangeExpr *>(&expr)) {
            if (rangeExpr->lo) {
                VisitExpr(*rangeExpr->lo);
            }
            if (rangeExpr->hi) {
                VisitExpr(*rangeExpr->hi);
            }
        }
        else if (const auto *callExpr = dynamic_cast<const CallExpr *>(&expr)) {
            if (callExpr->callee) {
                VisitExpr(*callExpr->callee);
            }
            for (const auto &arg : callExpr->args) {
                if (arg) {
                    VisitExpr(*arg);
                }
            }
        }
        else if (const auto *idxExpr = dynamic_cast<const IndexExpr *>(&expr)) {
            if (idxExpr->object) {
                VisitExpr(*idxExpr->object);
            }
            if (idxExpr->index) {
                VisitExpr(*idxExpr->index);
            }
        }
        else if (const auto *fieldExpr = dynamic_cast<const FieldExpr *>(&expr)) {
            if (fieldExpr->object) {
                VisitExpr(*fieldExpr->object);
            }
        }
        else if (const auto *structInit = dynamic_cast<const StructInitExpr *>(&expr)) {
            for (const auto &f : structInit->fields) {
                if (f.value) {
                    VisitExpr(*f.value);
                }
            }
        }
        else if (const auto *sliceExpr = dynamic_cast<const SliceExpr *>(&expr)) {
            for (const auto &el : sliceExpr->elements) {
                if (el) {
                    VisitExpr(*el);
                }
            }
        }
        else if (const auto *spreadExpr = dynamic_cast<const SpreadExpr *>(&expr)) {
            if (spreadExpr->operand) {
                VisitExpr(*spreadExpr->operand);
            }
        }
        else if (const auto *tupleExpr = dynamic_cast<const TupleExpr *>(&expr)) {
            for (const auto &el : tupleExpr->elements) {
                if (el) {
                    VisitExpr(*el);
                }
            }
        }
        else if (const auto *castExpr = dynamic_cast<const CastExpr *>(&expr)) {
            if (castExpr->operand) {
                VisitExpr(*castExpr->operand);
            }
        }
        else if (const auto *isExpr = dynamic_cast<const IsExpr *>(&expr)) {
            if (isExpr->operand) {
                VisitExpr(*isExpr->operand);
            }
        }
        else if (const auto *blockExpr = dynamic_cast<const BlockExpr *>(&expr)) {
            if (blockExpr->block) {
                VisitBlock(*blockExpr->block);
            }
        }
        else if (const auto *matchExpr = dynamic_cast<const MatchExpr *>(&expr)) {
            if (matchExpr->subject) {
                VisitExpr(*matchExpr->subject);
            }
            for (const auto &arm : matchExpr->arms) {
                if (arm.pattern) {
                    VisitPattern(*arm.pattern);
                }
                if (arm.body) {
                    VisitExpr(*arm.body);
                }
            }
        }
    }
};

} // namespace

bool LintResult::HasErrors() const noexcept {
    return std::ranges::any_of(diagnostics, [](const Diagnostic &diagnostic) { return diagnostic.IsError(); });
}

LintResult Lint(std::string source, std::string sourceName) {
    Lexer lexer(std::move(source), sourceName);
    auto lexed = lexer.Tokenize();
    LintResult result{std::move(lexed.diagnostics)};
    if (result.HasErrors()) {
        return result;
    }

    Parser parser(std::move(lexed.tokens), std::move(sourceName));
    auto parsed = parser.Parse();
    result.diagnostics.insert(result.diagnostics.end(), std::make_move_iterator(parsed.diagnostics.begin()),
                              std::make_move_iterator(parsed.diagnostics.end()));

    if (result.HasErrors()) {
        return result;
    }

    // Run style checks if there are no parsing errors
    LinterVisitor styleVisitor(parsed.module.name);
    styleVisitor.VisitModule(parsed.module);

    result.diagnostics.insert(result.diagnostics.end(), std::make_move_iterator(styleVisitor.diagnostics.begin()),
                              std::make_move_iterator(styleVisitor.diagnostics.end()));

    return result;
}

} // namespace Rux::Linting
