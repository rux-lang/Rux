#include "CodeGen/AArch64/Encoder.h"

#include <bit>

namespace Rux {
namespace {
// A value whose set bits form one contiguous run ending at bit 0.
constexpr bool IsMask(const std::uint64_t value) {
    return value != 0 && ((value + 1) & value) == 0;
}

// A value whose set bits form one contiguous run anywhere in the word.
constexpr bool IsShiftedMask(const std::uint64_t value) {
    return value != 0 && IsMask((value - 1) | value);
}

// All ones in the low `bits` of a 64-bit word.
constexpr std::uint64_t LowMask(const unsigned bits) {
    return ~0ULL >> (64U - bits);
}

// The halfword index of the only non-zero halfword of `value`, or -1 when it
// has none or more than one. A zero value reports halfword 0, which encodes as
// MOVZ #0.
constexpr int SoleHalfword(const std::uint64_t value, const unsigned halfwords) {
    for (unsigned hw = 0; hw < halfwords; ++hw) {
        if ((value & ~(0xFFFFULL << (hw * 16U))) == 0) {
            return static_cast<int>(hw);
        }
    }
    return -1;
}
} // namespace

std::string_view A64StatusName(const A64Status status) {
    switch (status) {
    case A64Status::Ok:
        return "ok";
    case A64Status::InvalidRegister:
        return "invalid register";
    case A64Status::InvalidImmediate:
        return "invalid immediate";
    case A64Status::OutOfRange:
        return "offset out of range";
    case A64Status::Unaligned:
        return "misaligned offset";
    }
    return "unknown status";
}

std::optional<A64BitmaskImm> TryEncodeBitmaskImm(std::uint64_t value, const bool is64) {
    const unsigned regSize = is64 ? 64U : 32U;
    if (!is64 && (value >> 32U) != 0) {
        return std::nullopt;
    }
    // A run of ones cannot cover the whole register or none of it, because
    // both patterns are also every rotation of themselves.
    if (value == 0 || value == LowMask(regSize)) {
        return std::nullopt;
    }

    // The element size is the narrowest power-of-two slice the value repeats
    // in; the search stops at 2 bits, the narrowest element the encoding has.
    unsigned size = regSize;
    while (size > 2) {
        size /= 2;
        const std::uint64_t half = LowMask(size);
        if ((value & half) != ((value >> size) & half)) {
            size *= 2;
            break;
        }
    }

    // Within one element the value must be a rotation of `0*1*`. `rotation` is
    // how far right that run has moved, `ones` how long it is.
    const std::uint64_t mask = LowMask(size);
    value &= mask;
    unsigned rotation = 0;
    unsigned ones = 0;
    if (IsShiftedMask(value)) {
        rotation = static_cast<unsigned>(std::countr_zero(value));
        ones = static_cast<unsigned>(std::countr_one(value >> rotation));
    }
    else {
        // The run wraps around the top of the element, so it is the zeros that
        // are contiguous. Widening the value with ones above the element lets
        // the same count work on the whole word.
        value |= ~mask;
        if (!IsShiftedMask(~value)) {
            return std::nullopt;
        }
        const unsigned leadingOnes = static_cast<unsigned>(std::countl_one(value));
        rotation = 64U - leadingOnes;
        ones = leadingOnes + static_cast<unsigned>(std::countr_one(value)) - (64U - size);
    }

    // immr rotates the canonical run right into place; imms carries the run
    // length in the low bits under a prefix that names the element size, and
    // the N bit is the sixth bit of that prefix, inverted.
    const std::uint64_t imms = (~static_cast<std::uint64_t>(size - 1U) << 1U) | (ones - 1U);
    A64BitmaskImm encoded;
    encoded.n = static_cast<std::uint8_t>(((imms >> 6U) & 1U) ^ 1U);
    encoded.immr = static_cast<std::uint8_t>((size - rotation) & (size - 1U));
    encoded.imms = static_cast<std::uint8_t>(imms & 0x3FU);
    return encoded;
}

std::optional<A64ArithImm> TryEncodeArithImm12(const std::uint64_t value) {
    if (value <= 0xFFFU) {
        return A64ArithImm{static_cast<std::uint16_t>(value), false};
    }
    if ((value & 0xFFFU) == 0 && (value >> 12U) <= 0xFFFU) {
        return A64ArithImm{static_cast<std::uint16_t>(value >> 12U), true};
    }
    return std::nullopt;
}

std::optional<A64MovwImm> TryEncodeMovwImm(const std::uint64_t value, const bool is64) {
    const unsigned regSize = is64 ? 64U : 32U;
    if (!is64 && (value >> 32U) != 0) {
        return std::nullopt;
    }

    const unsigned halfwords = regSize / 16U;
    if (const int hw = SoleHalfword(value, halfwords); hw >= 0) {
        const unsigned shift = static_cast<unsigned>(hw) * 16U;
        return A64MovwImm{static_cast<std::uint16_t>(value >> shift), static_cast<std::uint8_t>(hw), false};
    }
    // MOVN writes the inverse, so a value that is mostly ones costs one
    // instruction too.
    const std::uint64_t inverted = ~value & LowMask(regSize);
    if (const int hw = SoleHalfword(inverted, halfwords); hw >= 0) {
        const unsigned shift = static_cast<unsigned>(hw) * 16U;
        return A64MovwImm{static_cast<std::uint16_t>(inverted >> shift), static_cast<std::uint8_t>(hw), true};
    }
    return std::nullopt;
}

std::uint32_t A64Enc::Size() const {
    return static_cast<std::uint32_t>(out.size());
}

void A64Enc::Word(const std::uint32_t word) const {
    out.push_back(static_cast<std::uint8_t>(word & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((word >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((word >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((word >> 24U) & 0xFFU));
}

std::uint32_t A64Enc::WordAt(const std::uint32_t offset) const {
    std::uint32_t word = 0;
    for (std::uint32_t i = 0; i < InstrSize; ++i) {
        word |= static_cast<std::uint32_t>(out[offset + i]) << (i * 8U);
    }
    return word;
}

void A64Enc::PatchWord(const std::uint32_t offset, const std::uint32_t word) const {
    for (std::uint32_t i = 0; i < InstrSize; ++i) {
        out[offset + i] = static_cast<std::uint8_t>((word >> (i * 8U)) & 0xFFU);
    }
}

void A64Enc::PatchField(const std::uint32_t offset, const unsigned lsb, const unsigned width,
                        const std::uint32_t value) const {
    const std::uint32_t mask = width >= 32U ? ~0U : (1U << width) - 1U;
    const std::uint32_t word = (WordAt(offset) & ~(mask << lsb)) | ((value & mask) << lsb);
    PatchWord(offset, word);
}
} // namespace Rux
