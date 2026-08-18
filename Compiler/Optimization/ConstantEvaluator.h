#pragma once

#include "Lexer/Token.h"
#include "Numeric/WideInteger.h"
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
    [[nodiscard]] std::uint32_t Width() const noexcept {
        return width;
    }

    /// The complete stored bit pattern, masked to `Width` and never sign-extended.
    [[nodiscard]] const WideInteger &Bits() const noexcept {
        return bits;
    }

    /// The low machine word. Kept for narrow optimizer queries; use `Bits` whenever high words affect identity.
    [[nodiscard]] std::uint64_t RawBits() const noexcept {
        return bits.Word64(0);
    }

    [[nodiscard]] const TypeRef &Type() const noexcept {
        return type;
    }

    /// The value, or nullopt when this constant is not a boolean.
    [[nodiscard]] std::optional<bool> BooleanValue() const noexcept;

    /// The signed value when both the kind and magnitude fit a host `int64_t`, or nullopt otherwise.
    [[nodiscard]] std::optional<std::int64_t> SignedValue() const noexcept;

    /// Render back to source spelling, so a folded expression can be substituted into HIR as an ordinary literal.
    [[nodiscard]] std::string ToLiteral() const;

private:
    friend class TypedConstantFactory;
    TypedConstant(TypeRef inputType, Kind inputKind, WideInteger inputBits);

    TypeRef type;
    Kind kind;
    std::uint32_t width;
    WideInteger bits;
};

/// These helpers intentionally return no value for operations that must remain in HIR, including traps and inputs
/// outside the evaluator's integer model.
[[nodiscard]] std::optional<TypedConstant> ParseConstant(std::string_view literal, const TypeRef &type);
[[nodiscard]] std::optional<TypedConstant> EvaluateUnary(TokenKind op, const TypedConstant &operand);
[[nodiscard]] std::optional<TypedConstant> EvaluateBinary(TokenKind op, const TypedConstant &left,
                                                          const TypedConstant &right);
[[nodiscard]] std::optional<TypedConstant> CastConstant(const TypedConstant &value, const TypeRef &targetType);
} // namespace Rux::Optimization
