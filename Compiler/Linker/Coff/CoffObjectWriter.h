#pragma once

#include "Object/Rcu/Rcu.h"
#include "Target/Target.h"

#include <cstdint>
#include <string>

namespace Rux {
struct NativeObject;

/// COFF machine identifier for `targetArch`, or 0 when the COFF writer has no encoding for it. Shared with the
/// import-library writer, which stamps the same field into every member it generates.
[[nodiscard]] std::uint16_t CoffMachine(Target::Arch targetArch) noexcept;

[[nodiscard]] bool WriteCoffObject(const RcuFile &file, Target::Arch targetArch, NativeObject &output,
                                   std::string &error);
} // namespace Rux
