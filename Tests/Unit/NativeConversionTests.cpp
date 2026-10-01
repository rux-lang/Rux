// The native conversion rules on resolved types: identity wins, exactly one permitted route converts, and routes with
// different meanings are ambiguous rather than chosen silently.

#include "Types/NativeConversion.h"

#include <doctest.h>

using namespace Rux;

namespace {
using Kind = NativeConversionStep::Kind;
using Outcome = NativeConversion::Outcome;

Outcome OutcomeOf(const TypeRef &source, const TypeRef &destination) {
    return ClassifyNativeConversion(source, destination).outcome;
}

std::vector<Kind> RouteOf(const TypeRef &source, const TypeRef &destination) {
    const NativeConversion conversion = ClassifyNativeConversion(source, destination);
    REQUIRE(conversion.outcome == Outcome::Converted);
    std::vector<Kind> kinds;
    for (const NativeConversionStep &step : conversion.route) {
        kinds.push_back(step.kind);
    }
    return kinds;
}

const TypeRef A = TypeRef::MakeNamed("A");
const TypeRef B = TypeRef::MakeNamed("B");
const TypeRef C = TypeRef::MakeNamed("C");
const TypeRef E = TypeRef::MakeNamed("E");
const TypeRef F = TypeRef::MakeNamed("F");
} // namespace

TEST_CASE("identity wins outright") {
    CHECK(OutcomeOf(A, A) == Outcome::Identity);
    CHECK(OutcomeOf(TypeRef::MakeSum({A, B}), TypeRef::MakeSum({B, A})) == Outcome::Identity);
    const TypeRef r = TypeRef::MakeFallible(TypeRef::MakeInt32(), E);
    CHECK(OutcomeOf(r, r) == Outcome::Identity);
}

TEST_CASE("a sum takes an exact member or a subset and searches no further") {
    const TypeRef sum = TypeRef::MakeSum({A, B, C});
    CHECK_EQ(RouteOf(A, sum), std::vector{Kind::Inject});
    CHECK_EQ(ClassifyNativeConversion(B, sum).route.front().member, 1);
    CHECK_EQ(RouteOf(TypeRef::MakeSum({A, C}), sum), std::vector{Kind::WidenSum});
    // No narrowing and no conversion into an individual member.
    CHECK(OutcomeOf(sum, TypeRef::MakeSum({A, B})) == Outcome::Incompatible);
    CHECK(OutcomeOf(sum, A) == Outcome::Incompatible);
    CHECK(OutcomeOf(TypeRef::MakeInt8(), TypeRef::MakeSum({TypeRef::MakeInt32(), TypeRef::MakeBool()})) ==
          Outcome::Incompatible);
    // Presence is never constructed inside a sum member.
    CHECK(OutcomeOf(A, TypeRef::MakeSum({TypeRef::MakeOptional(A), B})) == Outcome::Incompatible);
}

TEST_CASE("presence and success compose through the expected type") {
    const TypeRef optional = TypeRef::MakeOptional(TypeRef::MakeInt32());
    CHECK_EQ(RouteOf(TypeRef::MakeInt32(), optional), std::vector{Kind::Presence, Kind::Identity});
    CHECK_EQ(RouteOf(TypeRef::MakeInt32(), TypeRef::MakeFallible(optional, E)),
             (std::vector{Kind::Success, Kind::Presence, Kind::Identity}));
    // An ordinary widening applies inside the constructed presence.
    CHECK_EQ(RouteOf(TypeRef::MakeInt8(), optional), std::vector{Kind::Presence, Kind::Identity});
    // Identity wins at each level: an absent `int32?` becomes present-absent in `int32??`.
    CHECK_EQ(RouteOf(optional, TypeRef::MakeOptional(optional)), std::vector{Kind::Presence, Kind::Identity});
    // An optional never loses its wrapper.
    CHECK(OutcomeOf(optional, TypeRef::MakeInt32()) == Outcome::Incompatible);
    CHECK(OutcomeOf(TypeRef::MakeFallible(A, E), A) == Outcome::Incompatible);
}

TEST_CASE("an existing optional widens its payload while keeping absence") {
    const TypeRef source = TypeRef::MakeOptional(A);
    CHECK_EQ(RouteOf(source, TypeRef::MakeOptional(TypeRef::MakeSum({A, B}))),
             (std::vector{Kind::WidenOptional, Kind::Inject}));
    CHECK_EQ(
        RouteOf(TypeRef::MakeOptional(TypeRef::MakeSum({A, B})), TypeRef::MakeOptional(TypeRef::MakeSum({A, B, C}))),
        (std::vector{Kind::WidenOptional, Kind::WidenSum}));
    // `A?` into `(A | (A?))?` could keep the absence or store the whole optional as a member.
    CHECK(OutcomeOf(source, TypeRef::MakeOptional(TypeRef::MakeSum({A, source}))) == Outcome::Ambiguous);
}

TEST_CASE("an existing fallible widens its channels") {
    CHECK_EQ(
        RouteOf(TypeRef::MakeFallible(A, E), TypeRef::MakeFallible(TypeRef::MakeSum({A, B}), TypeRef::MakeSum({E, F}))),
        std::vector{Kind::WidenFallible});
    // A channel never gains a wrapper.
    CHECK(OutcomeOf(TypeRef::MakeFallible(A, E), TypeRef::MakeFallible(A, TypeRef::MakeOptional(E))) ==
          Outcome::Incompatible);
}

TEST_CASE("the forwarding and nesting routes of a fallible source") {
    const TypeRef int32 = TypeRef::MakeInt32();
    const TypeRef r = TypeRef::MakeFallible(int32, E);
    // Forwarding R and storing it as successful data are both possible.
    CHECK(OutcomeOf(r, TypeRef::MakeFallible(TypeRef::MakeSum({int32, r}), E)) == Outcome::Ambiguous);
    // With an unrelated error channel, R can only be stored as data.
    CHECK_EQ(RouteOf(r, TypeRef::MakeFallible(r, F)), (std::vector{Kind::Success, Kind::Identity}));
    // Its error could reach the destination's error channel, so storing it as data is not chosen silently.
    CHECK(OutcomeOf(r, TypeRef::MakeFallible(r, TypeRef::MakeSum({E, F}))) == Outcome::Ambiguous);
    CHECK_EQ(RouteOf(r, TypeRef::MakeFallible(int32, TypeRef::MakeSum({E, F}))), std::vector{Kind::WidenFallible});
    CHECK_EQ(RouteOf(r, TypeRef::MakeOptional(r)), (std::vector{Kind::Presence, Kind::Identity}));
    // Channel widening never constructs presence inside the success channel.
    CHECK(OutcomeOf(r, TypeRef::MakeFallible(TypeRef::MakeOptional(int32), E)) == Outcome::Incompatible);
}

TEST_CASE("routes are rechecked after generic collapse") {
    // `T | U` at `T = U = int32` is plain `int32`, so the value converts by identity.
    const TypeRef collapsed = TypeRef::MakeSum({TypeRef::MakeInt32(), TypeRef::MakeInt32()});
    CHECK(OutcomeOf(TypeRef::MakeInt32(), collapsed) == Outcome::Identity);
}

TEST_CASE("native mentions are found at any level") {
    CHECK(MentionsNativeType(TypeRef::MakeOptional(A)));
    CHECK(MentionsNativeType(TypeRef::MakeSlice(TypeRef::MakeFallible(A, E))));
    CHECK(MentionsNativeType(TypeRef::MakeTuple({A, TypeRef::MakeSum({A, B})})));
    CHECK_FALSE(MentionsNativeType(TypeRef::MakeTuple({A, B})));
    CHECK_FALSE(MentionsNativeType(TypeRef::MakeUnit()));
}

TEST_CASE("contextual none becomes the absence of the optional its context reaches") {
    const TypeRef none = TypeRef::MakeNoneValue();
    CHECK(none.IsNoneValue());
    CHECK_FALSE(TypeRef::MakeOptional(TypeRef::MakeInt32()).IsNoneValue());
    CHECK_EQ(RouteOf(none, TypeRef::MakeOptional(TypeRef::MakeInt32())), std::vector{Kind::Absent});
    CHECK_EQ(RouteOf(none, TypeRef::MakeFallible(TypeRef::MakeOptional(A), E)),
             (std::vector{Kind::Success, Kind::Absent}));
    // The outer level of a nested optional is the one that becomes absent.
    CHECK_EQ(RouteOf(none, TypeRef::MakeOptional(TypeRef::MakeOptional(A))), std::vector{Kind::Absent});
    CHECK(OutcomeOf(none, TypeRef::MakeInt32()) == Outcome::Incompatible);
    CHECK(OutcomeOf(none, TypeRef::MakeSum({A, TypeRef::MakeOptional(B)})) == Outcome::Incompatible);
    CHECK(OutcomeOf(none, TypeRef::MakeFallible(A, TypeRef::MakeOptional(E))) == Outcome::Incompatible);
}
