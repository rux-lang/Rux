// Lowering for the basic expression forms — literals, names, operators — and
// the type substitution a generic body needs while they are lowered.

#include "Lowering/AstToHir/Detail/AstToHirContext.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdlib>
#include <string_view>
#include <utility>

namespace Rux::AstToHirDetail {
namespace {
bool IsIdentifierCharacter(const char character) {
    return std::isalnum(static_cast<unsigned char>(character)) || character == '_';
}

/// Replace type parameters inside a written type name with the arguments of this instantiation, so a generic body
/// lowers once per concrete set of arguments.
std::string SubstituteTypeName(std::string name, const std::unordered_map<std::string, TypeRef> &substitutions) {
    std::vector<std::pair<std::string_view, const TypeRef *>> ordered;
    ordered.reserve(substitutions.size());
    for (const auto &[parameter, type] : substitutions) {
        ordered.emplace_back(parameter, &type);
    }
    std::ranges::sort(ordered, [](const auto &left, const auto &right) {
        return left.first.size() != right.first.size() ? left.first.size() > right.first.size()
                                                       : left.first < right.first;
    });

    for (const auto &[parameter, type] : ordered) {
        std::size_t position = 0;
        while ((position = name.find(parameter, position)) != std::string::npos) {
            const bool beginsIdentifier = position != 0 && IsIdentifierCharacter(name[position - 1]);
            const std::size_t end = position + parameter.size();
            const bool endsIdentifier = end < name.size() && IsIdentifierCharacter(name[end]);
            if (beginsIdentifier || endsIdentifier) {
                position = end;
                continue;
            }
            const std::string replacement = type->ToString();
            name.replace(position, parameter.size(), replacement);
            position += replacement.size();
        }
    }
    return name;
}

/// Substitute type parameters throughout a type, recursing into pointees, elements, and type arguments so a parameter
/// is replaced wherever it is nested.
TypeRef SubstituteType(TypeRef type, const std::unordered_map<std::string, TypeRef> &substitutions) {
    if (type.kind == TypeRef::Kind::TypeParam || type.kind == TypeRef::Kind::Named) {
        if (const auto substitution = substitutions.find(type.name); substitution != substitutions.end()) {
            TypeRef substituted = substitution->second;
            // `*var T` marks the pointee writable on the `T` slot, so the substitution carries that mark onto
            // whatever `T` turns out to be -- here as much as in analysis, or the two sides name the same
            // instantiation differently and the layout registered under one name is looked up under the other.
            substituted.isMut = type.isMut;
            return substituted;
        }
        if (type.kind == TypeRef::Kind::Named) {
            type.name = SubstituteTypeName(std::move(type.name), substitutions);
        }
    }
    for (TypeRef &inner : type.inner) {
        inner = SubstituteType(std::move(inner), substitutions);
    }
    return type;
}

} // namespace

TypeRef AstToHirContext::RestoreEnumLayoutMarkers(TypeRef type) const {
    for (TypeRef &inner : type.inner) {
        inner = RestoreEnumLayoutMarkers(std::move(inner));
    }
    // Any enum, not only an instantiated one. Substitution drops what `inner` said, and `inner` is the only place a
    // type says how large it is; an enum left without it is sized as a pointer, so a write through `*var E` in a
    // generic body stored eight bytes into whatever room the real enum had.
    if (type.kind != TypeRef::Kind::Named || !type.inner.empty() || !enumDecls.contains(BaseTypeName(type.name))) {
        return type;
    }
    if (const ResolvedTypeLayout *layout = model.TryGetLayout(type)) {
        type.inner.push_back(TypeRef::MakeArray(TypeRef::MakeChar8(), layout->size));
    }
    return type;
}

TypeRef AstToHirContext::ResolvedExpressionType(const Expr &expression) const {
    const TypeRef *type = model.TryGetType(expression);
    if (!type) {
        assert(!dynamic_cast<const LiteralExpr *>(&expression) && "literal is missing a semantic type fact");
        assert(!dynamic_cast<const IdentExpr *>(&expression) && "identifier is missing a semantic type fact");
        assert(!dynamic_cast<const SelfExpr *>(&expression) && "self expression is missing a semantic type fact");
        assert(!dynamic_cast<const UnaryExpr *>(&expression) && "unary expression is missing a semantic type fact");
        assert(!dynamic_cast<const PostfixExpr *>(&expression) && "postfix expression is missing a semantic type fact");
        assert(!dynamic_cast<const BinaryExpr *>(&expression) && "binary expression is missing a semantic type fact");
        assert(!dynamic_cast<const AssignExpr *>(&expression) && "assignment is missing a semantic type fact");
        assert(!dynamic_cast<const TernaryExpr *>(&expression) && "ternary expression is missing a semantic type fact");
        assert(!dynamic_cast<const IndexExpr *>(&expression) && "index expression is missing a semantic type fact");
        assert(!dynamic_cast<const FieldExpr *>(&expression) && "field expression is missing a semantic type fact");
        assert(!dynamic_cast<const CastExpr *>(&expression) && "cast expression is missing a semantic type fact");
        assert(!dynamic_cast<const IsExpr *>(&expression) && "type check is missing a semantic type fact");
        assert(false && "accepted expression is missing a required semantic type fact");
        std::abort();
    }
    TypeRef substituted = RestoreEnumLayoutMarkers(SubstituteType(*type, currentSubstitutions));
    NoteStructInstantiation(substituted);
    return substituted;
}

TypeRef AstToHirContext::SubstituteCurrentType(TypeRef type) const {
    return RestoreEnumLayoutMarkers(SubstituteType(std::move(type), currentSubstitutions));
}

/// The name to lower an identifier under, which for one naming a function is that function's linker name.
///
/// A call says which function it means through its binding, but an identifier used as a value carried only what was
/// written -- and what was written is the name before anything disambiguates it. The back end looks the name up
/// among the functions it emitted, found nothing when two packages made the name ambiguous, and fell through to
/// loading a global that does not exist: a function pointer built out of whatever the address held, which faults
/// when it is called. A name that resolves to anything else, a local above all, is left exactly as written.
const std::string &AstToHirContext::FunctionReferenceName(const IdentExpr &identifier) {
    const HirSymbol *symbol = currentScope->Lookup(identifier.name);
    if (!symbol || symbol->kind != HirSymbol::Kind::Func || symbol->funcOverloads.empty()) {
        return identifier.name;
    }
    const TypeRef referenceType = ResolvedExpressionType(identifier);
    const FuncDecl *chosen = nullptr;
    for (const FuncDecl *candidate : symbol->funcOverloads) {
        if (!candidate->typeParams.empty()) {
            continue;
        }
        // Overloads share a name and differ by signature, so the type the reference resolved to is what picks one.
        if (symbol->funcOverloads.size() > 1 &&
            !(MakeFuncType(candidate->params, candidate->returnType, {}) == referenceType)) {
            continue;
        }
        // The last match rather than the first, because declarations arrive dependencies first and the package
        // being built last -- so the nearest declaration of a name is the one at the end. Two packages declaring
        // the same signature is the case this decides, and it decides it the way a call already does: the package
        // that wrote the reference means its own.
        chosen = candidate;
    }
    if (!chosen) {
        return identifier.name;
    }
    const ResolvedSymbolIdentity *identity = model.TryGetSymbolIdentity(*chosen);
    return identity && !identity->linkerName.empty() ? identity->linkerName : identifier.name;
}

HirExprPtr AstToHirContext::LowerBasicExpr(const Expr &expression) {
    if (const auto *move = dynamic_cast<const MoveExpr *>(&expression)) {
        return LowerExpr(*move->operand);
    }
    if (const auto *literal = dynamic_cast<const LiteralExpr *>(&expression)) {
        auto lowered = std::make_unique<HirLiteralExpr>();
        lowered->location = literal->location;
        lowered->type = ResolvedExpressionType(*literal);
        lowered->value = LowerLiteralValue(*literal);
        return lowered;
    }

    if (const auto *identifier = dynamic_cast<const IdentExpr *>(&expression)) {
        if (HirExprPtr compilerValue = LowerCompilerParamIdentifier(*identifier)) {
            return compilerValue;
        }
        auto lowered = std::make_unique<HirVarExpr>();
        lowered->location = identifier->location;
        lowered->name = FunctionReferenceName(*identifier);
        lowered->type = ResolvedExpressionType(*identifier);
        return lowered;
    }

    if (dynamic_cast<const SelfExpr *>(&expression)) {
        auto lowered = std::make_unique<HirSelfExpr>();
        lowered->location = expression.location;
        lowered->type = ResolvedExpressionType(expression);
        return lowered;
    }

    if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expression)) {
        auto lowered = std::make_unique<HirUnaryExpr>();
        lowered->location = unary->location;
        lowered->op = unary->op;
        lowered->operand = LowerExpr(*unary->operand);
        lowered->type = ResolvedExpressionType(*unary);
        return lowered;
    }

    if (const auto *tryExpression = dynamic_cast<const TryExpr *>(&expression)) {
        return LowerTryExpr(*tryExpression);
    }

    if (const auto *postfix = dynamic_cast<const PostfixExpr *>(&expression)) {
        auto lowered = std::make_unique<HirPostfixExpr>();
        lowered->location = postfix->location;
        lowered->op = postfix->op;
        lowered->operand = LowerExpr(*postfix->operand);
        lowered->type = ResolvedExpressionType(*postfix);
        return lowered;
    }

    if (const auto *binary = dynamic_cast<const BinaryExpr *>(&expression)) {
        auto lowerOperand = [&](const Expr &operand, const Expr &other) {
            if (IsNullLiteral(operand)) {
                const TypeRef otherType = ResolvedExpressionType(other);
                if (otherType.kind == TypeRef::Kind::Pointer) {
                    return LowerExprAs(operand, otherType);
                }
            }
            return LowerExpr(operand);
        };
        HirExprPtr left = lowerOperand(*binary->left, *binary->right);
        HirExprPtr right = lowerOperand(*binary->right, *binary->left);
        if (const ResolvedVariantEquality *equality = model.TryGetVariantEquality(*binary)) {
            return LowerVariantEquality(*binary, std::move(left), std::move(right), *equality);
        }
        if (HirExprPtr overloaded = TryLowerOverloadedBinary(*binary, left, right)) {
            overloaded->type = ResolvedExpressionType(*binary);
            return overloaded;
        }

        auto lowered = std::make_unique<HirBinaryExpr>();
        lowered->location = binary->location;
        lowered->op = binary->op;
        lowered->left = std::move(left);
        lowered->right = std::move(right);
        lowered->type = ResolvedExpressionType(*binary);
        return lowered;
    }

    if (const auto *assignment = dynamic_cast<const AssignExpr *>(&expression)) {
        auto lowered = std::make_unique<HirAssignExpr>();
        lowered->location = assignment->location;
        lowered->op = assignment->op;
        lowered->target = LowerExpr(*assignment->target);
        lowered->value = LowerExprAs(*assignment->value, lowered->target->type);
        if (assignment->op == TokenKind::Assign || assignment->op == TokenKind::MoveArrow) {
            lowered->overwriteCleanup = OverwriteCleanup(*lowered->target, assignment->location);
        }
        // Assignment's semantic result is opaque because it is not a value,
        // while HIR needs the checked place type to select the store width.
        static_cast<void>(ResolvedExpressionType(*assignment));
        lowered->type = ResolvedExpressionType(*assignment->target);
        return lowered;
    }

    if (const auto *ternary = dynamic_cast<const TernaryExpr *>(&expression)) {
        auto lowered = std::make_unique<HirTernaryExpr>();
        lowered->location = ternary->location;
        lowered->condition = LowerExpr(*ternary->condition);
        lowered->type = ResolvedExpressionType(*ternary);
        lowered->thenExpr = IsNullLiteral(*ternary->thenExpr) ? LowerExprAs(*ternary->thenExpr, lowered->type)
                                                              : LowerExpr(*ternary->thenExpr);
        lowered->elseExpr = IsNullLiteral(*ternary->elseExpr) ? LowerExprAs(*ternary->elseExpr, lowered->type)
                                                              : LowerExpr(*ternary->elseExpr);
        return lowered;
    }

    if (const auto *index = dynamic_cast<const IndexExpr *>(&expression)) {
        auto lowered = std::make_unique<HirIndexExpr>();
        lowered->location = index->location;
        lowered->object = LowerExpr(*index->object);
        lowered->index = LowerExpr(*index->index);
        lowered->type = ResolvedExpressionType(*index);
        return lowered;
    }

    if (const auto *field = dynamic_cast<const FieldExpr *>(&expression)) {
        if (HirExprPtr compilerValue = LowerCompilerParamFieldExpression(*field)) {
            return compilerValue;
        }
        auto lowered = std::make_unique<HirFieldExpr>();
        lowered->location = field->location;
        lowered->object = LowerExpr(*field->object);
        lowered->field = field->field;
        lowered->type = ResolvedExpressionType(*field);
        return lowered;
    }

    if (const auto *cast = dynamic_cast<const CastExpr *>(&expression)) {
        auto lowered = std::make_unique<HirCastExpr>();
        lowered->location = cast->location;
        lowered->targetType = ResolvedExpressionType(*cast);
        lowered->operand = IsNullLiteral(*cast->operand) ? LowerExprAs(*cast->operand, lowered->targetType)
                                                         : LowerExpr(*cast->operand);
        lowered->type = lowered->targetType;
        return lowered;
    }

    if (const auto *typeCheck = dynamic_cast<const IsExpr *>(&expression)) {
        auto lowered = std::make_unique<HirLiteralExpr>();
        lowered->location = typeCheck->location;
        lowered->value = LowerExpr(*typeCheck->operand)->type == ResolveType(*typeCheck->type) ? "true" : "false";
        lowered->type = ResolvedExpressionType(*typeCheck);
        return lowered;
    }

    return nullptr;
}
} // namespace Rux::AstToHirDetail
