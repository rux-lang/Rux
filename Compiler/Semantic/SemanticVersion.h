#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

namespace Rux {
/// The numeric core of a semantic version, with any pre-release or build suffix already discarded.
///
/// Deliberately lighter than `Package/Version.h`, which models a full `SemanticVersion` including suffix ordering and
/// is what package resolution compares. This one exists for compile-time `#if` conditions, which only ever ask about
/// major/minor/patch and must not pull the package layer into the semantic analyzer.
struct ParsedSemanticVersion {
    std::uint64_t major = 0;
    std::uint64_t minor = 0;
    std::uint64_t patch = 0;
};

/// Parse `major.minor.patch`, ignoring any `-pre` or `+build` suffix.
///
/// Strict about the core: exactly three components, no leading zeros, nothing but digits. A malformed version is
/// rejected rather than coerced, so a typo in a `#if` condition fails instead of silently comparing as 0.0.0.
///
/// @return nullopt when the core is not three well-formed components
inline std::optional<ParsedSemanticVersion> ParseSemanticVersion(const std::string_view text) {
    ParsedSemanticVersion version;
    const std::size_t suffix = text.find_first_of("-+");
    const std::string_view core = text.substr(0, suffix);

    auto parseComponent = [](const std::string_view component, std::uint64_t &value) {
        if (component.empty() || (component.size() > 1 && component.front() == '0')) {
            return false;
        }
        const auto [end, error] = std::from_chars(component.data(), component.data() + component.size(), value);
        return error == std::errc{} && end == component.data() + component.size();
    };

    const std::size_t firstDot = core.find('.');
    if (firstDot == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t secondDot = core.find('.', firstDot + 1);
    if (secondDot == std::string_view::npos || core.find('.', secondDot + 1) != std::string_view::npos) {
        return std::nullopt;
    }

    if (!parseComponent(core.substr(0, firstDot), version.major) ||
        !parseComponent(core.substr(firstDot + 1, secondDot - firstDot - 1), version.minor) ||
        !parseComponent(core.substr(secondDot + 1), version.patch)) {
        return std::nullopt;
    }
    return version;
}

/// Order two versions by precedence.
///
/// @return -1, 0, or 1 as `left` orders before, with, or after `right`
inline int CompareSemanticVersions(const ParsedSemanticVersion &left, const ParsedSemanticVersion &right) {
    if (left.major != right.major)
        return left.major < right.major ? -1 : 1;
    if (left.minor != right.minor)
        return left.minor < right.minor ? -1 : 1;
    if (left.patch != right.patch)
        return left.patch < right.patch ? -1 : 1;
    return 0;
}
} // namespace Rux
