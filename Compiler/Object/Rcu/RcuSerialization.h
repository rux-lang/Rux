#pragma once

#include "Object/Rcu/Rcu.h"

#include <string>
#include <vector>

namespace Rux {
[[nodiscard]] std::vector<std::uint8_t> SerializeRcuFile(const RcuFile &file);
[[nodiscard]] std::string DumpRcuFileText(const RcuFile &file);
} // namespace Rux
