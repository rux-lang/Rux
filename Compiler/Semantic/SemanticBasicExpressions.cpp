// Checking for literals, names, operators and assignment, including the
// mutability rules an assignment target has to satisfy.

#include "Numeric/IntegerLiteral.h"
#include "Semantic/Detail/SemanticAnalyzerContext.h"
#include "Semantic/PrimitiveCatalog.h"

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
            TypeRef substituted = it->second;
            // `*var T` records that its pointee is writable on the `T` slot itself, so the substitution has to carry
            // that mark onto whatever `T` turns out to be. Dropping it turned `*var T` into a read-only pointer the
            // moment a type argument arrived -- silently, because the two render identically.
            substituted.isMut = type.isMut;
            return substituted;
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
    return type.IsNumeric() || type.IsBool() || type.IsChar() || type.kind == TypeRef::Kind::Pointer;
}
} // namespace

void SemanticAnalyzerContext::ValidateSuffixedIntegerLiteral(const LiteralExpr &literal, const bool negative) {
    if (literal.token.kind != TokenKind::IntLiteral) {
        return;
    }
    const NumericLiteralSuffixInfo *suffix = FindNumericLiteralSuffix(NumericLiteralSuffixOf(literal.token.text));
    if (!suffix || suffix->isFloat) {
        return;
    }
    // A pointer-sized suffix takes the target's width, which is what the value will actually be stored at.
    const std::uint32_t bits =
        suffix->bits != 0 ? suffix->bits : static_cast<std::uint32_t>(context.target.pointer_size * 8);
    const auto magnitude = DecodeIntegerLiteral(literal.token.text, WideInteger::MaxBits);
    if (!magnitude) {
        // Beyond even the widest width there is, so no suffix could have held it.
        EmitError(literal.location,
                  std::format("integer literal is out of range for type '{}'", LiteralType(literal.token).ToString()));
        return;
    }
    if (!IntegerLiteralFits(*magnitude, negative, bits, suffix->isSigned)) {
        EmitError(literal.location,
                  std::format("integer literal is out of range for type '{}'", LiteralType(literal.token).ToString()));
    }
}

std::optional<TypeRef> SemanticAnalyzerContext::CheckBasicExpression(const Expr &expression) {
    if (const auto *literal = dynamic_cast<const LiteralExpr *>(&expression)) {
        if (!negatedIntegerLiterals.contains(literal)) {
            ValidateSuffixedIntegerLiteral(*literal, false);
        }
        const TypeRef type = LiteralType(literal->token);
        if (const PrimitiveInfo *primitive = FindPrimitive(type.kind); primitive && !primitive->implemented) {
            EmitError(literal->location,
                      std::format("primitive type '{}' is reserved but is not implemented in this compiler version",
                                  primitive->name));
            return type;
        }
        return type;
    }
    if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expression)) {
        // A minus and the literal under it are one written value, so the magnitude is checked against the negative
        // end of the range rather than the positive one: `-128i8` is in range where `128i8` is not.
        if (unary->op == TokenKind::Minus) {
            if (const auto *operand = dynamic_cast<const LiteralExpr *>(unary->operand.get())) {
                negatedIntegerLiterals.insert(operand);
                ValidateSuffixedIntegerLiteral(*operand, true);
            }
        }
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
    if (const auto *tryExpression = dynamic_cast<const TryExpr *>(&expression)) {
        return CheckTryExpression(*tryExpression);
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
        if (binary->op == TokenKind::AmpAmp || binary->op == TokenKind::PipePipe) {
            return CheckShortCircuitExpression(*binary);
        }
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
                if (RejectSelfMove(*assignment->target, *assignment->value, value, assignment->location)) {
                    return TypeRef::MakeOpaque();
                }
                ConsumeValue(*assignment->value, value, ValueConsumptionKind::Assignment, assignment->location);
                MarkTrackedAssignment(*assignment->target, assignment->location);
            }
        }
        return TypeRef::MakeOpaque();
    }
    if (const auto *cast = dynamic_cast<const CastExpr *>(&expression)) {
        const TypeRef operandType = CheckExpr(*cast->operand);
        TypeRef targetType = ResolveType(*cast->type);
        // An unsubstituted type parameter is not yet any particular type, so whether it can be cast is a question only
        // an instantiation can answer. Defer it there, the same way a unary or binary operator on a `T` already is,
        // rather than rejecting the generic body for a cast that every instantiation of it would accept.
        if (operandType.kind == TypeRef::Kind::TypeParam || targetType.kind == TypeRef::Kind::TypeParam) {
            if (currentFunctionDecl) {
                deferredCastChecks[currentFunctionDecl].push_back({operandType, targetType, cast->location});
            }
            return targetType;
        }
        CheckCast(operandType, targetType, cast->location);
        if (!CastTypesAreCompatible(operandType, targetType)) {
            return targetType;
        }
        ValidateCastConstant(*cast, operandType, targetType);
        return targetType;
    }
    return std::nullopt;
}

/// Whether one type's values can be respelled as another's at all, which is the whole of what a cast requires: the
/// value's own range is checked separately, and only for a constant.
bool SemanticAnalyzerContext::CastTypesAreCompatible(const TypeRef &operand, const TypeRef &target) const {
    const auto isCastValue = [&](const TypeRef &type) {
        if (IsCastValue(type)) {
            return true;
        }
        const std::string typeName = NamedBaseTypeName(type);
        return !typeName.empty() && enumDecls.contains(typeName);
    };
    if (operand.IsUnknown() || target.IsUnknown()) {
        return true;
    }
    if (operand.kind == TypeRef::Kind::Func || target.kind == TypeRef::Kind::Func) {
        return IsAddressValue(operand) && IsAddressValue(target);
    }
    return isCastValue(operand) && isCastValue(target);
}

void SemanticAnalyzerContext::CheckCast(const TypeRef &operand, const TypeRef &target, const SourceLocation location) {
    if (!CastTypesAreCompatible(operand, target)) {
        EmitError(location,
                  std::format("cannot cast value of type '{}' to '{}'", operand.ToString(), target.ToString()));
    }
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
    if (const auto it = deferredCastChecks.find(&declaration); it != deferredCastChecks.end()) {
        for (const DeferredCastCheck &check : it->second) {
            CheckCast(SubstituteTypeParams(check.operand, substitutions),
                      SubstituteTypeParams(check.target, substitutions), check.location);
        }
    }
    if (const auto it = deferredConsumptions.find(&declaration); it != deferredConsumptions.end()) {
        for (const DeferredConsumption &deferred : it->second) {
            // What the parameter stands for decides. A copyable argument consumes nothing and needs no fact
            // recorded; a move-only one hands its value over, and the fact is what tells lowering not to destroy the
            // source afterwards.
            const TypeRef resolved = SubstituteTypeParams(deferred.type, substitutions);
            if (!ClassifyTypeProperties(resolved).IsMoveOnly()) {
                continue;
            }
            // Recorded without asking whether the source was a legal place to move from. That question is answered
            // for a concrete type where the move is written, and answering it here would reject what a container
            // exists to do: take a value out of storage it owns. Nothing in the language distinguishes an owned
            // pointer from a borrowed one, so a container moving out of its own block and a caller moving out of a
            // borrowed slice look alike. Diagnosing the second without forbidding the first needs a way to say which
            // is which; until there is one, this records what to consume and leaves the legality alone.
            valueConsumptions.insert_or_assign(deferred.expression,
                                               ValueConsumption{deferred.kind, resolved, deferred.location});
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

    const auto isNumericOrChar = [](const TypeRef &type) { return type.IsNumeric() || type.IsChar(); };
    const auto isIntegerOrChar = [](const TypeRef &type) { return type.IsInteger() || type.IsChar(); };
    const auto compatibleType = [&](const Expr &leftExpr, const TypeRef &leftType, const Expr &rightExpr,
                                    const TypeRef &rightType) -> std::optional<TypeRef> {
        if ((leftType.IsInteger() && rightType.IsChar()) || (rightType.IsInteger() && leftType.IsChar())) {
            return leftType.IsInteger() ? leftType : rightType;
        }
        if (leftType.IsChar() && rightType.IsChar()) {
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
            return type.IsInteger() || type.IsBool() || type.IsChar();
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
