#pragma once

#include "Types/Type.h"

#include <cstddef>
#include <vector>

namespace Rux {
/// What a typed pattern `v: T`, or a membership test `x is T`, selects from its subject. It is decided from the two
/// normalized types alone, so semantic analysis and lowering answer identically after generic substitution.
struct NativeSelection {
    enum class Kind {
        /// The annotation is exactly the subject's type: the whole subject, whatever form the subject has.
        Whole,
        /// Members of a sum subject; `members` holds their indices in canonical order.
        Members,
        /// One outer presence level of an optional subject. `members` holds the selected members of a sum payload in
        /// canonical order, or is empty when the annotation is the whole payload.
        Presence,
        /// The annotation selects nothing from this subject.
        Invalid,
    };

    Kind kind = Kind::Invalid;
    std::vector<std::size_t> members;

    bool operator==(const NativeSelection &other) const noexcept = default;
};

/// Classify `annotation` against `subject`.
///
/// An annotation identical to the subject is the whole subject on every form. Otherwise a sum subject selects an exact
/// member or a subset of its members, and an optional subject selects its outer presence when the annotation is the
/// payload or a member or subset of a sum payload. Nothing reaches through a fallible channel, past more than one
/// optional level, or into a plain type.
[[nodiscard]] NativeSelection ClassifyNativeSelection(const TypeRef &subject, const TypeRef &annotation);
} // namespace Rux
