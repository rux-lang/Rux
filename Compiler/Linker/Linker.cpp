// Linker facade: constructs the object writers and dispatches Link() to the
// one matching the target's object format.

#include "Linker/Linker.h"

#include <utility>

namespace Rux {

Linker::Linker(std::vector<RcuFile> objects, std::string packageName,
               std::vector<std::filesystem::path> importSearchDirs, bool isDll, Target::OS targetOs)
    : objects(std::move(objects))
    , packageName(std::move(packageName))
    , importSearchDirs(std::move(importSearchDirs))
    , isDll(isDll)
    , targetOs(targetOs) {
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
