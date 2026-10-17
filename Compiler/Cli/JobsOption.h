#pragma once

#include <charconv>
#include <cstddef>
#include <string_view>
#include <system_error>

namespace Rux::CliSupport {
/// Parse the value of `--jobs`: a positive decimal integer and nothing else.
inline bool ParseJobCount(const std::string_view value, std::size_t &jobs) {
    std::size_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || parsed == 0)
        return false;
    jobs = parsed;
    return true;
}
} // namespace Rux::CliSupport
