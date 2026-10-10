#pragma once

// Target-neutral constant serialization for module constants, arrays and slices.
// Both native back ends and the assembly printer emit the same little-endian representation.

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
#include <string>
#include <string_view>
#include <unordered_set>
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

/// The bytes a constant that is not a sequence publishes under its name: its type's runtime size, each scalar of its
/// value at the offset its path of members reaches, and zero in every byte no scalar covers, which is the padding of a
/// struct or tuple and the six high bytes of a `float80`. A scalar constant is its one scalar at offset zero.
///
/// The offsets come from the same layout rules a field or element access reads, so `*@C` and `(@C).field` see the
/// value the constant names. A scalar whose place would reach past the value is left out rather than written over a
/// neighbour; the HIR lowering only records paths into the constant's own type, so none does.
///
/// Module variables start as the same bytes, so their initial value is laid out by the same rule.
[[nodiscard]] inline std::vector<std::uint8_t> ValueBytes(const TypeRef &type,
                                                          const std::vector<LirConstScalar> &scalars,
                                                          const Layout::LayoutMap &layouts,
                                                          const std::unordered_set<std::string> &interfaceNames) {
    const int size = Layout::RuntimeSizeOf(type, layouts, interfaceNames);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(std::max(size, 0)), 0);
    std::vector<std::uint8_t> scalar;
    for (const LirConstScalar &part : scalars) {
        int offset = 0;
        for (const LirConstMember &member : part.path) {
            if (member.aggregate.kind == TypeRef::Kind::Array) {
                const int stride = member.aggregate.inner.empty()
                                     ? 0
                                     : Layout::RuntimeSizeOf(member.aggregate.inner.front(), layouts, interfaceNames);
                offset += stride * std::stoi(member.member);
            }
            else {
                offset += Layout::FieldOffsetOf(TypeRef::MakePointer(member.aggregate), member.member, layouts,
                                                interfaceNames);
            }
        }
        scalar.clear();
        AppendScalarConstant(scalar, part.literal, part.type);
        if (offset < 0 || static_cast<std::size_t>(offset) + scalar.size() > bytes.size()) {
            continue;
        }
        std::ranges::copy(scalar, bytes.begin() + offset);
    }
    return bytes;
}

[[nodiscard]] inline std::vector<std::uint8_t>
ConstantValueBytes(const LirConstDecl &constant, const Layout::LayoutMap &layouts,
                   const std::unordered_set<std::string> &interfaceNames) {
    return ValueBytes(constant.type, constant.scalars, layouts, interfaceNames);
}

/// The alignment a value of `type` is placed at in module storage, the one `alignof` reports for the type.
[[nodiscard]] inline int ValueAlignment(const TypeRef &type, const Layout::LayoutMap &layouts,
                                        const std::unordered_set<std::string> &interfaceNames) {
    return std::max(Layout::RuntimeAlignmentOf(type, layouts, interfaceNames), 1);
}

/// The alignment a constant that is not a sequence is placed at, the one `alignof` reports for its type.
[[nodiscard]] inline int ConstantValueAlignment(const LirConstDecl &constant, const Layout::LayoutMap &layouts,
                                                const std::unordered_set<std::string> &interfaceNames) {
    return ValueAlignment(constant.type, layouts, interfaceNames);
}
} // namespace Rux
