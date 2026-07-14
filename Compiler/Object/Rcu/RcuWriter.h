#pragma once

#include "Object/Rcu/Rcu.h"

#include <filesystem>

namespace Rux {
class RcuWriter {
public:
    static bool Write(const RcuFile &file, const std::filesystem::path &path);
};
} // namespace Rux
