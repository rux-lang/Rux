#pragma once

#include "Lexer/Token.h"
#include "Semantic/Type.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Rux::Optimization {
class TypedConstantFactory;

/**
 * @brief A compile-time value carried together with the type it was written at.
 *
 * Folding has to reproduce what the target would compute, so the width and signedness travel with the value rather than
 * being recovered later: adding two `int8` constants must wrap where an `int8` wraps. The value is held as raw bits
 * masked to that width, which makes wrapping the default and sign-extension something a reader asks for explicitly.
 *
 * Only integers and booleans are modelled. Floating point is deliberately absent, since folding it would have to
 * reproduce the target's rounding.
 */
class TypedConstant {
public:
    enum class Kind {
        Boolean,
        SignedInteger,
        UnsignedInteger,
    };

    /// Named `GetKind` because `Kind` is already the nested enumeration.
    [[nodiscard]] Kind GetKind() const noexcept {
        return kind;
    }

    /// Width in bits, and the width the stored bits are masked to.
    [[nodiscard]] std::uint8_t Width() const noexcept {
        return width;
    }

    /// The value as stored: masked to `Width`, and therefore not sign-extended even when the kind is signed.
    /// `SignedValue` is what applies the sign.
    [[nodiscard]] std::uint64_t RawBits() const noexcept {
        return bits;
    }

    [[nodiscard]] const TypeRef &Type() const noexcept {
        return type;
    }

    /// The value, or nullopt when this constant is not a boolean.
    [[nodiscard]] std::optional<bool> BooleanValue() const noexcept;

    /// The value sign-extended out of `Width`, or nullopt when the kind is not a signed integer. An unsigned constant
    /// is read through `RawBits`.
    [[nodiscard]] std::optional<std::int64_t> SignedValue() const noexcept;

    /// Render back to source spelling, so a folded expression can be substituted into HIR as an ordinary literal.
    [[nodiscard]] std::string ToLiteral() const;

private:
    friend class TypedConstantFactory;
    TypedConstant(TypeRef inputType, Kind inputKind, std::uint8_t inputWidth, std::uint64_t inputBits);

    TypeRef type;
    Kind kind;
    std::uint8_t width;
    std::uint64_t bits;
};

/// These helpers intentionally return no value for operations that must remain in HIR, including traps and inputs
/// outside the evaluator's integer model.
[[nodiscard]] std::optional<TypedConstant> ParseConstant(std::string_view literal, const TypeRef &type);
[[nodiscard]] std::optional<TypedConstant> EvaluateUnary(TokenKind op, const TypedConstant &operand);
[[nodiscard]] std::optional<TypedConstant> EvaluateBinary(TokenKind op, const TypedConstant &left,
                                                          const TypedConstant &right);
[[nodiscard]] std::optional<TypedConstant> CastConstant(const TypedConstant &value, const TypeRef &targetType);
} // namespace Rux::Optimization
