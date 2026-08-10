// Linker facade: constructs the object writers and dispatches Link() to the
// one matching the target's object format.

#include "Linker/Linker.h"

#include "Linker/ArchiveWriter.h"
#include "Linker/NativeObjectWriter.h"

#include <algorithm>
#include <utility>

namespace Rux {
Linker::Linker(std::vector<RcuFile> inputObjects, std::string inputPackageName,
               std::vector<std::filesystem::path> inputImportSearchDirs, const ArtifactKind inputArtifactKind,
               const Target::OS inputTargetOs)
    : objects(std::move(inputObjects))
    , packageName(std::move(inputPackageName))
    , importSearchDirs(std::move(inputImportSearchDirs))
    , artifactKind(inputArtifactKind)
    , targetOs(inputTargetOs) {
}

void Linker::Error(std::string msg) {
    errors.push_back({std::move(msg)});
}

bool Linker::Link(const std::filesystem::path &outputPath) {
    if (artifactKind == ArtifactKind::StaticLibrary) {
        return WriteStaticLibrary(outputPath);
    }
    switch (targetOs) {
    case Target::OS::Windows: {
        if (!LinkPe64(outputPath)) {
            return false;
        }
        if (artifactKind == ArtifactKind::SharedLibrary) {
            std::vector<std::string> exports;
            for (const auto &object : objects) {
                for (const auto &symbol : object.symbols) {
                    if (symbol.visibility != RcuSymVis::Local && symbol.sectionIdx != RCU_SEC_EXTERNAL) {
                        exports.push_back(symbol.name);
                    }
                }
            }
            std::ranges::sort(exports);
            exports.erase(std::unique(exports.begin(), exports.end()), exports.end());
            auto importLibraryPath = outputPath;
            importLibraryPath.replace_extension(".lib");
            std::string error;
            if (!WriteWindowsImportLibrary(outputPath.filename().string(), exports, importLibraryPath, error)) {
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
        if (!WriteNativeObject(object, targetOs, nativeObject, error)) {
            Error(std::move(error));
            return false;
        }
        nativeObjects.push_back(std::move(nativeObject));
    }
    std::string error;
    if (!WriteNativeArchive(nativeObjects, targetOs, outputPath, error)) {
        Error(std::move(error));
        return false;
    }
    return true;
}
} // namespace Rux
