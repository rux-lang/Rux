#include "Semantic/Conditional/ConditionalEvaluatorInternal.h"

#include <algorithm>
#include <compare>

namespace Rux {
std::optional<CompileTimeValue> ConditionalEvaluator::Impl::EvalWideBinary(const BinaryExpr &expr, const Value &left,
                                                                           const Value &right) {
    const auto *leftWide = std::get_if<CompileTimeWideInteger>(&left);
    const auto *rightWide = std::get_if<CompileTimeWideInteger>(&right);
    if (!leftWide && !rightWide) {
        return std::nullopt;
    }
    const std::uint32_t width =
        std::max(leftWide ? leftWide->bits.Width() : 64, rightWide ? rightWide->bits.Width() : 64);
    const bool signedResult = leftWide ? leftWide->isSigned : rightWide->isSigned;
    const auto convert = [&](const Value &value) -> std::optional<WideInteger> {
        if (const auto *wide = std::get_if<CompileTimeWideInteger>(&value)) {
            return wide->bits.Extended(width, wide->isSigned);
        }
        if (const auto *number = std::get_if<std::int64_t>(&value)) {
            return WideInteger::FromUnsigned(static_cast<std::uint64_t>(*number), 64).Extended(width, true);
        }
        if (const auto *number = std::get_if<std::uint64_t>(&value)) {
            return WideInteger::FromUnsigned(*number, width);
        }
        return std::nullopt;
    };
    const auto lhs = convert(left);
    const auto rhs = convert(right);
    if (!lhs || !rhs) {
        EmitError(expr.location, "wide compile-time arithmetic requires integer operands");
        reportedError = true;
        return std::nullopt;
    }
    const auto result = [&](WideInteger bits) -> std::optional<Value> {
        return Value{CompileTimeWideInteger{bits, signedResult}};
    };
    const bool leftSigned = leftWide ? leftWide->isSigned : std::holds_alternative<std::int64_t>(left);
    const bool rightSigned = rightWide ? rightWide->isSigned : std::holds_alternative<std::int64_t>(right);
    const bool leftNegative = leftSigned && lhs->IsNegative();
    const bool rightNegative = rightSigned && rhs->IsNegative();
    const auto order = leftNegative != rightNegative
                         ? (leftNegative ? std::strong_ordering::less : std::strong_ordering::greater)
                         : lhs->Compare(*rhs, leftNegative);
    switch (expr.op) {
    case TokenKind::Equal:
        return Value{order == 0};
    case TokenKind::BangEqual:
        return Value{order != 0};
    case TokenKind::Less:
        return Value{order < 0};
    case TokenKind::LessEqual:
        return Value{order <= 0};
    case TokenKind::Greater:
        return Value{order > 0};
    case TokenKind::GreaterEqual:
        return Value{order >= 0};
    case TokenKind::Plus:
        return result(lhs->Added(*rhs));
    case TokenKind::Minus:
        return result(lhs->Subtracted(*rhs));
    case TokenKind::Star:
        return result(lhs->MultipliedWrapping(*rhs));
    case TokenKind::Amp:
        return result(lhs->BitwiseAnd(*rhs));
    case TokenKind::Pipe:
        return result(lhs->BitwiseOr(*rhs));
    case TokenKind::Caret:
        return result(lhs->BitwiseXor(*rhs));
    case TokenKind::Slash:
    case TokenKind::Percent: {
        const auto division = lhs->Divided(*rhs, signedResult);
        if (division.HasValue()) {
            return result(expr.op == TokenKind::Slash ? division.quotient : division.remainder);
        }
        EmitError(expr.location, "invalid division in a wide compile-time expression");
        break;
    }
    case TokenKind::LessLess:
    case TokenKind::GreaterGreater:
    case TokenKind::GreaterGreaterGreater: {
        const auto amount = rhs->ToUnsigned();
        if (!amount || *amount >= width) {
            EmitError(expr.location, "wide compile-time shift count is outside the operand width");
            break;
        }
        return result(expr.op == TokenKind::LessLess
                          ? lhs->ShiftedLeft(static_cast<std::uint32_t>(*amount))
                          : lhs->ShiftedRight(static_cast<std::uint32_t>(*amount),
                                              signedResult && expr.op == TokenKind::GreaterGreater));
    }
    default:
        EmitError(expr.location, "unsupported wide compile-time operator");
        break;
    }
    reportedError = true;
    return std::nullopt;
}
} // namespace Rux
