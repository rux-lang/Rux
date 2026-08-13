#pragma once

#include <cstdint>
#include <string_view>

namespace Rux {
enum class BuildProfile : std::uint8_t {
    Debug,
    Release,
};

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
