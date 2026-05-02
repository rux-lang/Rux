/*
    Rux Compiler
    Copyright © 2026 Ivan Muzyka
    Licensed under the MIT License
*/

#pragma once

#include <string>
#include <vector>

namespace Rux {

// Resolved type representation used by the semantic analyzer.
struct TypeRef {
    enum class Kind {
        Unknown,   // unresolved / error recovery
        Opaque,
        Bool8, Bool16, Bool32,
        Char8, Char16, Char32,
        Int8, Int16, Int32, Int64,
        UInt8, UInt16, UInt32, UInt64,
        Float32, Float64,
        Str,       // String
        Pointer,   // *T  — inner[0] = pointee
        Array,     // T[] / T[N]  — inner[0] = element
        Tuple,     // (T, U, ...) — inner = elements
        Named,     // user-defined struct/enum/union — name = type name
        TypeParam, // generic parameter T — name = param name
        Func,      // func(...) -> T — inner[0..n-2] = params, inner[n-1] = return
        // Aliases — must come after all concrete values so they don't shift the counter
        Bool = Bool8,   // bool is an alias for bool8
        Char = Char32,  // char is an alias for char32
    };

    Kind kind = Kind::Unknown;
    std::string name;
    std::vector<TypeRef> inner; // C++17: vector<incomplete T> is valid

    // ── Factories ─────────────────────────────────────────────────────────────

    static TypeRef MakeUnknown()  { return {}; }
    static TypeRef MakeOpaque()     { TypeRef t; t.kind = Kind::Opaque;    return t; }
    static TypeRef MakeBool8()    { TypeRef t; t.kind = Kind::Bool8;   return t; }
    static TypeRef MakeBool16()   { TypeRef t; t.kind = Kind::Bool16;  return t; }
    static TypeRef MakeBool32()   { TypeRef t; t.kind = Kind::Bool32;  return t; }
    static TypeRef MakeBool()     { TypeRef t; t.kind = Kind::Bool8;   return t; }
    static TypeRef MakeChar8()    { TypeRef t; t.kind = Kind::Char8;   return t; }
    static TypeRef MakeChar16()   { TypeRef t; t.kind = Kind::Char16;  return t; }
    static TypeRef MakeChar32()   { TypeRef t; t.kind = Kind::Char32;  return t; }
    static TypeRef MakeChar()     { TypeRef t; t.kind = Kind::Char32;  return t; }
    static TypeRef MakeStr()      { TypeRef t; t.kind = Kind::Str;     return t; }
    static TypeRef MakeInt8()     { TypeRef t; t.kind = Kind::Int8;    return t; }
    static TypeRef MakeInt16()    { TypeRef t; t.kind = Kind::Int16;   return t; }
    static TypeRef MakeInt32()    { TypeRef t; t.kind = Kind::Int32;   return t; }
    static TypeRef MakeInt64()    { TypeRef t; t.kind = Kind::Int64;   return t; }
    static TypeRef MakeUInt8()    { TypeRef t; t.kind = Kind::UInt8;   return t; }
    static TypeRef MakeUInt16()   { TypeRef t; t.kind = Kind::UInt16;  return t; }
    static TypeRef MakeUInt32()   { TypeRef t; t.kind = Kind::UInt32;  return t; }
    static TypeRef MakeUInt64()   { TypeRef t; t.kind = Kind::UInt64;  return t; }
    static TypeRef MakeFloat32()  { TypeRef t; t.kind = Kind::Float32; return t; }
    static TypeRef MakeFloat64()  { TypeRef t; t.kind = Kind::Float64; return t; }

    static TypeRef MakeNamed(std::string n) {
        TypeRef t; t.kind = Kind::Named; t.name = std::move(n); return t;
    }
    static TypeRef MakeTypeParam(std::string n) {
        TypeRef t; t.kind = Kind::TypeParam; t.name = std::move(n); return t;
    }
    static TypeRef MakePointer(TypeRef pointee) {
        TypeRef t; t.kind = Kind::Pointer;
        t.inner.push_back(std::move(pointee));
        return t;
    }
    static TypeRef MakeArray(TypeRef elem) {
        TypeRef t; t.kind = Kind::Array;
        t.inner.push_back(std::move(elem));
        return t;
    }
    static TypeRef MakeTuple(std::vector<TypeRef> elems) {
        TypeRef t; t.kind = Kind::Tuple;
        t.inner = std::move(elems);
        return t;
    }
    static TypeRef MakeFunc(std::vector<TypeRef> params, TypeRef ret) {
        TypeRef t; t.kind = Kind::Func;
        t.inner = std::move(params);
        t.inner.push_back(std::move(ret));
        return t;
    }

    // ── Predicates ────────────────────────────────────────────────────────────

    [[nodiscard]] bool IsUnknown()  const noexcept { return kind == Kind::Unknown; }
    [[nodiscard]] bool IsOpaque()     const noexcept { return kind == Kind::Opaque; }
    [[nodiscard]] bool IsBool()     const noexcept { return kind == Kind::Bool8 || kind == Kind::Bool16 || kind == Kind::Bool32; }
    [[nodiscard]] bool IsNumeric()  const noexcept;
    [[nodiscard]] bool IsInteger()  const noexcept;
    [[nodiscard]] bool IsFloat()    const noexcept;
    [[nodiscard]] bool IsSigned()   const noexcept;

    // True when this type can be assigned to `other` (lenient: Unknown is compatible with anything).
    [[nodiscard]] bool IsAssignableTo(const TypeRef& other) const noexcept;

    [[nodiscard]] std::string ToString() const;

    bool operator==(const TypeRef& other) const noexcept;
    bool operator!=(const TypeRef& other) const noexcept { return !(*this == other); }
};

} // namespace Rux
