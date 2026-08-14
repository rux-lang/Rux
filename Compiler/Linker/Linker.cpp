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

bool Linker::BuildGraph() {
    graph = RcuLinkGraph::Build(objects, packageName, artifactKind, targetArch);
    for (const RcuLinkDiagnostic &diagnostic : graph->Diagnostics()) {
        switch (diagnostic.kind) {
        case RcuLinkDiagnosticKind::UnsupportedArchitecture:
            Error(std::format("cannot link for {}: no object writer exists for this architecture",
                              Target::ToDisplayString(targetArch)));
            break;
        case RcuLinkDiagnosticKind::ArchitectureMismatch:
            Error(std::format("object {} was compiled for {}, but the link target is {}", diagnostic.objectName,
                              RcuArchName(diagnostic.actualArchitecture),
                              RcuArchName(diagnostic.expectedArchitecture)));
            break;
        case RcuLinkDiagnosticKind::DuplicateDefinition:
            Error(targetOs == Target::OS::MacOS ? "duplicate definition of symbol '" + diagnostic.symbol + "'"
                                                : "duplicate symbol '" + diagnostic.symbol + "'");
            break;
        case RcuLinkDiagnosticKind::UndefinedSymbol:
            Error(targetOs == Target::OS::MacOS
                      ? "undefined symbol '" + diagnostic.symbol + "'"
                      : "undefined symbol '" + diagnostic.symbol + "' — no definition or external import was found");
            break;
        case RcuLinkDiagnosticKind::MissingEntryPoint:
            Error("undefined symbol 'Main' — no entry point found");
            break;
        }
    }
    return !graph->HasErrors();
}

std::vector<std::string> Linker::WindowsExportNames() const {
    std::vector<std::string> exports;
    if (!graph) {
        return exports;
    }
    for (const RcuSymbolLocation location : graph->ExportRoots()) {
        const RcuSymbol &symbol = objects[location.objectIndex].symbols[location.symbolIndex];
        if (symbol.kind == RcuSymKind::Func && symbol.name != "DllMain") {
            exports.push_back(symbol.name);
        }
    }
    std::ranges::sort(exports);
    exports.erase(std::unique(exports.begin(), exports.end()), exports.end());
    return exports;
}

bool Linker::Link(const std::filesystem::path &outputPath) {
    // Preserve the target-profile diagnostic before graph policy (such as a
    // missing executable entry) for an OS that has no complete image writer.
    if (targetOs == Target::OS::Unknown) {
        return LinkElf64(outputPath);
    }
    if (!BuildGraph()) {
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
            const std::vector<std::string> exports = WindowsExportNames();
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
        // FreeBSD and Linux both emit ELF64.
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
