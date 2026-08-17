#pragma once

#include "Ir/Lir/Lir.h"

#include <filesystem>

namespace Rux {
/// Writes LIR in a readable form, for inspecting the blocks and registers a function was lowered into. A debugging aid
/// only — nothing reads it back.
class LirPrinter {
public:
    /// @return false when the file could not be written
    static bool Dump(const LirPackage &package, const std::filesystem::path &path);
};
} // namespace Rux
