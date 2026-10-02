#include "Types/NativeSelection.h"

#include <algorithm>
#include <optional>

namespace Rux {
namespace {
/// The members of `sum` that `annotation` names: itself when it is one member, or each of its own members when it is a
/// subset sum. Nothing when any part of it is not a member.
std::optional<std::vector<std::size_t>> SelectedMembers(const TypeRef &sum, const TypeRef &annotation) {
    const auto indexOf = [&](const TypeRef &member) -> std::optional<std::size_t> {
        const auto found = std::ranges::find(sum.inner, member);
        if (found == sum.inner.end()) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(found - sum.inner.begin());
    };
    if (const std::optional<std::size_t> member = indexOf(annotation)) {
        return std::vector<std::size_t>{*member};
    }
    if (!annotation.IsSum()) {
        return std::nullopt;
    }
    std::vector<std::size_t> members;
    for (const TypeRef &member : annotation.inner) {
        const std::optional<std::size_t> index = indexOf(member);
        if (!index) {
            return std::nullopt;
        }
        members.push_back(*index);
    }
    std::ranges::sort(members);
    return members;
}
} // namespace

NativeSelection ClassifyNativeSelection(const TypeRef &subject, const TypeRef &annotation) {
    if (subject == annotation) {
        return {NativeSelection::Kind::Whole, {}};
    }
    if (subject.IsSum()) {
        if (std::optional<std::vector<std::size_t>> members = SelectedMembers(subject, annotation)) {
            return {NativeSelection::Kind::Members, std::move(*members)};
        }
        return {};
    }
    if (subject.IsOptional() && !subject.inner.empty()) {
        const TypeRef &payload = subject.inner[0];
        if (payload == annotation) {
            return {NativeSelection::Kind::Presence, {}};
        }
        if (payload.IsSum()) {
            if (std::optional<std::vector<std::size_t>> members = SelectedMembers(payload, annotation)) {
                return {NativeSelection::Kind::Presence, std::move(*members)};
            }
        }
    }
    return {};
}
} // namespace Rux
