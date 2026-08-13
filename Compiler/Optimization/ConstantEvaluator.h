#pragma once

#include "Lexer/Token.h"
#include "Semantic/Type.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Rux::Optimization {
class TypedConstantFactory;

class TypedConstant {
public:
    enum class Kind {
        Boolean,
        SignedInteger,
        UnsignedInteger,
    };

    [[nodiscard]] Kind GetKind() const noexcept {
        return kind_;
    }

    [[nodiscard]] std::uint8_t Width() const noexcept {
        return width_;
    }

    [[nodiscard]] std::uint64_t RawBits() const noexcept {
        return bits_;
    }

    [[nodiscard]] const TypeRef &Type() const noexcept {
        return type_;
    }

    [[nodiscard]] std::optional<bool> BooleanValue() const noexcept;
    [[nodiscard]] std::optional<std::int64_t> SignedValue() const noexcept;
    [[nodiscard]] std::string ToLiteral() const;

private:
    friend class TypedConstantFactory;
    TypedConstant(TypeRef type, Kind kind, std::uint8_t width, std::uint64_t bits);

    TypeRef type_;
    Kind kind_;
    std::uint8_t width_;
    std::uint64_t bits_;
};

// These helpers intentionally return no value for operations that must remain
// in HIR, including traps and inputs outside the evaluator's integer model.
[[nodiscard]] std::optional<TypedConstant> ParseConstant(std::string_view literal, const TypeRef &type);
[[nodiscard]] std::optional<TypedConstant> EvaluateUnary(TokenKind op, const TypedConstant &operand);
[[nodiscard]] std::optional<TypedConstant> EvaluateBinary(TokenKind op, const TypedConstant &left,
                                                          const TypedConstant &right);
[[nodiscard]] std::optional<TypedConstant> CastConstant(const TypedConstant &value, const TypeRef &targetType);
} // namespace Rux::Optimization
