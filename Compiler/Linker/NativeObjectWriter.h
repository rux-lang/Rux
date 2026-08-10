#pragma once

#include "Object/Rcu/Rcu.h"
#include "Target/Target.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Rux {
struct NativeObject {
    std::string name;
    std::vector<std::uint8_t> bytes;
    std::vector<std::string> publicSymbols;
};

[[nodiscard]] bool WriteNativeObject(const RcuFile &file, Target::OS targetOs, NativeObject &output,
                                     std::string &error);
} // namespace Rux
