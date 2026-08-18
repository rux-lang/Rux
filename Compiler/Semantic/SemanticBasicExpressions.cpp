// Checking for literals, names, operators and assignment, including the
// mutability rules an assignment target has to satisfy.

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

/// A slice borrows its elements through the read-only `*T` in its `data` field, so writing one writes through that
/// pointer whatever the binding itself is declared as. `MutableSlice` is the writable counterpart.
bool IsSliceType(const TypeRef &type) {
    return type.kind == TypeRef::Kind::Named && (type.name.starts_with("Slice<") || type.name == "Slice");
}

bool IsNullLiteral(const Expr &expression) {
    const auto *literal = dynamic_cast<const LiteralExpr *>(&expression);
    return literal && literal->token.kind == TokenKind::NullKeyword;
}

std::string_view OperatorName(const TokenKind op) noexcept {
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
    case TK::Assign:
        return "=";
    case TK::PlusAssign:
        return "+=";
    case TK::MinusAssign:
        return "-=";
    case TK::StarAssign:
        return "*=";
    case TK::SlashAssign:
        return "/=";
    case TK::PercentAssign:
        return "%=";
    case TK::AmpAssign:
        return "&=";
    case TK::PipeAssign:
        return "|=";
    case TK::CaretAssign:
        return "^=";
    case TK::LessLessAssign:
        return "<<=";
    case TK::GreaterGreaterAssign:
        return ">>=";
    case TK::GreaterGreaterGreaterAssign:
        return ">>>=";
    case TK::Bang:
        return "!";
    case TK::Tilde:
        return "~";
    case TK::PlusPlus:
        return "++";
    case TK::MinusMinus:
        return "--";
    default:
        return {};
    }
}

TokenKind BinaryOperation(const TokenKind op) noexcept {
    using TK = TokenKind;
    switch (op) {
    case TK::PlusAssign:
        return TK::Plus;
    case TK::MinusAssign:
        return TK::Minus;
    case TK::StarAssign:
        return TK::Star;
    case TK::SlashAssign:
        return TK::Slash;
    case TK::PercentAssign:
        return TK::Percent;
    case TK::AmpAssign:
        return TK::Amp;
    case TK::PipeAssign:
        return TK::Pipe;
    case TK::CaretAssign:
        return TK::Caret;
    case TK::LessLessAssign:
        return TK::LessLess;
    case TK::GreaterGreaterAssign:
        return TK::GreaterGreater;
    case TK::GreaterGreaterGreaterAssign:
        return TK::GreaterGreaterGreater;
    default:
        return op;
    }
}

bool IsCharacter(const TypeRef &type) noexcept {
    return type.kind == TypeRef::Kind::Char8 || type.kind == TypeRef::Kind::Char16 ||
           type.kind == TypeRef::Kind::Char32;
}

bool IsAssignablePlace(const Expr &expression) noexcept {
    if (dynamic_cast<const IdentExpr *>(&expression) || dynamic_cast<const FieldExpr *>(&expression) ||
        dynamic_cast<const IndexExpr *>(&expression)) {
        return true;
    }
    const auto *unary = dynamic_cast<const UnaryExpr *>(&expression);
    return unary && unary->op == TokenKind::Star;
}

/// Addresses: a function value is the address of its entry point, so it converts to and from pointers, but never to a
/// numeric or character value.
bool IsAddressValue(const TypeRef &type) noexcept {
    return type.kind == TypeRef::Kind::Pointer || type.kind == TypeRef::Kind::Func;
}

bool IsCastValue(const TypeRef &type) noexcept {
    return type.IsNumeric() || type.IsBool() || IsCharacter(type) || type.kind == TypeRef::Kind::Pointer;
}
} // namespace

std::optional<TypeRef> SemanticAnalyzerContext::CheckBasicExpression(const Expr &expression) {
    if (const auto *literal = dynamic_cast<const LiteralExpr *>(&expression)) {
        return LiteralType(literal->token);
    }
    if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expression)) {
        const bool savedAssignmentTarget = checkingPlainAssignmentTarget;
        checkingPlainAssignmentTarget = checkingPlainAssignmentTarget || unary->op == TokenKind::At;
        TypeRef operandType = CheckExpr(*unary->operand);
        checkingPlainAssignmentTarget = savedAssignmentTarget;
        if (unary->op == TokenKind::PlusPlus || unary->op == TokenKind::MinusMinus) {
            if (!CheckAssignableTarget(*unary->operand, operandType, OperatorName(unary->op))) {
                return operandType;
            }
        }
        if (unary->op == TokenKind::At) {
            checkingPlainAssignmentTarget = true;
            operandType.isMut = PlaceIsWritable(*unary->operand, operandType);
            checkingPlainAssignmentTarget = savedAssignmentTarget;
            if (operandType.isMut) {
                MarkTrackedAssignment(*unary->operand, unary->location);
            }
            return TypeRef::MakePointer(std::move(operandType));
        }
        return CheckUnary(unary->op, operandType, unary->location);
    }
    if (const auto *postfix = dynamic_cast<const PostfixExpr *>(&expression)) {
        TypeRef operandType = CheckExpr(*postfix->operand);
        const bool isAssignable = CheckAssignableTarget(*postfix->operand, operandType, OperatorName(postfix->op));
        if (isAssignable && !operandType.IsUnknown() && !operandType.IsNumeric()) {
            EmitError(postfix->location, std::format("operator '{}' requires a numeric operand, but found '{}'",
                                                     OperatorName(postfix->op), operandType.ToString()));
        }
        return operandType;
    }
    if (const auto *binary = dynamic_cast<const BinaryExpr *>(&expression)) {
        TypeRef left = CheckExpr(*binary->left);
        TypeRef right = CheckExpr(*binary->right);
        return CheckBinary(binary->op, left, right, *binary->left, *binary->right, binary->location);
    }
    if (const auto *assignment = dynamic_cast<const AssignExpr *>(&expression)) {
        const bool savedAssignmentTarget = checkingPlainAssignmentTarget;
        checkingPlainAssignmentTarget = assignment->op == TokenKind::Assign;
        TypeRef target = CheckExpr(*assignment->target);
        checkingPlainAssignmentTarget = savedAssignmentTarget;
        TypeRef value = CheckExpr(*assignment->value);
        checkingPlainAssignmentTarget = assignment->op == TokenKind::Assign;
        const bool isAssignable = CheckAssignableTarget(*assignment->target, target, OperatorName(assignment->op));
        checkingPlainAssignmentTarget = savedAssignmentTarget;
        if (isAssignable && assignment->op != TokenKind::Assign && !target.IsUnknown() && !value.IsUnknown()) {
            const TypeRef result = CheckBinary(assignment->op, target, value, *assignment->target, *assignment->value,
                                               assignment->location);
            if (!result.IsUnknown() && !result.IsAssignableTo(target)) {
                EmitError(assignment->location,
                          std::format("operator '{}' produces '{}', which cannot be stored in target type '{}'",
                                      OperatorName(assignment->op), result.ToString(), target.ToString()));
            }
        }
        else if (isAssignable) {
            const bool compatible =
                target.IsUnknown() || value.IsUnknown() || CanAssignExprTo(*assignment->value, value, target);
            if (!compatible) {
                EmitError(assignment->location,
                          AssignmentErrorMessage(
                              *assignment->value, target,
                              std::format("cannot assign '{}' to '{}'", value.ToString(), target.ToString())));
            }
            else if (assignment->op == TokenKind::Assign) {
                MarkTrackedAssignment(*assignment->target, assignment->location);
            }
        }
        return TypeRef::MakeOpaque();
    }
    if (const auto *cast = dynamic_cast<const CastExpr *>(&expression)) {
        const TypeRef operandType = CheckExpr(*cast->operand);
        TypeRef targetType = ResolveType(*cast->type);
        const auto isCastValue = [&](const TypeRef &type) {
            if (IsCastValue(type)) {
                return true;
            }
            const std::string typeName = NamedBaseTypeName(type);
            return !typeName.empty() && enumDecls.contains(typeName);
        };
        const bool involvesFunc = operandType.kind == TypeRef::Kind::Func || targetType.kind == TypeRef::Kind::Func;
        const bool castable = involvesFunc ? IsAddressValue(operandType) && IsAddressValue(targetType)
                                           : isCastValue(operandType) && isCastValue(targetType);
        if (!operandType.IsUnknown() && !targetType.IsUnknown() && !castable) {
            EmitError(cast->location, std::format("cannot cast value of type '{}' to '{}'", operandType.ToString(),
                                                  targetType.ToString()));
        }
        else {
            ValidateCastConstant(*cast, operandType, targetType);
        }
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
            EmitError(location,
                      std::format("operator '!' requires a bool operand, but found '{}'", operand.ToString()));
        }
        return TypeRef::MakeBool();
    case TokenKind::Minus:
        if (!operand.IsNumeric()) {
            EmitError(location,
                      std::format("operator '-' requires a numeric operand, but found '{}'", operand.ToString()));
        }
        return operand;
    case TokenKind::Tilde:
        if (!operand.IsInteger() && !operand.IsBool()) {
            EmitError(location, std::format("operator '~' requires an integer or bool operand, but found '{}'",
                                            operand.ToString()));
        }
        return operand;
    case TokenKind::Star:
        if (operand.kind != TypeRef::Kind::Pointer) {
            EmitError(location,
                      std::format("operator '*' requires a pointer operand, but found '{}'", operand.ToString()));
        }
        return operand.inner.empty() ? TypeRef::MakeUnknown() : operand.inner[0];
    case TokenKind::At:
        return TypeRef::MakePointer(operand);
    case TokenKind::PlusPlus:
    case TokenKind::MinusMinus:
        if (!operand.IsNumeric()) {
            EmitError(location, std::format("operator '{}' requires a numeric operand, but found '{}'",
                                            OperatorName(op), operand.ToString()));
        }
        return operand;
    default:
        return TypeRef::MakeUnknown();
    }
}

TypeRef SemanticAnalyzerContext::CheckBinary(const TokenKind op, const TypeRef &left, const TypeRef &right,
                                             const Expr &leftExpression, const Expr &rightExpression,
                                             const SourceLocation location) {
    const TokenKind operation = BinaryOperation(op);
    const std::string_view operatorName = OperatorName(op);
    const bool comparesNullPointer = (IsNullLiteral(leftExpression) && right.kind == TypeRef::Kind::Pointer) ||
                                     (IsNullLiteral(rightExpression) && left.kind == TypeRef::Kind::Pointer);
    if (comparesNullPointer && (operation == TokenKind::Equal || operation == TokenKind::BangEqual)) {
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
        switch (operation) {
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

    const std::string_view methodOperatorName = OperatorName(operation);
    if (!methodOperatorName.empty()) {
        if (const FuncDecl *method = LookupOperatorMethod(left, std::string(methodOperatorName), {right})) {
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

    const auto isNumericOrChar = [](const TypeRef &type) { return type.IsNumeric() || IsCharacter(type); };
    const auto isIntegerOrChar = [](const TypeRef &type) { return type.IsInteger() || IsCharacter(type); };
    const auto compatibleType = [&](const Expr &leftExpr, const TypeRef &leftType, const Expr &rightExpr,
                                    const TypeRef &rightType) -> std::optional<TypeRef> {
        if ((leftType.IsInteger() && IsCharacter(rightType)) || (rightType.IsInteger() && IsCharacter(leftType))) {
            return leftType.IsInteger() ? leftType : rightType;
        }
        if (IsCharacter(leftType) && IsCharacter(rightType)) {
            return leftType;
        }
        if (leftType.IsInteger() && rightType.IsInteger()) {
            // Two integers normally settle on the left operand's type. An unsuffixed literal carries the default
            // `int`, though, so that rule made `2 * count` an `int` while `count * 2` was a `uint` — the same
            // expression mirrored, one spelling compiling and the other not. A literal takes the other operand's
            // type whichever side it is written on. The guard is narrow: an `int`-typed expression that is not a
            // literal is not assignable to another width, so only a literal can flip the result.
            if (leftType.kind == TypeRef::Kind::Int && rightType.kind != TypeRef::Kind::Int &&
                CanAssignExprTo(leftExpr, leftType, rightType)) {
                return rightType;
            }
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
    switch (operation) {
    case TK::Plus: {
        if (left.kind == TypeRef::Kind::Pointer && isIntegerOrChar(right)) {
            return left;
        }
        if (isIntegerOrChar(left) && right.kind == TypeRef::Kind::Pointer) {
            return right;
        }
        if (isNumericOrChar(left) && isNumericOrChar(right)) {
            if (const auto result = compatibleType(leftExpression, left, rightExpression, right)) {
                return *result;
            }
        }
        EmitError(location, std::format("operator '{}' cannot combine left operand '{}' with right operand '{}'",
                                        operatorName, left.ToString(), right.ToString()));
        return left;
    }
    case TK::Minus: {
        if (left.kind == TypeRef::Kind::Pointer && isIntegerOrChar(right)) {
            return left;
        }
        if (isNumericOrChar(left) && isNumericOrChar(right)) {
            if (const auto result = compatibleType(leftExpression, left, rightExpression, right)) {
                return *result;
            }
        }
        EmitError(location, std::format("operator '{}' cannot combine left operand '{}' with right operand '{}'",
                                        operatorName, left.ToString(), right.ToString()));
        return left;
    }
    case TK::Star:
    case TK::Slash:
    case TK::Percent:
    case TK::StarStar: {
        if (isNumericOrChar(left) && isNumericOrChar(right)) {
            if (const auto result = compatibleType(leftExpression, left, rightExpression, right)) {
                return *result;
            }
        }
        EmitError(location, std::format("operator '{}' cannot combine left operand '{}' with right operand '{}'",
                                        operatorName, left.ToString(), right.ToString()));
        return left;
    }
    case TK::Amp:
    case TK::Pipe:
    case TK::Caret: {
        const auto isBitwiseOperand = [](const TypeRef &type) {
            return type.IsInteger() || type.IsBool() || IsCharacter(type);
        };
        if (!isBitwiseOperand(left)) {
            EmitError(location,
                      std::format("operator '{}' requires an integer, bool, or character left operand, but found '{}'",
                                  operatorName, left.ToString()));
        }
        else if (!isBitwiseOperand(right)) {
            EmitError(location, std::format("operator '{}' requires an integer, bool, or character right operand, "
                                            "but found '{}'",
                                            operatorName, right.ToString()));
        }
        else if (const auto result = compatibleType(leftExpression, left, rightExpression, right)) {
            return *result;
        }
        else {
            EmitError(location, std::format("operator '{}' cannot combine left operand '{}' with right operand '{}'",
                                            operatorName, left.ToString(), right.ToString()));
        }
        return left;
    }
    case TK::LessLess:
    case TK::GreaterGreater:
        if (!isIntegerOrChar(left)) {
            EmitError(location,
                      std::format("operator '{}' requires an integer or character left operand, but found '{}'",
                                  operatorName, left.ToString()));
        }
        if (!right.IsInteger()) {
            EmitError(location, std::format("operator '{}' requires an integer right operand, but found '{}'",
                                            operatorName, right.ToString()));
        }
        return left;
    case TK::GreaterGreaterGreater:
        if (!left.IsSigned()) {
            EmitError(location, std::format("operator '{}' requires a signed integer left operand, but found '{}'",
                                            operatorName, left.ToString()));
        }
        if (!right.IsInteger()) {
            EmitError(location, std::format("operator '{}' requires an integer right operand, but found '{}'",
                                            operatorName, right.ToString()));
        }
        return left;
    case TK::AmpAmp:
    case TK::PipePipe:
        if (!left.IsBool()) {
            EmitError(location, std::format("operator '{}' requires a bool left operand, but found '{}'", operatorName,
                                            left.ToString()));
        }
        if (!right.IsBool()) {
            EmitError(location, std::format("operator '{}' requires a bool right operand, but found '{}'", operatorName,
                                            right.ToString()));
        }
        return TypeRef::MakeBool();
    case TK::Equal:
    case TK::BangEqual:
    case TK::Less:
    case TK::LessEqual:
    case TK::Greater:
    case TK::GreaterEqual: {
        // A struct has no ordering of its own. Reaching here means no operator method matched, and comparing the
        // operands as machine values would compare their raw bytes — quietly answering a question the type never
        // defined. Derive what can be derived from the operators the type does declare, and reject the rest.
        // Only when both sides are the same struct. Comparing a struct with something else is a plain type mismatch,
        // and the message below says so more usefully than "not defined" would.
        if (left.kind == TypeRef::Kind::Named && right.kind == TypeRef::Kind::Named &&
            BaseTypeName(left.name) == BaseTypeName(right.name) && structDecls.contains(BaseTypeName(left.name))) {
            const auto declares = [&](const std::string &name, const TypeRef &receiver, const TypeRef &argument) {
                return LookupOperatorMethod(receiver, name, {argument}) != nullptr;
            };
            const bool ordered = operation == TK::LessEqual || operation == TK::GreaterEqual;
            const bool derivable = (operation == TK::BangEqual && declares("==", left, right)) ||
                                   (operation == TK::Greater && declares("<", right, left)) ||
                                   (ordered && declares("<", left, right) && declares("==", left, right));
            if (derivable) {
                return TypeRef::MakeBool();
            }

            // `==` and `<` are the operators everything else derives from, so they can only be declared. Anything
            // else reached here because what it derives from is missing too.
            std::string help = std::format("declare '{}' on '{}'", operatorName, left.ToString());
            if (operation == TK::BangEqual) {
                help += ", or the '==' it is derived from";
            }
            else if (operation == TK::Greater) {
                help += ", or the '<' it is derived from";
            }
            else if (ordered) {
                help += ", or the '<' and '==' it is derived from";
            }
            EmitError(location, std::format("operator '{}' is not defined for '{}'", operatorName, left.ToString()),
                      {"a struct is compared through the operators it declares, never by its representation"},
                      std::move(help));
            return TypeRef::MakeBool();
        }

        const bool boolIntegerComparison =
            (operation == TK::Equal || operation == TK::BangEqual) &&
            ((left.IsBool() && right.IsInteger()) || (left.IsInteger() && right.IsBool()));
        if (!boolIntegerComparison && !compatibleType(leftExpression, left, rightExpression, right)) {
            EmitError(location, std::format("operator '{}' cannot compare left operand '{}' with right operand '{}'",
                                            operatorName, left.ToString(), right.ToString()));
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
    // The receiver is a parameter like any other. A receiver taken by value is a copy the method may not rewrite unless
    // it was declared `var self: T`; a receiver taken by reference is decided by what the reference points at, which
    // the branches below read off the pointer.
    if (dynamic_cast<const SelfExpr *>(&place)) {
        const Symbol *symbol = currentScope->Lookup("self");
        return symbol != nullptr && !symbol->isMut;
    }
    if (const auto *field = dynamic_cast<const FieldExpr *>(&place)) {
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
            EmitError(target.location, std::format("cannot modify constant '{}'", identifier->name));
            return;
        }
        if (symbol->kind == Symbol::Kind::Var && !symbol->isMut) {
            EmitError(target.location, std::format("cannot modify immutable variable '{}'", identifier->name), {},
                      std::format("declare '{}' with 'var' to make it mutable", identifier->name));
        }
    }
    else if (const auto *unary = dynamic_cast<const UnaryExpr *>(&target); unary && unary->op == TokenKind::Star) {
        const TypeRef pointer = CheckExpr(*unary->operand);
        if (pointer.kind == TypeRef::Kind::Pointer && !pointer.inner.empty() && !pointer.inner[0].isMut) {
            EmitError(target.location,
                      std::format("cannot modify data through read-only pointer '{}'", pointer.ToString()));
        }
    }
    else if (dynamic_cast<const SelfExpr *>(&target)) {
        if (const Symbol *symbol = currentScope->Lookup("self"); symbol && !symbol->isMut) {
            EmitError(target.location, "cannot modify immutable receiver 'self'", {},
                      "declare the receiver with 'var' to make it mutable");
        }
    }
    else if (const auto *field = dynamic_cast<const FieldExpr *>(&target)) {
        const TypeRef objectType = CheckExpr(*field->object);
        if (objectType.kind == TypeRef::Kind::Pointer && !objectType.inner.empty()) {
            if (!objectType.inner[0].isMut) {
                EmitError(target.location,
                          std::format("cannot modify data through read-only pointer '{}'", objectType.ToString()));
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
                EmitError(target.location,
                          std::format("cannot modify data through read-only pointer '{}'", objectType.ToString()));
            }
        }
        else if (IsSliceType(objectType)) {
            // Checking the binding's own mutability would be the wrong question here: a slice reaches its elements
            // through a read-only pointer, so declaring the slice `var` lets it be re-pointed, never rewritten.
            EmitError(target.location,
                      std::format("cannot modify elements through read-only slice '{}'", objectType.ToString()), {},
                      "declare the sequence as a 'MutableSlice' from 'Rux/Core' to write through it");
        }
        else {
            CheckMutability(*index->object);
        }
    }
}

bool SemanticAnalyzerContext::CheckAssignableTarget(const Expr &target, const TypeRef &targetType,
                                                    const std::string_view operatorName) {
    if (!IsAssignablePlace(target)) {
        EmitError(target.location,
                  std::format("operator '{}' requires an assignable target, but its left operand has type '{}'",
                              operatorName, targetType.ToString()));
        return false;
    }
    CheckMutability(target);
    return true;
}
} // namespace Rux::SemanticDetail
