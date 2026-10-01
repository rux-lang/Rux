#pragma once

#include "Types/Type.h"

#include <vector>

namespace Rux {
/// One step of an implicit conversion into a native type. A route lists the steps from the outermost destination
/// level inward, so lowering builds the destination value from the outside in.
struct NativeConversionStep {
    enum class Kind {
        /// The source already has the destination type at this level.
        Identity,
        /// The source is one member of the destination sum; `member` is that member's index in canonical order.
        Inject,
        /// The source is a sum whose members are all members of the destination sum; tags are remapped.
        WidenSum,
        /// Construct outer presence of the destination optional, then continue into its payload.
        Presence,
        /// Construct outer success of the destination fallible, then continue into its success payload.
        Success,
        /// Keep an existing optional's presence or absence and widen its payload by injection or subset.
        WidenOptional,
        /// Keep an existing fallible's channel and widen each channel by injection or subset.
        WidenFallible,
        /// Construct the outer absence of the destination optional: what contextual `none` becomes.
        Absent,
    };

    Kind kind = Kind::Identity;
    std::size_t member = 0;

    bool operator==(const NativeConversionStep &other) const noexcept = default;
};

/// The outcome of looking for an implicit conversion of a value of one type to another, under the native conversion
/// rules: identity wins outright; otherwise exactly one permitted route is accepted and several routes with different
/// meanings are ambiguous.
struct NativeConversion {
    enum class Outcome {
        /// The types are identical; no conversion is needed.
        Identity,
        /// Exactly one route converts the value.
        Converted,
        /// No permitted route exists.
        Incompatible,
        /// Several routes produce different values, so the source must say which one it means.
        Ambiguous,
    };

    Outcome outcome = Outcome::Incompatible;
    /// The accepted route, outermost destination level first, when `outcome` is Converted.
    std::vector<NativeConversionStep> route;

    [[nodiscard]] bool Accepted() const noexcept {
        return outcome == Outcome::Identity || outcome == Outcome::Converted;
    }
};

/// Classify the implicit conversion of a value of type `source` to type `destination`.
///
/// Routes descend through the destination: a sum accepts an exact member or a subset sum without searching inside its
/// members; an optional accepts presence construction of a uniquely convertible payload, or an existing optional with
/// a widened payload; a fallible accepts success construction of a uniquely convertible payload, or an existing
/// fallible with widened channels. A fallible source whose error could reach the destination's error channel is never
/// silently stored as successful data: that conversion is ambiguous unless channel widening is the only route.
[[nodiscard]] NativeConversion ClassifyNativeConversion(const TypeRef &source, const TypeRef &destination);

/// Whether `type` is, or contains at any level, a native sum, optional, or fallible.
[[nodiscard]] bool MentionsNativeType(const TypeRef &type) noexcept;
} // namespace Rux
