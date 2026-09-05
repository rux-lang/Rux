#pragma once

#include <optional>
#include <string_view>

namespace Rux::Testing {
/// Bytes the handle-inheritance test writes into its pipe; the probe reports inheritance only when it reads them back.
inline constexpr std::string_view handleProbeToken = "rux-handle-probe-token";

std::optional<int> RunProcessProbe(int argc, char **argv);
} // namespace Rux::Testing
