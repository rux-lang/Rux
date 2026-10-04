#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Rux {
/// Resolved type representation used by the semantic analyzer.
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

    Kind kind = Kind::Unknown;
    std::string name;
    std::vector<TypeRef> inner; // C++17: vector<incomplete T> is valid
    std::optional<std::uint64_t> arrayLength;
    bool isVariadic = false; // Func kind: trailing C-style ... (extern) or
    // Rux variadic; extra call args are allowed
    bool isMut = false; // this type, viewed as a pointee or referent, is writable
                        // (*var T / &var T). The default is read-only. Pointer
                        // mutability is deliberately not part of operator== so
                        // it never leaks onto loaded value types; reference
                        // identity handles it at the enclosing type.

    // Factories
    static TypeRef MakeSlice(TypeRef element, const bool writable = false) {
        TypeRef type;
        type.kind = Kind::Slice;
        element.isMut = writable;
        type.inner.push_back(std::move(element));
        return type;
    }

    static TypeRef MakeUnknown() {
        return {};
    }

    /// The one factory for a primitive kind. A primitive carries nothing but its kind, so a table that already knows
    /// the kind builds the type from it rather than routing through a per-width named factory.
    static TypeRef MakePrimitive(const Kind primitive) {
        TypeRef t;
        t.kind = primitive;
        return t;
    }

    static TypeRef MakeOpaque() {
        TypeRef t;
        t.kind = Kind::Opaque;
        return t;
    }

    static TypeRef MakeBool8() {
        TypeRef t;
        t.kind = Kind::Bool8;
        return t;
    }

    static TypeRef MakeBool16() {
        TypeRef t;
        t.kind = Kind::Bool16;
        return t;
    }

    static TypeRef MakeBool32() {
        TypeRef t;
        t.kind = Kind::Bool32;
        return t;
    }

    static TypeRef MakeBool() {
        TypeRef t;
        t.kind = Kind::Bool8;
        return t;
    }

    static TypeRef MakeChar8() {
        TypeRef t;
        t.kind = Kind::Char8;
        return t;
    }

    static TypeRef MakeChar16() {
        TypeRef t;
        t.kind = Kind::Char16;
        return t;
    }

    static TypeRef MakeChar32() {
        TypeRef t;
        t.kind = Kind::Char32;
        return t;
    }

    static TypeRef MakeChar() {
        TypeRef t;
        t.kind = Kind::Char32;
        return t;
    }

    static TypeRef MakeInt8() {
        TypeRef t;
        t.kind = Kind::Int8;
        return t;
    }

    static TypeRef MakeInt16() {
        TypeRef t;
        t.kind = Kind::Int16;
        return t;
    }

    static TypeRef MakeInt32() {
        TypeRef t;
        t.kind = Kind::Int32;
        return t;
    }

    static TypeRef MakeInt64() {
        TypeRef t;
        t.kind = Kind::Int64;
        return t;
    }

    static TypeRef MakeUInt8() {
        TypeRef t;
        t.kind = Kind::UInt8;
        return t;
    }

    static TypeRef MakeByte() {
        return MakeUInt8();
    }

    static TypeRef MakeUInt16() {
        TypeRef t;
        t.kind = Kind::UInt16;
        return t;
    }

    static TypeRef MakeUInt32() {
        TypeRef t;
        t.kind = Kind::UInt32;
        return t;
    }

    static TypeRef MakeUInt64() {
        TypeRef t;
        t.kind = Kind::UInt64;
        return t;
    }

    static TypeRef MakeInt() {
        TypeRef t;
        t.kind = Kind::Int;
        return t;
    }

    static TypeRef MakeUInt() {
        TypeRef t;
        t.kind = Kind::UInt;
        return t;
    }

    static TypeRef MakeFloat32() {
        TypeRef t;
        t.kind = Kind::Float32;
        return t;
    }

    static TypeRef MakeFloat64() {
        TypeRef t;
        t.kind = Kind::Float64;
        return t;
    }

    static TypeRef MakeFloat() {
        TypeRef t;
        t.kind = Kind::Float64;
        return t;
    }

    /// The type of text in one encoding: a read-only slice of that encoding's code units, which is what a string
    /// literal is.
    static TypeRef MakeText(const Kind codeUnit) {
        return MakeSlice(MakePrimitive(codeUnit));
    }

    static TypeRef MakeNamed(std::string n) {
        TypeRef t;
        t.kind = Kind::Named;
        t.name = std::move(n);
        return t;
    }

    static TypeRef MakeTypeParam(std::string n) {
        TypeRef t;
        t.kind = Kind::TypeParam;
        t.name = std::move(n);
        return t;
    }

    static TypeRef MakePointer(TypeRef pointee) {
        TypeRef t;
        t.kind = Kind::Pointer;
        t.inner.push_back(std::move(pointee));
        return t;
    }

    static TypeRef MakeReference(TypeRef referent) {
        TypeRef t;
        t.kind = Kind::Reference;
        t.inner.push_back(std::move(referent));
        return t;
    }

    static TypeRef MakeArray(TypeRef elem, std::optional<std::uint64_t> length = std::nullopt) {
        TypeRef t;
        t.kind = Kind::Array;
        t.inner.push_back(std::move(elem));
        t.arrayLength = length;
        return t;
    }

    static TypeRef MakeRange(TypeRef elem, bool hasStart = true, bool hasEnd = true, bool inclusive = false) {
        TypeRef t;
        if (hasStart && hasEnd) {
            t.kind = inclusive ? Kind::RangeInclusive : Kind::Range;
        }
        else if (hasStart) {
            t.kind = Kind::RangeFrom;
        }
        else if (hasEnd) {
            t.kind = inclusive ? Kind::RangeToInclusive : Kind::RangeTo;
        }
        else {
            t.kind = Kind::RangeFull;
        }
        if (t.kind != Kind::RangeFull) {
            t.inner.push_back(std::move(elem));
        }
        return t;
    }

    static TypeRef MakeRangeFull() {
        TypeRef t;
        t.kind = Kind::RangeFull;
        return t;
    }

    static TypeRef MakeTuple(std::vector<TypeRef> elems) {
        TypeRef t;
        t.kind = Kind::Tuple;
        t.inner = std::move(elems);
        return t;
    }

    static TypeRef MakeFunc(std::vector<TypeRef> params, TypeRef ret) {
        TypeRef t;
        t.kind = Kind::Func;
        t.inner = std::move(params);
        t.inner.push_back(std::move(ret));
        return t;
    }

    /// The built-in unit: the empty tuple `()`, whose only value is `()`. It is a type of its own and never the
    /// ordinary `Core::Unit` struct, which a package may still declare under that name.
    static TypeRef MakeUnit() {
        return MakeTuple({});
    }

    /// A sum of `members` in its one canonical form: nested sums are flattened into their members, duplicates are
    /// removed, and the remainder is ordered by canonical spelling, so `A | B`, `B | A`, and `A | A | B` are one type.
    /// A single remaining member is that member itself rather than a sum of one.
    ///
    /// Aliases must already be resolved; an alias name is display-only and never a member of its own.
    ///
    /// @return the canonical sum, the sole member when only one remains, or Unknown when `members` is empty
    static TypeRef MakeSum(std::vector<TypeRef> members);

    /// `type` in canonical form again after its members may have changed, as they do when generic substitution
    /// replaces a type parameter: a sum is normalized again, so `T | U` with equal arguments becomes the shared member,
    /// and every other type is returned as it is. Only the outermost level is renormalized; a substitution renormalizes
    /// each level as it rebuilds it.
    static TypeRef Renormalize(TypeRef type) {
        return type.kind == Kind::Sum ? MakeSum(std::move(type.inner)) : type;
    }

    /// The type of contextual `none` before its context gives it one: an optional with no payload type. It is never a
    /// declared type; the native conversion rules turn it into the absence of the optional the context expects.
    static TypeRef MakeNoneValue() {
        return MakeOptional(MakeOpaque());
    }

    /// An optional of `payload`. Optionals do not collapse: an optional payload stays a separate presence level.
    static TypeRef MakeOptional(TypeRef payload) {
        TypeRef t;
        t.kind = Kind::Optional;
        t.inner.push_back(std::move(payload));
        return t;
    }

    /// A fallible with distinct `success` and `error` channels. A unit success is the type written `! E`.
    static TypeRef MakeFallible(TypeRef success, TypeRef error) {
        TypeRef t;
        t.kind = Kind::Fallible;
        t.inner.push_back(std::move(success));
        t.inner.push_back(std::move(error));
        return t;
    }

    // Predicates
    [[nodiscard]] bool IsUnknown() const noexcept {
        return kind == Kind::Unknown;
    }

    [[nodiscard]] bool IsOpaque() const noexcept {
        return kind == Kind::Opaque;
    }

    [[nodiscard]] bool IsBool() const noexcept;
    [[nodiscard]] bool IsChar() const noexcept;
    [[nodiscard]] bool IsNumeric() const noexcept;
    [[nodiscard]] bool IsInteger() const noexcept;

    [[nodiscard]] bool IsRange() const noexcept {
        return kind == Kind::Range || kind == Kind::RangeInclusive || kind == Kind::RangeFrom ||
               kind == Kind::RangeTo || kind == Kind::RangeToInclusive || kind == Kind::RangeFull;
    }

    [[nodiscard]] bool IsIterableRange() const noexcept {
        return kind == Kind::Range || kind == Kind::RangeInclusive || kind == Kind::RangeFrom;
    }

    [[nodiscard]] bool RangeHasStart() const noexcept {
        return kind == Kind::Range || kind == Kind::RangeInclusive || kind == Kind::RangeFrom;
    }

    [[nodiscard]] bool RangeHasEnd() const noexcept {
        return kind == Kind::Range || kind == Kind::RangeInclusive || kind == Kind::RangeTo ||
               kind == Kind::RangeToInclusive;
    }

    [[nodiscard]] bool IsInclusiveRange() const noexcept {
        return kind == Kind::RangeInclusive || kind == Kind::RangeToInclusive;
    }

    /// Whether this is a slice: a borrowed `{data, length}` view over contiguous elements.
    [[nodiscard]] bool IsSlice() const noexcept {
        return kind == Kind::Slice;
    }

    /// Whether this is a slice whose elements may be written through it.
    [[nodiscard]] bool IsWritableSlice() const noexcept {
        return kind == Kind::Slice && !inner.empty() && inner[0].isMut;
    }

    /// Whether this is the built-in unit, the empty tuple `()`.
    [[nodiscard]] bool IsUnit() const noexcept {
        return kind == Kind::Tuple && inner.empty();
    }

    [[nodiscard]] bool IsSum() const noexcept {
        return kind == Kind::Sum;
    }

    [[nodiscard]] bool IsOptional() const noexcept {
        return kind == Kind::Optional;
    }

    /// Whether this is the type of contextual `none`, which only its expected type can complete.
    [[nodiscard]] bool IsNoneValue() const noexcept {
        return kind == Kind::Optional && inner.size() == 1 && inner[0].kind == Kind::Opaque;
    }

    /// Whether this is the provisional type of a native value its context has not completed yet: `none`, or a
    /// constructor such as `.Success(v)` whose other channel is still unknown, at any native level. Such a type is
    /// never declared; the value's expected type supplies the missing part.
    [[nodiscard]] bool IsIncompleteNative() const noexcept {
        if (kind != Kind::Optional && kind != Kind::Fallible) {
            return false;
        }
        for (const TypeRef &child : inner) {
            if (child.kind == Kind::Opaque || child.IsIncompleteNative()) {
                return true;
            }
        }
        return false;
    }

    /// Whether this type is, or holds at any level, the provisional type of an incomplete native value.
    [[nodiscard]] bool MentionsIncompleteNative() const noexcept {
        if (IsIncompleteNative()) {
            return true;
        }
        for (const TypeRef &child : inner) {
            if (child.MentionsIncompleteNative()) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool IsFallible() const noexcept {
        return kind == Kind::Fallible;
    }

    /// The success payload of a fallible, the type a `T ! E` produces when it succeeds.
    [[nodiscard]] const TypeRef &FallibleSuccess() const noexcept {
        return inner[0];
    }

    /// The error payload of a fallible, the type a `T ! E` carries when it fails.
    [[nodiscard]] const TypeRef &FallibleError() const noexcept {
        return inner[1];
    }

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
    [[nodiscard]] std::string ToString() const;

    /// The spelling a diagnostic shows for a value of this type. It is `ToString()`, except for the two internal
    /// placeholders `opaque` stands in: "no value" (a call to a function without a return type, an assignment) reads
    /// as the unit `()`, and the still-untyped absence of a bare `none` reads as `none`. Source writes `opaque` only
    /// behind a pointer, so neither placeholder can be mistaken for a written type.
    [[nodiscard]] std::string DisplayString() const {
        if (IsOpaque()) {
            return "()";
        }
        if (kind == Kind::Optional && inner.size() == 1 && inner[0].IsOpaque()) {
            return "none";
        }
        return ToString();
    }

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

    bool operator==(const TypeRef &other) const noexcept;

    bool operator!=(const TypeRef &other) const noexcept {
        return !(*this == other);
    }
};
} // namespace Rux
