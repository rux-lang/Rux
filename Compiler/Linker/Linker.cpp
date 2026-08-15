// Linker facade: constructs the object writers and dispatches Link() to the
// one matching the target's object format.

#include "Linker/Linker.h"

#include "Linker/ArchiveWriter.h"
#include "Linker/NativeObjectWriter.h"

#include <algorithm>
#include <format>
#include <string_view>
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

namespace {
[[nodiscard]] constexpr std::string_view FormatName(const Target::OS os) noexcept {
    switch (os) {
    case Target::OS::Windows:
        return "PE/COFF";
    case Target::OS::MacOS:
        return "Mach-O";
    case Target::OS::FreeBSD:
    case Target::OS::Linux:
    case Target::OS::Unknown:
        return "ELF";
    default:
        return "unknown-format";
    }
}

[[nodiscard]] constexpr std::string_view ArtifactName(const ArtifactKind kind) noexcept {
    switch (kind) {
    case ArtifactKind::Executable:
        return "executable";
    case ArtifactKind::SharedLibrary:
        return "shared library";
    case ArtifactKind::StaticLibrary:
        return "static library";
    }
    return "artifact";
}

[[nodiscard]] constexpr std::string_view RelocationRangeNote(const std::uint16_t type) noexcept {
    switch (type) {
    case RcuRelType::AArch64Call26:
    case RcuRelType::AArch64Jump26:
        return "supported range: -134217728 to 134217724 bytes, aligned to 4 bytes";
    case RcuRelType::AArch64CondBr19:
        return "supported range: -1048576 to 1048572 bytes, aligned to 4 bytes";
    case RcuRelType::AArch64TstBr14:
        return "supported range: -32768 to 32764 bytes, aligned to 4 bytes";
    case RcuRelType::AArch64AdrPrelPgHi21:
        return "supported range: -4294967296 to 4294963200 bytes, aligned to 4096 bytes";
    case RcuRelType::AArch64Prel32:
    case RcuRelType::Rel32:
        return "supported range: -2147483648 to 2147483647 bytes";
    case RcuRelType::Abs32:
        return "supported range: 0 to 4294967295";
    default:
        return {};
    }
}
} // namespace

void Linker::Error(std::string msg, std::vector<std::string> notes) {
    errors.push_back(
        {std::format("cannot link {} {} '{}': {}", FormatName(targetOs), ArtifactName(artifactKind), packageName, msg),
         std::move(notes)});
}

std::vector<std::string> Linker::RelocationNotes(const RcuLinkReference &reference,
                                                 const std::string_view error) const {
    const RcuFile &object = objects[reference.objectIndex];
    const RcuSection &section = object.sections[reference.sectionIndex];
    const RcuReloc &relocation = section.relocs[reference.relocationIndex];
    const std::string objectName = object.sourcePath.empty() ? packageName : object.sourcePath;
    std::vector<std::string> notes{std::format("RCU object '{}', source section '{}', offset {}", objectName,
                                               section.name, relocation.sectionOffset)};
    if (error.contains("out of range") || error.contains("does not fit")) {
        if (const std::string_view range = RelocationRangeNote(relocation.type); !range.empty()) {
            notes.emplace_back(range);
        }
    }
    return notes;
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
            Error(std::format("RCU object '{}' was compiled for {}, but the link target is {}", diagnostic.objectName,
                              RcuArchName(diagnostic.actualArchitecture),
                              RcuArchName(diagnostic.expectedArchitecture)));
            break;
        case RcuLinkDiagnosticKind::InvalidObject:
            Error(diagnostic.message, diagnostic.notes);
            break;
        case RcuLinkDiagnosticKind::DuplicateDefinition:
            Error(targetOs == Target::OS::MacOS ? "duplicate definition of symbol '" + diagnostic.symbol + "'"
                                                : "duplicate symbol '" + diagnostic.symbol + "'",
                  {std::format("first defined by RCU object '{}'", diagnostic.objectName),
                   std::format("also defined by RCU object '{}'", diagnostic.relatedObjectName)});
            break;
        case RcuLinkDiagnosticKind::UndefinedSymbol:
            Error(targetOs == Target::OS::MacOS
                      ? "undefined symbol '" + diagnostic.symbol + "'"
                      : "undefined symbol '" + diagnostic.symbol + "' — no definition or external import was found",
                  diagnostic.notes);
            break;
        case RcuLinkDiagnosticKind::MissingEntryPoint:
            Error("entry point symbol 'Main' is undefined",
                  {"an executable artifact requires one global 'Main' definition"});
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
