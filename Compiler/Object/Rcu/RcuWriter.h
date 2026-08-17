#pragma once

#include "Object/Rcu/Rcu.h"

#include <filesystem>

namespace Rux {
/// Writes an RCU object to disk. RCU is the compiler's own object format, produced by every code generator and consumed
/// by the linker, so this is the boundary where an in-memory module becomes a file.
class RcuWriter {
public:
    /// @return false when the file could not be written
    static bool Write(const RcuFile &file, const std::filesystem::path &path);
};
} // namespace Rux
