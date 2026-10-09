#pragma once

#include "Object/Rcu/Rcu.h"
#include "Target/Target.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Rux {
struct NativeObject {
    std::string name;
    std::vector<std::uint8_t> bytes;
    std::vector<std::string> publicSymbols;
};

/// Writes `file` as a relocatable object in the target's native format: COFF on Windows, Mach-O on macOS, ELF
/// everywhere else. The object's own architecture must match `targetArch`, which selects the format's machine
/// identifier.
[[nodiscard]] bool WriteNativeObject(const RcuFile &file, Target::OS targetOs, Target::Arch targetArch,
                                     NativeObject &output, std::string &error);

/// The position in `file.sections` of the section a symbol's fixed section index (RCU_TEXT_IDX and its siblings)
/// names: the first section of that type, which is where the linker places the symbol too. Empty for an external or
/// absolute symbol and for an index naming a section the object does not have.
[[nodiscard]] std::optional<std::size_t> SymbolSectionIndex(const RcuFile &file, std::uint16_t sectionIdx);

} // namespace Rux
