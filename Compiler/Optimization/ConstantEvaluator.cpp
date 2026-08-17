#include "Optimization/ConstantEvaluator.h"

#include <charconv>
#include <utility>

namespace Rux::Optimization {
class TypedConstantFactory {
public:
    static TypedConstant Make(const TypeRef &type, const TypedConstant::Kind kind, const std::uint8_t width,
                              const std::uint64_t bits) {
        return TypedConstant(type, kind, width, bits);
    }
};

namespace {
struct TypeProperties {
    TypedConstant::Kind kind;
    std::uint8_t width;
};

std::optional<TypeProperties> Properties(const TypeRef &type) {
    if (type.IsBool()) {
        return TypeProperties{TypedConstant::Kind::Boolean,
                              static_cast<std::uint8_t>(type.SizeInBytes().value_or(0) * 8)};
    }
    if (!type.IsInteger()) {
        return std::nullopt;
    }
    return TypeProperties{type.IsSigned() ? TypedConstant::Kind::SignedInteger : TypedConstant::Kind::UnsignedInteger,
                          static_cast<std::uint8_t>(type.SizeInBytes().value_or(0) * 8)};
}

std::uint64_t Mask(const std::uint8_t width) {
    return width == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << width) - 1;
}

bool SignBit(const TypedConstant &value) {
    return (value.RawBits() & (std::uint64_t{1} << (value.Width() - 1))) != 0;
}

TypedConstant Make(const TypeRef &type, const std::uint64_t bits) {
    const TypeProperties properties = *Properties(type);
    return TypedConstantFactory::Make(type, properties.kind, properties.width, bits);
}

TypedConstant MakeBoolean(const bool value) {
    return Make(TypeRef::MakeBool(), value ? 1 : 0);
}

bool SameIntegerType(const TypedConstant &left, const TypedConstant &right) {
    return left.GetKind() != TypedConstant::Kind::Boolean && left.GetKind() == right.GetKind() &&
           left.Width() == right.Width();
}

std::optional<std::uint64_t> ShiftAmount(const TypedConstant &value, const std::uint8_t width) {
    if (value.GetKind() == TypedConstant::Kind::Boolean ||
        (value.GetKind() == TypedConstant::Kind::SignedInteger && SignBit(value))) {
        return std::nullopt;
    }
    if (value.RawBits() >= width) {
        return std::nullopt;
    }
    return value.RawBits();
}

std::uint64_t SignExtendedBits(const TypedConstant &value) {
    if (value.GetKind() != TypedConstant::Kind::SignedInteger || !SignBit(value)) {
        return value.RawBits();
    }
    return value.RawBits() | ~Mask(value.Width());
}
} // namespace

TypedConstant::TypedConstant(TypeRef inputType, const Kind inputKind, const std::uint8_t inputWidth,
                             const std::uint64_t inputBits)
    : type(std::move(inputType))
    , kind(inputKind)
    , width(inputWidth)
    , bits(inputBits & Mask(inputWidth)) {
}

std::optional<bool> TypedConstant::BooleanValue() const noexcept {
    return kind == Kind::Boolean ? std::optional{bits != 0} : std::nullopt;
}

std::optional<std::int64_t> TypedConstant::SignedValue() const noexcept {
    if (kind != Kind::SignedInteger) {
        return std::nullopt;
    }
    if (!SignBit(*this)) {
        return static_cast<std::int64_t>(bits);
    }
    return -1 - static_cast<std::int64_t>((~bits) & Mask(width));
}

std::string TypedConstant::ToLiteral() const {
    if (const auto value = BooleanValue()) {
        return *value ? "true" : "false";
    }
    if (const auto value = SignedValue()) {
        return std::to_string(*value);
    }
    return std::to_string(bits);
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

    std::string normalized;
    normalized.reserve(literal.size());
    for (const char c : literal) {
        if (c != '_') {
            normalized.push_back(c);
        }
    }
    std::string_view digits = normalized;
    bool negative = false;
    if (!digits.empty() && (digits.front() == '-' || digits.front() == '+')) {
        negative = digits.front() == '-';
        digits.remove_prefix(1);
    }

    int base = 10;
    if (digits.size() > 2 && digits[0] == '0') {
        if (digits[1] == 'x' || digits[1] == 'X') {
            base = 16;
        }
        else if (digits[1] == 'b' || digits[1] == 'B') {
            base = 2;
        }
        else if (digits[1] == 'o' || digits[1] == 'O') {
            base = 8;
        }
        if (base != 10) {
            digits.remove_prefix(2);
        }
    }
    if (digits.empty()) {
        return std::nullopt;
    }

    std::uint64_t magnitude = 0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), magnitude, base);
    if (error != std::errc{} || end != digits.data() + digits.size()) {
        return std::nullopt;
    }

    const std::uint64_t mask = Mask(properties->width);
    if (negative) {
        if (properties->kind != TypedConstant::Kind::SignedInteger) {
            return std::nullopt;
        }
        const std::uint64_t minimumMagnitude = std::uint64_t{1} << (properties->width - 1);
        if (magnitude > minimumMagnitude) {
            return std::nullopt;
        }
        return Make(type, std::uint64_t{0} - magnitude);
    }
    if (magnitude > mask) {
        return std::nullopt;
    }
    if (properties->kind == TypedConstant::Kind::SignedInteger) {
        const std::uint64_t minimumMagnitude = std::uint64_t{1} << (properties->width - 1);
        if (magnitude > minimumMagnitude) {
            return std::nullopt;
        }
    }
    return Make(type, magnitude);
}

std::optional<TypedConstant> EvaluateUnary(const TokenKind op, const TypedConstant &operand) {
    if (operand.GetKind() == TypedConstant::Kind::Boolean) {
        if (op == TokenKind::Bang) {
            return MakeBoolean(operand.RawBits() == 0);
        }
        return std::nullopt;
    }
    switch (op) {
    case TokenKind::Plus:
        return operand;
    case TokenKind::Minus:
        return Make(operand.Type(), std::uint64_t{0} - operand.RawBits());
    case TokenKind::Tilde:
        return Make(operand.Type(), ~operand.RawBits());
    default:
        return std::nullopt;
    }
}

std::optional<TypedConstant> EvaluateBinary(const TokenKind op, const TypedConstant &left, const TypedConstant &right) {
    if (left.GetKind() == TypedConstant::Kind::Boolean && right.GetKind() == TypedConstant::Kind::Boolean) {
        const bool lhs = left.RawBits() != 0;
        const bool rhs = right.RawBits() != 0;
        switch (op) {
        case TokenKind::Amp:
            return Make(left.Type(), left.RawBits() & right.RawBits());
        case TokenKind::Pipe:
            return Make(left.Type(), left.RawBits() | right.RawBits());
        case TokenKind::Caret:
            return Make(left.Type(), left.RawBits() ^ right.RawBits());
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
    if (!SameIntegerType(left, right) && op != TokenKind::LessLess && op != TokenKind::GreaterGreater &&
        op != TokenKind::GreaterGreaterGreater) {
        return std::nullopt;
    }

    const std::uint64_t lhs = left.RawBits();
    const std::uint64_t rhs = right.RawBits();
    switch (op) {
    case TokenKind::Plus:
        return Make(left.Type(), lhs + rhs);
    case TokenKind::Minus:
        return Make(left.Type(), lhs - rhs);
    case TokenKind::Star:
        return Make(left.Type(), lhs * rhs);
    case TokenKind::StarStar: {
        if (right.GetKind() == TypedConstant::Kind::SignedInteger && SignBit(right)) {
            return Make(left.Type(), 0);
        }
        std::uint64_t base = lhs;
        std::uint64_t exponent = rhs;
        std::uint64_t result = 1;
        while (exponent != 0) {
            if ((exponent & 1) != 0) {
                result *= base;
            }
            base *= base;
            exponent >>= 1;
        }
        return Make(left.Type(), result);
    }
    case TokenKind::Slash:
    case TokenKind::Percent:
        if (rhs == 0) {
            return std::nullopt;
        }
        if (left.GetKind() == TypedConstant::Kind::SignedInteger) {
            const std::int64_t signedLeft = *left.SignedValue();
            const std::int64_t signedRight = *right.SignedValue();
            const std::int64_t minimum = left.Width() == 64 ? INT64_MIN : -(std::int64_t{1} << (left.Width() - 1));
            if (signedLeft == minimum && signedRight == -1) {
                return std::nullopt;
            }
            return Make(left.Type(), static_cast<std::uint64_t>(op == TokenKind::Slash ? signedLeft / signedRight
                                                                                       : signedLeft % signedRight));
        }
        return Make(left.Type(), op == TokenKind::Slash ? lhs / rhs : lhs % rhs);
    case TokenKind::Amp:
        return Make(left.Type(), lhs & rhs);
    case TokenKind::Pipe:
        return Make(left.Type(), lhs | rhs);
    case TokenKind::Caret:
        return Make(left.Type(), lhs ^ rhs);
    case TokenKind::LessLess:
    case TokenKind::GreaterGreater:
    case TokenKind::GreaterGreaterGreater: {
        const auto amount = ShiftAmount(right, left.Width());
        if (!amount) {
            return std::nullopt;
        }
        if (op == TokenKind::LessLess) {
            return Make(left.Type(), lhs << *amount);
        }
        if (op == TokenKind::GreaterGreaterGreater || left.GetKind() == TypedConstant::Kind::UnsignedInteger) {
            return Make(left.Type(), lhs >> *amount);
        }
        if (*amount == 0 || !SignBit(left)) {
            return Make(left.Type(), lhs >> *amount);
        }
        const std::uint64_t fill = Mask(left.Width()) ^ (Mask(left.Width()) >> *amount);
        return Make(left.Type(), (lhs >> *amount) | fill);
    }
    case TokenKind::Equal:
        return MakeBoolean(lhs == rhs);
    case TokenKind::BangEqual:
        return MakeBoolean(lhs != rhs);
    case TokenKind::Less:
    case TokenKind::LessEqual:
    case TokenKind::Greater:
    case TokenKind::GreaterEqual: {
        bool result = false;
        if (left.GetKind() == TypedConstant::Kind::SignedInteger) {
            const std::int64_t signedLeft = *left.SignedValue();
            const std::int64_t signedRight = *right.SignedValue();
            result = op == TokenKind::Less      ? signedLeft < signedRight
                   : op == TokenKind::LessEqual ? signedLeft <= signedRight
                   : op == TokenKind::Greater   ? signedLeft > signedRight
                                                : signedLeft >= signedRight;
        }
        else {
            result = op == TokenKind::Less      ? lhs < rhs
                   : op == TokenKind::LessEqual ? lhs <= rhs
                   : op == TokenKind::Greater   ? lhs > rhs
                                                : lhs >= rhs;
        }
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
    std::uint64_t bits =
        value.GetKind() == TypedConstant::Kind::SignedInteger ? SignExtendedBits(value) : value.RawBits();
    bits &= Mask(target->width);
    if (target->kind == TypedConstant::Kind::Boolean) {
        bits = bits != 0 ? 1 : 0;
    }
    return Make(targetType, bits);
}
} // namespace Rux::Optimization
