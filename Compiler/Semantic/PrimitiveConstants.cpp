#include "Semantic/PrimitiveConstants.h"

#include <cstdint>
#include <format>

namespace Rux {
namespace {
std::optional<std::uint32_t> StorageBits(const TypeRef &type, const CompileTimeContext &context) {
    using K = TypeRef::Kind;
    switch (type.kind) {
    case K::Bool8:
    case K::Char8:
    case K::Int8:
    case K::UInt8:
        return 8;
    case K::Bool16:
    case K::Char16:
    case K::Int16:
    case K::UInt16:
        return 16;
    case K::Bool32:
    case K::Char32:
    case K::Int32:
    case K::UInt32:
    case K::Float32:
        return 32;
    case K::Int64:
    case K::UInt64:
    case K::Float64:
        return 64;
    case K::Int:
    case K::UInt:
        return static_cast<std::uint32_t>(context.target.pointer_size * 8);
    default:
        return std::nullopt;
    }
}

bool IsCharacter(const TypeRef::Kind kind) {
    return kind == TypeRef::Kind::Char8 || kind == TypeRef::Kind::Char16 || kind == TypeRef::Kind::Char32;
}

std::string SignedMin(const std::uint32_t bits) {
    if (bits == 64) {
        return "-9223372036854775808";
    }
    return std::format("-{}", std::uint64_t{1} << (bits - 1));
}

std::string SignedMax(const std::uint32_t bits) {
    if (bits == 64) {
        return "9223372036854775807";
    }
    return std::to_string((std::uint64_t{1} << (bits - 1)) - 1);
}

std::string UnsignedMax(const std::uint32_t bits) {
    if (bits == 64) {
        return "18446744073709551615";
    }
    return std::to_string((std::uint64_t{1} << bits) - 1);
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

std::optional<TypeRef> PrimitiveTypeFromName(const std::string_view name) {
    if (name == "bool" || name == "bool8")
        return TypeRef::MakeBool8();
    if (name == "bool16")
        return TypeRef::MakeBool16();
    if (name == "bool32")
        return TypeRef::MakeBool32();
    if (name == "char8")
        return TypeRef::MakeChar8();
    if (name == "char16")
        return TypeRef::MakeChar16();
    if (name == "char" || name == "char32")
        return TypeRef::MakeChar32();
    if (name == "int8")
        return TypeRef::MakeInt8();
    if (name == "int16")
        return TypeRef::MakeInt16();
    if (name == "int32")
        return TypeRef::MakeInt32();
    if (name == "int64")
        return TypeRef::MakeInt64();
    if (name == "int")
        return TypeRef::MakeInt();
    if (name == "uint8")
        return TypeRef::MakeUInt8();
    if (name == "uint16")
        return TypeRef::MakeUInt16();
    if (name == "uint32")
        return TypeRef::MakeUInt32();
    if (name == "uint64")
        return TypeRef::MakeUInt64();
    if (name == "uint")
        return TypeRef::MakeUInt();
    if (name == "float32")
        return TypeRef::MakeFloat32();
    if (name == "float" || name == "float64")
        return TypeRef::MakeFloat64();
    return std::nullopt;
}

std::optional<PrimitiveConstant> LookupPrimitiveConstant(const TypeRef &type, const std::string_view name,
                                                         const CompileTimeContext &context) {
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

    if (IsCharacter(type.kind)) {
        if (name == "Min") {
            return PrimitiveConstant{type, "0"};
        }
        if (name == "Max") {
            const std::string value = type.kind == TypeRef::Kind::Char8  ? "255"
                                    : type.kind == TypeRef::Kind::Char16 ? "65535"
                                                                         : "1114111";
            return PrimitiveConstant{type, value};
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
