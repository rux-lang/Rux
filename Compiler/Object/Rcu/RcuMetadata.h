#pragma once

// The two header fields an RCU object carries about its own making: which
// compiler wrote it and when. Neither depends on the architecture the object
// holds code for, so both back ends fill them from here rather than each
// reading RUX_VERSION for itself.

#include "Driver/Version.h"

#include <chrono>
#include <cstdint>
#include <string_view>

namespace Rux {
// This compiler's version, packed as major:minor:patch one byte apart, which is
// the form byte 8 onward of the RCU header takes.
[[nodiscard]] inline std::uint32_t RcuCompilerVersion() noexcept {
    unsigned parts[3] = {0, 0, 0};
    std::string_view text = RUX_VERSION;
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

// The moment the object is being written, in seconds since the epoch.
[[nodiscard]] inline std::uint64_t RcuBuildTimestamp() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(now).count());
}
} // namespace Rux
