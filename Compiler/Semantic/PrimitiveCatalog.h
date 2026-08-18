#pragma once

#include "Semantic/Type.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace Rux {
/// The family a primitive belongs to. Rules that hold for a whole family -- bool normalization, Unicode validation,
/// integer promotion, float rounding -- ask this instead of listing every kind in the family.
enum class PrimitiveCategory : std::uint8_t {
    Bool,
    Char,
    SignedInt,
    UnsignedInt,
    Float,
};

/// One primitive type, described once.
///
/// Every place that needs a primitive's spelling, width, storage size or alignment reads it from this table, so a
/// width is added in one place rather than in each switch that happens to enumerate widths.
struct PrimitiveInfo {
    TypeRef::Kind kind = TypeRef::Kind::Unknown;
    /// Canonical spelling. An alias resolves to this, so nothing downstream ever sees the alias.
    std::string_view name;
    PrimitiveCategory category = PrimitiveCategory::UnsignedInt;
    /// Value width in bits, or 0 for a pointer-sized type whose width the target supplies.
    std::uint32_t bits = 0;
    /// Storage size in bytes, or 0 for a pointer-sized type. `float80` stores 80 value bits in 16 bytes, so this is
    /// deliberately not `bits / 8`.
    std::uint32_t size = 0;
    /// Storage alignment in bytes, or 0 for a pointer-sized type. Capped at 16, the largest scalar alignment any
    /// supported ABI asks for. Record layout caps field alignment further; see `Target/Layout.h`.
    std::uint32_t align = 0;
    /// Whether the representation is lowered end to end. A reserved primitive resolves here so its spelling, width
    /// and family are known, and semantic analysis rejects a use of it until the representation lands.
    bool implemented = false;
};

/// One alternative spelling of a primitive. An alias is a spelling only: it names the same kind.
struct PrimitiveAlias {
    std::string_view name;
    TypeRef::Kind kind = TypeRef::Kind::Unknown;
};

/// Every primitive type, in declaration order within each family.
[[nodiscard]] std::span<const PrimitiveInfo> PrimitiveCatalog() noexcept;

/// Every alternative spelling: `bool`, `byte`, `char` and `float`.
[[nodiscard]] std::span<const PrimitiveAlias> PrimitiveAliases() noexcept;

/// @return the catalog entry for `kind`, or nullptr when `kind` is not a primitive
[[nodiscard]] const PrimitiveInfo *FindPrimitive(TypeRef::Kind kind) noexcept;

/// @return the catalog entry `name` spells, resolving aliases, or nullptr when `name` names no primitive
[[nodiscard]] const PrimitiveInfo *FindPrimitive(std::string_view name) noexcept;

/// @return the canonical spelling of `name`, or `name` unchanged when it names no primitive
[[nodiscard]] std::string_view CanonicalPrimitiveName(std::string_view name) noexcept;

/// The type `name` spells, resolving aliases.
///
/// A reserved primitive resolves like any other, so a caller that must reject one asks `PrimitiveInfo::implemented`.
///
/// @return nullopt when `name` names no primitive
[[nodiscard]] std::optional<TypeRef> PrimitiveTypeFromName(std::string_view name) noexcept;

/// Value width in bits, with `pointerBits` standing in for a pointer-sized type.
///
/// @return nullopt when `kind` is not a primitive
[[nodiscard]] std::optional<std::uint32_t> PrimitiveBits(TypeRef::Kind kind, std::uint32_t pointerBits) noexcept;

/// Storage size in bytes, with `pointerSize` standing in for a pointer-sized type.
///
/// @return nullopt when `kind` is not a primitive
[[nodiscard]] std::optional<std::uint32_t> PrimitiveSize(TypeRef::Kind kind, std::uint32_t pointerSize) noexcept;

/// Storage alignment in bytes, with `pointerSize` standing in for a pointer-sized type.
///
/// @return nullopt when `kind` is not a primitive
[[nodiscard]] std::optional<std::uint32_t> PrimitiveAlign(TypeRef::Kind kind, std::uint32_t pointerSize) noexcept;
} // namespace Rux
