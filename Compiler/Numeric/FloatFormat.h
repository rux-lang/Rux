#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace Rux {
/// One binary floating-point representation. `precisionBits` includes the leading significand bit, whether it is
/// implicit (the interchange formats) or explicit (`float80`).
struct FloatFormat {
    std::string_view name;
    std::uint32_t valueBits = 0;
    std::uint32_t storageBytes = 0;
    std::uint32_t exponentBits = 0;
    std::uint32_t precisionBits = 0;
    std::int32_t exponentBias = 0;
    bool explicitIntegerBit = false;

    [[nodiscard]] constexpr std::uint32_t FractionBits() const noexcept {
        return precisionBits - 1;
    }

    [[nodiscard]] constexpr std::uint32_t SignificandFieldBits() const noexcept {
        return FractionBits() + (explicitIntegerBit ? 1U : 0U);
    }

    [[nodiscard]] constexpr std::uint32_t MaxExponentField() const noexcept {
        return (std::uint32_t{1} << exponentBits) - 1;
    }
};

/// The language's float formats in width order. `float8` is the IEEE-style E4M3 encoding; widths from 16 through
/// 512 use the binary interchange progression, while `float80` uses the x87 extended encoding in 16-byte storage.
[[nodiscard]] std::span<const FloatFormat> FloatFormats() noexcept;

/// @return the format with `valueBits`, or nullptr when the language has no float of that width
[[nodiscard]] const FloatFormat *FindFloatFormat(std::uint32_t valueBits) noexcept;
} // namespace Rux
