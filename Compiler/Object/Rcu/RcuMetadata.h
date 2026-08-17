#pragma once

// The two header fields an RCU object carries about its own making: which
// compiler wrote it and when. Both back ends receive the same immutable value
// used by the frontend rather than consulting generated or host state.

#include "BuildInfo/BuildInfo.h"

#include <cstdint>
#include <string_view>

namespace Rux {
/// This compiler's version, packed as major:minor:patch one byte apart, which is the form byte 8 onward of the RCU
/// header takes.
[[nodiscard]] inline std::uint32_t RcuCompilerVersion(const BuildInfo &buildInfo) noexcept {
    unsigned parts[3] = {0, 0, 0};
    std::string_view text = buildInfo.CompilerVersion();
    for (unsigned &part : parts) {
        while (!text.empty() && (text.front() < '0' || text.front() > '9')) {
            text.remove_prefix(1);
        }
        while (!text.empty() && text.front() >= '0' && text.front() <= '9') {
            part = part * 10 + static_cast<unsigned>(text.front() - '0');
            text.remove_prefix(1);
        }
    }
    return parts[0] << 16U | parts[1] << 8U | parts[2];
}

/// The one composition-time instant shared by the whole compilation.
[[nodiscard]] inline std::uint64_t RcuBuildTimestamp(const BuildInfo &buildInfo) noexcept {
    return static_cast<std::uint64_t>(buildInfo.Timestamp());
}
} // namespace Rux
