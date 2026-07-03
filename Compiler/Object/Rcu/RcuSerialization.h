#pragma once

#include <string>
#include <vector>

#include "Object/Rcu/Rcu.h"

namespace Rux {

[[nodiscard]] std::vector<std::uint8_t> SerializeRcuFile(const RcuFile &file);
[[nodiscard]] std::string DumpRcuFileText(const RcuFile &file);

} // namespace Rux
