#include "Lexer/Lexer.h"
#include "Numeric/IntegerLiteral.h"
#include "Semantic/Analysis/AnalysisContext.h"
#include "Semantic/Conditional/ConditionalCompilation.h"
#include "Target/Layout.h"
#include "Target/Target.h"
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

bool AnalysisContext::CanAssignExprTo(const Expr &expr, const TypeRef &exprType, const TypeRef &targetType) {
    if (targetType.kind == TypeRef::Kind::Reference) {
        TypeRef targetReferent = targetType.inner.front();
        TypeRef sourceReferent =
            exprType.kind == TypeRef::Kind::Reference && !exprType.inner.empty() ? exprType.inner.front() : exprType;
        targetReferent.isMut = false;
        sourceReferent.isMut = false;
        const bool interfaceView = TypeImplementsInterface(sourceReferent, targetReferent);
        if (!exprType.CanImplicitlyBorrowTo(targetType) && !interfaceView) {
            return false;
        }
        if (exprType.kind == TypeRef::Kind::Reference) {
            return !exprType.inner.empty() && (!targetType.inner.front().isMut || exprType.inner.front().isMut);
        }
        // An index that resolved to a declared `[]` is a call whose result is a temporary, so it is not a place a
        // reference can borrow, however much it looks like one.
        const bool addressable =
            dynamic_cast<const IdentExpr *>(&expr) || dynamic_cast<const SelfExpr *>(&expr) ||
            dynamic_cast<const FieldExpr *>(&expr) ||
            (dynamic_cast<const IndexExpr *>(&expr) && !IsIndexOperatorCall(expr)) ||
            (dynamic_cast<const UnaryExpr *>(&expr) && static_cast<const UnaryExpr &>(expr).op == TokenKind::Star);
        if (!addressable) {
            return false;
        }
        return targetType.inner.empty() || !targetType.inner.front().isMut || !PlaceIsImmutable(expr);
    }
    if (exprType.kind == TypeRef::Kind::Array && exprType.arrayLength && !exprType.inner.empty()) {
        if (const auto sliceElement = SliceElementType(targetType)) {
            if (const auto *array = dynamic_cast<const ArrayExpr *>(&expr)) {
                for (const auto &element : array->elements) {
                    const TypeRef elementType = CheckExpr(*element);
                    if (!CanAssignExprTo(*element, elementType, *sliceElement)) {
                        return false;
                    }
                }
                return true;
            }
            if (const auto *repeat = dynamic_cast<const ArrayRepeatExpr *>(&expr)) {
                const TypeRef elementType = CheckExpr(*repeat->value);
                return CanAssignExprTo(*repeat->value, elementType, *sliceElement);
            }
            return exprType.inner[0].IsAssignableTo(*sliceElement);
        }
    }

    if (const auto *array = dynamic_cast<const ArrayExpr *>(&expr);
        array && targetType.kind == TypeRef::Kind::Array && targetType.arrayLength && !targetType.inner.empty()) {
        if (array->elements.size() != *targetType.arrayLength) {
            return false;
        }
        for (const auto &element : array->elements) {
            const TypeRef elementType = CheckExpr(*element);
            if (!CanAssignExprTo(*element, elementType, targetType.inner[0])) {
                return false;
            }
        }
        return true;
    }

    if (const auto *repeat = dynamic_cast<const ArrayRepeatExpr *>(&expr);
        repeat && targetType.kind == TypeRef::Kind::Array && targetType.arrayLength && !targetType.inner.empty()) {
        if (exprType.kind != TypeRef::Kind::Array || exprType.arrayLength != targetType.arrayLength) {
            return false;
        }
        const TypeRef elementType = CheckExpr(*repeat->value);
        return CanAssignExprTo(*repeat->value, elementType, targetType.inner[0]);
    }

    // Tuple literals are contextually typed element-by-element. This lets
    // each element use the same assignment rules as a scalar expression
    // (notably range-checked unsuffixed integer literals), and naturally
    // handles nested tuple literals as well.
    if (const auto *tuple = dynamic_cast<const TupleExpr *>(&expr);
        tuple && exprType.kind == TypeRef::Kind::Tuple && targetType.kind == TypeRef::Kind::Tuple) {
        if (tuple->elements.size() != targetType.inner.size() || exprType.inner.size() != targetType.inner.size()) {
            return false;
        }
        for (std::size_t i = 0; i < tuple->elements.size(); ++i) {
            if (!CanAssignExprTo(*tuple->elements[i], exprType.inner[i], targetType.inner[i])) {
                return false;
            }
        }
        return true;
    }

    if (targetType.IsInteger() && IsUnsuffixedIntegerLiteral(expr)) {
        return UnsuffixedIntegerLiteralFits(expr, targetType);
    }

    // A constant integer expression (e.g. 10 + 2 * (5 - 3)) coerces to
    // any integer type it fits in, the same way a bare literal does.
    if (targetType.IsInteger()) {
        if (const auto folded = EvalConstInt(expr); folded && ConstantFitsTarget(*folded, targetType)) {
            return true;
        }
    }

    if (const auto *ternary = dynamic_cast<const TernaryExpr *>(&expr)) {
        const TypeRef thenType = CheckExpr(*ternary->thenExpr);
        const TypeRef elseType = CheckExpr(*ternary->elseExpr);
        if (CanAssignExprTo(*ternary->thenExpr, thenType, targetType) &&
            CanAssignExprTo(*ternary->elseExpr, elseType, targetType)) {
            return true;
        }
    }

    return exprType.IsAssignableTo(targetType) || (IsNullLiteral(expr) && targetType.kind == TypeRef::Kind::Pointer) ||
           UnsuffixedIntegerLiteralFits(expr, targetType) || TypeImplementsInterface(exprType, targetType);
}

std::optional<std::uint64_t> AnalysisContext::EvalArrayLength(const Expr &expr) const {
    const auto value = EvalConstInt(expr);
    if (!value || *value < 0) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(*value);
}

// Validate array extents AnalysisContext::and reject flexible arrays everywhere except the
// final, top-level field of a struct. A nested T[] is never a tail field.
void AnalysisContext::ValidateArrayType(const TypeExpr &type, bool allowFlexibleTail) {
    if (const auto *array = dynamic_cast<const ArrayTypeExpr *>(&type)) {
        if (!array->size) {
            if (!allowFlexibleTail) {
                EmitError(array->location, "flexible array type is only allowed as the final field of a struct");
            }
        }
        else if (!EvalArrayLength(*array->size)) {
            EmitError(array->size->location, "array length must be a non-negative compile-time integer");
        }
        ValidateArrayType(*array->element);
        return;
    }
    if (const auto *pointer = dynamic_cast<const PointerTypeExpr *>(&type)) {
        ValidateArrayType(*pointer->pointee);
        return;
    }
    if (const auto *reference = dynamic_cast<const ReferenceTypeExpr *>(&type)) {
        ValidateArrayType(*reference->pointee);
        return;
    }
    if (const auto *tuple = dynamic_cast<const TupleTypeExpr *>(&type)) {
        for (const auto &element : tuple->elements) {
            ValidateArrayType(*element);
        }
        return;
    }
    if (const auto *function = dynamic_cast<const FunctionTypeExpr *>(&type)) {
        for (const auto &param : function->params) {
            ValidateArrayType(*param);
        }
        if (function->returnType) {
            ValidateArrayType(**function->returnType);
        }
        return;
    }
    if (const auto *named = dynamic_cast<const NamedTypeExpr *>(&type)) {
        for (const auto &arg : named->typeArgs) {
            ValidateArrayType(*arg);
        }
    }
}

Symbol *AnalysisContext::FindUniquePackageType(const std::string &name) const {
    auto sameSymbol = [](const Symbol &lhs, const Symbol &rhs) {
        return lhs.kind == rhs.kind && lhs.name == rhs.name && lhs.location.line == rhs.location.line &&
               lhs.location.column == rhs.location.column;
    };

    Symbol *matched = nullptr;
    for (const auto &[_, moduleScopes] : packageModuleScopes) {
        for (const auto &[__, scope] : moduleScopes) {
            auto *sym = const_cast<Scope *>(scope)->LookupLocal(name);
            if (!sym || (sym->kind != Symbol::Kind::Type && sym->kind != Symbol::Kind::Interface)) {
                continue;
            }
            if (matched && !sameSymbol(*matched, *sym)) {
                return nullptr;
            }
            matched = sym;
        }
    }
    return matched;
}

TypeRef AnalysisContext::ResolveType(const TypeExpr &expr) {
    if (const auto accepted = typeNodeTypes.find(&expr);
        accepted != typeNodeTypes.end() && accepted->second.IsString()) {
        return accepted->second;
    }
    TypeRef type = ResolveTypeImpl(expr);
    if (!type.IsUnknown()) {
        typeNodeTypes.insert_or_assign(&expr, type);
    }
    return type;
}
} // namespace Rux::SemanticDetail
