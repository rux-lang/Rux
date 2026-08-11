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

// COFF machine identifier for `targetArch`, or 0 when the COFF writer has no
// encoding for it. Shared with the import-library writer, which stamps the same
// field into every member it generates.
[[nodiscard]] std::uint16_t CoffMachine(Target::Arch targetArch) noexcept;
} // namespace Rux
