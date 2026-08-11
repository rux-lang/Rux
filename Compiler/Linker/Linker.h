#pragma once

#include "Linker/ArtifactKind.h"
#include "Object/Rcu/Rcu.h"
#include "Target/Target.h"

#include <filesystem>
#include <string>
#include <vector>

namespace Rux {
struct LinkerError {
    std::string message;
};

// Links one or more RcuFile objects into a native executable, choosing the
// object format from the target OS: Windows → PE32+, macOS → Mach-O, every
// other supported OS (Linux, the BSDs, illumos) → ELF64. All three writers are
// always compiled; Link() dispatches on `targetOs` at run time.
//
// The target architecture is given explicitly rather than taken from the host,
// and every input object must have been compiled for it. Only x86-64 machine
// code can be laid out today; the AArch64 writers arrive with the native
// AArch64 back end.
class Linker {
public:
    explicit Linker(std::vector<RcuFile> inputObjects, std::string inputPackageName,
                    std::vector<std::filesystem::path> inputImportSearchDirs, ArtifactKind inputArtifactKind,
                    Target::OS inputTargetOs, Target::Arch inputTargetArch);

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
    ArtifactKind artifactKind = ArtifactKind::Executable;
    Target::OS targetOs = Target::HostOS;
    Target::Arch targetArch = Target::HostArch;

    void Error(std::string msg);

    // Rejects an input compiled for another architecture, and an architecture
    // no object writer supports, before any bytes are laid out.
    [[nodiscard]] bool CheckArchitecture();

    // Object-format writers, one per target family. Each is always compiled;
    // Link() selects the one matching `targetOs`.
    [[nodiscard]] bool LinkPe64(const std::filesystem::path &outputPath);
    [[nodiscard]] bool LinkElf64(const std::filesystem::path &outputPath);
    [[nodiscard]] bool LinkMachO64(const std::filesystem::path &outputPath);
    [[nodiscard]] bool WriteStaticLibrary(const std::filesystem::path &outputPath);
};
} // namespace Rux
