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

TypeRef AnalysisContext::ReadBorrowedScalar(const Expr &expression, const TypeRef &type) {
    if (type.kind != TypeRef::Kind::Reference || type.inner.empty()) {
        return type;
    }
    TypeRef value = type.inner.front();
    value.isMut = false;
    if (!type.CanReadScalarTo(value)) {
        return type;
    }
    if (value.kind == TypeRef::Kind::TypeParam && currentFunctionDecl) {
        deferredScalarReads[currentFunctionDecl].push_back({type, expression.location});
    }
    borrowedScalarReads.insert(&expression);
    return value;
}

bool AnalysisContext::CanAssignExprTo(const Expr &expr, const TypeRef &exprType, const TypeRef &targetType) {
    if (const auto *construct = dynamic_cast<const NativeConstructExpr *>(&expr)) {
        return CanConstructNative(*construct, targetType);
    }
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
        if (auto sliceElement = SliceElementType(targetType)) {
            // The view's writability travels on the element; what is checked here is the element's own type.
            sliceElement->isMut = false;
            // An array literal is fresh storage, so it may be viewed either way; a named array may be viewed
            // writably only from a place that can be written.
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
            if (targetType.IsWritableSlice() && !PlaceIsWritable(expr, exprType)) {
                return false;
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

    // An arm that was already checked keeps its recorded type: checking it again would run outside the scope of the
    // bindings it was checked under, and would track its moves a second time.
    const auto checkedType = [&](const Expr &arm) {
        const auto checked = expressionTypes.find(&arm);
        return checked != expressionTypes.end() ? checked->second : CheckExpr(arm);
    };

    if (const auto *ternary = dynamic_cast<const TernaryExpr *>(&expr)) {
        const TypeRef thenType = checkedType(*ternary->thenExpr);
        const TypeRef elseType = checkedType(*ternary->elseExpr);
        if (CanAssignExprTo(*ternary->thenExpr, thenType, targetType) &&
            CanAssignExprTo(*ternary->elseExpr, elseType, targetType)) {
            return true;
        }
    }

    // A match reaches a native expected type arm by arm, the way a ternary does, so `none` in one arm and `.Some(v)`
    // in another both take the expected type. The match then has that type: converting its provisional type as a
    // whole would treat an open `opaque?` as absence whichever arm ran.
    if (const auto *match = dynamic_cast<const MatchExpr *>(&expr);
        match && !targetType.IsUnknown() && (MentionsNativeType(targetType) || exprType.MentionsIncompleteNative())) {
        bool accepted = true;
        for (const auto &arm : match->arms) {
            if (IsDivergingExpression(*arm.body)) {
                continue;
            }
            const auto checked = expressionTypes.find(arm.body.get());
            if (checked == expressionTypes.end() || !CanAssignExprTo(*arm.body, checked->second, targetType)) {
                accepted = false;
                break;
            }
        }
        if (accepted) {
            expressionTypes.insert_or_assign(&expr, targetType);
            nativeConversions.erase(&expr);
            return true;
        }
        // An open match has no type of its own to convert, so an arm the expected type refuses refuses the match.
        if (exprType.MentionsIncompleteNative()) {
            return false;
        }
    }

    if (MentionsNativeType(targetType) || MentionsNativeType(exprType)) {
        return CanConvertToNativeType(expr, exprType, targetType);
    }

    if (exprType.CanReadScalarTo(targetType)) {
        static_cast<void>(ReadBorrowedScalar(expr, exprType));
        return true;
    }

    return exprType.IsAssignableTo(targetType) || (IsNullLiteral(expr) && targetType.kind == TypeRef::Kind::Pointer) ||
           UnsuffixedIntegerLiteralFits(expr, targetType) || TypeImplementsInterface(exprType, targetType);
}

bool AnalysisContext::CanConstructNative(const NativeConstructExpr &construct, const TypeRef &targetType) {
    // The constructor selects the outermost level of its own form. A `.Some` may still sit inside the success of an
    // expected fallible, and a `.Success` or `.Failure` inside the presence of an expected optional, each a wrapper
    // the context constructs around it; any other expected form is diagnosed.
    const bool optionalForm = construct.kind == NativeConstructExpr::Kind::Some;
    std::vector<NativeConversionStep> route;
    TypeRef level = targetType;
    while (optionalForm ? level.IsFallible() : level.IsOptional()) {
        route.push_back(NativeConversionStep{optionalForm ? NativeConversionStep::Kind::Success
                                                          : NativeConversionStep::Kind::Presence});
        TypeRef payload = level.inner.front();
        level = std::move(payload);
    }
    if (level.IsUnknown() || targetType.IsUnknown()) {
        return true;
    }
    if (optionalForm ? !level.IsOptional() || level.IsNoneValue() : !level.IsFallible() || level.IsIncompleteNative()) {
        nativeConversions.erase(&construct);
        return false;
    }
    if (!construct.operand) {
        return true;
    }
    const TypeRef &channel = construct.kind == NativeConstructExpr::Kind::Failure ? level.inner[1] : level.inner[0];
    const auto checked = expressionTypes.find(construct.operand.get());
    const TypeRef operandType = checked != expressionTypes.end() ? checked->second : CheckExpr(*construct.operand);
    if (!CanAssignExprTo(*construct.operand, operandType, channel)) {
        nativeConversions.erase(&construct);
        return false;
    }
    if (route.empty()) {
        nativeConversions.erase(&construct);
    }
    else {
        nativeConversions.insert_or_assign(&construct, std::move(route));
    }
    return true;
}

bool AnalysisContext::CanConvertToNativeType(const Expr &expr, const TypeRef &exprType, const TypeRef &targetType) {
    std::vector<NativeConversionStep> route;
    const auto record = [&](const bool accepted) {
        // A conversion is recorded only when it is the accepted one; a speculative overload check that later loses, or
        // an identity, leaves no stale route behind.
        if (accepted && !(route.size() == 1 && route.front().kind == NativeConversionStep::Kind::Identity) &&
            !route.empty()) {
            nativeConversions.insert_or_assign(&expr, route);
        }
        else {
            nativeConversions.erase(&expr);
        }
        return accepted;
    };

    // An unsuffixed literal has no width of its own yet. It descends through presence and success levels, which it can
    // only enter as a present or successful value, and then targets a sum by its literal kind, never by its value.
    const auto *literal = dynamic_cast<const LiteralExpr *>(&expr);
    const bool floatLiteral =
        literal && literal->token.kind == TokenKind::FloatLiteral && NumericLiteralSuffix(literal->token.text).empty();
    if (IsUnsuffixedIntegerLiteral(expr) || floatLiteral) {
        TypeRef level = targetType;
        while (level.IsOptional() || level.IsFallible()) {
            route.push_back(NativeConversionStep{level.IsOptional() ? NativeConversionStep::Kind::Presence
                                                                    : NativeConversionStep::Kind::Success});
            TypeRef payload = level.inner.front();
            level = std::move(payload);
        }
        if (!level.IsSum()) {
            route.push_back(NativeConversionStep{NativeConversionStep::Kind::Identity});
            return record(!MentionsNativeType(level) && CanAssignExprTo(expr, exprType, level));
        }
        std::optional<std::size_t> member;
        for (std::size_t index = 0; index < level.inner.size(); ++index) {
            const TypeRef &candidate = level.inner[index];
            if (floatLiteral ? candidate.IsFloat() : candidate.IsInteger()) {
                if (member) {
                    return record(false);
                }
                member = index;
            }
        }
        if (!member) {
            return record(false);
        }
        route.push_back(NativeConversionStep{NativeConversionStep::Kind::Inject, *member});
        return record(floatLiteral || UnsuffixedIntegerLiteralFits(expr, level.inner[*member]));
    }

    NativeConversion conversion = ClassifyNativeConversion(exprType, targetType);
    route = std::move(conversion.route);
    return record(conversion.Accepted());
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
            // A range where the length goes is the nearest miss to the slice spelling, so it is answered as one.
            std::optional<std::string> help;
            if (dynamic_cast<const RangeExpr *>(array->size.get())) {
                help = "write 'T[..]' for a slice";
            }
            EmitError(array->size->location, "array length must be a non-negative compile-time integer", {},
                      std::move(help));
        }
        ValidateArrayType(*array->element);
        return;
    }
    if (const auto *slice = dynamic_cast<const SliceTypeExpr *>(&type)) {
        ValidateArrayType(*slice->element);
        return;
    }
    if (const auto *range = dynamic_cast<const RangeTypeExpr *>(&type)) {
        if (range->start) {
            ValidateArrayType(*range->start);
        }
        if (range->end) {
            ValidateArrayType(*range->end);
        }
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
        return;
    }
    for (const TypeExpr *child : NativeTypeChildren(type)) {
        ValidateArrayType(*child);
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
    const ScopedTypeOwner owner(*this, expr);
    TypeRef type = ResolveTypeImpl(expr);
    if (!type.IsUnknown()) {
        typeNodeTypes.insert_or_assign(&expr, type);
        if (const auto *named = dynamic_cast<const NamedTypeExpr *>(&expr)) {
            if (const Symbol *symbol = currentScope->Lookup(named->name)) {
                if (const Decl *binding = IntrinsicTypeBinding(*symbol)) {
                    intrinsicTypeBindings[&expr] = binding;
                }
            }
        }
    }
    return type;
}
} // namespace Rux::SemanticDetail
