#include "Semantic/PrimitiveCatalog.h"
#include "Syntax/Parser/Parser.h"

#include <algorithm>
#include <doctest.h>
#include <string>
#include <unordered_set>

using namespace Rux;

namespace Rux {
/// Defined in the parser, which declares it the same way its own callers do rather than through a header.
std::string ImplTypeName(const TypeExpr &type);
} // namespace Rux

namespace {
/// Every primitive the language currently spells, in the order the catalog declares them.
constexpr std::string_view ExpectedNames[] = {
    "bool8",    "bool16",   "bool32",  "bool64",   "bool128",  "bool256", "bool512", "char8",   "char16",
    "char32",   "char64",   "char128", "char256",  "char512",  "int8",    "int16",   "int32",   "int64",
    "int128",   "int256",   "int512",  "int",      "uint8",    "uint16",  "uint32",  "uint64",  "uint128",
    "uint256",  "uint512",  "uint",    "float8",   "float16",  "float32", "float64", "float80", "float128",
    "float256", "float512", "string8", "string16", "string32",
};

[[nodiscard]] const PrimitiveInfo &Entry(const std::string_view name) {
    const PrimitiveInfo *info = FindPrimitive(name);
    REQUIRE(info != nullptr);
    return *info;
}
} // namespace

TEST_CASE("the catalog lists every primitive exactly once") {
    const auto catalog = PrimitiveCatalog();
    REQUIRE_EQ(catalog.size(), std::size(ExpectedNames));

    std::unordered_set<std::string> names;
    std::unordered_set<int> kinds;
    for (std::size_t index = 0; index < catalog.size(); ++index) {
        CHECK_EQ(catalog[index].name, ExpectedNames[index]);
        CHECK(names.insert(std::string(catalog[index].name)).second);
        CHECK(kinds.insert(static_cast<int>(catalog[index].kind)).second);
    }
}

TEST_CASE("an alias resolves to its canonical spelling and kind") {
    CHECK_EQ(CanonicalPrimitiveName("bool"), "bool8");
    CHECK_EQ(CanonicalPrimitiveName("byte"), "uint8");
    CHECK_EQ(CanonicalPrimitiveName("char"), "char32");
    CHECK_EQ(CanonicalPrimitiveName("float"), "float64");
    CHECK_EQ(CanonicalPrimitiveName("string"), "string8");

    CHECK_EQ(PrimitiveTypeFromName("bool"), TypeRef::MakeBool8());
    CHECK_EQ(PrimitiveTypeFromName("byte"), TypeRef::MakeUInt8());
    CHECK_EQ(PrimitiveTypeFromName("char"), TypeRef::MakeChar32());
    CHECK_EQ(PrimitiveTypeFromName("float"), TypeRef::MakeFloat64());
    CHECK_EQ(PrimitiveTypeFromName("string"), TypeRef::MakeString8());
}

TEST_CASE("a name that is not a primitive is left alone") {
    CHECK_EQ(CanonicalPrimitiveName("String"), "String");
    CHECK_EQ(CanonicalPrimitiveName("Vector"), "Vector");
    CHECK_FALSE(PrimitiveTypeFromName("String").has_value());
    CHECK_FALSE(PrimitiveTypeFromName("").has_value());
    CHECK_EQ(FindPrimitive(TypeRef::Kind::Named), nullptr);
    CHECK_EQ(FindPrimitive(TypeRef::Kind::Unknown), nullptr);
}

TEST_CASE("the parser's alias normalization matches the catalog") {
    // The parser sits below the semantic type system and repeats the alias table; the two must agree, or an
    // extension written on `bool` binds to a different key than one written on `bool8`.
    for (const PrimitiveAlias &alias : PrimitiveAliases()) {
        NamedTypeExpr written;
        written.name = std::string(alias.name);
        CHECK_EQ(ImplTypeName(written), CanonicalPrimitiveName(alias.name));
    }
    for (const PrimitiveInfo &primitive : PrimitiveCatalog()) {
        NamedTypeExpr written;
        written.name = std::string(primitive.name);
        CHECK_EQ(ImplTypeName(written), primitive.name);
    }
}

TEST_CASE("a fixed-width primitive reports its own size and alignment") {
    CHECK_EQ(PrimitiveSize(TypeRef::Kind::UInt8, 8), 1);
    CHECK_EQ(PrimitiveSize(TypeRef::Kind::Int32, 8), 4);
    CHECK_EQ(PrimitiveSize(TypeRef::Kind::Float64, 8), 8);
    CHECK_EQ(PrimitiveAlign(TypeRef::Kind::Int16, 8), 2);
    CHECK_EQ(PrimitiveBits(TypeRef::Kind::Char32, 8), 32);
}

TEST_CASE("a pointer-sized primitive takes the target's width") {
    CHECK_EQ(PrimitiveBits(TypeRef::Kind::Int, 64), 64);
    CHECK_EQ(PrimitiveBits(TypeRef::Kind::UInt, 32), 32);
    CHECK_EQ(PrimitiveSize(TypeRef::Kind::Int, 4), 4);
    CHECK_EQ(PrimitiveSize(TypeRef::Kind::UInt, 8), 8);
    CHECK_EQ(PrimitiveAlign(TypeRef::Kind::Int, 4), 4);
}

TEST_CASE("a wide primitive is 16-byte aligned whatever its size") {
    for (const std::string_view name :
         {"int128", "int256", "int512", "uint128", "uint256", "uint512", "bool128", "bool256", "bool512", "char128",
          "char256", "char512", "float128", "float256", "float512"}) {
        CHECK_EQ(Entry(name).align, 16);
    }
    CHECK_EQ(Entry("int128").size, 16);
    CHECK_EQ(Entry("int256").size, 32);
    CHECK_EQ(Entry("int512").size, 64);
}

TEST_CASE("a string is a 16-byte view whose width is one code unit of its encoding") {
    for (const std::string_view name : {"string8", "string16", "string32"}) {
        CAPTURE(name);
        CHECK_EQ(Entry(name).size, 16);
        CHECK_EQ(Entry(name).align, 8);
    }
    CHECK_EQ(Entry("string8").bits, 8);
    CHECK_EQ(Entry("string16").bits, 16);
    CHECK_EQ(Entry("string32").bits, 32);
}

TEST_CASE("a string is made of the characters of its encoding") {
    CHECK_EQ(StringCodeUnitKind(TypeRef::Kind::String8), TypeRef::Kind::Char8);
    CHECK_EQ(StringCodeUnitKind(TypeRef::Kind::String16), TypeRef::Kind::Char16);
    CHECK_EQ(StringCodeUnitKind(TypeRef::Kind::String32), TypeRef::Kind::Char32);
    CHECK_EQ(StringCodeUnitKind(TypeRef::Kind::Char8), TypeRef::Kind::Unknown);
    CHECK_EQ(StringCodeUnitKind(TypeRef::Kind::Named), TypeRef::Kind::Unknown);
}

TEST_CASE("float80 holds 80 value bits in 16 bytes of storage") {
    const PrimitiveInfo &extended = Entry("float80");
    CHECK_EQ(extended.bits, 80);
    CHECK_EQ(extended.size, 16);
    CHECK_EQ(extended.align, 16);
}

TEST_CASE("a primitive's family drives the type predicates") {
    for (const PrimitiveInfo &primitive : PrimitiveCatalog()) {
        const TypeRef type = TypeRef::MakePrimitive(primitive.kind);
        CHECK(type.IsPrimitive());
        CHECK_EQ(type.ToString(), primitive.name);
        CHECK_EQ(type.IsBool(), primitive.category == PrimitiveCategory::Bool);
        CHECK_EQ(type.IsChar(), primitive.category == PrimitiveCategory::Char);
        CHECK_EQ(type.IsString(), primitive.category == PrimitiveCategory::String);
        CHECK_EQ(type.IsFloat(), primitive.category == PrimitiveCategory::Float);
        CHECK_EQ(type.IsSigned(), primitive.category == PrimitiveCategory::SignedInt);
        CHECK_EQ(type.IsInteger(), primitive.category == PrimitiveCategory::SignedInt ||
                                       primitive.category == PrimitiveCategory::UnsignedInt);
        CHECK_EQ(type.IsNumeric(), type.IsInteger() || type.IsFloat());
    }
}

TEST_CASE("a non-primitive answers no to every primitive predicate") {
    const TypeRef named = TypeRef::MakeNamed("Vector<int32>");
    CHECK_FALSE(named.IsPrimitive());
    CHECK_FALSE(named.IsBool());
    CHECK_FALSE(named.IsChar());
    CHECK_FALSE(named.IsString());
    CHECK_FALSE(named.IsNumeric());
    CHECK_FALSE(named.IsInteger());
    CHECK_FALSE(named.IsFloat());
    CHECK_FALSE(named.IsSigned());
}

TEST_CASE("TypeRef sizes agree with the catalog") {
    // `TypeRef::SizeInBytes` has no target, so it answers for a 64-bit one.
    for (const PrimitiveInfo &primitive : PrimitiveCatalog()) {
        CHECK_EQ(TypeRef::MakePrimitive(primitive.kind).SizeInBytes(), PrimitiveSize(primitive.kind, 8));
    }
}

TEST_CASE("the implemented widths are the ones with a representation today") {
    // One list, not two: a width is reserved exactly when it is named here, so implementing one is a single edit.
    const std::unordered_set<std::string_view> reserved{
        "bool128", "bool256", "bool512", "char128",  "char256",  "char512",
        "float8",  "float16", "float80", "float128", "float256", "float512",
    };

    for (const PrimitiveInfo &primitive : PrimitiveCatalog()) {
        CAPTURE(primitive.name);
        CHECK_EQ(primitive.implemented, !reserved.contains(primitive.name));
    }
}

TEST_CASE("a character width carries either a code unit or a scalar value") {
    CHECK_EQ(CharacterDomainOf(TypeRef::Kind::Char8), CharacterDomain::CodeUnit);
    CHECK_EQ(CharacterDomainOf(TypeRef::Kind::Char16), CharacterDomain::CodeUnit);
    for (const std::string_view name : {"char32", "char64", "char128", "char256", "char512"}) {
        CAPTURE(name);
        CHECK_EQ(CharacterDomainOf(Entry(name).kind), CharacterDomain::ScalarValue);
    }
    CHECK_FALSE(CharacterDomainOf(TypeRef::Kind::UInt32).has_value());
    CHECK_FALSE(CharacterDomainOf(TypeRef::Kind::Named).has_value());
}

TEST_CASE("a code unit reaches its width's maximum and a scalar value stops at U+10FFFF") {
    CHECK_EQ(MaxCharacterValue(TypeRef::Kind::Char8), 0xFF);
    CHECK_EQ(MaxCharacterValue(TypeRef::Kind::Char16), 0xFFFF);
    // Every scalar-valued width stops at the last code point, however much room it has beyond it.
    for (const std::string_view name : {"char32", "char64", "char128", "char256", "char512"}) {
        CAPTURE(name);
        CHECK_EQ(MaxCharacterValue(Entry(name).kind), 0x10FFFF);
    }
    CHECK_FALSE(MaxCharacterValue(TypeRef::Kind::Int32).has_value());
}

TEST_CASE("a surrogate is a code unit but never a scalar value") {
    CHECK(IsSurrogate(0xD800));
    CHECK(IsSurrogate(0xDC00));
    CHECK(IsSurrogate(0xDFFF));
    CHECK_FALSE(IsSurrogate(0xD7FF));
    CHECK_FALSE(IsSurrogate(0xE000));

    // A UTF-16 surrogate is exactly a code unit, so char16 holds one and every scalar-valued width refuses it.
    CHECK(IsValidCharacterValue(TypeRef::Kind::Char16, 0xD800));
    CHECK(IsValidCharacterValue(TypeRef::Kind::Char16, 0xDFFF));
    for (const std::string_view name : {"char32", "char64", "char128", "char256", "char512"}) {
        CAPTURE(name);
        CHECK_FALSE(IsValidCharacterValue(Entry(name).kind, 0xD800));
        CHECK_FALSE(IsValidCharacterValue(Entry(name).kind, 0xDFFF));
        CHECK(IsValidCharacterValue(Entry(name).kind, 0x10FFFF));
        CHECK_FALSE(IsValidCharacterValue(Entry(name).kind, 0x110000));
    }
}

TEST_CASE("a character value must fit the width that holds it") {
    CHECK(IsValidCharacterValue(TypeRef::Kind::Char8, 0xFF));
    CHECK_FALSE(IsValidCharacterValue(TypeRef::Kind::Char8, 0x100));
    CHECK(IsValidCharacterValue(TypeRef::Kind::Char16, 0xFFFF));
    CHECK_FALSE(IsValidCharacterValue(TypeRef::Kind::Char16, 0x10000));
    CHECK_FALSE(IsValidCharacterValue(TypeRef::Kind::UInt8, 1));
}

TEST_CASE("a character widens only to a wider one carrying the same thing") {
    const TypeRef scalar32 = TypeRef::MakeChar32();
    const TypeRef scalar64 = TypeRef::MakePrimitive(TypeRef::Kind::Char64);
    CHECK(scalar32.IsAssignableTo(scalar64));
    CHECK(scalar32.IsAssignableTo(scalar32));
    CHECK_FALSE(scalar64.IsAssignableTo(scalar32));

    // The two encodings do not mix: a UTF-8 byte above 0x7F is not the UTF-16 word of the same number, and a UTF-16
    // word may be half of a pair rather than a character.
    const TypeRef unit8 = TypeRef::MakeChar8();
    const TypeRef unit16 = TypeRef::MakeChar16();
    CHECK_FALSE(unit8.IsAssignableTo(unit16));
    CHECK_FALSE(unit8.IsAssignableTo(scalar32));
    CHECK_FALSE(unit16.IsAssignableTo(scalar32));
    CHECK_FALSE(unit16.IsAssignableTo(unit8));
}
