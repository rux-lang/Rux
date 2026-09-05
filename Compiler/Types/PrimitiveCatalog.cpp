#include "Types/PrimitiveCatalog.h"

#include <algorithm>
#include <array>

namespace Rux {
namespace {
using K = TypeRef::Kind;
using C = PrimitiveCategory;

/// A pointer-sized type carries no fixed width: its three size fields are zero and the target fills them in.
constexpr std::uint32_t PointerSized = 0;

/// The catalog.
///
/// A width is `implemented` once its representation survives every stage from literal to code generation; until then
/// the entry still fixes the spelling, family and layout that the implementation has to produce.
///
/// Alignment is capped at 16, the largest scalar alignment any supported ABI asks for, so the wider integers and
/// floats share the alignment their 128-bit counterparts already have.
constexpr std::array<PrimitiveInfo, 41> Catalog{{
    {K::Bool8, "bool8", C::Bool, 8, 1, 1, true},
    {K::Bool16, "bool16", C::Bool, 16, 2, 2, true},
    {K::Bool32, "bool32", C::Bool, 32, 4, 4, true},
    {K::Bool64, "bool64", C::Bool, 64, 8, 8, true},
    {K::Bool128, "bool128", C::Bool, 128, 16, 16, false},
    {K::Bool256, "bool256", C::Bool, 256, 32, 16, false},
    {K::Bool512, "bool512", C::Bool, 512, 64, 16, false},

    {K::Char8, "char8", C::Char, 8, 1, 1, true},
    {K::Char16, "char16", C::Char, 16, 2, 2, true},
    {K::Char32, "char32", C::Char, 32, 4, 4, true},
    {K::Char64, "char64", C::Char, 64, 8, 8, true},
    {K::Char128, "char128", C::Char, 128, 16, 16, false},
    {K::Char256, "char256", C::Char, 256, 32, 16, false},
    {K::Char512, "char512", C::Char, 512, 64, 16, false},

    {K::Int8, "int8", C::SignedInt, 8, 1, 1, true},
    {K::Int16, "int16", C::SignedInt, 16, 2, 2, true},
    {K::Int32, "int32", C::SignedInt, 32, 4, 4, true},
    {K::Int64, "int64", C::SignedInt, 64, 8, 8, true},
    {K::Int128, "int128", C::SignedInt, 128, 16, 16, true},
    {K::Int256, "int256", C::SignedInt, 256, 32, 16, true},
    {K::Int512, "int512", C::SignedInt, 512, 64, 16, true},
    {K::Int, "int", C::SignedInt, PointerSized, PointerSized, PointerSized, true},

    {K::UInt8, "uint8", C::UnsignedInt, 8, 1, 1, true},
    {K::UInt16, "uint16", C::UnsignedInt, 16, 2, 2, true},
    {K::UInt32, "uint32", C::UnsignedInt, 32, 4, 4, true},
    {K::UInt64, "uint64", C::UnsignedInt, 64, 8, 8, true},
    {K::UInt128, "uint128", C::UnsignedInt, 128, 16, 16, true},
    {K::UInt256, "uint256", C::UnsignedInt, 256, 32, 16, true},
    {K::UInt512, "uint512", C::UnsignedInt, 512, 64, 16, true},
    {K::UInt, "uint", C::UnsignedInt, PointerSized, PointerSized, PointerSized, true},

    {K::Float8, "float8", C::Float, 8, 1, 1, false},
    {K::Float16, "float16", C::Float, 16, 2, 2, false},
    {K::Float32, "float32", C::Float, 32, 4, 4, true},
    {K::Float64, "float64", C::Float, 64, 8, 8, true},
    // x87 extended precision: 80 value bits held in 16 bytes of storage, which is why size is not bits / 8.
    {K::Float80, "float80", C::Float, 80, 16, 16, false},
    {K::Float128, "float128", C::Float, 128, 16, 16, false},
    {K::Float256, "float256", C::Float, 256, 32, 16, false},
    {K::Float512, "float512", C::Float, 512, 64, 16, false},

    // A string is a borrowed view -- a pointer to its code units and a length counted in them -- so `bits` is the
    // width of one code unit of the encoding and `size` is the width of the view, the same 16 bytes whatever the
    // encoding.
    {K::String8, "string8", C::String, 8, 16, 8, true},
    {K::String16, "string16", C::String, 16, 16, 8, true},
    {K::String32, "string32", C::String, 32, 16, 8, true},
}};

// Derive the kind index from the catalog so aliases, widths and implementation status still have one owner.
constexpr auto KindIndex = [] {
    constexpr auto maximum = std::ranges::max(Catalog, {}, &PrimitiveInfo::kind).kind;
    std::array<const PrimitiveInfo *, static_cast<std::size_t>(maximum) + 1> index{};
    for (const auto &primitive : Catalog) {
        index[static_cast<std::size_t>(primitive.kind)] = &primitive;
    }
    return index;
}();

static_assert([] {
    for (const auto &primitive : Catalog) {
        if (KindIndex[static_cast<std::size_t>(primitive.kind)] != &primitive) {
            return false; // Duplicate kinds would make an earlier catalog entry unreachable.
        }
    }
    return true;
}());

/// The last code point Unicode defines, and so the ceiling on every scalar-valued character width.
constexpr std::uint32_t MaxScalarValue = 0x10FFFF;

constexpr std::array<PrimitiveAlias, 5> Aliases{{
    {"bool", K::Bool8},
    {"byte", K::UInt8},
    {"char", K::Char32},
    {"float", K::Float64},
    // `string` is the UTF-8 encoding because a bare string literal is UTF-8, where `char` is the widest character
    // because that is the one that holds a whole scalar value.
    {"string", K::String8},
}};
} // namespace

std::span<const PrimitiveInfo> PrimitiveCatalog() noexcept {
    return Catalog;
}

std::span<const PrimitiveAlias> PrimitiveAliases() noexcept {
    return Aliases;
}

const PrimitiveInfo *FindPrimitive(const TypeRef::Kind kind) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    return index < KindIndex.size() ? KindIndex[index] : nullptr;
}

const PrimitiveInfo *FindPrimitive(const std::string_view name) noexcept {
    if (const auto found = std::ranges::find(Catalog, name, &PrimitiveInfo::name); found != Catalog.end()) {
        return &*found;
    }
    const auto alias = std::ranges::find(Aliases, name, &PrimitiveAlias::name);
    return alias == Aliases.end() ? nullptr : FindPrimitive(alias->kind);
}

std::string_view CanonicalPrimitiveName(const std::string_view name) noexcept {
    const PrimitiveInfo *info = FindPrimitive(name);
    return info ? info->name : name;
}

std::optional<TypeRef> PrimitiveTypeFromName(const std::string_view name) noexcept {
    const PrimitiveInfo *info = FindPrimitive(name);
    if (!info) {
        return std::nullopt;
    }
    return TypeRef::MakePrimitive(info->kind);
}

std::optional<std::uint32_t> PrimitiveBits(const TypeRef::Kind kind, const std::uint32_t pointerBits) noexcept {
    const PrimitiveInfo *info = FindPrimitive(kind);
    if (!info) {
        return std::nullopt;
    }
    return info->bits == PointerSized ? pointerBits : info->bits;
}

std::optional<std::uint32_t> PrimitiveSize(const TypeRef::Kind kind, const std::uint32_t pointerSize) noexcept {
    const PrimitiveInfo *info = FindPrimitive(kind);
    if (!info) {
        return std::nullopt;
    }
    return info->size == PointerSized ? pointerSize : info->size;
}

std::optional<std::uint32_t> PrimitiveAlign(const TypeRef::Kind kind, const std::uint32_t pointerSize) noexcept {
    const PrimitiveInfo *info = FindPrimitive(kind);
    if (!info) {
        return std::nullopt;
    }
    return info->align == PointerSized ? pointerSize : info->align;
}

std::optional<CharacterDomain> CharacterDomainOf(const TypeRef::Kind kind) noexcept {
    const PrimitiveInfo *info = FindPrimitive(kind);
    if (!info || info->category != PrimitiveCategory::Char) {
        return std::nullopt;
    }
    // The two encoding widths carry units of that encoding; every wider character carries a scalar value, since
    // nothing above `char32` has more of Unicode to reach.
    return info->bits <= 16 ? CharacterDomain::CodeUnit : CharacterDomain::ScalarValue;
}

std::optional<std::uint32_t> MaxCharacterValue(const TypeRef::Kind kind) noexcept {
    const auto domain = CharacterDomainOf(kind);
    if (!domain) {
        return std::nullopt;
    }
    if (*domain == CharacterDomain::ScalarValue) {
        return MaxScalarValue;
    }
    const PrimitiveInfo *info = FindPrimitive(kind);
    return info->bits == 8 ? 0xFFU : 0xFFFFU;
}

TypeRef::Kind StringCodeUnitKind(const TypeRef::Kind kind) noexcept {
    const PrimitiveInfo *info = FindPrimitive(kind);
    if (!info || info->category != PrimitiveCategory::String) {
        return TypeRef::Kind::Unknown;
    }
    switch (info->bits) {
    case 8:
        return TypeRef::Kind::Char8;
    case 16:
        return TypeRef::Kind::Char16;
    default:
        return TypeRef::Kind::Char32;
    }
}

bool IsSurrogate(const std::uint64_t value) noexcept {
    return value >= 0xD800 && value <= 0xDFFF;
}

bool IsValidCharacterValue(const TypeRef::Kind kind, const std::uint64_t value) noexcept {
    // Both answers come from the same question -- whether `kind` is a character -- so both are asked once and
    // checked together rather than one being taken on the strength of the other.
    const auto domain = CharacterDomainOf(kind);
    const auto maximum = MaxCharacterValue(kind);
    if (!domain || !maximum) {
        return false;
    }
    if (value > *maximum) {
        return false;
    }
    return *domain == CharacterDomain::CodeUnit || !IsSurrogate(value);
}
} // namespace Rux
