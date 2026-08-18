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
    "bool8",  "bool16",  "bool32",  "bool64",  "bool128", "bool256",  "bool512",  "char8",    "char16",  "char32",
    "char64", "char128", "char256", "char512", "int8",    "int16",    "int32",    "int64",    "int128",  "int256",
    "int512", "int",     "uint8",   "uint16",  "uint32",  "uint64",   "uint128",  "uint256",  "uint512", "uint",
    "float8", "float16", "float32", "float64", "float80", "float128", "float256", "float512",
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

    CHECK_EQ(PrimitiveTypeFromName("bool"), TypeRef::MakeBool8());
    CHECK_EQ(PrimitiveTypeFromName("byte"), TypeRef::MakeUInt8());
    CHECK_EQ(PrimitiveTypeFromName("char"), TypeRef::MakeChar32());
    CHECK_EQ(PrimitiveTypeFromName("float"), TypeRef::MakeFloat64());
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
    const auto catalog = PrimitiveCatalog();
    const auto reserved = std::ranges::count_if(catalog, [](const PrimitiveInfo &p) { return !p.implemented; });
    CHECK_EQ(reserved, 20);

    for (const std::string_view name :
         {"bool8", "bool16", "bool32", "char8", "char16", "char32", "int8", "int16", "int32", "int64", "int", "uint8",
          "uint16", "uint32", "uint64", "uint", "float32", "float64"}) {
        CHECK(Entry(name).implemented);
    }
    for (const std::string_view name : {"bool64",  "bool128", "bool256", "bool512",  "char64",   "char128", "char256",
                                        "char512", "int128",  "int256",  "int512",   "uint128",  "uint256", "uint512",
                                        "float8",  "float16", "float80", "float128", "float256", "float512"}) {
        CHECK_FALSE(Entry(name).implemented);
    }
}
