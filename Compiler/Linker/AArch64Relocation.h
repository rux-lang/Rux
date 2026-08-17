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
/// Patch one relocation into an already-placed instruction.
///
/// @param patchAt Byte offset of the instruction within `buf`, not of the immediate field inside it
/// @param siteVA Virtual address the instruction will load at, since AArch64 forms are mostly PC-relative
/// @param writerName Names the calling image writer in the diagnostic, so an out-of-range offset says which one
/// @return false when the value does not fit the instruction's field, with `error` explaining what
[[nodiscard]] bool ApplyAArch64Relocation(Buf &buf, size_t patchAt, uint16_t type, uint64_t targetVA, int64_t addend,
                                          uint64_t siteVA, std::string_view symbolName, std::string_view writerName,
                                          std::string &error);
} // namespace Rux
