#pragma once

#include "Ir/Hir/Hir.h"

#include <filesystem>

namespace Rux {
class HirPrinter {
public:
    static bool Dump(const HirPackage &package, const std::filesystem::path &path);
};
} // namespace Rux
