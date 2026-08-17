#pragma once

#include <cstdint>
#include <string_view>

namespace Rux {
/// Which optimization policy one build runs under. The pass pipeline reads it to decide which passes are enabled;
/// `Release` is the default and the configuration CI tests.
enum class BuildProfile : std::uint8_t {
    Debug,
    Release,
};

/// The display spelling used in progress and outcome lines. Report prose names the profile this way rather than by
/// re-deriving it.
[[nodiscard]] constexpr std::string_view ToString(const BuildProfile profile) noexcept {
    switch (profile) {
    case BuildProfile::Debug:
        return "Debug";
    case BuildProfile::Release:
        return "Release";
    }
    return "Debug";
}
} // namespace Rux
