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
        Float512,         // String
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

    [[nodiscard]] bool IsFloat() const noexcept;
    [[nodiscard]] bool IsSigned() const noexcept;

    /// Whether this type is a primitive at all -- a bool, character, integer or float of any width.
    [[nodiscard]] bool IsPrimitive() const noexcept;

    /// True when this type can be assigned to `other` (lenient: Unknown is compatible with anything).
    [[nodiscard]] bool IsAssignableTo(const TypeRef &other) const noexcept;
    /// Whether this value type can be implicitly borrowed as `other`. This answers only type compatibility; semantic
    /// analysis separately requires addressable storage and write permission for an exclusive borrow.
    [[nodiscard]] bool CanImplicitlyBorrowTo(const TypeRef &other) const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> SizeInBytes() const noexcept;
    [[nodiscard]] std::string ToString() const;

    /// The name a generic instantiation is identified by: `Base<Arg, Arg>`, or plain `Base` with no arguments. One
    /// spelling, because a type is recorded, looked up, and compared by this string, and two spellings of it are two
    /// types as far as every table keyed by it is concerned.
    [[nodiscard]] static std::string InstantiationName(std::string_view base, const std::vector<TypeRef> &typeArgs);

    bool operator==(const TypeRef &other) const noexcept;

    bool operator!=(const TypeRef &other) const noexcept {
        return !(*this == other);
    }
};
} // namespace Rux
