#pragma once

#include "Ir/Lir/Lir.h"

#include <filesystem>

namespace Rux {
class LirPrinter {
public:
    static bool Dump(const LirPackage &package, const std::filesystem::path &path);
};
} // namespace Rux
