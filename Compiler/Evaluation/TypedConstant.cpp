#include "Evaluation/TypedConstant.h"

#include "Numeric/FloatParsing.h"
#include "Numeric/IntegerLiteral.h"
#include "Numeric/SoftwareFloat.h"
#include "Types/PrimitiveCatalog.h"

#include <utility>

namespace Rux::Evaluation {
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
        const PrimitiveInfo *primitive = FindPrimitive(type->kind);
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
    : type(inputType)
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
    case TokenKind::Minus: {
        // Negation is subtraction from zero and overflows where that does: at a signed minimum, and for an unsigned
        // value at anything but zero. Such a negation stays in HIR, where it stops the program.
        const bool isSigned = operand.GetKind() == TypedConstant::Kind::SignedInteger;
        const WideInteger &bits = operand.Bits();
        if (isSigned ? bits == WideInteger::MinMagnitude(operand.Width(), true).Negated() : !bits.IsZero()) {
            return std::nullopt;
        }
        return Make(operand.Type(), bits.Negated());
    }
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
    // `+`, `-` and `*` are checked: a result the type cannot hold is no value here, and the operation stays in IR to
    // stop the program. The wrapping operators keep the low bits.
    const auto sign = [](const WideInteger &value) { return value.IsNegative(); };
    switch (op) {
    case TokenKind::Plus: {
        const WideInteger sum = lhs.Added(rhs);
        const bool overflow = isSigned ? sign(lhs) == sign(rhs) && sign(sum) != sign(lhs)
                                       : sum.Compare(lhs, false) == std::strong_ordering::less;
        return overflow ? std::nullopt : std::optional{Make(left.Type(), sum)};
    }
    case TokenKind::Minus: {
        const WideInteger difference = lhs.Subtracted(rhs);
        const bool overflow = isSigned ? sign(lhs) != sign(rhs) && sign(difference) != sign(lhs)
                                       : lhs.Compare(rhs, false) == std::strong_ordering::less;
        return overflow ? std::nullopt : std::optional{Make(left.Type(), difference)};
    }
    case TokenKind::Star: {
        const auto product = lhs.MultipliedChecked(rhs, isSigned);
        return product ? std::optional{Make(left.Type(), *product)} : std::nullopt;
    }
    case TokenKind::PlusPercent:
        return Make(left.Type(), lhs.Added(rhs));
    case TokenKind::MinusPercent:
        return Make(left.Type(), lhs.Subtracted(rhs));
    case TokenKind::StarPercent:
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

std::optional<TypedConstant> CastConstantExactly(const TypedConstant &value, const TypeRef &targetType) {
    const auto target = Properties(targetType);
    const bool integers =
        (value.GetKind() == TypedConstant::Kind::SignedInteger ||
         value.GetKind() == TypedConstant::Kind::UnsignedInteger) &&
        target &&
        (target->kind == TypedConstant::Kind::SignedInteger || target->kind == TypedConstant::Kind::UnsignedInteger);
    if (integers && !value.Bits().ConvertedChecked(target->width, value.GetKind() == TypedConstant::Kind::SignedInteger,
                                                   target->kind == TypedConstant::Kind::SignedInteger)) {
        return std::nullopt;
    }
    return CastConstant(value, targetType);
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
        const bool targetSigned = target->kind == TypedConstant::Kind::SignedInteger;
        const FloatEncoding encoding = FloatEncoding::FromBits(FormatOf(value), value.Bits());
        const FloatToIntegerResult converted = FloatToInteger(encoding, target->width, targetSigned);
        if (converted.HasValue()) {
            return Make(targetType, converted.value);
        }
        // Outside the range the conversion saturates, and a NaN becomes zero, as it does at run time.
        const FloatClass classification = encoding.Classify();
        if (classification != FloatClass::Infinity && classification != FloatClass::Normal &&
            classification != FloatClass::Subnormal) {
            return Make(targetType, WideInteger::Zero(target->width));
        }
        if (encoding.IsNegative()) {
            return Make(targetType, targetSigned ? WideInteger::MinMagnitude(target->width, true).Negated()
                                                 : WideInteger::Zero(target->width));
        }
        return Make(targetType, WideInteger::MaxValue(target->width, targetSigned));
    }
    const bool sourceSigned = value.GetKind() == TypedConstant::Kind::SignedInteger;
    return Make(targetType, value.Bits().ConvertedWrapping(target->width, sourceSigned));
}

namespace {
bool IsInteger(const TypedConstant &value) {
    return value.GetKind() == TypedConstant::Kind::SignedInteger ||
           value.GetKind() == TypedConstant::Kind::UnsignedInteger;
}

CheckedConstant Checked(std::optional<TypedConstant> value) {
    return CheckedConstant{std::move(value), CheckedFailure::Unsupported};
}

CheckedConstant Failed(const CheckedFailure failure) {
    return CheckedConstant{std::nullopt, failure};
}

/// Whether `result`, computed with wrapping, is not the exact sum or difference of `left` and `right`.
bool AdditionOverflows(const TokenKind op, const WideInteger &left, const WideInteger &right, const WideInteger &result,
                       const bool isSigned) {
    if (!isSigned) {
        // An unsigned sum wraps below its left operand, and an unsigned difference wraps when the right one is larger.
        return op == TokenKind::Plus ? result.Compare(left, false) == std::strong_ordering::less
                                     : left.Compare(right, false) == std::strong_ordering::less;
    }
    // A signed sum overflows when both operands share a sign the result lost; a difference, when they differ and the
    // result lost the left one's.
    const bool leftNegative = left.IsNegative();
    const bool rightNegative = right.IsNegative();
    const bool resultNegative = result.IsNegative();
    if (op == TokenKind::Plus) {
        return leftNegative == rightNegative && resultNegative != leftNegative;
    }
    return leftNegative != rightNegative && resultNegative != leftNegative;
}
} // namespace

CheckedConstant EvaluateUnaryChecked(const TokenKind op, const TypedConstant &operand) {
    if (op == TokenKind::Minus && IsInteger(operand) && !operand.Bits().IsZero()) {
        // Only zero negates to itself in an unsigned type, and the signed minimum has no positive counterpart.
        const bool isSigned = operand.GetKind() == TypedConstant::Kind::SignedInteger;
        if (!isSigned || operand.Bits() == WideInteger::MinMagnitude(operand.Width(), true).Negated()) {
            return Failed(CheckedFailure::Overflow);
        }
    }
    return Checked(EvaluateUnary(op, operand));
}

CheckedConstant EvaluateBinaryChecked(const TokenKind op, const TypedConstant &left, const TypedConstant &right) {
    if (!IsInteger(left) || !IsInteger(right)) {
        return Checked(EvaluateBinary(op, left, right));
    }
    const bool isSigned = left.GetKind() == TypedConstant::Kind::SignedInteger;
    switch (op) {
    case TokenKind::Plus:
    case TokenKind::Minus: {
        // `EvaluateBinary` has no value for an integer sum or difference the type cannot hold.
        auto result = EvaluateBinary(op, left, right);
        if (!result || AdditionOverflows(op, left.Bits(), right.Bits(), result->Bits(), isSigned)) {
            return Failed(CheckedFailure::Overflow);
        }
        return Checked(std::move(result));
    }
    case TokenKind::Star: {
        if (left.GetKind() != right.GetKind() || left.Width() != right.Width()) {
            return Failed(CheckedFailure::Unsupported);
        }
        if (!left.Bits().MultipliedChecked(right.Bits(), isSigned)) {
            return Failed(CheckedFailure::Overflow);
        }
        return Checked(EvaluateBinary(op, left, right));
    }
    case TokenKind::Slash:
    case TokenKind::Percent: {
        if (right.Bits().IsZero()) {
            return Failed(CheckedFailure::DivisionByZero);
        }
        // The signed minimum divided by -1 has no quotient in the type, and its remainder traps with it.
        const WideInteger minimum = WideInteger::MinMagnitude(left.Width(), true).Negated();
        if (isSigned && left.Bits() == minimum && right.Bits() == WideInteger::AllOnes(right.Width())) {
            return Failed(CheckedFailure::Overflow);
        }
        return Checked(EvaluateBinary(op, left, right));
    }
    case TokenKind::LessLess:
    case TokenKind::GreaterGreater:
    case TokenKind::GreaterGreaterGreater: {
        const bool negative = right.GetKind() == TypedConstant::Kind::SignedInteger && right.Bits().IsNegative();
        const auto amount = right.Bits().ToUnsigned();
        if (negative || !amount || *amount >= left.Width()) {
            return Failed(CheckedFailure::ShiftOutOfRange);
        }
        return Checked(EvaluateBinary(op, left, right));
    }
    default:
        return Checked(EvaluateBinary(op, left, right));
    }
}
} // namespace Rux::Evaluation
