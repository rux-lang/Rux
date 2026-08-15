#pragma once

// Target-neutral scalar constant serialization for aggregate and slice data.
// Both native back ends emit the same little-endian RCU representation.

#include "CodeGen/FloatLiteral.h"
#include "CodeGen/IntegerLiteral.h"
#include "CodeGen/Layout.h"
#include "Semantic/Type.h"

#include <bit>
#include <cstdint>
#include <string_view>
#include <vector>

namespace Rux {
inline void AppendScalarConstant(std::vector<std::uint8_t> &data, const std::string_view literal, const TypeRef &type) {
    std::uint64_t bits = 0;
    if (type.kind == TypeRef::Kind::Float64) {
        bits = std::bit_cast<std::uint64_t>(ParseFloatLiteral<double>(literal));
    }
    else if (type.kind == TypeRef::Kind::Float32) {
        bits = std::bit_cast<std::uint32_t>(ParseFloatLiteral<float>(literal));
    }
    else if (type.IsBool()) {
        bits = literal == "true" || literal == "1" ? 1 : 0;
    }
    else {
        bits = ParseIntegerLiteralBits(literal).value_or(0);
    }
    for (int byte = 0; byte < Layout::SizeOf(type); ++byte) {
        data.push_back(static_cast<std::uint8_t>(bits >> (8 * byte) & 0xFFU));
    }
}
} // namespace Rux
