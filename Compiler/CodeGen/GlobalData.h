#pragma once

// Target-neutral placement of module variables. Both native back ends lay a variable's initial value out with the
// same rule constants use, then choose its section from the bytes: initialized data when any byte is non-zero, and
// zero-fill storage otherwise, which costs nothing in the file.

#include "CodeGen/ConstantData.h"
#include "CodeGen/RcuModuleBuilder.h"
#include "Ir/Lir/Lir.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace Rux {
/// Where a module variable's storage was placed.
struct PlacedGlobal {
    RcuModuleSection section = RcuModuleSection::Data;
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
};

/// Append a module variable's initial bytes to the section they belong in, aligned as `alignof` reports for its type.
[[nodiscard]] inline PlacedGlobal PlaceGlobal(RcuModuleBuilder &builder, const LirGlobal &global,
                                              const Layout::LayoutMap &layouts,
                                              const std::unordered_set<std::string> &interfaceNames) {
    const std::vector<std::uint8_t> bytes = ValueBytes(global.type, global.scalars, layouts, interfaceNames);
    const bool zero = std::ranges::all_of(bytes, [](const std::uint8_t byte) { return byte == 0; });
    PlacedGlobal placed;
    // A thread-local variable's bytes are the template every thread's copy starts from, zero or not.
    placed.section = global.isThreadLocal ? RcuModuleSection::Tls
                   : zero                 ? RcuModuleSection::Bss
                                          : RcuModuleSection::Data;
    placed.offset = builder.AlignSection(
        placed.section, static_cast<std::uint16_t>(ValueAlignment(global.type, layouts, interfaceNames)));
    (void)builder.Append(placed.section, std::span<const std::uint8_t>(bytes));
    placed.size = static_cast<std::uint32_t>(bytes.size());
    return placed;
}
} // namespace Rux
