// Linker facade: constructs the object writers and dispatches Link() to the
// one matching the target's object format.

#include "Linker/Linker.h"

#include "Linker/ArchiveWriter.h"
#include "Linker/NativeObjectWriter.h"

#include <algorithm>
#include <format>
#include <utility>

namespace Rux {
Linker::Linker(std::vector<RcuFile> inputObjects, std::string inputPackageName,
               std::vector<std::filesystem::path> inputImportSearchDirs, const ArtifactKind inputArtifactKind,
               const Target::OS inputTargetOs, const Target::Arch inputTargetArch)
    : objects(std::move(inputObjects))
    , packageName(std::move(inputPackageName))
    , importSearchDirs(std::move(inputImportSearchDirs))
    , artifactKind(inputArtifactKind)
    , targetOs(inputTargetOs)
    , targetArch(inputTargetArch) {
}

void Linker::Error(std::string msg) {
    errors.push_back({std::move(msg)});
}

bool Linker::CheckArchitecture() {
    const uint8_t expected = RcuArchFor(targetArch);
    if (expected == RcuArch::Unknown) {
        Error(std::format("cannot link for {}: no object writer exists for this architecture",
                          Target::ToDisplayString(targetArch)));
        return false;
    }
    for (const auto &object : objects) {
        if (object.arch != expected) {
            Error(std::format("object {} was compiled for {}, but the link target is {}",
                              object.sourcePath.empty() ? packageName : object.sourcePath, RcuArchName(object.arch),
                              RcuArchName(expected)));
            return false;
        }
    }
    if (targetArch == Target::Arch::X86_64 || artifactKind == ArtifactKind::StaticLibrary) {
        return true;
    }
    // Beyond x86-64, ELF, PE, and Mach-O lay out AArch64 executable and shared
    // library images.
    if (targetOs == Target::OS::Windows && targetArch == Target::Arch::AArch64) {
        return true;
    }
    const bool elf = targetOs != Target::OS::Windows && targetOs != Target::OS::MacOS;
    if (elf && targetArch == Target::Arch::AArch64) {
        return true;
    }
    if (targetOs == Target::OS::MacOS && targetArch == Target::Arch::AArch64) {
        return true;
    }
    Error(std::format("linking {} for {} is not implemented yet",
                      artifactKind == ArtifactKind::SharedLibrary ? "a shared library" : "an executable",
                      Target::ToDisplayString(targetArch)));
    return false;
}

bool Linker::Link(const std::filesystem::path &outputPath) {
    if (!CheckArchitecture()) {
        return false;
    }
    if (artifactKind == ArtifactKind::StaticLibrary) {
        return WriteStaticLibrary(outputPath);
    }
    switch (targetOs) {
    case Target::OS::Windows: {
        if (!LinkPe32Plus(outputPath)) {
            return false;
        }
        if (artifactKind == ArtifactKind::SharedLibrary) {
            std::vector<std::string> exports;
            for (const auto &object : objects) {
                for (const auto &symbol : object.symbols) {
                    if (symbol.kind == RcuSymKind::Func && symbol.visibility != RcuSymVis::Local &&
                        !symbol.name.empty() && symbol.name != "DllMain") {
                        exports.push_back(symbol.name);
                    }
                }
            }
            std::ranges::sort(exports);
            exports.erase(std::unique(exports.begin(), exports.end()), exports.end());
            auto importLibraryPath = outputPath;
            importLibraryPath.replace_extension(".lib");
            std::string error;
            if (!WriteWindowsImportLibrary(outputPath.filename().string(), exports, targetArch, importLibraryPath,
                                           error)) {
                Error(std::move(error));
                return false;
            }
        }
        return true;
    }
    case Target::OS::MacOS:
        return LinkMachO64(outputPath);
    default:
        // Linux, the BSDs, Solaris, and illumos all emit ELF64.
        return LinkElf64(outputPath);
    }
}

bool Linker::WriteStaticLibrary(const std::filesystem::path &outputPath) {
    std::vector<NativeObject> nativeObjects;
    nativeObjects.reserve(objects.size());
    for (const auto &object : objects) {
        NativeObject nativeObject;
        std::string error;
        if (!WriteNativeObject(object, targetOs, targetArch, nativeObject, error)) {
            Error(std::move(error));
            return false;
        }
        nativeObjects.push_back(std::move(nativeObject));
    }
    std::string error;
    if (!WriteNativeArchive(nativeObjects, targetOs, targetArch, outputPath, error)) {
        Error(std::move(error));
        return false;
    }
    return true;
}
} // namespace Rux
