#include "Semantic/Detail/SemanticAnalyzerContext.h"

#include <format>

namespace Rux::SemanticDetail {
namespace {
bool ContainsTypeParam(const TypeRef &type) {
    if (type.kind == TypeRef::Kind::TypeParam) {
        return true;
    }
    return std::ranges::any_of(type.inner, [](const TypeRef &inner) { return ContainsTypeParam(inner); });
}

TypeRef SubstituteTypeParams(TypeRef type, const std::unordered_map<std::string, TypeRef> &substitutions) {
    if (type.kind == TypeRef::Kind::TypeParam) {
        if (const auto it = substitutions.find(type.name); it != substitutions.end()) {
            return it->second;
        }
        return type;
    }
    for (TypeRef &inner : type.inner) {
        inner = SubstituteTypeParams(std::move(inner), substitutions);
    }
    return type;
}

bool IsNullLiteral(const Expr &expression) {
    const auto *literal = dynamic_cast<const LiteralExpr *>(&expression);
    return literal && literal->token.kind == TokenKind::NullKeyword;
}

std::string_view BinaryOperatorName(const TokenKind op) noexcept {
    using TK = TokenKind;
    switch (op) {
    case TK::Plus:
        return "+";
    case TK::Minus:
        return "-";
    case TK::Star:
        return "*";
    case TK::Slash:
        return "/";
    case TK::Percent:
        return "%";
    case TK::StarStar:
        return "**";
    case TK::Amp:
        return "&";
    case TK::At:
        return "@";
    case TK::Pipe:
        return "|";
    case TK::Caret:
        return "^";
    case TK::LessLess:
        return "<<";
    case TK::GreaterGreater:
        return ">>";
    case TK::GreaterGreaterGreater:
        return ">>>";
    case TK::AmpAmp:
        return "&&";
    case TK::PipePipe:
        return "||";
    case TK::Equal:
        return "==";
    case TK::BangEqual:
        return "!=";
    case TK::Less:
        return "<";
    case TK::LessEqual:
        return "<=";
    case TK::Greater:
        return ">";
    case TK::GreaterEqual:
        return ">=";
    default:
        return {};
    }
}
} // namespace

std::optional<TypeRef> SemanticAnalyzerContext::CheckBasicExpression(const Expr &expression) {
    if (const auto *literal = dynamic_cast<const LiteralExpr *>(&expression)) {
        return LiteralType(literal->token);
    }
    if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expression)) {
        if (unary->op == TokenKind::PlusPlus || unary->op == TokenKind::MinusMinus) {
            CheckMutability(*unary->operand);
        }
        TypeRef operandType = CheckExpr(*unary->operand);
        if (unary->op == TokenKind::At) {
            operandType.isMut = PlaceIsWritable(*unary->operand, operandType);
            return TypeRef::MakePointer(std::move(operandType));
        }
        return CheckUnary(unary->op, operandType, unary->location);
    }
    if (const auto *postfix = dynamic_cast<const PostfixExpr *>(&expression)) {
        CheckMutability(*postfix->operand);
        TypeRef operandType = CheckExpr(*postfix->operand);
        if (!operandType.IsUnknown() && !operandType.IsNumeric()) {
            EmitError(postfix->location,
                      std::format("'{}' applied to non-numeric type '{}'",
                                  postfix->op == TokenKind::PlusPlus ? "++" : "--", operandType.ToString()));
        }
        return operandType;
    }
    if (const auto *binary = dynamic_cast<const BinaryExpr *>(&expression)) {
        TypeRef left = CheckExpr(*binary->left);
        TypeRef right = CheckExpr(*binary->right);
        return CheckBinary(binary->op, left, right, *binary->left, *binary->right, binary->location);
    }
    if (const auto *assignment = dynamic_cast<const AssignExpr *>(&expression)) {
        CheckMutability(*assignment->target);
        TypeRef target = CheckExpr(*assignment->target);
        TypeRef value = CheckExpr(*assignment->value);
        if (assignment->op == TokenKind::GreaterGreaterGreaterAssign) {
            if (!target.IsSigned()) {
                EmitError(assignment->location,
                          std::format("'>>>=' requires a signed integer target, got '{}'", target.ToString()));
            }
            if (!value.IsInteger()) {
                EmitError(assignment->location,
                          std::format("'>>>=' right operand must be an integer, got '{}'", value.ToString()));
            }
        }
        if (!target.IsUnknown() && !value.IsUnknown() && !CanAssignExprTo(*assignment->value, value, target)) {
            EmitError(assignment->location, AssignmentErrorMessage(*assignment->value, target,
                                                                   std::format("cannot assign '{}' to '{}'",
                                                                               value.ToString(), target.ToString())));
        }
        return TypeRef::MakeOpaque();
    }
    if (const auto *cast = dynamic_cast<const CastExpr *>(&expression)) {
        const TypeRef operandType = CheckExpr(*cast->operand);
        TypeRef targetType = ResolveType(*cast->type);
        ValidateCastConstant(*cast, operandType, targetType);
        return targetType;
    }
    return std::nullopt;
}

void SemanticAnalyzerContext::ValidateDeferredBasicExpressionChecks(
    const FuncDecl &declaration, const std::unordered_map<std::string, TypeRef> &substitutions) {
    if (const auto it = deferredUnaryChecks.find(&declaration); it != deferredUnaryChecks.end()) {
        for (const DeferredUnaryCheck &check : it->second) {
            static_cast<void>(CheckUnary(check.op, SubstituteTypeParams(check.operand, substitutions), check.location));
        }
    }
    if (const auto it = deferredBinaryChecks.find(&declaration); it != deferredBinaryChecks.end()) {
        for (const DeferredBinaryCheck &check : it->second) {
            static_cast<void>(CheckBinary(check.op, SubstituteTypeParams(check.left, substitutions),
                                          SubstituteTypeParams(check.right, substitutions), *check.leftExpression,
                                          *check.rightExpression, check.location));
        }
    }
}

TypeRef SemanticAnalyzerContext::CheckUnary(const TokenKind op, const TypeRef &operand, const SourceLocation location) {
    if (operand.IsUnknown()) {
        return TypeRef::MakeUnknown();
    }
    if (operand.kind == TypeRef::Kind::TypeParam &&
        (op == TokenKind::Bang || op == TokenKind::Minus || op == TokenKind::Tilde || op == TokenKind::PlusPlus ||
         op == TokenKind::MinusMinus)) {
        if (currentFunctionDecl) {
            deferredUnaryChecks[currentFunctionDecl].push_back({op, operand, location});
        }
        return op == TokenKind::Bang ? TypeRef::MakeBool() : operand;
    }
    switch (op) {
    case TokenKind::Bang:
        if (!operand.IsBool()) {
            EmitError(location, std::format("'!' applied to non-bool type '{}'", operand.ToString()));
        }
        return TypeRef::MakeBool();
    case TokenKind::Minus:
        if (!operand.IsNumeric()) {
            EmitError(location, std::format("unary '-' applied to non-numeric type '{}'", operand.ToString()));
        }
        return operand;
    case TokenKind::Tilde:
        if (!operand.IsInteger() && !operand.IsBool()) {
            EmitError(location, std::format("'~' applied to non-integer type '{}'", operand.ToString()));
        }
        return operand;
    case TokenKind::Star:
        if (operand.kind != TypeRef::Kind::Pointer) {
            EmitError(location, std::format("'*' (dereference) applied to non-pointer type '{}'", operand.ToString()));
        }
        return operand.inner.empty() ? TypeRef::MakeUnknown() : operand.inner[0];
    case TokenKind::At:
        return TypeRef::MakePointer(operand);
    case TokenKind::PlusPlus:
    case TokenKind::MinusMinus:
        if (!operand.IsNumeric()) {
            EmitError(location, std::format("'{}' applied to non-numeric type '{}'",
                                            op == TokenKind::PlusPlus ? "++" : "--", operand.ToString()));
        }
        return operand;
    default:
        return TypeRef::MakeUnknown();
    }
}

TypeRef SemanticAnalyzerContext::CheckBinary(const TokenKind op, const TypeRef &left, const TypeRef &right,
                                             const Expr &leftExpression, const Expr &rightExpression,
                                             const SourceLocation location) {
    const bool comparesNullPointer = (IsNullLiteral(leftExpression) && right.kind == TypeRef::Kind::Pointer) ||
                                     (IsNullLiteral(rightExpression) && left.kind == TypeRef::Kind::Pointer);
    if (comparesNullPointer && (op == TokenKind::Equal || op == TokenKind::BangEqual)) {
        return TypeRef::MakeBool();
    }
    if (left.IsUnknown() || right.IsUnknown()) {
        return TypeRef::MakeUnknown();
    }
    if (ContainsTypeParam(left) || ContainsTypeParam(right)) {
        if (currentFunctionDecl) {
            deferredBinaryChecks[currentFunctionDecl].push_back(
                {op, left, right, &leftExpression, &rightExpression, location});
        }
        switch (op) {
        case TokenKind::AmpAmp:
        case TokenKind::PipePipe:
        case TokenKind::Equal:
        case TokenKind::BangEqual:
        case TokenKind::Less:
        case TokenKind::LessEqual:
        case TokenKind::Greater:
        case TokenKind::GreaterEqual:
            return TypeRef::MakeBool();
        default:
            return left;
        }
    }

    const std::string_view operatorName = BinaryOperatorName(op);
    if (!operatorName.empty()) {
        if (const FuncDecl *method = LookupOperatorMethod(left, std::string(operatorName), {right})) {
            const std::vector<TypeRef> parameterTypes = ResolveOperatorParameterTypes(left, *method);
            TypeRef returnType = ResolveOperatorReturnType(left, *method);
            if (parameterTypes.size() != 1) {
                EmitError(location,
                          std::format("operator '{}' expects 1 argument, got {}", operatorName, parameterTypes.size()));
            }
            else if (!parameterTypes[0].IsUnknown() && !CanAssignExprTo(rightExpression, right, parameterTypes[0])) {
                EmitError(rightExpression.location, std::format("cannot pass '{}' to parameter of type '{}'",
                                                                right.ToString(), parameterTypes[0].ToString()));
            }
            return returnType;
        }
    }

    const auto isNumericOrChar = [](const TypeRef &type) {
        return type.IsNumeric() || type.kind == TypeRef::Kind::Char8 || type.kind == TypeRef::Kind::Char16 ||
               type.kind == TypeRef::Kind::Char32;
    };
    const auto isIntegerOrChar = [](const TypeRef &type) {
        return type.IsInteger() || type.kind == TypeRef::Kind::Char8 || type.kind == TypeRef::Kind::Char16 ||
               type.kind == TypeRef::Kind::Char32;
    };
    const auto isChar = [](const TypeRef::Kind kind) {
        return kind == TypeRef::Kind::Char8 || kind == TypeRef::Kind::Char16 || kind == TypeRef::Kind::Char32;
    };
    const auto compatibleType = [&](const Expr &leftExpr, const TypeRef &leftType, const Expr &rightExpr,
                                    const TypeRef &rightType) -> std::optional<TypeRef> {
        if ((leftType.IsInteger() && isChar(rightType.kind)) || (rightType.IsInteger() && isChar(leftType.kind))) {
            return leftType.IsInteger() ? leftType : rightType;
        }
        if (isChar(leftType.kind) && isChar(rightType.kind)) {
            return leftType;
        }
        if (leftType.IsInteger() && rightType.IsInteger()) {
            return leftType;
        }
        if (CanAssignExprTo(rightExpr, rightType, leftType)) {
            return leftType;
        }
        if (CanAssignExprTo(leftExpr, leftType, rightType)) {
            return rightType;
        }
        return std::nullopt;
    };

    using TK = TokenKind;
    switch (op) {
    case TK::Plus: {
        if (left.kind == TypeRef::Kind::Pointer && isIntegerOrChar(right)) {
            return left;
        }
        if (isIntegerOrChar(left) && right.kind == TypeRef::Kind::Pointer) {
            return right;
        }
        if (!isNumericOrChar(left)) {
            EmitError(location, std::format("'+' applied to non-numeric type '{}'", left.ToString()));
        }
        else if (!isNumericOrChar(right)) {
            EmitError(location, std::format("'+' right operand must be numeric, got '{}'", right.ToString()));
        }
        else if (const auto result = compatibleType(leftExpression, left, rightExpression, right)) {
            return *result;
        }
        else {
            EmitError(location,
                      std::format("mismatched types in addition: '{}' and '{}'", left.ToString(), right.ToString()));
        }
        return left;
    }
    case TK::Minus: {
        if (left.kind == TypeRef::Kind::Pointer && isIntegerOrChar(right)) {
            return left;
        }
        if (!isNumericOrChar(left)) {
            EmitError(location, std::format("'-' applied to non-numeric type '{}'", left.ToString()));
        }
        else if (!isNumericOrChar(right)) {
            EmitError(location, std::format("'-' right operand must be numeric, got '{}'", right.ToString()));
        }
        else if (const auto result = compatibleType(leftExpression, left, rightExpression, right)) {
            return *result;
        }
        else {
            EmitError(location,
                      std::format("mismatched types in subtraction: '{}' and '{}'", left.ToString(), right.ToString()));
        }
        return left;
    }
    case TK::Star:
    case TK::Slash:
    case TK::Percent:
    case TK::StarStar: {
        const std::string operatorText = op == TK::Star ? "*" : op == TK::Slash ? "/" : op == TK::Percent ? "%" : "**";
        if (!isNumericOrChar(left)) {
            EmitError(location, std::format("'{}' applied to non-numeric type '{}'", operatorText, left.ToString()));
        }
        else if (!isNumericOrChar(right)) {
            EmitError(location,
                      std::format("'{}' right operand must be numeric, got '{}'", operatorText, right.ToString()));
        }
        else if (const auto result = compatibleType(leftExpression, left, rightExpression, right)) {
            return *result;
        }
        else {
            EmitError(location, std::format("mismatched types in binary operation: '{}' and '{}'", left.ToString(),
                                            right.ToString()));
        }
        return left;
    }
    case TK::Amp:
    case TK::Pipe:
    case TK::Caret:
    case TK::LessLess:
    case TK::GreaterGreater: {
        const auto isBitwiseOperand = [](const TypeRef &type) {
            return type.IsInteger() || type.IsBool() || type.kind == TypeRef::Kind::Char8 ||
                   type.kind == TypeRef::Kind::Char16 || type.kind == TypeRef::Kind::Char32;
        };
        if (!isBitwiseOperand(left)) {
            EmitError(location, std::format("bitwise operator applied to non-integer type '{}'", left.ToString()));
        }
        else if (!isBitwiseOperand(right)) {
            EmitError(location,
                      std::format("bitwise operator right operand must be integer, got '{}'", right.ToString()));
        }
        else if (const auto result = compatibleType(leftExpression, left, rightExpression, right)) {
            return *result;
        }
        else {
            EmitError(location, std::format("mismatched types in bitwise operation: '{}' and '{}'", left.ToString(),
                                            right.ToString()));
        }
        return left;
    }
    case TK::GreaterGreaterGreater:
        if (!left.IsSigned()) {
            EmitError(location, std::format("'>>>' requires a signed integer left operand, got '{}'", left.ToString()));
        }
        if (!right.IsInteger()) {
            EmitError(location, std::format("'>>>' right operand must be an integer, got '{}'", right.ToString()));
        }
        return left;
    case TK::AmpAmp:
    case TK::PipePipe:
        if (!left.IsBool()) {
            EmitError(location, std::format("'{}' applied to non-bool type '{}'", op == TK::AmpAmp ? "&&" : "||",
                                            left.ToString()));
        }
        if (!right.IsBool()) {
            EmitError(location, std::format("'{}' applied to non-bool type '{}'", op == TK::AmpAmp ? "&&" : "||",
                                            right.ToString()));
        }
        return TypeRef::MakeBool();
    case TK::Equal:
    case TK::BangEqual:
    case TK::Less:
    case TK::LessEqual:
    case TK::Greater:
    case TK::GreaterEqual: {
        const bool boolIntegerComparison =
            (op == TK::Equal || op == TK::BangEqual) &&
            ((left.IsBool() && right.IsInteger()) || (left.IsInteger() && right.IsBool()));
        if (!boolIntegerComparison && !compatibleType(leftExpression, left, rightExpression, right)) {
            EmitError(location,
                      std::format("cannot compare mismatched types '{}' and '{}'", left.ToString(), right.ToString()));
        }
        return TypeRef::MakeBool();
    }
    default:
        return TypeRef::MakeUnknown();
    }
}

bool SemanticAnalyzerContext::PlaceIsImmutable(const Expr &place) {
    if (const auto *identifier = dynamic_cast<const IdentExpr *>(&place)) {
        const Symbol *symbol = currentScope->Lookup(identifier->name);
        if (!symbol) {
            return false;
        }
        if (symbol->kind == Symbol::Kind::Const) {
            return true;
        }
        return symbol->kind == Symbol::Kind::Var && !symbol->isMut;
    }
    if (const auto *field = dynamic_cast<const FieldExpr *>(&place)) {
        if (dynamic_cast<const SelfExpr *>(field->object.get())) {
            return false;
        }
        const TypeRef objectType = CheckExpr(*field->object);
        if (objectType.kind == TypeRef::Kind::Pointer && !objectType.inner.empty()) {
            return !objectType.inner[0].isMut;
        }
        return PlaceIsImmutable(*field->object);
    }
    if (const auto *index = dynamic_cast<const IndexExpr *>(&place)) {
        const TypeRef objectType = CheckExpr(*index->object);
        if (objectType.kind == TypeRef::Kind::Pointer && !objectType.inner.empty()) {
            return !objectType.inner[0].isMut;
        }
        return PlaceIsImmutable(*index->object);
    }
    if (const auto *unary = dynamic_cast<const UnaryExpr *>(&place); unary && unary->op == TokenKind::Star) {
        const TypeRef pointer = CheckExpr(*unary->operand);
        return pointer.kind == TypeRef::Kind::Pointer && !pointer.inner.empty() && !pointer.inner[0].isMut;
    }
    return false;
}

bool SemanticAnalyzerContext::PlaceIsWritable(const Expr &place, const TypeRef &placeType) {
    if (const auto *unary = dynamic_cast<const UnaryExpr *>(&place); unary && unary->op == TokenKind::Star) {
        return placeType.isMut;
    }
    return !PlaceIsImmutable(place);
}

void SemanticAnalyzerContext::CheckMutability(const Expr &target) {
    if (const auto *identifier = dynamic_cast<const IdentExpr *>(&target)) {
        const Symbol *symbol = currentScope->Lookup(identifier->name);
        if (!symbol) {
            return;
        }
        if (symbol->kind == Symbol::Kind::Const) {
            EmitError(target.location, std::format("cannot assign to constant '{}'", identifier->name));
            return;
        }
        if (symbol->kind == Symbol::Kind::Var && !symbol->isMut) {
            EmitError(target.location, std::format("cannot assign to immutable variable '{}'", identifier->name));
        }
    }
    else if (const auto *unary = dynamic_cast<const UnaryExpr *>(&target); unary && unary->op == TokenKind::Star) {
        const TypeRef pointer = CheckExpr(*unary->operand);
        if (pointer.kind == TypeRef::Kind::Pointer && !pointer.inner.empty() && !pointer.inner[0].isMut) {
            EmitError(target.location, "cannot assign through a pointer to immutable data");
        }
    }
    else if (const auto *field = dynamic_cast<const FieldExpr *>(&target)) {
        if (dynamic_cast<const SelfExpr *>(field->object.get())) {
            return;
        }
        const TypeRef objectType = CheckExpr(*field->object);
        if (objectType.kind == TypeRef::Kind::Pointer && !objectType.inner.empty()) {
            if (!objectType.inner[0].isMut) {
                EmitError(target.location, "cannot assign through a pointer to immutable data");
            }
        }
        else {
            CheckMutability(*field->object);
        }
    }
    else if (const auto *index = dynamic_cast<const IndexExpr *>(&target)) {
        const TypeRef objectType = CheckExpr(*index->object);
        if (objectType.kind == TypeRef::Kind::Pointer && !objectType.inner.empty()) {
            if (!objectType.inner[0].isMut) {
                EmitError(target.location, "cannot assign through a pointer to immutable data");
            }
        }
        else {
            CheckMutability(*index->object);
        }
    }
}
} // namespace Rux::SemanticDetail
