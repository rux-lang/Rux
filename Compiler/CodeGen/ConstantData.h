#pragma once

// Target-neutral scalar constant serialization for aggregate and slice data.
// Both native back ends emit the same little-endian RCU representation.

#include "CodeGen/FloatLiteral.h"
#include "CodeGen/IntegerLiteral.h"
#include "CodeGen/Layout.h"
#include "Numeric/FloatFormat.h"
#include "Numeric/FloatParsing.h"
#include "Numeric/WideInteger.h"
#include "Types/PrimitiveCatalog.h"
#include "Types/Type.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <string_view>
#include <vector>

namespace Rux {
/// The bits of one scalar constant, held at `width` bits. A literal that does not read as the type it is stored as is
/// zero, which is what the scalar paths have always written for one.
[[nodiscard]] inline WideInteger ScalarConstantBits(const std::string_view literal, const TypeRef &type,
                                                    const std::uint32_t width) {
    if (type.kind == TypeRef::Kind::Float64) {
        return WideInteger::FromUnsigned(std::bit_cast<std::uint64_t>(ParseFloatLiteral<double>(literal)), width);
    }
    if (type.kind == TypeRef::Kind::Float32) {
        return WideInteger::FromUnsigned(std::bit_cast<std::uint32_t>(ParseFloatLiteral<float>(literal)), width);
    }
    if (type.IsBool()) {
        // Every bool width holds its value in the lowest byte and zeros above it, however wide it is.
        return WideInteger::FromUnsigned(literal == "true" || literal == "1" ? 1 : 0, width);
    }
    if (type.IsFloat()) {
        const PrimitiveInfo *primitive = FindPrimitive(type.kind);
        const FloatFormat *format = primitive ? FindFloatFormat(primitive->bits) : nullptr;
        const auto encoding = format ? ParseFloatEncoding(literal, *format) : std::nullopt;
        return encoding ? encoding->Bits().ConvertedTruncating(width) : WideInteger::Zero(width);
    }
    const auto parts = SplitIntegerLiteral(literal);
    const auto magnitude = parts ? WideInteger::Parse(parts->digits, parts->base, WideInteger::MaxBits) : std::nullopt;
    if (!magnitude) {
        return WideInteger::Zero(width);
    }
    const WideInteger value = magnitude->Truncated(width);
    return parts->negative ? value.Negated() : value;
}

/// Append one scalar constant to an aggregate's initialized data.
///
/// Emits exactly the type's own size in bytes, little-endian, so a struct or array literal is laid out by appending its
/// elements in order. Floats are written as their bit pattern rather than re-parsed by the backend, which is what keeps
/// both code generators emitting identical data. Wide integers, wide bools and the software floats are written word by
/// word; `float80`'s six bytes of padding come out zero because its bits stop at bit 80.
inline void AppendScalarConstant(std::vector<std::uint8_t> &data, const std::string_view literal, const TypeRef &type) {
    const int size = Layout::SizeOf(type);
    if (size <= 0) {
        return;
    }
    const auto width = static_cast<std::uint32_t>(std::min(size * 8, static_cast<int>(WideInteger::MaxBits)));
    const WideInteger bits = ScalarConstantBits(literal, type, width);
    for (int byte = 0; byte < size; ++byte) {
        const std::uint64_t word = bits.Word64(static_cast<std::size_t>(byte / 8));
        data.push_back(static_cast<std::uint8_t>(word >> (8 * (byte % 8)) & 0xFFU));
    }
}
} // namespace Rux
