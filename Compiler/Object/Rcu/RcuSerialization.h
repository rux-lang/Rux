#pragma once

#include "Object/Rcu/Rcu.h"

#include <string>
#include <vector>

namespace Rux {
/// Encode an RCU object to its on-disk bytes. Split from `RcuWriter` so the same encoding can be produced in memory,
/// which is what the unit tests round-trip against without touching the filesystem.
[[nodiscard]] std::vector<std::uint8_t> SerializeRcuFile(const RcuFile &file);

/// Render an RCU object as readable text, the form behind `RcuDumper`.
[[nodiscard]] std::string DumpRcuFileText(const RcuFile &file);
} // namespace Rux
