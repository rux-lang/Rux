// Linker facade: constructs the object writers and dispatches Link() to the
// one matching the target's object format.

#include "Linker/Linker.h"

#include <utility>

namespace Rux {

Linker::Linker(std::vector<RcuFile> inputObjects, std::string inputPackageName,
               std::vector<std::filesystem::path> inputImportSearchDirs, const bool inputIsDll,
               const Target::OS inputTargetOs)
    : objects(std::move(inputObjects))
    , packageName(std::move(inputPackageName))
    , importSearchDirs(std::move(inputImportSearchDirs))
    , isDll(inputIsDll)
    , targetOs(inputTargetOs) {
}

void Linker::Error(std::string msg) {
    errors.push_back({std::move(msg)});
}

bool Linker::Link(const std::filesystem::path &outputPath) {
    switch (targetOs) {
    case Target::OS::Windows:
        return LinkPe64(outputPath);
    case Target::OS::MacOS:
        return LinkMachO64(outputPath);
    default:
        // Linux, the BSDs, Solaris, and illumos all emit ELF64.
        return LinkElf64(outputPath);
    }
}
} // namespace Rux
