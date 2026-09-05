#include "Lexer/Lexer.h"
#include "Numeric/IntegerLiteral.h"
#include "Semantic/Analysis/AnalysisContext.h"
#include "Semantic/Conditional/ConditionalCompilation.h"
#include "Target/Layout.h"
#include "Target/Target.h"
#include "Types/PrimitiveCatalog.h"
#include "Types/Type.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <charconv>
#include <format>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace Rux::SemanticDetail {
using Layout::AlignUp;

TypeRef AnalysisContext::CheckExprImpl(const Expr &expr) {
    if (const std::optional<TypeRef> basicType = CheckBasicExpression(expr)) {
        return *basicType;
    }

    if (auto *e = dynamic_cast<const IdentExpr *>(&expr)) {
        Symbol *sym = currentScope->Lookup(e->name);
        if (sym) {
            return ReadTrackedSymbol(*sym, e->location);
        }
        EmitUndefinedName(e->location, e->name);
        return TypeRef::MakeUnknown();
    }

    if (dynamic_cast<const SelfExpr *>(&expr)) {
        if (!inImpl) {
            EmitError(expr.location, "'self' used outside of an extend block");
        }
        return currentSelfType.IsUnknown() ? TypeRef::MakeNamed("self") : currentSelfType;
    }

    if (auto *e = dynamic_cast<const PathExpr *>(&expr)) {
        if (e->segments.empty()) {
            return TypeRef::MakeUnknown();
        }
        Symbol *first = currentScope->Lookup(e->segments[0]);
        if (!first) {
            EmitUndefinedName(e->location, e->segments[0]);
            return TypeRef::MakeUnknown();
        }
        if (e->segments.size() >= 2 && (first->kind == Symbol::Kind::Type || first->kind == Symbol::Kind::Interface)) {
            if (first->kind == Symbol::Kind::Type) {
                if (e->segments.size() == 2) {
                    if (const auto *constant = LookupAssociatedConstant(*first, e->segments[1])) {
                        if (!IsAccessible(*constant)) {
                            EmitPrivacyError(e->location, *constant, "associated constant", constant->name);
                            return TypeRef::MakeUnknown();
                        }
                        const TypeRef type = CheckAssociatedConstant(*constant);
                        associatedConstants[e] = constant;
                        return type;
                    }
                }
                const std::string &variantName = e->segments[1];
                if (const auto resolved = LookupCase(first->name, variantName)) {
                    const EnumDecl::Variant *variant = resolved->selectedCase;
                    if (e->segments.size() > 2) {
                        EmitError(e->location,
                                  std::format("'{}' is a {} {}, not a module", variantName,
                                              resolved->form == EnumDecl::Form::Variant ? "variant" : "enum",
                                              resolved->form == EnumDecl::Form::Variant ? "case" : "enumerator"));
                        return TypeRef::MakeUnknown();
                    }
                    if (!variant->fields.empty() || !variant->namedFields.empty()) {
                        return EnumVariantConstructorType(*resolved->declaration, *variant);
                    }
                    return EnumType(*resolved->declaration);
                }
            }
            TypeRef receiverType = first->type.IsUnknown() ? TypeRef::MakeNamed(first->name) : first->type;
            const std::string &methodName = e->segments[1];
            const FuncDecl *method = LookupMethod(receiverType, methodName);
            if (!method) {
                const std::vector<const FuncDecl *> accessible = AccessibleMethodCandidates(receiverType, methodName);
                if (accessible.empty()) {
                    const auto methods = methodsByType.find(NamedBaseTypeName(receiverType));
                    if (methods != methodsByType.end()) {
                        const auto named = methods->second.find(methodName);
                        if (named != methods->second.end() && !named->second.empty()) {
                            EmitPrivacyError(e->location, *named->second.front(), "associated function", methodName);
                            return TypeRef::MakeUnknown();
                        }
                    }
                }
                EmitError(e->location, std::format("'{}' not found in extend for type '{}'", methodName, first->name));
                return TypeRef::MakeUnknown();
            }
            if (e->segments.size() > 2) {
                EmitError(e->location, std::format("'{}' is a function, not a module", methodName));
                return TypeRef::MakeUnknown();
            }
            return AssociatedFunctionType(receiverType, *method);
        }
        Symbol *current = first;
        Scope *moduleScope = nullptr;
        for (std::size_t i = 1; i < e->segments.size(); ++i) {
            if (current->kind != Symbol::Kind::Module || !current->moduleScope) {
                EmitError(e->location,
                          std::format("name '{}' is a {}, not a module", current->name, SymbolKindName(current->kind)),
                          {DeclarationNote(*current)});
                return TypeRef::MakeUnknown();
            }
            moduleScope = current->moduleScope;
            Symbol *item = moduleScope->LookupLocal(e->segments[i]);
            if (!item) {
                EmitError(e->location,
                          std::format("'{}' not found in module '{}'", e->segments[i], e->segments[i - 1]));
                return TypeRef::MakeUnknown();
            }
            if (!IsAccessible(*item)) {
                EmitPrivacyError(e->location, *item);
                return TypeRef::MakeUnknown();
            }
            current = item;
        }
        return current->type;
    }

    if (auto *e = dynamic_cast<const TypeQueryExpr *>(&expr)) {
        return CheckTypeQueryExpression(*e);
    }

    if (dynamic_cast<const IntrinsicExpr *>(&expr)) {
        const auto *e = static_cast<const IntrinsicExpr *>(&expr);
        using K = IntrinsicExpr::Kind;
        const bool takesArgument = e->kind == K::TargetFeature || e->kind == K::CompilerHasFeature ||
                                   e->kind == K::Config || e->kind == K::HasConfig;
        if (takesArgument) {
            if (e->args.size() != 1 || !e->args[0]) {
                EmitError(e->location, "compile-time intrinsic expects exactly one argument");
            }
            else if (e->kind == K::TargetFeature && dynamic_cast<const EnumShorthandExpr *>(e->args[0].get())) {
                // `.AVX2` is given its meaning by #target.HasFeature.
            }
            else {
                const TypeRef argType = CheckExpr(*e->args[0]);
                if (!argType.IsUnknown() && !argType.IsString()) {
                    EmitError(e->args[0]->location, "compile-time intrinsic argument must be a string");
                }
            }
        }

        if (e->kind == K::Line || e->kind == K::Column || e->kind == K::PointerBits) {
            return TypeRef::MakeUInt();
        }
        if (e->kind == K::BuildTimestamp) {
            return TypeRef::MakeUInt64();
        }
        if (e->kind == K::TargetFeature || e->kind == K::CompilerHasFeature || e->kind == K::HasConfig ||
            e->kind == K::DebugAssertions || e->kind == K::DebugInfo || e->kind == K::IsTest) {
            return TypeRef::MakeBool();
        }
        if (e->kind == K::Os || e->kind == K::Arch || e->kind == K::Abi || e->kind == K::Endian ||
            e->kind == K::DataModel || e->kind == K::ObjectFormat || e->kind == K::BuildMode ||
            e->kind == K::Optimization || e->kind == K::OutputKind) {
            const char *name = e->kind == K::Os           ? "#target.os"
                             : e->kind == K::Arch         ? "#target.arch"
                             : e->kind == K::Abi          ? "#target.abi"
                             : e->kind == K::Endian       ? "#target.endian"
                             : e->kind == K::DataModel    ? "#target.dataModel"
                             : e->kind == K::ObjectFormat ? "#target.objectFormat"
                             : e->kind == K::BuildMode    ? "#build.mode"
                             : e->kind == K::Optimization ? "#build.optimization"
                                                          : "#build.outputKind";
            EmitError(e->location, std::string("'") + name + "' can only be used in a 'when' condition");
            return TypeRef::MakeUnknown();
        }
        return TypeRef::MakeString8();
    }

    if (const auto *e = dynamic_cast<const EnumShorthandExpr *>(&expr)) {
        // The bare `.Variant` shorthand is not part of the language; the
        // variant must always be written out in full.
        EmitError(e->location, std::format("'.{}' must be written in full, as in 'Enum::{}'", e->variant, e->variant));
        return TypeRef::MakeUnknown();
    }

    if (auto *e = dynamic_cast<const TernaryExpr *>(&expr)) {
        return CheckTernaryExpression(*e);
    }

    if (auto *e = dynamic_cast<const RangeExpr *>(&expr)) {
        TypeRef loType = e->lo ? CheckExpr(*e->lo) : TypeRef::MakeUnknown();
        TypeRef hiType = e->hi ? CheckExpr(*e->hi) : TypeRef::MakeUnknown();
        if ((!loType.IsUnknown() && !loType.IsNumeric()) || (!hiType.IsUnknown() && !hiType.IsNumeric())) {
            EmitError(e->location, "range bounds must be numeric");
        }
        if (e->lo && e->hi) {
            const auto start = EvalConstInt(*e->lo);
            const auto end = EvalConstInt(*e->hi);
            if (start && end && *start > *end) {
                EmitError(e->location, "range start cannot be greater than its end");
            }
        }
        TypeRef elemType = loType.IsUnknown() ? hiType : loType;
        if (e->lo && e->hi && hiType.IsInteger() && UnsuffixedIntegerLiteralFits(*e->lo, hiType)) {
            elemType = hiType;
        }
        else if (e->lo && e->hi && loType.IsInteger() && UnsuffixedIntegerLiteralFits(*e->hi, loType)) {
            elemType = loType;
        }
        if (elemType.IsUnknown()) {
            return TypeRef::MakeRangeFull();
        }
        return TypeRef::MakeRange(elemType, e->lo != nullptr, e->hi != nullptr, e->inclusive);
    }

    if (const auto *e = dynamic_cast<const CallExpr *>(&expr)) {
        return CheckCallExpression(*e);
    }

    if (const std::optional<TypeRef> aggregateType = CheckAggregateExpression(expr)) {
        return *aggregateType;
    }

    if (auto *e = dynamic_cast<const IsExpr *>(&expr)) {
        TypeRef operandType = CheckExpr(*e->operand);
        const std::string ifaceName = NamedBaseTypeName(ResolveType(*e->type));
        if (!ifaceName.empty()) {
            Symbol *sym = currentScope->Lookup(ifaceName);
            if (sym && sym->kind == Symbol::Kind::Interface) {
                EmitError(e->location, std::format("type test 'is {}' is unavailable: interface checks are not "
                                                   "implemented",
                                                   ifaceName));
            }
        }
        return TypeRef::MakeBool();
    }
    if (auto *e = dynamic_cast<const MatchExpr *>(&expr)) {
        return CheckMatchExpression(*e);
    }

    if (auto *e = dynamic_cast<const BlockExpr *>(&expr)) {
        CheckBlock(*e->block);
        return TypeRef::MakeUnknown();
    }

    if (auto *e = dynamic_cast<const SpreadExpr *>(&expr)) {
        return CheckExpr(*e->operand);
    }

    return TypeRef::MakeUnknown();
}

TypeRef AnalysisContext::LiteralType(const Token &tok) const {
    switch (tok.kind) {
    case TokenKind::IntLiteral:
    case TokenKind::FloatLiteral:
        return SuffixedLiteralType(tok);
    case TokenKind::StringLiteral:
        return StringLiteralType(tok);
    case TokenKind::CharLiteral:
        return CharLiteralType(tok);
    case TokenKind::BoolLiteral:
        return TypeRef::MakeBool();
    default:
        return TypeRef::MakeUnknown();
    }
}

/// Reject a constant that the target character width cannot hold.
///
/// The width's own rule decides: a code unit accepts everything that fits in it, a scalar value stops at U+10FFFF
/// AnalysisContext::and refuses the surrogates. Every character width is covered, so a width gains this check by
/// joining the catalog rather than by being listed here.
void AnalysisContext::ValidateCastConstant(const CastExpr &expression, const TypeRef &operandType,
                                           const TypeRef &targetType) const {
    const auto maximum = MaxCharacterValue(targetType.kind);
    if (!maximum || !(operandType.IsInteger() || operandType.IsChar())) {
        return;
    }
    const auto outOfRange = [&] {
        EmitError(expression.location, std::format("constant cast from '{}' to '{}' is outside the target type's range",
                                                   operandType.ToString(), targetType.ToString()));
    };
    if (const auto value = EvalConstInt(*expression.operand); value && *value < 0) {
        outOfRange();
        return;
    }
    const auto charValue = EvalConstCharCastValue(*expression.operand);
    if (!charValue) {
        return;
    }
    if (*charValue > *maximum) {
        outOfRange();
    }
    else if (!IsValidCharacterValue(targetType.kind, *charValue)) {
        EmitError(expression.location, std::format("cast from '{}' to '{}' uses invalid surrogate code point U+{:04X}",
                                                   operandType.ToString(), targetType.ToString(), *charValue));
    }
}

/// Counts a type's structural nodes, stopping once the limit is passed so a runaway type costs no more to
/// measure than a well-behaved one.
std::size_t AnalysisContext::TypeNodeCount(const TypeRef &type, const std::size_t limit) {
    std::size_t count = 1;
    for (const TypeRef &inner : type.inner) {
        if (count > limit) {
            return count;
        }
        count += TypeNodeCount(inner, limit - std::min(count, limit));
    }
    return count;
}
} // namespace Rux::SemanticDetail
