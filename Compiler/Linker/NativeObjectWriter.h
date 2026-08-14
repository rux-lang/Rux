#pragma once

#include "Object/Rcu/Rcu.h"
#include "Target/Target.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Rux {
struct NativeObject {
    std::string name;
    std::vector<std::uint8_t> bytes;
    std::vector<std::string> publicSymbols;
};

// Writes `file` as a relocatable object in the target's native format: COFF on
// Windows, Mach-O on macOS, ELF everywhere else. The object's own architecture
// must match `targetArch`, which selects the format's machine identifier.
[[nodiscard]] bool WriteNativeObject(const RcuFile &file, Target::OS targetOs, Target::Arch targetArch,
                                     NativeObject &output, std::string &error);

} // namespace Rux
