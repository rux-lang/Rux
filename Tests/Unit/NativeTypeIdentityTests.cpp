// Identity, normalization, and canonical spelling of the native sum, optional, fallible, and unit type forms.

#include "Types/Type.h"

#include <doctest.h>
#include <vector>

using namespace Rux;

namespace {
TypeRef Named(const char *name) {
    return TypeRef::MakeNamed(name);
}
} // namespace

TEST_CASE("unit is the empty tuple and never the Core::Unit struct") {
    const TypeRef unit = TypeRef::MakeUnit();
    CHECK(unit.IsUnit());
    CHECK_EQ(unit, TypeRef::MakeTuple({}));
    CHECK_EQ(unit.ToString(), "()");
    CHECK_EQ(unit.SizeInBytes(), 0);
    CHECK_NE(unit, Named("Unit"));
    CHECK_NE(unit, Named("Core::Unit"));
    CHECK_FALSE(Named("Unit").IsUnit());
    CHECK_FALSE(TypeRef::MakeTuple({TypeRef::MakeInt32()}).IsUnit());
}

TEST_CASE("a sum normalizes order, nesting, and duplicates into one identity") {
    const TypeRef a = Named("Options");
    const TypeRef b = Named("Defaults");
    const TypeRef c = Named("ConfigError");

    const TypeRef ab = TypeRef::MakeSum({a, b});
    CHECK(ab.IsSum());
    CHECK_EQ(ab, TypeRef::MakeSum({b, a}));
    CHECK_EQ(ab, TypeRef::MakeSum({a, a, b}));
    CHECK_EQ(ab.ToString(), "Defaults | Options");

    // A nested sum flattens into its members rather than becoming one member of its own.
    const TypeRef nested = TypeRef::MakeSum({TypeRef::MakeSum({a, b}), c});
    CHECK_EQ(nested, TypeRef::MakeSum({c, b, a}));
    CHECK_EQ(nested.inner.size(), 3);
    CHECK_EQ(nested.ToString(), "ConfigError | Defaults | Options");
}

TEST_CASE("a sum of one member is that member") {
    CHECK_EQ(TypeRef::MakeSum({TypeRef::MakeInt32()}), TypeRef::MakeInt32());
    // Two spellings that resolve to the same type, such as an alias and its target, leave one member.
    CHECK_EQ(TypeRef::MakeSum({TypeRef::MakeInt32(), TypeRef::MakeInt32()}), TypeRef::MakeInt32());
    CHECK_FALSE(TypeRef::MakeSum({TypeRef::MakeInt32(), TypeRef::MakeInt32()}).IsSum());
    CHECK(TypeRef::MakeSum({}).IsUnknown());
    CHECK(TypeRef::MakeSum({TypeRef::MakeInt32(), TypeRef::MakeUnknown()}).IsUnknown());
}

TEST_CASE("sums never flatten through an optional or fallible boundary") {
    const TypeRef a = Named("A");
    const TypeRef b = Named("B");
    const TypeRef optionalSum = TypeRef::MakeOptional(TypeRef::MakeSum({a, b}));
    const TypeRef sumWithOptional = TypeRef::MakeSum({a, TypeRef::MakeOptional(b)});
    CHECK_NE(optionalSum, sumWithOptional);
    CHECK_EQ(optionalSum.ToString(), "(A | B)?");
    CHECK_EQ(sumWithOptional.ToString(), "A | (B?)");

    const TypeRef fallibleMember = TypeRef::MakeFallible(a, b);
    const TypeRef sumWithFallible = TypeRef::MakeSum({fallibleMember, a});
    CHECK_EQ(sumWithFallible.inner.size(), 2);
    CHECK_EQ(sumWithFallible.ToString(), "A | (A ! B)");
}

TEST_CASE("optionals keep every presence level") {
    const TypeRef once = TypeRef::MakeOptional(TypeRef::MakeInt32());
    const TypeRef twice = TypeRef::MakeOptional(once);
    const TypeRef thrice = TypeRef::MakeOptional(twice);
    CHECK(once.IsOptional());
    CHECK_NE(once, twice);
    CHECK_NE(twice, thrice);
    CHECK_NE(once, TypeRef::MakeInt32());
    CHECK_EQ(once.ToString(), "int32?");
    CHECK_EQ(twice.ToString(), "int32??");
    CHECK_EQ(thrice.ToString(), "int32???");
}

TEST_CASE("fallible channels stay distinct even for one payload type") {
    const TypeRef sameChannels = TypeRef::MakeFallible(TypeRef::MakeInt32(), TypeRef::MakeInt32());
    CHECK(sameChannels.IsFallible());
    CHECK_EQ(sameChannels.FallibleSuccess(), TypeRef::MakeInt32());
    CHECK_EQ(sameChannels.FallibleError(), TypeRef::MakeInt32());
    CHECK_EQ(sameChannels.ToString(), "int32 ! int32");

    const TypeRef options = Named("Options");
    const TypeRef parse = Named("ParseError");
    CHECK_NE(TypeRef::MakeFallible(options, parse), TypeRef::MakeFallible(parse, options));
    CHECK_EQ(TypeRef::MakeFallible(TypeRef::MakeSum({options, Named("Defaults")}),
                                   TypeRef::MakeSum({parse, Named("IoError")}))
                 .ToString(),
             "(Defaults | Options) ! (IoError | ParseError)");
}

TEST_CASE("a unit success is the type written ! E") {
    const TypeRef io = Named("IoError");
    const TypeRef completion = TypeRef::MakeFallible(TypeRef::MakeUnit(), io);
    CHECK(completion.FallibleSuccess().IsUnit());
    CHECK_EQ(completion.ToString(), "! IoError");
    CHECK_NE(completion, TypeRef::MakeFallible(Named("Unit"), io));
    CHECK_EQ(TypeRef::MakeFallible(Named("Unit"), io).ToString(), "Unit ! IoError");
}

TEST_CASE("nested fallibles and optional errors keep every channel") {
    const TypeRef parse = Named("ParseError");
    const TypeRef io = Named("IoError");
    const TypeRef inner = TypeRef::MakeFallible(TypeRef::MakeInt32(), parse);

    const TypeRef nestedSuccess = TypeRef::MakeFallible(inner, io);
    const TypeRef nestedError = TypeRef::MakeFallible(TypeRef::MakeInt32(), TypeRef::MakeFallible(parse, io));
    CHECK_NE(nestedSuccess, nestedError);
    CHECK_EQ(nestedSuccess.ToString(), "(int32 ! ParseError) ! IoError");
    CHECK_EQ(nestedError.ToString(), "int32 ! (ParseError ! IoError)");

    const TypeRef optionalError = TypeRef::MakeFallible(Named("Options"), TypeRef::MakeOptional(parse));
    const TypeRef optionalResult = TypeRef::MakeOptional(TypeRef::MakeFallible(Named("Options"), parse));
    const TypeRef optionalSuccess = TypeRef::MakeFallible(TypeRef::MakeOptional(Named("Options")), parse);
    CHECK_NE(optionalError, optionalResult);
    CHECK_NE(optionalError, optionalSuccess);
    CHECK_NE(optionalResult, optionalSuccess);
    CHECK_EQ(optionalError.ToString(), "Options ! (ParseError?)");
    CHECK_EQ(optionalResult.ToString(), "(Options ! ParseError)?");
    CHECK_EQ(optionalSuccess.ToString(), "Options? ! ParseError");
}

TEST_CASE("prefix and suffix spellings group native forms") {
    const TypeRef optional = TypeRef::MakeOptional(TypeRef::MakeInt32());
    CHECK_EQ(TypeRef::MakePointer(optional).ToString(), "*(int32?)");
    CHECK_EQ(TypeRef::MakeOptional(TypeRef::MakePointer(TypeRef::MakeInt32())).ToString(), "(*int32)?");
    CHECK_NE(TypeRef::MakePointer(optional), TypeRef::MakeOptional(TypeRef::MakePointer(TypeRef::MakeInt32())));
    CHECK_EQ(TypeRef::MakeReference(TypeRef::MakeSum({Named("A"), Named("B")})).ToString(), "&(A | B)");

    CHECK_EQ(TypeRef::MakeSlice(optional).ToString(), "int32?[..]");
    CHECK_EQ(TypeRef::MakeOptional(TypeRef::MakeSlice(TypeRef::MakeInt32())).ToString(), "int32[..]?");
    CHECK_EQ(TypeRef::MakeOptional(TypeRef::MakeSlice(TypeRef::MakeChar8(), true)).ToString(), "(var char8[..])?");
    CHECK_EQ(TypeRef::MakeArray(TypeRef::MakeFallible(TypeRef::MakeInt32(), Named("E")), 4).ToString(),
             "(int32 ! E)[4]");
    CHECK_EQ(TypeRef::MakeSum({TypeRef::MakeFunc({}, Named("A")), Named("B")}).ToString(), "B | (func() -> A)");
}

TEST_CASE("legacy outcome shapes gain no native identity") {
    CHECK_NE(Named("Option<int32>"), TypeRef::MakeOptional(TypeRef::MakeInt32()));
    CHECK_NE(Named("Result<int32, ParseError>"), TypeRef::MakeFallible(TypeRef::MakeInt32(), Named("ParseError")));
    CHECK_FALSE(Named("Option<int32>").IsOptional());
    CHECK_FALSE(Named("Result<int32, ParseError>").IsFallible());
}

TEST_CASE("native forms publish no guessed size") {
    CHECK_FALSE(TypeRef::MakeOptional(TypeRef::MakeInt32()).SizeInBytes().has_value());
    CHECK_FALSE(TypeRef::MakeFallible(TypeRef::MakeUnit(), Named("E")).SizeInBytes().has_value());
    CHECK_FALSE(TypeRef::MakeSum({TypeRef::MakeInt32(), TypeRef::MakeInt64()}).SizeInBytes().has_value());
}
