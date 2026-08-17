#include "Linter/Linter.h"

#include "Lexer/Lexer.h"
#include "Syntax/Parser/Parser.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <iterator>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace Rux::Linting {
namespace {
/// A compiler-injected intrinsic such as `#target` or `#Error`. Its spelling is dictated by the compiler, not the
/// source's naming conventions, so the naming lints leave it alone.
bool IsIntrinsicName(std::string_view name) {
    return !name.empty() && name.front() == '#';
}

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

enum class NamingConvention {
    PascalCase,
    CamelCase,
};

/// Whether a word starts at `index`, which is what a case check has to agree on before it can judge the spelling. An
/// acronym run is the awkward case: the boundary in `HTTPServer` is before the `S`, not after the `P`.
bool IsWordBoundary(const std::string_view name, const std::size_t index) {
    if (index == 0 || name[index - 1] == '_') {
        return true;
    }

    const auto previous = static_cast<unsigned char>(name[index - 1]);
    const auto current = static_cast<unsigned char>(name[index]);
    if (std::islower(previous) && std::isupper(current)) {
        return true;
    }

    return std::isupper(previous) && std::isupper(current) && index + 1 < name.size() &&
           std::islower(static_cast<unsigned char>(name[index + 1]));
}

/// Rewrite a name into the convention it should follow, so the diagnostic can show the suggestion rather than only
/// naming the rule.
///
/// @return nullopt when the name already conforms, which is what suppresses the warning
std::optional<std::string> NormalizeName(const std::string_view name, const NamingConvention convention) {
    std::vector<std::string> words;
    for (std::size_t index = 0; index < name.size();) {
        while (index < name.size() && name[index] == '_') {
            ++index;
        }
        if (index == name.size()) {
            break;
        }

        std::string word;
        do {
            const auto value = static_cast<unsigned char>(name[index]);
            word.push_back(static_cast<char>(std::tolower(value)));
            ++index;
        }
        while (index < name.size() && name[index] != '_' && !IsWordBoundary(name, index));
        words.push_back(std::move(word));
    }

    if (words.empty()) {
        return std::nullopt;
    }

    std::string normalized;
    for (std::size_t index = 0; index < words.size(); ++index) {
        auto &word = words[index];
        if (index != 0 || convention == NamingConvention::PascalCase) {
            word.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(word.front())));
        }
        normalized += word;
    }

    const bool valid = convention == NamingConvention::PascalCase ? IsPascalCase(normalized) : IsCamelCase(normalized);
    if (!valid || normalized == name) {
        return std::nullopt;
    }
    return normalized;
}

std::string_view ConventionName(const NamingConvention convention) {
    return convention == NamingConvention::PascalCase ? "PascalCase" : "camelCase";
}

/// The name a declaration should be checked under.
///
/// @return nullopt for a declaration that introduces no name of its own
std::optional<std::string> DeclName(const Decl &decl) {
    if (const auto *function = dynamic_cast<const FuncDecl *>(&decl)) {
        return function->name;
    }
    if (const auto *structure = dynamic_cast<const StructDecl *>(&decl)) {
        return structure->name;
    }
    if (const auto *enumeration = dynamic_cast<const EnumDecl *>(&decl)) {
        return enumeration->name;
    }
    if (const auto *unionDecl = dynamic_cast<const UnionDecl *>(&decl)) {
        return unionDecl->name;
    }
    if (const auto *interfaceDecl = dynamic_cast<const InterfaceDecl *>(&decl)) {
        return interfaceDecl->name;
    }
    if (const auto *module = dynamic_cast<const ModuleDecl *>(&decl)) {
        return module->name;
    }
    if (const auto *constant = dynamic_cast<const ConstDecl *>(&decl)) {
        return constant->name;
    }
    if (const auto *alias = dynamic_cast<const TypeAliasDecl *>(&decl)) {
        return alias->name;
    }
    if (const auto *function = dynamic_cast<const ExternFuncDecl *>(&decl)) {
        return function->name;
    }
    if (const auto *variable = dynamic_cast<const ExternVarDecl *>(&decl)) {
        return variable->name;
    }
    return std::nullopt;
}

std::vector<std::string> DeclNames(const std::span<const DeclPtr> declarations) {
    std::vector<std::string> names;
    for (const auto &declaration : declarations) {
        if (declaration) {
            if (auto name = DeclName(*declaration)) {
                names.push_back(std::move(*name));
            }
        }
    }
    return names;
}

template <typename T>
std::vector<std::string> Names(const std::vector<T> &items) {
    std::vector<std::string> names;
    names.reserve(items.size());
    for (const auto &item : items) {
        names.push_back(item.name);
    }
    return names;
}

/// Collect the bindings a pattern introduces, recursing through nested patterns, since each is a name the convention
/// applies to.
void AppendPatternNames(const Pattern &pattern, std::vector<std::string> &names) {
    if (const auto *identifier = dynamic_cast<const IdentPattern *>(&pattern)) {
        if (!identifier->name.empty()) {
            names.push_back(identifier->name);
        }
    }
    else if (const auto *range = dynamic_cast<const RangePattern *>(&pattern)) {
        if (range->lo) {
            AppendPatternNames(*range->lo, names);
        }
        if (range->hi) {
            AppendPatternNames(*range->hi, names);
        }
    }
    else if (const auto *enumeration = dynamic_cast<const EnumPattern *>(&pattern)) {
        for (const auto &argument : enumeration->args) {
            if (argument) {
                AppendPatternNames(*argument, names);
            }
        }
        for (const auto &argument : enumeration->namedArgs) {
            if (argument.pattern) {
                AppendPatternNames(*argument.pattern, names);
            }
        }
    }
    else if (const auto *structure = dynamic_cast<const StructPattern *>(&pattern)) {
        for (const auto &field : structure->fields) {
            if (field.pattern) {
                AppendPatternNames(*field.pattern, names);
            }
        }
    }
    else if (const auto *tuple = dynamic_cast<const TuplePattern *>(&pattern)) {
        for (const auto &element : tuple->elements) {
            if (element) {
                AppendPatternNames(*element, names);
            }
        }
    }
    else if (const auto *guarded = dynamic_cast<const GuardedPattern *>(&pattern)) {
        if (guarded->inner) {
            AppendPatternNames(*guarded->inner, names);
        }
    }
}

/// Every name visible inside a block, its own bindings plus those inherited from enclosing scopes. Shadowing checks
/// need the inherited set, which is why it is threaded through rather than recomputed.
std::vector<std::string> BlockBindingNames(const Block &block, const std::span<const std::string> inheritedNames) {
    std::vector<std::string> names(inheritedNames.begin(), inheritedNames.end());
    for (const auto &statement : block.stmts) {
        if (const auto *binding = dynamic_cast<const LetStmt *>(statement.get())) {
            if (!binding->name.empty()) {
                names.push_back(binding->name);
            }
            if (binding->pattern) {
                AppendPatternNames(*binding->pattern, names);
            }
        }
    }
    return names;
}

bool SuggestionCollides(const std::string_view original, const std::string_view suggestion,
                        const NamingConvention convention, const std::span<const std::string> names) {
    bool skippedOriginal = false;
    for (const auto &name : names) {
        if (!skippedOriginal && name == original) {
            skippedOriginal = true;
            continue;
        }

        const auto normalized = NormalizeName(name, convention);
        if ((normalized && *normalized == suggestion) || (!normalized && name == suggestion)) {
            return true;
        }
    }
    return false;
}

/// Whether the declaration carries an `#Allow` for this rule, which is how a deliberate exception silences the warning
/// at the point it applies.
bool Allows(const Decl &decl, const std::string_view rule) {
    return std::ranges::find(decl.allowedLints, rule) != decl.allowedLints.end();
}

/// Whether the name is an operator spelling rather than an identifier, and so exempt from naming conventions that only
/// make sense for words.
bool IsSymbolicOperatorName(const std::string_view name) {
    return !name.empty() && std::ranges::none_of(name, [](const char c) {
        const auto value = static_cast<unsigned char>(c);
        return std::isalnum(value) || c == '_';
    });
}

class LinterVisitor {
public:
    explicit LinterVisitor(std::string inputSourceName)
        : sourceName(std::move(inputSourceName)) {
    }

    std::vector<Diagnostic> diagnostics;

    void VisitModule(const Module &module) {
        const auto names = DeclNames(module.items);
        for (const auto &item : module.items) {
            if (item) {
                VisitDecl(*item, names);
            }
        }
    }

private:
    std::string sourceName;

    void Warn(SourceLocation loc, std::string message, std::optional<std::string> help = std::nullopt) {
        Diagnostic d;
        d.severity = Diagnostic::Severity::Warning;
        d.sourceName = sourceName;
        d.location = loc;
        d.message = std::move(message);
        d.help = std::move(help);
        diagnostics.push_back(std::move(d));
    }

    void WarnNaming(const SourceLocation loc, const std::string_view description, const std::string_view name,
                    const NamingConvention convention, const std::span<const std::string> names) {
        auto suggestion = NormalizeName(name, convention);
        if (suggestion && SuggestionCollides(name, *suggestion, convention, names)) {
            suggestion.reset();
        }

        std::optional<std::string> help;
        if (suggestion) {
            help = std::format("rename it to '{}'", *suggestion);
        }
        Warn(loc, std::format("{} '{}' should be {}", description, name, ConventionName(convention)), std::move(help));
    }

    void VisitDecl(const Decl &decl, const std::span<const std::string> siblingNames) {
        if (const auto *fn = dynamic_cast<const FuncDecl *>(&decl)) {
            if (!IsIntrinsicName(fn->name) && !IsSymbolicOperatorName(fn->name) && !IsPascalCase(fn->name)) {
                WarnNaming(fn->location, "function name", fn->name, NamingConvention::PascalCase, siblingNames);
            }
            const auto parameterNames = Names(fn->params);
            for (const auto &p : fn->params) {
                if (!IsCamelCase(p.name)) {
                    WarnNaming(p.location, "parameter name", p.name, NamingConvention::CamelCase, parameterNames);
                }
            }
            if (fn->body) {
                VisitBlock(*fn->body, parameterNames);
            }
        }
        else if (const auto *st = dynamic_cast<const StructDecl *>(&decl)) {
            const bool allowTypeNaming = Allows(decl, "naming.type");
            if (!allowTypeNaming && !IsPascalCase(st->name)) {
                WarnNaming(st->location, "struct name", st->name, NamingConvention::PascalCase, siblingNames);
            }
            const auto fieldNames = Names(st->fields);
            for (const auto &f : st->fields) {
                if (!allowTypeNaming && !IsCamelCase(f.name)) {
                    WarnNaming(f.location, "struct field name", f.name, NamingConvention::CamelCase, fieldNames);
                }
            }
        }
        else if (const auto *en = dynamic_cast<const EnumDecl *>(&decl)) {
            const bool allowTypeNaming = Allows(decl, "naming.type");
            if (!allowTypeNaming && !IsPascalCase(en->name)) {
                WarnNaming(en->location, "enum name", en->name, NamingConvention::PascalCase, siblingNames);
            }
            const auto variantNames = Names(en->variants);
            for (const auto &v : en->variants) {
                if (!allowTypeNaming && !IsPascalCase(v.name)) {
                    WarnNaming(v.location, "enum variant name", v.name, NamingConvention::PascalCase, variantNames);
                }
                const auto fieldNames = Names(v.namedFields);
                for (const auto &nf : v.namedFields) {
                    if (!allowTypeNaming && !IsCamelCase(nf.name)) {
                        WarnNaming(nf.location, "enum variant field name", nf.name, NamingConvention::CamelCase,
                                   fieldNames);
                    }
                }
            }
        }
        else if (const auto *un = dynamic_cast<const UnionDecl *>(&decl)) {
            const bool allowTypeNaming = Allows(decl, "naming.type");
            if (!allowTypeNaming && !IsPascalCase(un->name)) {
                WarnNaming(un->location, "union name", un->name, NamingConvention::PascalCase, siblingNames);
            }
            const auto fieldNames = Names(un->fields);
            for (const auto &f : un->fields) {
                if (!allowTypeNaming && !IsCamelCase(f.name)) {
                    WarnNaming(f.location, "union field name", f.name, NamingConvention::CamelCase, fieldNames);
                }
            }
        }
        else if (const auto *iface = dynamic_cast<const InterfaceDecl *>(&decl)) {
            if (!IsPascalCase(iface->name)) {
                WarnNaming(iface->location, "interface name", iface->name, NamingConvention::PascalCase, siblingNames);
            }
            std::vector<std::string> methodNames;
            methodNames.reserve(iface->methods.size());
            for (const auto &method : iface->methods) {
                if (method) {
                    methodNames.push_back(method->name);
                }
            }
            for (const auto &m : iface->methods) {
                if (m) {
                    VisitDecl(*m, methodNames);
                }
            }
        }
        else if (const auto *impl = dynamic_cast<const ImplDecl *>(&decl)) {
            std::vector<std::string> methodNames;
            methodNames.reserve(impl->methods.size());
            for (const auto &method : impl->methods) {
                if (method) {
                    methodNames.push_back(method->name);
                }
            }
            for (const auto &m : impl->methods) {
                if (m) {
                    VisitDecl(*m, methodNames);
                }
            }
        }
        else if (const auto *mod = dynamic_cast<const ModuleDecl *>(&decl)) {
            if (!IsPascalCase(mod->name)) {
                WarnNaming(mod->location, "module name", mod->name, NamingConvention::PascalCase, siblingNames);
            }
            const auto itemNames = DeclNames(mod->items);
            for (const auto &item : mod->items) {
                if (item) {
                    VisitDecl(*item, itemNames);
                }
            }
        }
        else if (const auto *cnst = dynamic_cast<const ConstDecl *>(&decl)) {
            if (!IsIntrinsicName(cnst->name) && !IsPascalCase(cnst->name)) {
                WarnNaming(cnst->location, "constant name", cnst->name, NamingConvention::PascalCase, siblingNames);
            }
            if (cnst->value) {
                VisitExpr(*cnst->value);
            }
        }
        else if (const auto *alias = dynamic_cast<const TypeAliasDecl *>(&decl)) {
            if (!Allows(decl, "naming.type") && !IsPascalCase(alias->name)) {
                WarnNaming(alias->location, "type alias name", alias->name, NamingConvention::PascalCase, siblingNames);
            }
        }
        else if (const auto *extBlock = dynamic_cast<const ExternBlockDecl *>(&decl)) {
            const auto itemNames = DeclNames(extBlock->items);
            for (const auto &item : extBlock->items) {
                if (item) {
                    VisitDecl(*item, itemNames);
                }
            }
        }
    }

    void VisitBlock(const Block &block, const std::span<const std::string> inheritedNames = {}) {
        const auto names = BlockBindingNames(block, inheritedNames);
        for (const auto &stmt : block.stmts) {
            if (stmt) {
                VisitStmt(*stmt, names);
            }
        }
    }

    void VisitStmt(const Stmt &stmt, const std::span<const std::string> scopeNames) {
        if (const auto *exprStmt = dynamic_cast<const ExprStmt *>(&stmt)) {
            if (exprStmt->expr) {
                VisitExpr(*exprStmt->expr);
            }
        }
        else if (const auto *letStmt = dynamic_cast<const LetStmt *>(&stmt)) {
            if (!letStmt->name.empty() && !IsCamelCase(letStmt->name)) {
                WarnNaming(letStmt->location, "local variable name", letStmt->name, NamingConvention::CamelCase,
                           scopeNames);
            }
            if (letStmt->pattern) {
                VisitPattern(*letStmt->pattern, scopeNames);
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
                VisitBlock(*ifStmt->thenBlock, scopeNames);
            }
            for (const auto &elif : ifStmt->elseIfs) {
                if (elif.condition) {
                    VisitExpr(*elif.condition);
                }
                if (elif.block) {
                    VisitBlock(*elif.block, scopeNames);
                }
            }
            if (ifStmt->elseBlock) {
                VisitBlock(*ifStmt->elseBlock, scopeNames);
            }
        }
        else if (const auto *whileStmt = dynamic_cast<const WhileStmt *>(&stmt)) {
            if (whileStmt->condition) {
                VisitExpr(*whileStmt->condition);
            }
            if (whileStmt->body) {
                VisitBlock(*whileStmt->body, scopeNames);
            }
        }
        else if (const auto *doWhileStmt = dynamic_cast<const DoWhileStmt *>(&stmt)) {
            if (doWhileStmt->body) {
                VisitBlock(*doWhileStmt->body, scopeNames);
            }
            if (doWhileStmt->condition) {
                VisitExpr(*doWhileStmt->condition);
            }
        }
        else if (const auto *loopStmt = dynamic_cast<const LoopStmt *>(&stmt)) {
            if (loopStmt->body) {
                VisitBlock(*loopStmt->body, scopeNames);
            }
        }
        else if (const auto *forStmt = dynamic_cast<const ForStmt *>(&stmt)) {
            std::vector loopNames(scopeNames.begin(), scopeNames.end());
            loopNames.push_back(forStmt->variable);
            if (!forStmt->variable.empty() && !IsCamelCase(forStmt->variable)) {
                WarnNaming(forStmt->location, "loop variable name", forStmt->variable, NamingConvention::CamelCase,
                           loopNames);
            }
            if (forStmt->iterable) {
                VisitExpr(*forStmt->iterable);
            }
            if (forStmt->body) {
                VisitBlock(*forStmt->body, loopNames);
            }
        }
        else if (const auto *matchStmt = dynamic_cast<const MatchStmt *>(&stmt)) {
            if (matchStmt->subject) {
                VisitExpr(*matchStmt->subject);
            }
            for (const auto &arm : matchStmt->arms) {
                if (arm.pattern) {
                    std::vector armNames(scopeNames.begin(), scopeNames.end());
                    AppendPatternNames(*arm.pattern, armNames);
                    VisitPattern(*arm.pattern, armNames);
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
                std::vector<std::string> name;
                if (auto declarationName = DeclName(*declStmt->decl)) {
                    name.push_back(std::move(*declarationName));
                }
                VisitDecl(*declStmt->decl, name);
            }
        }
    }

    void VisitPattern(const Pattern &pattern, const std::span<const std::string> scopeNames) {
        if (const auto *idPat = dynamic_cast<const IdentPattern *>(&pattern)) {
            if (!idPat->name.empty() && !IsCamelCase(idPat->name)) {
                WarnNaming(idPat->location, "pattern binding name", idPat->name, NamingConvention::CamelCase,
                           scopeNames);
            }
        }
        else if (const auto *rangePat = dynamic_cast<const RangePattern *>(&pattern)) {
            if (rangePat->lo) {
                VisitPattern(*rangePat->lo, scopeNames);
            }
            if (rangePat->hi) {
                VisitPattern(*rangePat->hi, scopeNames);
            }
        }
        else if (const auto *enumPat = dynamic_cast<const EnumPattern *>(&pattern)) {
            for (const auto &arg : enumPat->args) {
                if (arg) {
                    VisitPattern(*arg, scopeNames);
                }
            }
            for (const auto &narg : enumPat->namedArgs) {
                if (narg.pattern) {
                    VisitPattern(*narg.pattern, scopeNames);
                }
            }
        }
        else if (const auto *structPat = dynamic_cast<const StructPattern *>(&pattern)) {
            for (const auto &f : structPat->fields) {
                if (f.pattern) {
                    VisitPattern(*f.pattern, scopeNames);
                }
            }
        }
        else if (const auto *tuplePat = dynamic_cast<const TuplePattern *>(&pattern)) {
            for (const auto &el : tuplePat->elements) {
                if (el) {
                    VisitPattern(*el, scopeNames);
                }
            }
        }
        else if (const auto *guardedPat = dynamic_cast<const GuardedPattern *>(&pattern)) {
            if (guardedPat->inner) {
                VisitPattern(*guardedPat->inner, scopeNames);
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
        else if (const auto *sliceExpr = dynamic_cast<const ArrayExpr *>(&expr)) {
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
                    std::vector<std::string> names;
                    AppendPatternNames(*arm.pattern, names);
                    VisitPattern(*arm.pattern, names);
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
