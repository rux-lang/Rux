#include "Semantic/PrimitiveConstants.h"

#include "Numeric/WideInteger.h"
#include "Semantic/PrimitiveCatalog.h"

#include <cstdint>
#include <format>

namespace Rux {
namespace {
/// The width the target stores this type at.
///
/// Read from the target rather than the host, because a pointer-sized type's limits are the target's: `usize::Max`
/// must differ between a 32- and a 64-bit target compiled from the same machine.
///
/// @return nullopt for a type that is not a primitive, or whose representation is still reserved
std::optional<std::uint32_t> StorageBits(const TypeRef &type, const CompileTimeContext &context) {
    const PrimitiveInfo *info = FindPrimitive(type.kind);
    if (!info || !info->implemented) {
        return std::nullopt;
    }
    return PrimitiveBits(type.kind, static_cast<std::uint32_t>(context.target.pointer_size * 8));
}

/// The limits of an integer of this width, rendered as decimal text.
///
/// Computed in a `WideInteger` rather than a machine word: past 64 bits a host integer cannot hold the value at all,
/// and shifting one by the width is undefined, which is how every width above 64 previously reported zero.
std::string SignedMin(const std::uint32_t bits) {
    return "-" + WideInteger::MinMagnitude(bits, true).ToDecimal();
}

std::string SignedMax(const std::uint32_t bits) {
    return WideInteger::MaxValue(bits, true).ToDecimal();
}

std::string UnsignedMax(const std::uint32_t bits) {
    return WideInteger::MaxValue(bits, false).ToDecimal();
}

std::optional<std::string_view> FloatConstant(const TypeRef::Kind kind, const std::string_view name) {
    if (kind == TypeRef::Kind::Float32) {
        if (name == "Lowest")
            return "-3.4028234663852886e+38";
        if (name == "Max")
            return "3.4028234663852886e+38";
        if (name == "MinPositive")
            return "1.1754943508222875e-38";
        if (name == "Epsilon")
            return "1.1920928955078125e-7";
        if (name == "Infinity")
            return "inf";
        if (name == "NaN")
            return "nan";
    }
    if (kind == TypeRef::Kind::Float64) {
        if (name == "Lowest")
            return "-1.7976931348623157e+308";
        if (name == "Max")
            return "1.7976931348623157e+308";
        if (name == "MinPositive")
            return "2.2250738585072014e-308";
        if (name == "Epsilon")
            return "2.2204460492503131e-16";
        if (name == "Infinity")
            return "inf";
        if (name == "NaN")
            return "nan";
    }
    return std::nullopt;
}
} // namespace

std::optional<PrimitiveConstant> LookupPrimitiveConstant(const TypeRef &type, const std::string_view name,
                                                         const CompileTimeContext &context) {
    // A string has no width constants: `Bits` and `Bytes` would have to choose between describing the view and
    // describing one code unit of the encoding, and `Min` and `Max` describe neither.
    if (type.IsString()) {
        return std::nullopt;
    }

    const auto bits = StorageBits(type, context);
    if (!bits) {
        return std::nullopt;
    }

    // Width metadata is target-native because it is commonly used in sizing
    // and target-selection expressions.
    if (name == "Bits") {
        return PrimitiveConstant{TypeRef::MakeUInt(), std::to_string(*bits)};
    }
    if (name == "Bytes") {
        return PrimitiveConstant{TypeRef::MakeUInt(), std::to_string(*bits / 8)};
    }

    if (type.IsInteger()) {
        if (name == "Min") {
            return PrimitiveConstant{type, type.IsSigned() ? SignedMin(*bits) : "0"};
        }
        if (name == "Max") {
            return PrimitiveConstant{type, type.IsSigned() ? SignedMax(*bits) : UnsignedMax(*bits)};
        }
        return std::nullopt;
    }

    if (type.IsChar()) {
        if (name == "Min") {
            return PrimitiveConstant{type, "0"};
        }
        if (name == "Max") {
            // The width's own ceiling: its last code unit, or U+10FFFF once the width carries scalar values, however
            // much room it has beyond that.
            return PrimitiveConstant{type, std::to_string(*MaxCharacterValue(type.kind))};
        }
        return std::nullopt;
    }

    if (const auto value = FloatConstant(type.kind, name)) {
        return PrimitiveConstant{type, std::string{*value}};
    }
    return std::nullopt;
}

std::optional<PrimitiveConstant> LookupPrimitiveConstant(const std::string_view typeName, const std::string_view name,
                                                         const CompileTimeContext &context) {
    const auto type = PrimitiveTypeFromName(typeName);
    return type ? LookupPrimitiveConstant(*type, name, context) : std::nullopt;
}
} // namespace Rux
