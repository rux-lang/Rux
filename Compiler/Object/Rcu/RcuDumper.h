#pragma once

#include "Object/Rcu/Rcu.h"

#include <filesystem>

namespace Rux {
class RcuDumper {
public:
    static bool Dump(const RcuFile &file, const std::filesystem::path &path);
};
} // namespace Rux
