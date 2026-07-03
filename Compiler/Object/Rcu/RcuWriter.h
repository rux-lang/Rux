#pragma once

#include <filesystem>

#include "Object/Rcu/Rcu.h"

namespace Rux {

class RcuWriter {
public:
    static bool Write(const RcuFile &file, const std::filesystem::path &path);
};

} // namespace Rux
