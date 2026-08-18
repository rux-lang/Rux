#include "Numeric/WideInteger.h"

#include <algorithm>
#include <bit>

namespace Rux {
namespace {
/// Clamp a requested width into the range a value can actually occupy.
[[nodiscard]] std::uint32_t ClampWidth(const std::uint32_t width) noexcept {
    return std::clamp<std::uint32_t>(width, 1, WideInteger::MaxBits);
}

/// The number of limbs a `width`-bit value spans, rounded up.
[[nodiscard]] std::size_t LimbsFor(const std::uint32_t width) noexcept {
    return (width + WideInteger::LimbBits - 1) / WideInteger::LimbBits;
}

/// @return the value of `digit` in `base`, or nullopt when it is not one of that base's digits
[[nodiscard]] std::optional<unsigned> DigitValue(const char digit, const unsigned base) noexcept {
    unsigned value = 0;
    if (digit >= '0' && digit <= '9') {
        value = static_cast<unsigned>(digit - '0');
    }
    else if (digit >= 'a' && digit <= 'f') {
        value = static_cast<unsigned>(digit - 'a') + 10;
    }
    else if (digit >= 'A' && digit <= 'F') {
        value = static_cast<unsigned>(digit - 'A') + 10;
    }
    else {
        return std::nullopt;
    }
    return value < base ? std::optional{value} : std::nullopt;
}
} // namespace

WideInteger WideInteger::Zero(const std::uint32_t width) noexcept {
    WideInteger value;
    value.width = ClampWidth(width);
    return value;
}

WideInteger WideInteger::FromUnsigned(const std::uint64_t source, const std::uint32_t width) noexcept {
    WideInteger value = Zero(width);
    value.limbs[0] = static_cast<Limb>(source & 0xFFFFFFFFU);
    value.limbs[1] = static_cast<Limb>(source >> LimbBits);
    value.Normalize();
    return value;
}

WideInteger WideInteger::FromWords(const std::span<const std::uint64_t> words, const std::uint32_t width) noexcept {
    WideInteger value = Zero(width);
    const std::size_t count = std::min(words.size(), MaxLimbs / 2);
    for (std::size_t index = 0; index < count; ++index) {
        value.limbs[index * 2] = static_cast<Limb>(words[index] & 0xFFFFFFFFU);
        value.limbs[index * 2 + 1] = static_cast<Limb>(words[index] >> LimbBits);
    }
    value.Normalize();
    return value;
}

WideInteger WideInteger::AllOnes(const std::uint32_t width) noexcept {
    WideInteger value = Zero(width);
    value.limbs.fill(~Limb{0});
    value.Normalize();
    return value;
}

WideInteger WideInteger::MaxValue(const std::uint32_t width, const bool isSigned) noexcept {
    const std::uint32_t clamped = ClampWidth(width);
    if (!isSigned) {
        return AllOnes(clamped);
    }
    // The top bit is the sign, so a signed width tops out one bit lower: every bit below the sign bit set.
    WideInteger value = AllOnes(clamped);
    const std::uint32_t signBit = clamped - 1;
    value.limbs[signBit / LimbBits] &= ~(Limb{1} << (signBit % LimbBits));
    return value;
}

WideInteger WideInteger::MinMagnitude(const std::uint32_t width, const bool isSigned) noexcept {
    const std::uint32_t clamped = ClampWidth(width);
    if (!isSigned) {
        return Zero(clamped);
    }
    // 2^(width-1): the magnitude of the most negative value, one past the largest positive one.
    WideInteger value = Zero(clamped);
    const std::uint32_t signBit = clamped - 1;
    value.limbs[signBit / LimbBits] = Limb{1} << (signBit % LimbBits);
    return value;
}

void WideInteger::Normalize() noexcept {
    const std::size_t used = LimbsFor(width);
    for (std::size_t index = used; index < MaxLimbs; ++index) {
        limbs[index] = 0;
    }
    if (const std::uint32_t topBits = width % LimbBits; topBits != 0) {
        limbs[used - 1] &= (Limb{1} << topBits) - 1;
    }
}

bool WideInteger::MultiplyAdd(const Limb factor, const Limb addend) noexcept {
    const std::size_t used = LimbsFor(width);
    std::uint64_t carry = addend;
    for (std::size_t index = 0; index < used; ++index) {
        const std::uint64_t product = static_cast<std::uint64_t>(limbs[index]) * factor + carry;
        limbs[index] = static_cast<Limb>(product & 0xFFFFFFFFU);
        carry = product >> LimbBits;
    }
    if (carry != 0) {
        return false;
    }
    // A zero carry is not enough on its own: the top limb may lie only partly inside the width.
    if (const std::uint32_t topBits = width % LimbBits; topBits != 0) {
        return (limbs[used - 1] >> topBits) == 0;
    }
    return true;
}

WideInteger::Limb WideInteger::DivideBy(const Limb divisor) noexcept {
    std::uint64_t remainder = 0;
    for (std::size_t index = LimbsFor(width); index-- > 0;) {
        const std::uint64_t current = (remainder << LimbBits) | limbs[index];
        limbs[index] = static_cast<Limb>(current / divisor);
        remainder = current % divisor;
    }
    return static_cast<Limb>(remainder);
}

std::optional<WideInteger> WideInteger::Parse(const std::string_view digits, const unsigned base,
                                              const std::uint32_t width) {
    if (base < 2 || base > 16) {
        return std::nullopt;
    }
    WideInteger value = Zero(width);
    bool sawDigit = false;
    for (const char character : digits) {
        if (character == '_') {
            continue;
        }
        const auto digit = DigitValue(character, base);
        if (!digit) {
            return std::nullopt;
        }
        sawDigit = true;
        if (!value.MultiplyAdd(static_cast<Limb>(base), static_cast<Limb>(*digit))) {
            return std::nullopt;
        }
    }
    return sawDigit ? std::optional{value} : std::nullopt;
}

bool WideInteger::IsZero() const noexcept {
    return std::ranges::all_of(limbs, [](const Limb limb) { return limb == 0; });
}

bool WideInteger::IsNegative() const noexcept {
    return BitSet(width - 1);
}

bool WideInteger::BitSet(const std::uint32_t index) const noexcept {
    if (index >= width) {
        return false;
    }
    return (limbs[index / LimbBits] >> (index % LimbBits) & 1U) != 0;
}

std::uint64_t WideInteger::Word64(const std::size_t index) const noexcept {
    const std::size_t low = index * 2;
    if (low >= MaxLimbs) {
        return 0;
    }
    const std::uint64_t high = low + 1 < MaxLimbs ? limbs[low + 1] : 0;
    return static_cast<std::uint64_t>(limbs[low]) | (high << LimbBits);
}

std::optional<std::uint64_t> WideInteger::ToUnsigned() const noexcept {
    for (std::size_t index = 2; index < MaxLimbs; ++index) {
        if (limbs[index] != 0) {
            return std::nullopt;
        }
    }
    return Word64(0);
}

std::string WideInteger::ToDecimal() const {
    if (IsZero()) {
        return "0";
    }
    // Peel nine digits at a time: 10^9 is the largest power of ten a limb holds, so every division stays exact in the
    // 64-bit arithmetic above. Digits come out least significant first and the whole string is reversed once.
    constexpr Limb Chunk = 1000000000;
    std::string digits;
    WideInteger remaining = *this;
    while (!remaining.IsZero()) {
        Limb chunk = remaining.DivideBy(Chunk);
        for (int written = 0; written < 9; ++written) {
            digits.push_back(static_cast<char>('0' + chunk % 10));
            chunk /= 10;
        }
    }
    while (digits.size() > 1 && digits.back() == '0') {
        digits.pop_back();
    }
    std::ranges::reverse(digits);
    return digits;
}

WideInteger WideInteger::Negated() const noexcept {
    WideInteger result = *this;
    std::uint64_t carry = 1;
    for (std::size_t index = 0; index < MaxLimbs; ++index) {
        const std::uint64_t complemented = static_cast<std::uint64_t>(static_cast<Limb>(~limbs[index])) + carry;
        result.limbs[index] = static_cast<Limb>(complemented & 0xFFFFFFFFU);
        carry = complemented >> LimbBits;
    }
    result.Normalize();
    return result;
}

WideInteger WideInteger::Added(const WideInteger &other) const noexcept {
    WideInteger result = Zero(width);
    std::uint64_t carry = 0;
    for (std::size_t index = 0; index < LimbsFor(width); ++index) {
        const std::uint64_t sum = static_cast<std::uint64_t>(limbs[index]) + other.limbs[index] + carry;
        result.limbs[index] = static_cast<Limb>(sum & 0xFFFFFFFFU);
        carry = sum >> LimbBits;
    }
    result.Normalize();
    return result;
}

WideInteger WideInteger::Subtracted(const WideInteger &other) const noexcept {
    WideInteger result = Zero(width);
    std::uint64_t borrow = 0;
    for (std::size_t index = 0; index < LimbsFor(width); ++index) {
        const std::uint64_t subtrahend = static_cast<std::uint64_t>(other.limbs[index]) + borrow;
        result.limbs[index] = static_cast<Limb>(static_cast<std::uint64_t>(limbs[index]) - subtrahend);
        borrow = static_cast<std::uint64_t>(limbs[index]) < subtrahend ? 1 : 0;
    }
    result.Normalize();
    return result;
}

WideInteger WideInteger::BitwiseNot() const noexcept {
    WideInteger result = *this;
    for (Limb &limb : result.limbs) {
        limb = ~limb;
    }
    result.Normalize();
    return result;
}

WideInteger WideInteger::BitwiseAnd(const WideInteger &other) const noexcept {
    WideInteger result = *this;
    for (std::size_t index = 0; index < MaxLimbs; ++index) {
        result.limbs[index] &= other.limbs[index];
    }
    result.Normalize();
    return result;
}

WideInteger WideInteger::BitwiseOr(const WideInteger &other) const noexcept {
    WideInteger result = *this;
    for (std::size_t index = 0; index < MaxLimbs; ++index) {
        result.limbs[index] |= other.limbs[index];
    }
    result.Normalize();
    return result;
}

WideInteger WideInteger::BitwiseXor(const WideInteger &other) const noexcept {
    WideInteger result = *this;
    for (std::size_t index = 0; index < MaxLimbs; ++index) {
        result.limbs[index] ^= other.limbs[index];
    }
    result.Normalize();
    return result;
}

WideInteger WideInteger::ShiftedLeft(const std::uint32_t amount) const noexcept {
    if (amount >= width) {
        return Zero(width);
    }
    WideInteger result = Zero(width);
    const std::size_t wholeLimbs = amount / LimbBits;
    const std::uint32_t partialBits = amount % LimbBits;
    const std::size_t used = LimbsFor(width);
    for (std::size_t source = 0; source < used; ++source) {
        const std::size_t destination = source + wholeLimbs;
        if (destination >= used) {
            break;
        }
        result.limbs[destination] |= limbs[source] << partialBits;
        if (partialBits != 0 && destination + 1 < used) {
            result.limbs[destination + 1] |= limbs[source] >> (LimbBits - partialBits);
        }
    }
    result.Normalize();
    return result;
}

WideInteger WideInteger::ShiftedRight(const std::uint32_t amount, const bool isSigned) const noexcept {
    const bool fill = isSigned && IsNegative();
    if (amount >= width) {
        return fill ? AllOnes(width) : Zero(width);
    }
    WideInteger result = Zero(width);
    const std::size_t wholeLimbs = amount / LimbBits;
    const std::uint32_t partialBits = amount % LimbBits;
    const std::size_t used = LimbsFor(width);
    for (std::size_t destination = 0; destination + wholeLimbs < used; ++destination) {
        const std::size_t source = destination + wholeLimbs;
        result.limbs[destination] = limbs[source] >> partialBits;
        if (partialBits != 0 && source + 1 < used) {
            result.limbs[destination] |= limbs[source + 1] << (LimbBits - partialBits);
        }
    }
    if (fill) {
        for (std::uint32_t index = width - amount; index < width; ++index) {
            result.limbs[index / LimbBits] |= Limb{1} << (index % LimbBits);
        }
    }
    result.Normalize();
    return result;
}

WideInteger WideInteger::RotatedLeft(const std::uint32_t amount) const noexcept {
    const std::uint32_t rotation = amount % width;
    if (rotation == 0) {
        return *this;
    }
    return ShiftedLeft(rotation).BitwiseOr(ShiftedRight(width - rotation, false));
}

WideInteger WideInteger::RotatedRight(const std::uint32_t amount) const noexcept {
    const std::uint32_t rotation = amount % width;
    if (rotation == 0) {
        return *this;
    }
    return ShiftedRight(rotation, false).BitwiseOr(ShiftedLeft(width - rotation));
}

std::strong_ordering WideInteger::Compare(const WideInteger &other, const bool isSigned) const noexcept {
    if (!isSigned) {
        return *this <=> other;
    }
    const std::uint32_t commonWidth = std::max(width, other.width);
    const WideInteger left = Extended(commonWidth, true);
    const WideInteger right = other.Extended(commonWidth, true);
    if (left.IsNegative() != right.IsNegative()) {
        return left.IsNegative() ? std::strong_ordering::less : std::strong_ordering::greater;
    }
    return left <=> right;
}

std::uint32_t WideInteger::CountLeadingZeros() const noexcept {
    const std::size_t used = LimbsFor(width);
    const std::uint32_t topBits = width % LimbBits == 0 ? LimbBits : width % LimbBits;
    std::uint32_t count = 0;
    for (std::size_t index = used; index-- > 0;) {
        const std::uint32_t available = index == used - 1 ? topBits : LimbBits;
        if (limbs[index] == 0) {
            count += available;
            continue;
        }
        count += std::countl_zero(limbs[index]) - (LimbBits - available);
        break;
    }
    return count;
}

std::uint32_t WideInteger::CountTrailingZeros() const noexcept {
    std::uint32_t count = 0;
    for (std::size_t index = 0; index < LimbsFor(width); ++index) {
        if (limbs[index] == 0) {
            count += LimbBits;
            continue;
        }
        count += std::countr_zero(limbs[index]);
        break;
    }
    return std::min(count, width);
}

std::uint32_t WideInteger::PopulationCount() const noexcept {
    std::uint32_t count = 0;
    for (std::size_t index = 0; index < LimbsFor(width); ++index) {
        count += std::popcount(limbs[index]);
    }
    return count;
}

WideIntegerProduct WideInteger::MultipliedFull(const WideInteger &other) const noexcept {
    std::array<Limb, MaxLimbs * 2> product{};
    const std::size_t used = LimbsFor(width);
    for (std::size_t leftIndex = 0; leftIndex < used; ++leftIndex) {
        std::uint64_t carry = 0;
        for (std::size_t rightIndex = 0; rightIndex < used; ++rightIndex) {
            const std::size_t resultIndex = leftIndex + rightIndex;
            const std::uint64_t partial =
                static_cast<std::uint64_t>(limbs[leftIndex]) * other.limbs[rightIndex] + product[resultIndex] + carry;
            product[resultIndex] = static_cast<Limb>(partial & 0xFFFFFFFFU);
            carry = partial >> LimbBits;
        }
        product[leftIndex + used] = static_cast<Limb>(carry);
    }

    WideIntegerProduct result{Zero(width), Zero(width)};
    for (std::uint32_t bit = 0; bit < width * 2; ++bit) {
        if ((product[bit / LimbBits] & (Limb{1} << (bit % LimbBits))) == 0) {
            continue;
        }
        WideInteger &half = bit < width ? result.low : result.high;
        const std::uint32_t halfBit = bit < width ? bit : bit - width;
        half.limbs[halfBit / LimbBits] |= Limb{1} << (halfBit % LimbBits);
    }
    result.low.Normalize();
    result.high.Normalize();
    return result;
}

WideInteger WideInteger::MultipliedWrapping(const WideInteger &other) const noexcept {
    return MultipliedFull(other).low;
}

std::optional<WideInteger> WideInteger::MultipliedChecked(const WideInteger &other,
                                                          const bool isSigned) const noexcept {
    if (!isSigned) {
        const WideIntegerProduct product = MultipliedFull(other);
        return product.high.IsZero() ? std::optional{product.low} : std::nullopt;
    }

    const bool negative = IsNegative() != other.IsNegative();
    const WideIntegerProduct magnitude = Magnitude(true).MultipliedFull(other.Magnitude(true));
    const WideInteger limit = negative ? MinMagnitude(width, true) : MaxValue(width, true);
    if (!magnitude.high.IsZero() || magnitude.low > limit) {
        return std::nullopt;
    }
    return negative ? magnitude.low.Negated() : magnitude.low;
}

WideInteger WideInteger::Magnitude(const bool isSigned) const noexcept {
    return isSigned && IsNegative() ? Negated() : *this;
}

WideInteger WideInteger::Truncated(const std::uint32_t newWidth) const noexcept {
    WideInteger result = *this;
    result.width = ClampWidth(newWidth);
    result.Normalize();
    return result;
}

WideInteger WideInteger::Extended(const std::uint32_t newWidth, const bool isSigned) const noexcept {
    const std::uint32_t clamped = ClampWidth(newWidth);
    if (clamped <= width) {
        return Truncated(clamped);
    }

    WideInteger result = *this;
    const bool fill = isSigned && IsNegative();
    result.width = clamped;
    if (fill) {
        for (std::uint32_t index = width; index < clamped; ++index) {
            result.limbs[index / LimbBits] |= Limb{1} << (index % LimbBits);
        }
    }
    result.Normalize();
    return result;
}

bool WideInteger::operator==(const WideInteger &other) const noexcept {
    return limbs == other.limbs;
}

std::strong_ordering WideInteger::operator<=>(const WideInteger &other) const noexcept {
    for (std::size_t index = MaxLimbs; index-- > 0;) {
        if (limbs[index] != other.limbs[index]) {
            return limbs[index] <=> other.limbs[index];
        }
    }
    return std::strong_ordering::equal;
}
} // namespace Rux
