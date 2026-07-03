#pragma once

#include <filesystem>

#include "Object/Rcu/Rcu.h"

namespace Rux {

class RcuDumper {
public:
    static bool Dump(const RcuFile &file, const std::filesystem::path &path);
};

} // namespace Rux
