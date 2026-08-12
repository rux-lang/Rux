#pragma once

// Applies the RCU relocation forms an AArch64 image writer resolves itself.
// The helper is linker-owned rather than tied to an image format so ELF and
// PE can share the instruction-field encoding and its validation.

#include "Linker/LinkerInternal.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Rux {
[[nodiscard]] bool ApplyAArch64Relocation(Buf &buf, size_t patchAt, uint16_t type, uint64_t targetVA, int64_t addend,
                                          uint64_t siteVA, std::string_view symbolName, std::string_view writerName,
                                          std::string &error);
} // namespace Rux
