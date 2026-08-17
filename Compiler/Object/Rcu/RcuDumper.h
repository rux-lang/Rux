#pragma once

#include "Object/Rcu/Rcu.h"

#include <filesystem>

namespace Rux {
/// Writes an RCU object in a readable form, for inspecting what a code generator emitted. A debugging aid: nothing
/// reads the text back, and `RcuWriter` produces the binary the linker actually consumes.
class RcuDumper {
public:
    /// @return false when the file could not be written
    static bool Dump(const RcuFile &file, const std::filesystem::path &path);
};
} // namespace Rux
