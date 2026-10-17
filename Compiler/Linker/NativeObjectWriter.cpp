#include "Linker/NativeObjectWriter.h"

#include "Linker/Coff/CoffObjectWriter.h"
#include "Linker/Elf/ElfObjectWriter.h"
#include "Linker/MachO/MachOObjectWriter.h"

#include <algorithm>
#include <format>

namespace Rux {
bool WriteNativeObject(const RcuFile &file, const Target::OS targetOs, const Target::Arch targetArch,
                       NativeObject &output, std::string &error) {
    output = {};
    const std::uint8_t expectedArch = RcuArchFor(targetArch);
    if (expectedArch == RcuArch::Unknown) {
        error = std::format("cannot write an object for {}: no object encoding exists for this architecture",
                            Target::ToDisplayString(targetArch));
        return false;
    }
    if (file.arch != expectedArch) {
        error = std::format("object was compiled for {}, but the target is {}", RcuArchName(file.arch),
                            RcuArchName(expectedArch));
        return false;
    }
    if (targetOs == Target::OS::Windows) {
        return WriteCoffObject(file, targetArch, output, error);
    }
    if (targetOs == Target::OS::MacOS) {
        return WriteMachOObject(file, targetArch, output, error);
    }
    return WriteElfObject(file, targetOs, targetArch, output, error);
}

bool ValidateSymbolSections(const RcuFile &file, const std::string_view writer, std::string &error) {
    for (const auto &symbol : file.symbols) {
        if (symbol.sectionIdx == RCU_SEC_EXTERNAL || symbol.sectionIdx == RCU_SEC_ABSOLUTE ||
            SymbolSectionIndex(file, symbol.sectionIdx)) {
            continue;
        }
        error = std::format("symbol '{}' is defined in section {}, which the object does not contain, so the {} "
                            "cannot place it",
                            symbol.name, symbol.sectionIdx, writer);
        return false;
    }
    return true;
}

std::optional<std::size_t> SymbolSectionIndex(const RcuFile &file, const std::uint16_t sectionIdx) {
    std::uint32_t type = RcuSecType::Null;
    switch (sectionIdx) {
    case RCU_TEXT_IDX:
        type = RcuSecType::Text;
        break;
    case RCU_RODATA_IDX:
        type = RcuSecType::RoData;
        break;
    case RCU_DATA_IDX:
        type = RcuSecType::Data;
        break;
    case RCU_BSS_IDX:
        type = RcuSecType::Bss;
        break;
    default:
        return std::nullopt;
    }
    const auto section =
        std::ranges::find_if(file.sections, [&](const RcuSection &candidate) { return candidate.type == type; });
    if (section == file.sections.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(section - file.sections.begin());
}
} // namespace Rux
