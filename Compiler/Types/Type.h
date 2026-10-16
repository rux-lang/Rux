#pragma once

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Rux {
struct TypeNode;

/// Resolved type representation used by the semantic analyzer.
///
/// A type is a handle to one interned, immutable `TypeNode`: every distinct structure is stored once for the life of
/// the process, so a copy is a pointer and two flags, and equality, hashing and spelling read what the node cached
/// when it was interned. The node's fields are reached through `->`; the two flags describing how the type is viewed
/// from its enclosing type stay in the handle, so changing them builds nothing.
struct TypeRef {
    enum class Kind {
        Unknown, // unresolved / error recovery
        Opaque,
        Bool8,
        Bool16,
        Bool32,
        Bool64,
        Bool128,
        Bool256,
        Bool512,
        Char8,
        Char16,
        Char32,
        Char64,
        Char128,
        Char256,
        Char512,
        Int8,
        Int16,
        Int32,
        Int64,
        Int128,
        Int256,
        Int512,
        UInt8,
        UInt16,
        UInt32,
        UInt64,
        UInt128,
        UInt256,
        UInt512,
        Int,
        UInt, // platform-dependent: 64-bit on x86-64, 32-bit on x86
        Float8,
        Float16,
        Float32,
        Float64,
        Float80,
        Float128,
        Float256,
        Float512,
        Pointer,          // *T  — inner[0] = pointee
        Reference,        // &T  — inner[0] = referent
        Array,            // T[] / T[N] — inner[0] = element; arrayLength is absent for a flexible tail
        Range,            // start..end — inner[0] = element
        RangeInclusive,   // start..=end — inner[0] = element
        RangeFrom,        // start.. — inner[0] = element
        RangeTo,          // ..end — inner[0] = element
        RangeToInclusive, // ..=end — inner[0] = element
        RangeFull,        // .. — no element type or fields
        Tuple,            // (T, U, ...) — inner = elements
        Named,            // user-defined struct/enum/union — name = type name
        TypeParam,        // generic parameter T — name = param name
        Func,             // func(...) -> T — inner[0..n-2] = params, inner[n-1] =
        // return
        // A slice: inner[0] = element, and inner[0].isMut marks a writable view the way `*var T` marks a writable
        // pointee. Its 16-byte {data, length} shape and by-address passing are fixed by the runtime and the calling
        // conventions rather than by any declaration, which is why no declaration can provide or change it.
        Slice,
        // A sum `A | B`: inner = the members, normalized by `MakeSum` into canonical order with nested sums flattened,
        // aliases already resolved, and duplicates removed, so two spellings of one set are one identity.
        Sum,
        // An optional `T?`: inner[0] = the payload. Optionals never collapse, so `T??` keeps both presence levels.
        Optional,
        // A fallible `T ! E`: inner[0] = the success payload, inner[1] = the error payload. The channels stay distinct
        // even when they hold the same type, and `! E` is the unit success `() ! E`.
        Fallible,

        // Aliases — must come after all concrete values so they don't shift
        // the counter
        Bool = Bool8,    // bool is an alias for bool8
        Byte = UInt8,    // byte is an alias for uint8
        Char = Char32,   // char is an alias for char32
        Float = Float64, // float is an alias for float64
    };

    bool isVariadic = false; // Func kind: trailing C-style ... (extern) or
    // Rux variadic; extra call args are allowed
    bool isMut = false; // this type, viewed as a pointee or referent, is writable
                        // (*var T / &var T). The default is read-only. Pointer
                        // mutability is deliberately not part of operator== so
                        // it never leaks onto loaded value types; reference
                        // identity handles it at the enclosing type.

    /// The unknown type, the value every type starts from.
    constexpr TypeRef() noexcept;

    /// The interned structure this handle names: kind, name, inner types and array length.
    [[nodiscard]] const TypeNode *operator->() const noexcept {
        return node;
    }

    /// The one interned type with this structure. Each inner type keeps its own flags, which are part of the
    /// structure, while the result's own flags start cleared.
    [[nodiscard]] static TypeRef Intern(Kind kind, std::string_view name, std::span<const TypeRef> inner,
                                        std::optional<std::uint64_t> arrayLength = std::nullopt);

    /// This type with `inner` in place of its inner types, keeping its kind, name, array length and flags.
    [[nodiscard]] TypeRef WithInner(std::span<const TypeRef> inner) const;

    /// This type with `name` and `inner` in place of its own, keeping its kind, array length and flags.
    [[nodiscard]] TypeRef With(std::string_view name, std::span<const TypeRef> inner) const;

    /// This pointer, reference or slice with its first inner type viewed as `writable` (`*var T`, `&var T`,
    /// `var T[..]`) or read-only, everything else kept.
    [[nodiscard]] TypeRef WithWritableInner(bool writable) const;

    // Factories
    static TypeRef MakeSlice(TypeRef element, const bool writable = false) {
        element.isMut = writable;
        return Intern(Kind::Slice, {}, {&element, 1});
    }

    static TypeRef MakeUnknown() noexcept {
        return {};
    }

    /// The one factory for a primitive kind. A primitive carries nothing but its kind, so a table that already knows
    /// the kind builds the type from it rather than routing through a per-width named factory. `primitive` is a kind
    /// that carries nothing else: a primitive, unknown, `opaque` or `..`, each of which has a node built in.
    static TypeRef MakePrimitive(Kind primitive) noexcept;

    static TypeRef MakeOpaque() noexcept {
        return MakePrimitive(Kind::Opaque);
    }

    static TypeRef MakeBool8() noexcept {
        return MakePrimitive(Kind::Bool8);
    }

    static TypeRef MakeBool16() noexcept {
        return MakePrimitive(Kind::Bool16);
    }

    static TypeRef MakeBool32() noexcept {
        return MakePrimitive(Kind::Bool32);
    }

    static TypeRef MakeBool() noexcept {
        return MakePrimitive(Kind::Bool8);
    }

    static TypeRef MakeChar8() noexcept {
        return MakePrimitive(Kind::Char8);
    }

    static TypeRef MakeChar16() noexcept {
        return MakePrimitive(Kind::Char16);
    }

    static TypeRef MakeChar32() noexcept {
        return MakePrimitive(Kind::Char32);
    }

    static TypeRef MakeChar() noexcept {
        return MakePrimitive(Kind::Char32);
    }

    static TypeRef MakeInt8() noexcept {
        return MakePrimitive(Kind::Int8);
    }

    static TypeRef MakeInt16() noexcept {
        return MakePrimitive(Kind::Int16);
    }

    static TypeRef MakeInt32() noexcept {
        return MakePrimitive(Kind::Int32);
    }

    static TypeRef MakeInt64() noexcept {
        return MakePrimitive(Kind::Int64);
    }

    static TypeRef MakeUInt8() noexcept {
        return MakePrimitive(Kind::UInt8);
    }

    static TypeRef MakeByte() noexcept {
        return MakeUInt8();
    }

    static TypeRef MakeUInt16() noexcept {
        return MakePrimitive(Kind::UInt16);
    }

    static TypeRef MakeUInt32() noexcept {
        return MakePrimitive(Kind::UInt32);
    }

    static TypeRef MakeUInt64() noexcept {
        return MakePrimitive(Kind::UInt64);
    }

    static TypeRef MakeInt() noexcept {
        return MakePrimitive(Kind::Int);
    }

    static TypeRef MakeUInt() noexcept {
        return MakePrimitive(Kind::UInt);
    }

    static TypeRef MakeFloat32() noexcept {
        return MakePrimitive(Kind::Float32);
    }

    static TypeRef MakeFloat64() noexcept {
        return MakePrimitive(Kind::Float64);
    }

    static TypeRef MakeFloat() noexcept {
        return MakePrimitive(Kind::Float64);
    }

    /// The type of text in one encoding: a read-only slice of that encoding's code units, which is what a string
    /// literal is.
    static TypeRef MakeText(const Kind codeUnit) {
        return MakeSlice(MakePrimitive(codeUnit));
    }

    static TypeRef MakeNamed(const std::string_view n) {
        return Intern(Kind::Named, n, {});
    }

    static TypeRef MakeTypeParam(const std::string_view n) {
        return Intern(Kind::TypeParam, n, {});
    }

    static TypeRef MakePointer(const TypeRef &pointee) {
        return Intern(Kind::Pointer, {}, {&pointee, 1});
    }

    static TypeRef MakeReference(const TypeRef &referent) {
        return Intern(Kind::Reference, {}, {&referent, 1});
    }

    static TypeRef MakeArray(const TypeRef &elem, const std::optional<std::uint64_t> length = std::nullopt) {
        return Intern(Kind::Array, {}, {&elem, 1}, length);
    }

    /// The storage marker a payload-carrying enum keeps in its `inner`: an array of unsigned words of `alignment`
    /// bytes that is `size` bytes long, so the marker states the enum's alignment as well as its size and the backend
    /// lays the enum out where the front end did. `size` is a multiple of `alignment`, which is a power of two no
    /// wider than sixteen.
    static TypeRef MakeStorageMarker(std::uint64_t size, std::uint64_t alignment);

    static TypeRef MakeRange(const TypeRef &elem, bool hasStart = true, bool hasEnd = true, bool inclusive = false) {
        Kind kind = Kind::RangeFull;
        if (hasStart && hasEnd) {
            kind = inclusive ? Kind::RangeInclusive : Kind::Range;
        }
        else if (hasStart) {
            kind = Kind::RangeFrom;
        }
        else if (hasEnd) {
            kind = inclusive ? Kind::RangeToInclusive : Kind::RangeTo;
        }
        if (kind == Kind::RangeFull) {
            return MakeRangeFull();
        }
        return Intern(kind, {}, {&elem, 1});
    }

    static TypeRef MakeRangeFull() noexcept {
        return MakePrimitive(Kind::RangeFull);
    }

    static TypeRef MakeTuple(const std::span<const TypeRef> elems) {
        return Intern(Kind::Tuple, {}, elems);
    }

    static TypeRef MakeTuple(const std::initializer_list<TypeRef> elems) {
        return Intern(Kind::Tuple, {}, {elems.begin(), elems.size()});
    }

    static TypeRef MakeFunc(std::vector<TypeRef> params, const TypeRef &ret) {
        params.push_back(ret);
        return Intern(Kind::Func, {}, params);
    }

    /// The built-in unit: the empty tuple `()`, whose only value is `()`. It is a type of its own and never the
    /// ordinary `Core::Unit` struct, which a package may still declare under that name.
    static TypeRef MakeUnit() {
        return MakeTuple(std::span<const TypeRef>{});
    }

    /// A sum of `members` in its one canonical form: nested sums are flattened into their members, duplicates are
    /// removed, and the remainder is ordered by canonical spelling, so `A | B`, `B | A`, and `A | A | B` are one type.
    /// A single remaining member is that member itself rather than a sum of one.
    ///
    /// Aliases must already be resolved; an alias name is display-only and never a member of its own.
    ///
    /// @return the canonical sum, the sole member when only one remains, or Unknown when `members` is empty
    static TypeRef MakeSum(std::span<const TypeRef> members);

    static TypeRef MakeSum(const std::initializer_list<TypeRef> members) {
        return MakeSum(std::span<const TypeRef>{members.begin(), members.size()});
    }

    /// `type` in canonical form again after its members may have changed, as they do when generic substitution
    /// replaces a type parameter: a sum is normalized again, so `T | U` with equal arguments becomes the shared member,
    /// and every other type is returned as it is. Only the outermost level is renormalized; a substitution renormalizes
    /// each level as it rebuilds it.
    static TypeRef Renormalize(const TypeRef &type);

    /// The type of contextual `none` before its context gives it one: an optional with no payload type. It is never a
    /// declared type; the native conversion rules turn it into the absence of the optional the context expects.
    static TypeRef MakeNoneValue() {
        return MakeOptional(MakeOpaque());
    }

    /// An optional of `payload`. Optionals do not collapse: an optional payload stays a separate presence level.
    static TypeRef MakeOptional(const TypeRef &payload) {
        return Intern(Kind::Optional, {}, {&payload, 1});
    }

    /// A fallible with distinct `success` and `error` channels. A unit success is the type written `! E`.
    static TypeRef MakeFallible(const TypeRef &success, const TypeRef &error) {
        const TypeRef channels[] = {success, error};
        return Intern(Kind::Fallible, {}, channels);
    }

    // Predicates
    [[nodiscard]] bool IsUnknown() const noexcept;
    [[nodiscard]] bool IsOpaque() const noexcept;
    [[nodiscard]] bool IsBool() const noexcept;
    [[nodiscard]] bool IsChar() const noexcept;
    [[nodiscard]] bool IsNumeric() const noexcept;
    [[nodiscard]] bool IsInteger() const noexcept;

    [[nodiscard]] bool IsRange() const noexcept;

    [[nodiscard]] bool IsIterableRange() const noexcept;

    [[nodiscard]] bool RangeHasStart() const noexcept;

    [[nodiscard]] bool RangeHasEnd() const noexcept;

    [[nodiscard]] bool IsInclusiveRange() const noexcept;

    /// Whether this is a slice: a borrowed `{data, length}` view over contiguous elements.
    [[nodiscard]] bool IsSlice() const noexcept;

    /// Whether this is a slice whose elements may be written through it.
    [[nodiscard]] bool IsWritableSlice() const noexcept;

    /// Whether this is the built-in unit, the empty tuple `()`.
    [[nodiscard]] bool IsUnit() const noexcept;

    [[nodiscard]] bool IsSum() const noexcept;

    [[nodiscard]] bool IsOptional() const noexcept;

    /// Whether this is the type of contextual `none`, which only its expected type can complete.
    [[nodiscard]] bool IsNoneValue() const noexcept;

    /// Whether this is the provisional type of a native value its context has not completed yet: `none`, or a
    /// constructor such as `.Success(v)` whose other channel is still unknown, at any native level. Such a type is
    /// never declared; the value's expected type supplies the missing part.
    [[nodiscard]] bool IsIncompleteNative() const noexcept;

    /// Whether this type is, or holds at any level, the provisional type of an incomplete native value.
    [[nodiscard]] bool MentionsIncompleteNative() const noexcept;

    [[nodiscard]] bool IsFallible() const noexcept;

    /// The success payload of a fallible, the type a `T ! E` produces when it succeeds.
    [[nodiscard]] const TypeRef &FallibleSuccess() const noexcept;

    /// The error payload of a fallible, the type a `T ! E` carries when it fails.
    [[nodiscard]] const TypeRef &FallibleError() const noexcept;

    [[nodiscard]] bool IsFloat() const noexcept;
    [[nodiscard]] bool IsSigned() const noexcept;

    /// Whether this type is a primitive at all -- a bool, character, integer or float of any width.
    [[nodiscard]] bool IsPrimitive() const noexcept;

    /// True when this type can be assigned to `other` (lenient: Unknown is compatible with anything).
    [[nodiscard]] bool IsAssignableTo(const TypeRef &other) const noexcept;
    /// Whether this value type can be implicitly borrowed as `other`. This answers only type compatibility; semantic
    /// analysis separately requires addressable storage and write permission for an exclusive borrow.
    [[nodiscard]] bool CanImplicitlyBorrowTo(const TypeRef &other) const noexcept;

    /// Whether a borrowed primitive can supply this value type. A type parameter remains provisional until analysis
    /// verifies each instantiation; this never grants an implicit aggregate copy.
    [[nodiscard]] bool CanReadScalarTo(const TypeRef &other) const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> SizeInBytes() const noexcept;
    /// The alignment a value of this type takes in memory, by the same purely syntactic reading as `SizeInBytes`: a
    /// primitive states its own in the catalog, a composite takes its strictest element's, and a named type answers
    /// only through its layout marker.
    [[nodiscard]] std::optional<std::uint64_t> AlignInBytes() const noexcept;
    /// The canonical spelling, cached in the node when it was interned.
    [[nodiscard]] const std::string &ToString() const noexcept;

    /// The spelling a diagnostic shows for a value of this type. It is `ToString()`, except for the two internal
    /// placeholders `opaque` stands in: "no value" (a call to a function without a return type, an assignment) reads
    /// as the unit `()`, and the still-untyped absence of a bare `none` reads as `none`. A function type without a
    /// return type is spelled without its arrow, as source writes it. Source writes `opaque` only behind a pointer, so
    /// no placeholder can be mistaken for a written type.
    [[nodiscard]] std::string DisplayString() const;

    /// The name a generic instantiation is identified by: `Base<Arg, Arg>`, or plain `Base` with no arguments. One
    /// spelling, because a type is recorded, looked up, and compared by this string, and two spellings of it are two
    /// types as far as every table keyed by it is concerned.
    [[nodiscard]] static std::string InstantiationName(std::string_view base, const std::vector<TypeRef> &typeArgs);

    /// The two sides of a range spelling: the text before and after the one range operator written outside every
    /// bracket, and whether that operator was `..=`. An empty side is an absent bound.
    struct RangeSpelling {
        std::string start;
        std::string end;
        bool inclusive = false;
    };

    /// Split a spelling at its range operator, the inverse of the spelling `ToString` gives a range; the two are kept
    /// beside each other so they cannot drift apart. A `..` inside `<>`, `()` or `[]` belongs to an inner type, so
    /// `int[..]` is not a range and `int[..]..int[..]` is a range of slices.
    ///
    /// @return nullopt when `text` has no range operator outside its brackets
    [[nodiscard]] static std::optional<RangeSpelling> SplitRangeSpelling(std::string_view text);

    /// The parts of a spelling around its loosest native operator outside every bracket, the inverse of the spelling
    /// `ToString` gives a fallible or a sum. A fallible splits at its one ` ! ` into the success and error spellings,
    /// with an empty success for `! E`; otherwise a sum splits at every ` | ` into its members.
    struct NativeSpelling {
        enum class Form {
            Fallible,
            Sum
        };

        Form form = Form::Sum;
        std::vector<std::string> parts;
    };

    /// @return nullopt when `text` has neither operator outside its brackets
    [[nodiscard]] static std::optional<NativeSpelling> SplitNativeSpelling(std::string_view text);

    /// The spelling a linker name embeds: alphanumerics and underscores as written, the native operators `?`, `|`,
    /// and `!` as distinct escapes, and every other character as `_`. Two native forms over the same members, such
    /// as `A | B` and `A ! B`, therefore never share a symbol.
    [[nodiscard]] std::string MangledSpelling() const;

    /// Whether the two are one type: the same structure apart from what type identity leaves out, which are the
    /// handle's flags, a named type's layout marker, and writability behind a pointer.
    bool operator==(const TypeRef &other) const noexcept;

    bool operator!=(const TypeRef &other) const noexcept {
        return !(*this == other);
    }

    /// The hash `operator==` agrees with. It is computed from the structure, never from an address, so a table keyed
    /// by types iterates in the same order on every run.
    [[nodiscard]] std::size_t Hash() const noexcept;

    /// This type in the form `operator==` compares, with its flags cleared: equal types have the same identity, so a
    /// table keyed by it finds each type under one entry.
    [[nodiscard]] TypeRef Identity() const noexcept;

    /// Whether the two handles name the very same interned structure, inner flags included, which is what the
    /// spelling and every other cached fact depend on.
    [[nodiscard]] bool IsSameNode(const TypeRef &other) const noexcept {
        return node == other.node;
    }

private:
    constexpr explicit TypeRef(const TypeNode *interned) noexcept
        : node(interned) {
    }

    const TypeNode *node;
};

/// One interned type structure. Only `TypeRef::Intern` creates nodes; they never change and live as long as the
/// process, so a `TypeRef` names one safely from any thread.
struct TypeNode {
    TypeRef::Kind kind = TypeRef::Kind::Unknown;
    std::string name;
    std::vector<TypeRef> inner; // each with its own flags, which are part of the structure
    std::optional<std::uint64_t> arrayLength;
    /// The structural hash of the identity node, the same on every run.
    std::size_t hash = 0;
    /// The canonical spelling, `TypeRef::ToString()`.
    std::string spelling;
    /// The node `operator==` compares: this structure without a named type's layout marker and with every inner flag
    /// cleared except the writability of a reference's referent or a slice's elements. Equal types share it.
    const TypeNode *identity = nullptr;
};

namespace Detail {
/// The kinds that carry nothing but themselves, one node each, built at compile time: unknown, `opaque`, `..` and
/// every primitive.
inline constexpr std::size_t LeafTypeKindCount = static_cast<std::size_t>(TypeRef::Kind::Fallible) + 1;
extern const std::array<TypeNode, LeafTypeKindCount> leafTypeNodes;
} // namespace Detail

constexpr TypeRef::TypeRef() noexcept
    : node(&Detail::leafTypeNodes[0]) {
}

inline TypeRef TypeRef::MakePrimitive(const Kind primitive) noexcept {
    return TypeRef(&Detail::leafTypeNodes[static_cast<std::size_t>(primitive)]);
}

inline const std::string &TypeRef::ToString() const noexcept {
    return node->spelling;
}

inline bool TypeRef::operator==(const TypeRef &other) const noexcept {
    return node->identity == other.node->identity;
}

inline std::size_t TypeRef::Hash() const noexcept {
    return node->hash;
}

inline const TypeRef &TypeRef::FallibleSuccess() const noexcept {
    return node->inner[0];
}

inline const TypeRef &TypeRef::FallibleError() const noexcept {
    return node->inner[1];
}

inline bool TypeRef::IsUnknown() const noexcept {
    return node->kind == Kind::Unknown;
}

inline bool TypeRef::IsOpaque() const noexcept {
    return node->kind == Kind::Opaque;
}

inline bool TypeRef::IsRange() const noexcept {
    return node->kind == Kind::Range || node->kind == Kind::RangeInclusive || node->kind == Kind::RangeFrom ||
           node->kind == Kind::RangeTo || node->kind == Kind::RangeToInclusive || node->kind == Kind::RangeFull;
}

inline bool TypeRef::IsIterableRange() const noexcept {
    return node->kind == Kind::Range || node->kind == Kind::RangeInclusive || node->kind == Kind::RangeFrom;
}

inline bool TypeRef::RangeHasStart() const noexcept {
    return node->kind == Kind::Range || node->kind == Kind::RangeInclusive || node->kind == Kind::RangeFrom;
}

inline bool TypeRef::RangeHasEnd() const noexcept {
    return node->kind == Kind::Range || node->kind == Kind::RangeInclusive || node->kind == Kind::RangeTo ||
           node->kind == Kind::RangeToInclusive;
}

inline bool TypeRef::IsInclusiveRange() const noexcept {
    return node->kind == Kind::RangeInclusive || node->kind == Kind::RangeToInclusive;
}

inline bool TypeRef::IsSlice() const noexcept {
    return node->kind == Kind::Slice;
}

inline bool TypeRef::IsWritableSlice() const noexcept {
    return node->kind == Kind::Slice && !node->inner.empty() && node->inner[0].isMut;
}

inline bool TypeRef::IsUnit() const noexcept {
    return node->kind == Kind::Tuple && node->inner.empty();
}

inline bool TypeRef::IsSum() const noexcept {
    return node->kind == Kind::Sum;
}

inline bool TypeRef::IsOptional() const noexcept {
    return node->kind == Kind::Optional;
}

inline bool TypeRef::IsNoneValue() const noexcept {
    return node->kind == Kind::Optional && node->inner.size() == 1 && node->inner[0]->kind == Kind::Opaque;
}

inline bool TypeRef::IsIncompleteNative() const noexcept {
    if (node->kind != Kind::Optional && node->kind != Kind::Fallible) {
        return false;
    }
    for (const TypeRef &child : node->inner) {
        if (child->kind == Kind::Opaque || child.IsIncompleteNative()) {
            return true;
        }
    }
    return false;
}

inline bool TypeRef::MentionsIncompleteNative() const noexcept {
    if (IsIncompleteNative()) {
        return true;
    }
    for (const TypeRef &child : node->inner) {
        if (child.MentionsIncompleteNative()) {
            return true;
        }
    }
    return false;
}

inline bool TypeRef::IsFallible() const noexcept {
    return node->kind == Kind::Fallible;
}

inline std::string TypeRef::DisplayString() const {
    if (IsOpaque()) {
        return "()";
    }
    if (node->kind == Kind::Optional && node->inner.size() == 1 && node->inner[0].IsOpaque()) {
        return "none";
    }
    // A function without a return type is written without an arrow, `func(T)`, and reads that way wherever it appears
    // in the spelling; `opaque` after an arrow is only ever that placeholder, since a written one sits behind a
    // pointer.
    std::string spelling = ToString();
    constexpr std::string_view placeholder = " -> opaque";
    for (std::size_t at = spelling.find(placeholder); at != std::string::npos; at = spelling.find(placeholder, at)) {
        const std::size_t end = at + placeholder.size();
        if (end < spelling.size() &&
            (std::isalnum(static_cast<unsigned char>(spelling[end])) || spelling[end] == '_')) {
            at = end;
            continue;
        }
        spelling.erase(at, placeholder.size());
    }
    return spelling;
}
} // namespace Rux

template <>
struct std::hash<Rux::TypeRef> {
    std::size_t operator()(const Rux::TypeRef &type) const noexcept {
        return type.Hash();
    }
};
