#pragma once

#include "Object/Rcu/Rcu.h"
#include "Target/Target.h"

#include <filesystem>
#include <string>
#include <vector>

namespace Rux {
struct LinkerError {
    std::string message;
};

// Links one or more RcuFile objects into a native x86-64 executable, choosing
// the object format from the target OS: Windows → PE32+, macOS → Mach-O,
// every other supported OS (Linux, the BSDs, illumos) → ELF64. All three
// writers are always compiled; Link() dispatches on `targetOs` at run time.
class Linker {
public:
    explicit Linker(std::vector<RcuFile> inputObjects, std::string inputPackageName,
                    std::vector<std::filesystem::path> inputImportSearchDirs = {}, bool inputIsDll = false,
                    Target::OS inputTargetOs = Target::HostOS);

    // Produce the EXE or DLL at outputPath. Creates parent directories as
    // needed. Returns false if any errors occurred; call Errors() for
    // details.
    [[nodiscard]] bool Link(const std::filesystem::path &outputPath);

    [[nodiscard]] const std::vector<LinkerError> &Errors() const {
        return errors;
    }

private:
    std::vector<RcuFile> objects;
    std::string packageName;
    std::vector<std::filesystem::path> importSearchDirs;
    std::vector<LinkerError> errors;
    bool isDll = false;
    Target::OS targetOs = Target::HostOS;

    void Error(std::string msg);

    // Object-format writers, one per target family. Each is always compiled;
    // Link() selects the one matching `targetOs`.
    [[nodiscard]] bool LinkPe64(const std::filesystem::path &outputPath);
    [[nodiscard]] bool LinkElf64(const std::filesystem::path &outputPath);
    [[nodiscard]] bool LinkMachO64(const std::filesystem::path &outputPath);
};
} // namespace Rux
