#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Rux {
/// A fixed-capacity unsigned integer wide enough for every integer width the language spells, up to 512 bits.
///
/// The value is held in 32-bit limbs, least significant first. A limb is half a machine word so that a limb-by-limb
/// multiply lands in a `std::uint64_t` and a two-limb divide by one limb fits one too: the whole of the arithmetic
/// below is then exact in plain C++, with no 128-bit extension and no reliance on the host's word size. Code
/// generation reads the value back as 64-bit words, which is what the machine stores.
///
/// The value is always normalized: every bit at or above `Width()` is zero, so two values compare by their limbs
/// alone. Signedness is not part of the value -- these are bit patterns, and the caller's type decides whether the top
/// bit means a sign. `Negated` is two's complement at the value's own width, which is what makes the most negative
/// value of a width representable.
class WideInteger {
public:
    using Limb = std::uint32_t;

    static constexpr std::uint32_t LimbBits = 32;
    static constexpr std::uint32_t MaxBits = 512;
    static constexpr std::size_t MaxLimbs = MaxBits / LimbBits;

    WideInteger() = default;

    /// Zero at `width` bits. A width above `MaxBits` is clamped to it, and a width of zero is treated as one bit, so
    /// a value always has somewhere to live.
    [[nodiscard]] static WideInteger Zero(std::uint32_t width) noexcept;

    /// `value` at `width` bits, truncated to it when it does not fit.
    [[nodiscard]] static WideInteger FromUnsigned(std::uint64_t value, std::uint32_t width) noexcept;

    /// Little-endian 64-bit `words` at `width` bits, truncated to it when they do not fit. Missing words are zero.
    [[nodiscard]] static WideInteger FromWords(std::span<const std::uint64_t> words, std::uint32_t width) noexcept;

    /// Every bit below `width` set: the largest value the width holds, read as unsigned.
    [[nodiscard]] static WideInteger AllOnes(std::uint32_t width) noexcept;

    /// The largest value a `width`-bit integer of this signedness holds: 2^(width-1) - 1 when signed, 2^width - 1
    /// when not.
    [[nodiscard]] static WideInteger MaxValue(std::uint32_t width, bool isSigned) noexcept;

    /// The magnitude of the smallest value a `width`-bit integer of this signedness holds: 2^(width-1) when signed,
    /// zero otherwise. Returned as a magnitude because the value itself is negative and this type is unsigned.
    [[nodiscard]] static WideInteger MinMagnitude(std::uint32_t width, bool isSigned) noexcept;

    /// Decode `digits` in `base` at `width` bits.
    ///
    /// `digits` carries no sign, no base prefix and no suffix; underscores are ignored wherever they appear.
    ///
    /// @return nullopt when `digits` is empty, holds a character that is not a digit of `base`, or names a value that
    ///         needs more than `width` bits
    [[nodiscard]] static std::optional<WideInteger> Parse(std::string_view digits, unsigned base, std::uint32_t width);

    [[nodiscard]] std::uint32_t Width() const noexcept {
        return width;
    }

    [[nodiscard]] bool IsZero() const noexcept;

    /// Whether the top bit is set, which means the bit pattern is negative when its type is signed.
    [[nodiscard]] bool IsNegative() const noexcept;

    /// Whether bit `index` is set. A bit at or above the width is always zero.
    [[nodiscard]] bool BitSet(std::uint32_t index) const noexcept;

    /// The 64-bit word at `index`, least significant first, which is the form code generation emits.
    [[nodiscard]] std::uint64_t Word64(std::size_t index) const noexcept;

    /// @return the value as a machine word, or nullopt when it needs more than 64 bits
    [[nodiscard]] std::optional<std::uint64_t> ToUnsigned() const noexcept;

    /// The value in base ten, without a sign. Never empty: zero renders as "0".
    [[nodiscard]] std::string ToDecimal() const;

    /// Two's complement at this width, so zero negates to zero and every other value wraps the way the machine wraps
    /// it.
    [[nodiscard]] WideInteger Negated() const noexcept;

    /// Wrapping addition at this value's width. The right operand is read as a bit pattern at the same width.
    [[nodiscard]] WideInteger Added(const WideInteger &other) const noexcept;

    /// Wrapping subtraction at this value's width. The right operand is read as a bit pattern at the same width.
    [[nodiscard]] WideInteger Subtracted(const WideInteger &other) const noexcept;

    [[nodiscard]] WideInteger BitwiseNot() const noexcept;
    [[nodiscard]] WideInteger BitwiseAnd(const WideInteger &other) const noexcept;
    [[nodiscard]] WideInteger BitwiseOr(const WideInteger &other) const noexcept;
    [[nodiscard]] WideInteger BitwiseXor(const WideInteger &other) const noexcept;

    /// The unsigned magnitude obtained by interpreting this bit pattern with the requested signedness.
    [[nodiscard]] WideInteger Magnitude(bool isSigned) const noexcept;

    /// The same value restated at `newWidth`, truncating any bit that no longer fits.
    [[nodiscard]] WideInteger Truncated(std::uint32_t newWidth) const noexcept;

    /// The same value restated at a wider width. A signed negative value is sign-extended; every other value is
    /// zero-extended. Narrowing is identical to `Truncated`.
    [[nodiscard]] WideInteger Extended(std::uint32_t newWidth, bool isSigned) const noexcept;

    [[nodiscard]] bool operator==(const WideInteger &other) const noexcept;

    /// Ordering by magnitude, which is unsigned ordering. A signed comparison belongs to the type that knows the
    /// operands are signed, not to the bit pattern.
    [[nodiscard]] std::strong_ordering operator<=>(const WideInteger &other) const noexcept;

private:
    /// Clear every bit at or above the width, which is the invariant every operation restores before returning.
    void Normalize() noexcept;

    /// Multiply by `factor` and add `addend`, both single limbs.
    ///
    /// @return false when the result needed more than the width, leaving the value unspecified
    [[nodiscard]] bool MultiplyAdd(Limb factor, Limb addend) noexcept;

    /// Divide by `divisor` in place.
    ///
    /// @return the remainder
    [[nodiscard]] Limb DivideBy(Limb divisor) noexcept;

    std::array<Limb, MaxLimbs> limbs{};
    std::uint32_t width = MaxBits;
};
} // namespace Rux
