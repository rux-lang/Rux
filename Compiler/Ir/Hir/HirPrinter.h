#pragma once

#include "Ir/Hir/Hir.h"

#include <filesystem>

namespace Rux {
/// Writes HIR in a readable form, for inspecting what lowering produced. A debugging aid only — nothing reads the
/// output back.
class HirPrinter {
public:
    /// @return false when the file could not be written
    static bool Dump(const HirPackage &package, const std::filesystem::path &path);
};
} // namespace Rux
