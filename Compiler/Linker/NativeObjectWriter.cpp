#include "Linker/NativeObjectWriter.h"

#include "Linker/Coff/CoffObjectWriter.h"
#include "Linker/Elf/ElfObjectWriter.h"
#include "Linker/MachO/MachOObjectWriter.h"

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
} // namespace Rux
