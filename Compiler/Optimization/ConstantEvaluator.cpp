#include "Optimization/ConstantEvaluator.h"

#include "Numeric/FloatParsing.h"
#include "Numeric/IntegerLiteral.h"
#include "Numeric/SoftwareFloat.h"
#include "Types/PrimitiveCatalog.h"

#include <utility>

namespace Rux::Optimization {
class TypedConstantFactory {
public:
    static TypedConstant Make(const TypeRef &type, const TypedConstant::Kind kind, const WideInteger bits) {
        return TypedConstant(type, kind, bits);
    }
};

namespace {
struct TypeProperties {
    TypedConstant::Kind kind;
    std::uint32_t width;
    const FloatFormat *floatFormat = nullptr;
};

/// The width and signedness the evaluator models a type at.
///
/// @return nullopt for any type outside the integer and boolean model, which is how folding declines to touch it
std::optional<TypeProperties> Properties(const TypeRef &type) {
    if (type.IsBool()) {
        // A bool is modeled one bit wide whatever it is stored at, because that is the only value it ever holds: the
        // storage width says how much room it occupies, never how much of it carries the value.
        return TypeProperties{TypedConstant::Kind::Boolean, 1};
    }
    if (!type.IsInteger()) {
        if (!type.IsFloat()) {
            return std::nullopt;
        }
        const PrimitiveInfo *primitive = FindPrimitive(type.kind);
        const FloatFormat *format = primitive ? FindFloatFormat(primitive->bits) : nullptr;
        return format ? std::optional{TypeProperties{TypedConstant::Kind::Floating, format->valueBits, format}}
                      : std::nullopt;
    }
    return TypeProperties{type.IsSigned() ? TypedConstant::Kind::SignedInteger : TypedConstant::Kind::UnsignedInteger,
                          static_cast<std::uint32_t>(type.SizeInBytes().value_or(0) * 8)};
}

TypedConstant Make(const TypeRef &type, WideInteger bits) {
    const TypeProperties properties = *Properties(type);
    return TypedConstantFactory::Make(type, properties.kind, bits.ConvertedTruncating(properties.width));
}

TypedConstant Make(const TypeRef &type, const std::uint64_t bits) {
    const TypeProperties properties = *Properties(type);
    return Make(type, WideInteger::FromUnsigned(bits, properties.width));
}

TypedConstant MakeBoolean(const bool value) {
    return Make(TypeRef::MakeBool(), value ? 1 : 0);
}

/// Whether two constants share a width and signedness. Binary evaluation requires it: folding operands of different
/// types would have to reproduce the language's conversion rules, which analysis has already applied.
bool SameIntegerType(const TypedConstant &left, const TypedConstant &right) {
    return left.GetKind() != TypedConstant::Kind::Boolean && left.GetKind() == right.GetKind() &&
           left.Width() == right.Width();
}

bool SameFloatType(const TypedConstant &left, const TypedConstant &right) {
    return left.GetKind() == TypedConstant::Kind::Floating && right.GetKind() == TypedConstant::Kind::Floating &&
           left.Width() == right.Width();
}

const FloatFormat &FormatOf(const TypedConstant &value) {
    return *FindFloatFormat(value.Width());
}

/// The shift distance, when it is one the result is defined for.
///
/// @return nullopt for a negative or too-large distance, leaving the shift in HIR rather than folding to a value the
/// target would not produce
std::optional<std::uint32_t> ShiftAmount(const TypedConstant &value, const std::uint32_t width) {
    if (value.GetKind() == TypedConstant::Kind::Boolean ||
        (value.GetKind() == TypedConstant::Kind::SignedInteger && value.Bits().IsNegative())) {
        return std::nullopt;
    }
    const auto amount = value.Bits().ToUnsigned();
    if (!amount || *amount >= width) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(*amount);
}
} // namespace

TypedConstant::TypedConstant(TypeRef inputType, const Kind inputKind, WideInteger inputBits)
    : type(std::move(inputType))
    , kind(inputKind)
    , width(inputBits.Width())
    , bits(inputBits) {
}

std::optional<bool> TypedConstant::BooleanValue() const noexcept {
    return kind == Kind::Boolean ? std::optional{!bits.IsZero()} : std::nullopt;
}

std::optional<std::int64_t> TypedConstant::SignedValue() const noexcept {
    if (kind != Kind::SignedInteger || width > 64) {
        return std::nullopt;
    }
    const auto magnitude = bits.Magnitude(true).ToUnsigned();
    if (!magnitude) {
        return std::nullopt;
    }
    if (!bits.IsNegative()) {
        return static_cast<std::int64_t>(*magnitude);
    }
    if (*magnitude == (std::uint64_t{1} << 63)) {
        return INT64_MIN;
    }
    return -static_cast<std::int64_t>(*magnitude);
}

std::string TypedConstant::ToLiteral() const {
    if (const auto value = BooleanValue()) {
        return *value ? "true" : "false";
    }
    if (kind == Kind::SignedInteger && bits.IsNegative()) {
        return "-" + bits.Magnitude(true).ToDecimal();
    }
    if (kind == Kind::Floating) {
        return FormatFloatEncoding(FloatEncoding::FromBits(*FindFloatFormat(width), bits));
    }
    return bits.ToDecimal();
}

std::optional<TypedConstant> ParseConstant(const std::string_view literal, const TypeRef &type) {
    const auto properties = Properties(type);
    if (!properties || properties->width == 0) {
        return std::nullopt;
    }
    if (properties->kind == TypedConstant::Kind::Boolean) {
        if (literal == "true" || literal == "false") {
            return Make(type, literal == "true" ? 1 : 0);
        }
        return std::nullopt;
    }
    if (properties->kind == TypedConstant::Kind::Floating) {
        const auto encoding = ParseFloatEncoding(literal, *properties->floatFormat);
        return encoding ? std::optional{Make(type, encoding->Bits())} : std::nullopt;
    }

    const auto parts = SplitIntegerLiteral(literal);
    if (!parts || !parts->suffix.empty()) {
        return std::nullopt;
    }
    const auto magnitude = WideInteger::Parse(parts->digits, parts->base, WideInteger::MaxBits);
    const bool isSigned = properties->kind == TypedConstant::Kind::SignedInteger;
    if (!magnitude) {
        return std::nullopt;
    }
    // HIR keeps the unary minus separate from its literal, including for the one magnitude that only fits after that
    // minus is applied. Admit that bit pattern here so `-128i8` can fold; semantic analysis has already rejected a
    // source-level positive `128i8`.
    const bool pendingMinimum =
        isSigned && !parts->negative && *magnitude == WideInteger::MinMagnitude(properties->width, true);
    if (!pendingMinimum && !IntegerLiteralFits(*magnitude, parts->negative, properties->width, isSigned)) {
        return std::nullopt;
    }
    WideInteger bits = magnitude->ConvertedTruncating(properties->width);
    if (parts->negative) {
        bits = bits.Negated();
    }
    return Make(type, bits);
}

std::optional<TypedConstant> EvaluateUnary(const TokenKind op, const TypedConstant &operand) {
    if (operand.GetKind() == TypedConstant::Kind::Boolean) {
        if (op == TokenKind::Bang) {
            return MakeBoolean(operand.Bits().IsZero());
        }
        return std::nullopt;
    }
    if (operand.GetKind() == TypedConstant::Kind::Floating) {
        if (op == TokenKind::Plus) {
            return operand;
        }
        if (op == TokenKind::Minus) {
            const WideInteger sign = WideInteger::FromUnsigned(1, operand.Width()).ShiftedLeft(operand.Width() - 1);
            return Make(operand.Type(), operand.Bits().BitwiseXor(sign));
        }
        return std::nullopt;
    }
    switch (op) {
    case TokenKind::Plus:
        return operand;
    case TokenKind::Minus:
        return Make(operand.Type(), operand.Bits().Negated());
    case TokenKind::Tilde:
        return Make(operand.Type(), operand.Bits().BitwiseNot());
    default:
        return std::nullopt;
    }
}

std::optional<TypedConstant> EvaluateBinary(const TokenKind op, const TypedConstant &left, const TypedConstant &right) {
    if (left.GetKind() == TypedConstant::Kind::Boolean && right.GetKind() == TypedConstant::Kind::Boolean) {
        const bool lhs = !left.Bits().IsZero();
        const bool rhs = !right.Bits().IsZero();
        switch (op) {
        case TokenKind::Amp:
            return Make(left.Type(), left.Bits().BitwiseAnd(right.Bits()));
        case TokenKind::Pipe:
            return Make(left.Type(), left.Bits().BitwiseOr(right.Bits()));
        case TokenKind::Caret:
            return Make(left.Type(), left.Bits().BitwiseXor(right.Bits()));
        case TokenKind::AmpAmp:
            return MakeBoolean(lhs && rhs);
        case TokenKind::PipePipe:
            return MakeBoolean(lhs || rhs);
        case TokenKind::Equal:
            return MakeBoolean(lhs == rhs);
        case TokenKind::BangEqual:
            return MakeBoolean(lhs != rhs);
        default:
            return std::nullopt;
        }
    }
    if (left.GetKind() == TypedConstant::Kind::Boolean || right.GetKind() == TypedConstant::Kind::Boolean) {
        return std::nullopt;
    }
    if (SameFloatType(left, right)) {
        const FloatFormat &format = FormatOf(left);
        const FloatEncoding lhs = FloatEncoding::FromBits(format, left.Bits());
        const FloatEncoding rhs = FloatEncoding::FromBits(format, right.Bits());
        switch (op) {
        case TokenKind::Plus:
            return Make(left.Type(), AddFloat(lhs, rhs).Bits());
        case TokenKind::Minus:
            return Make(left.Type(), SubtractFloat(lhs, rhs).Bits());
        case TokenKind::Star:
            return Make(left.Type(), MultiplyFloat(lhs, rhs).Bits());
        case TokenKind::Slash:
            return Make(left.Type(), DivideFloat(lhs, rhs).Bits());
        case TokenKind::Percent:
            return Make(left.Type(), RemainderFloat(lhs, rhs).Bits());
        case TokenKind::Equal:
            return MakeBoolean(CompareFloat(lhs, rhs) == FloatComparison::Equal);
        case TokenKind::BangEqual:
            return MakeBoolean(CompareFloat(lhs, rhs) != FloatComparison::Equal);
        case TokenKind::Less:
            return MakeBoolean(CompareFloat(lhs, rhs) == FloatComparison::Less);
        case TokenKind::LessEqual: {
            const FloatComparison comparison = CompareFloat(lhs, rhs);
            return MakeBoolean(comparison == FloatComparison::Less || comparison == FloatComparison::Equal);
        }
        case TokenKind::Greater:
            return MakeBoolean(CompareFloat(lhs, rhs) == FloatComparison::Greater);
        case TokenKind::GreaterEqual: {
            const FloatComparison comparison = CompareFloat(lhs, rhs);
            return MakeBoolean(comparison == FloatComparison::Greater || comparison == FloatComparison::Equal);
        }
        default:
            return std::nullopt;
        }
    }
    if (left.GetKind() == TypedConstant::Kind::Floating || right.GetKind() == TypedConstant::Kind::Floating) {
        return std::nullopt;
    }
    if (!SameIntegerType(left, right) && op != TokenKind::LessLess && op != TokenKind::GreaterGreater &&
        op != TokenKind::GreaterGreaterGreater) {
        return std::nullopt;
    }

    const WideInteger &lhs = left.Bits();
    const WideInteger &rhs = right.Bits();
    const bool isSigned = left.GetKind() == TypedConstant::Kind::SignedInteger;
    switch (op) {
    case TokenKind::Plus:
        return Make(left.Type(), lhs.Added(rhs));
    case TokenKind::Minus:
        return Make(left.Type(), lhs.Subtracted(rhs));
    case TokenKind::Star:
        return Make(left.Type(), lhs.MultipliedWrapping(rhs));
    case TokenKind::Slash:
    case TokenKind::Percent: {
        const WideIntegerDivision division = lhs.Divided(rhs, isSigned);
        if (!division.HasValue()) {
            return std::nullopt;
        }
        return Make(left.Type(), op == TokenKind::Slash ? division.quotient : division.remainder);
    }
    case TokenKind::Amp:
        return Make(left.Type(), lhs.BitwiseAnd(rhs));
    case TokenKind::Pipe:
        return Make(left.Type(), lhs.BitwiseOr(rhs));
    case TokenKind::Caret:
        return Make(left.Type(), lhs.BitwiseXor(rhs));
    case TokenKind::LessLess:
    case TokenKind::GreaterGreater:
    case TokenKind::GreaterGreaterGreater: {
        const auto amount = ShiftAmount(right, left.Width());
        if (!amount) {
            return std::nullopt;
        }
        if (op == TokenKind::LessLess) {
            return Make(left.Type(), lhs.ShiftedLeft(*amount));
        }
        const bool arithmetic = op != TokenKind::GreaterGreaterGreater && isSigned;
        return Make(left.Type(), lhs.ShiftedRight(*amount, arithmetic));
    }
    case TokenKind::Equal:
        return MakeBoolean(lhs == rhs);
    case TokenKind::BangEqual:
        return MakeBoolean(lhs != rhs);
    case TokenKind::Less:
    case TokenKind::LessEqual:
    case TokenKind::Greater:
    case TokenKind::GreaterEqual: {
        const std::strong_ordering ordering = lhs.Compare(rhs, isSigned);
        const bool result = op == TokenKind::Less      ? ordering == std::strong_ordering::less
                          : op == TokenKind::LessEqual ? ordering != std::strong_ordering::greater
                          : op == TokenKind::Greater   ? ordering == std::strong_ordering::greater
                                                       : ordering != std::strong_ordering::less;
        return MakeBoolean(result);
    }
    default:
        return std::nullopt;
    }
}

std::optional<TypedConstant> CastConstant(const TypedConstant &value, const TypeRef &targetType) {
    const auto target = Properties(targetType);
    if (!target || target->width == 0) {
        return std::nullopt;
    }
    if (target->kind == TypedConstant::Kind::Boolean) {
        // The whole source value decides the truth, so `256 as bool` is true. Masking to the bool's storage width
        // first would have asked about the low byte instead and disagreed with what the back ends emit.
        if (value.GetKind() == TypedConstant::Kind::Floating) {
            const FloatEncoding encoding = FloatEncoding::FromBits(FormatOf(value), value.Bits());
            return Make(targetType, encoding.Classify() == FloatClass::Zero ? 0 : 1);
        }
        return Make(targetType, value.Bits().IsZero() ? 0 : 1);
    }
    if (target->kind == TypedConstant::Kind::Floating) {
        FloatEncoding converted = FloatEncoding::Zero(*target->floatFormat);
        if (value.GetKind() == TypedConstant::Kind::Floating) {
            converted = ConvertFloat(FloatEncoding::FromBits(FormatOf(value), value.Bits()), *target->floatFormat);
        }
        else {
            const bool sourceSigned = value.GetKind() == TypedConstant::Kind::SignedInteger;
            converted = IntegerToFloat(value.Bits(), sourceSigned, *target->floatFormat);
        }
        return Make(targetType, converted.Bits());
    }
    if (value.GetKind() == TypedConstant::Kind::Floating) {
        const FloatToIntegerResult converted =
            FloatToInteger(FloatEncoding::FromBits(FormatOf(value), value.Bits()), target->width,
                           target->kind == TypedConstant::Kind::SignedInteger);
        return converted.HasValue() ? std::optional{Make(targetType, converted.value)} : std::nullopt;
    }
    const bool sourceSigned = value.GetKind() == TypedConstant::Kind::SignedInteger;
    return Make(targetType, value.Bits().ConvertedWrapping(target->width, sourceSigned));
}
} // namespace Rux::Optimization
