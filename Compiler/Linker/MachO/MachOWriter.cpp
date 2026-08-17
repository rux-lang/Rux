// Mach-O image writer for macOS x86-64 and AArch64. Freestanding x86-64 programs
// retain a static LC_UNIXTHREAD entry point; dynamic executables and shared
// libraries use dyld, eager symbol binding, and architecture-owned symbol stubs.
// AArch64 executables are always dynamic and position-independent because the
// macOS kernel rejects both static and non-PIE arm64 images.

#include "Crypto/Sha256.h"
#include "Linker/AArch64Relocation.h"
#include "Linker/Linker.h"
#include "Linker/LinkerInternal.h"
#include "Linker/MachO/CodeSignature.h"
#include "Linker/MachO/MachOLayout.h"
#include "Linker/MachO/MachOLinkEdit.h"
#include "Linker/RcuObjectLayout.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <vector>

namespace Rux {
namespace {
constexpr uint64_t kExecutableBase = 0x1'0000'0000ULL;
constexpr const char *kSystemLibName = "libSystem.B.dylib";
constexpr const char *kDefaultLib = "/usr/lib/libSystem.B.dylib";
constexpr std::size_t kMachUuidSize = 16;

enum class MachOEntryStrategy : uint8_t {
    Main,
    UnixThread,
};

struct MachOArchitectureProfile {
    Target::Arch architecture;
    uint32_t cpuType;
    uint32_t cpuSubtype;
    uint64_t vmPageAlignment;
    uint32_t fileAlignment;
    MachOEntryStrategy dynamicEntryStrategy;
    MachOEntryStrategy staticEntryStrategy;
    uint32_t instructionStubSize;
    uint32_t instructionStubAlignment;
    std::array<uint16_t, 8> supportedRelocations;
    uint32_t threadStateFlavor;
    uint32_t threadStateCount;
    uint32_t threadProgramCounterIndex;
    bool emitBuildVersion;
    // XNU refuses a static executable for every architecture except x86-64, and
    // refuses a dynamic one without MH_PIE where pie_required() holds. An arm64
    // image therefore has to be dyld-linked, marked MH_PIE, and slidable.
    bool requiresPositionIndependentExecutable;
};

constexpr MachOArchitectureProfile kX86_64Profile{
    .architecture = Target::Arch::X86_64,
    .cpuType = 0x0100'0007,
    .cpuSubtype = 0x0000'0003,
    .vmPageAlignment = 0x1000,
    .fileAlignment = 16,
    .dynamicEntryStrategy = MachOEntryStrategy::Main,
    .staticEntryStrategy = MachOEntryStrategy::UnixThread,
    .instructionStubSize = 6,
    .instructionStubAlignment = 2,
    .supportedRelocations = {RcuRelType::Rel32, RcuRelType::Abs64, RcuRelType::Abs32},
    .threadStateFlavor = 4,
    .threadStateCount = 42,
    .threadProgramCounterIndex = 16,
    .emitBuildVersion = false,
    .requiresPositionIndependentExecutable = false,
};

constexpr MachOArchitectureProfile kAArch64Profile{
    .architecture = Target::Arch::AArch64,
    .cpuType = 0x0100'000C,
    .cpuSubtype = 0,
    .vmPageAlignment = 0x4000,
    .fileAlignment = 16,
    .dynamicEntryStrategy = MachOEntryStrategy::Main,
    .staticEntryStrategy = MachOEntryStrategy::UnixThread,
    .instructionStubSize = 12,
    .instructionStubAlignment = 4,
    .supportedRelocations = {RcuRelType::Abs64, RcuRelType::Abs32, RcuRelType::AArch64Prel32, RcuRelType::AArch64Call26,
                             RcuRelType::AArch64Jump26, RcuRelType::AArch64AdrPrelPgHi21,
                             RcuRelType::AArch64AddAbsLo12Nc, RcuRelType::AArch64LdstAbsLo12Nc},
    .threadStateFlavor = 6, // ARM_THREAD_STATE64
    .threadStateCount = 68,
    .threadProgramCounterIndex = 32,
    .emitBuildVersion = true,
    .requiresPositionIndependentExecutable = true,
};

/// The Mach-O constants for one architecture, kept as a table so architecture-specific values stay out of the writer
/// itself.
///
/// @return nullptr for an architecture this writer does not emit
const MachOArchitectureProfile *ArchitectureProfile(const Target::Arch architecture) {
    if (architecture == kX86_64Profile.architecture) {
        return &kX86_64Profile;
    }
    if (architecture == kAArch64Profile.architecture) {
        return &kAArch64Profile;
    }
    return nullptr;
}

/// Add a signed addend to an address, reporting rather than wrapping on overflow: a wrapped address would place a
/// relocation somewhere plausible-looking and wrong.
bool AddSigned(const uint64_t value, const int32_t addend, uint64_t &result) {
    if (addend >= 0) {
        const auto positive = static_cast<uint64_t>(addend);
        if (value > std::numeric_limits<uint64_t>::max() - positive) {
            return false;
        }
        result = value + positive;
        return true;
    }
    const auto magnitude = static_cast<uint64_t>(-static_cast<int64_t>(addend));
    if (value < magnitude) {
        return false;
    }
    result = value - magnitude;
    return true;
}

bool SupportsRelocation(const MachOArchitectureProfile &profile, const uint16_t type) {
    return std::ranges::find(profile.supportedRelocations, type) != profile.supportedRelocations.end();
}

bool ApplyMachORelocation(const MachOArchitectureProfile &profile, Buf &buffer, const size_t patchOffset,
                          const uint64_t relocationVA, const uint64_t targetVA, const int32_t addend,
                          const uint16_t type, const std::string_view symbolName, std::string &error) {
    if (!SupportsRelocation(profile, type)) {
        error = "relocation " + std::string(RcuRelTypeName(type)) + " against '" + std::string(symbolName) +
                "' is not supported by the Mach-O " + std::string(Target::ToDisplayString(profile.architecture)) +
                " profile";
        return false;
    }
    uint64_t value = 0;
    if (!AddSigned(targetVA, addend, value)) {
        error = "relocation " + std::string(RcuRelTypeName(type)) + " against '" + std::string(symbolName) +
                "' overflows a 64-bit Mach-O address";
        return false;
    }
    if (profile.architecture == Target::Arch::AArch64) {
        const size_t width = type == RcuRelType::Abs64 ? 8 : 4;
        if (patchOffset > buffer.size() || buffer.size() - patchOffset < width) {
            error = std::string(RcuRelTypeName(type)) + " relocation against '" + std::string(symbolName) +
                    "' is outside its Mach-O section";
            return false;
        }
        if (type == RcuRelType::Abs32 && value > std::numeric_limits<uint32_t>::max()) {
            error = "ABS_32 relocation against '" + std::string(symbolName) + "' does not fit in 32 bits";
            return false;
        }
        return ApplyAArch64Relocation(buffer, patchOffset, type, value, 0, relocationVA, symbolName,
                                      "Mach-O AArch64 profile", error);
    }
    if (type == RcuRelType::Abs64) {
        if (patchOffset > buffer.size() || buffer.size() - patchOffset < 8) {
            error = "ABS_64 relocation against '" + std::string(symbolName) + "' is outside its Mach-O section";
            return false;
        }
        Patch64(buffer, patchOffset, value);
        return true;
    }
    if (patchOffset > buffer.size() || buffer.size() - patchOffset < 4) {
        error = std::string(RcuRelTypeName(type)) + " relocation against '" + std::string(symbolName) +
                "' is outside its Mach-O section";
        return false;
    }
    if (type == RcuRelType::Abs32) {
        if (value > std::numeric_limits<uint32_t>::max()) {
            error = "ABS_32 relocation against '" + std::string(symbolName) + "' does not fit in 32 bits";
            return false;
        }
        Patch32(buffer, patchOffset, static_cast<uint32_t>(value));
        return true;
    }

    const uint64_t nextInstruction = relocationVA + 4;
    const uint64_t magnitude = value >= nextInstruction ? value - nextInstruction : nextInstruction - value;
    if ((value >= nextInstruction && magnitude > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) ||
        (value < nextInstruction && magnitude > uint64_t{1} << 31U)) {
        error = "REL_32 relocation against '" + std::string(symbolName) + "' is out of range";
        return false;
    }
    const int64_t displacement =
        value >= nextInstruction ? static_cast<int64_t>(magnitude) : -static_cast<int64_t>(magnitude);
    Patch32(buffer, patchOffset, static_cast<uint32_t>(static_cast<int32_t>(displacement)));
    return true;
}

/// The install name a dependency is recorded under, so an image references a shared library the way the loader will
/// search for it.
std::string NormalizeDylibName(const std::string &name) {
    if (name == kSystemLibName) {
        return kDefaultLib;
    }
    return name;
}

} // namespace

bool Linker::LinkMachO64(const std::filesystem::path &outputPath) {
    const MachOArchitectureProfile *architecture = ArchitectureProfile(targetArch);
    if (architecture == nullptr) {
        Error("internal: no Mach-O architecture profile for " + std::string(Target::ToDisplayString(targetArch)));
        return false;
    }
    if (!graph) {
        Error("internal: Mach-O linking requires an RCU link graph");
        return false;
    }
    for (const RcuFile &object : objects) {
        for (const RcuSection &section : object.sections) {
            for (const RcuReloc &relocation : section.relocs) {
                if (relocation.symbolIndex >= object.symbols.size()) {
                    Error("relocation in Mach-O section '" + section.name + "' refers to missing symbol index " +
                          std::to_string(relocation.symbolIndex));
                }
            }
        }
    }
    if (!errors.empty()) {
        return false;
    }
    const bool isShared = artifactKind == ArtifactKind::SharedLibrary;
    const uint64_t imageBase = isShared ? 0 : kExecutableBase;

    // Extern declarations carry their #Link library in typeName. Calls in a
    // different RCU object may carry only the symbol name, so gather all
    // declarations before visiting relocations.
    std::unordered_map<std::string, std::string> explicitImportLib;
    for (const auto &object : objects) {
        for (const auto &symbol : object.symbols) {
            if (symbol.kind != RcuSymKind::ExternFunc || symbol.name.empty() || symbol.typeName.empty()) {
                continue;
            }
            const std::string library = NormalizeDylibName(symbol.typeName);
            const auto [it, inserted] = explicitImportLib.try_emplace(symbol.name, library);
            if (!inserted && it->second != library) {
                Error("external symbol '" + symbol.name + "' is assigned to both '" + it->second + "' and '" + library +
                      "'");
            }
        }
    }

    // Assign the graph's referenced externals to libraries. Function addresses
    // resolve to generated stubs. Imported data still needs GOT-aware lowering
    // and is rejected explicitly rather than producing an invalid direct
    // reference.
    std::unordered_map<std::string, std::string> importLib;
    for (const RcuReferencedExternal &external : graph->ReferencedExternals()) {
        if (external.kind == RcuSymKind::ExternData) {
            Error("external data symbol '" + external.name + "' cannot be imported by the Mach-O " +
                  std::string(Target::ToDisplayString(targetArch)) +
                  " linker because GOT-aware lowering is not implemented");
            continue;
        }
        if (external.kind != RcuSymKind::ExternFunc) {
            continue;
        }
        const auto explicitIt = explicitImportLib.find(external.name);
        for (const size_t referenceIndex : external.referenceIndices) {
            const RcuLinkReference &reference = graph->References()[referenceIndex];
            const RcuSymbol &callSite = objects[reference.objectIndex].symbols[reference.symbolIndex];
            const std::string library =
                explicitIt != explicitImportLib.end()
                    ? explicitIt->second
                    : NormalizeDylibName(callSite.typeName.empty() ? kDefaultLib : callSite.typeName);
            const auto [it, inserted] = importLib.try_emplace(external.name, library);
            if (!inserted && it->second != library) {
                Error("external symbol '" + external.name + "' is referenced from both '" + it->second + "' and '" +
                      library + "'");
            }
        }
    }
    if (!errors.empty()) {
        return false;
    }

    // An arm64 executable cannot be static, so it is dyld-linked even when it
    // imports nothing. Its image is then slid at load time, which makes every
    // absolute pointer the compiler built into constant or writable data a
    // rebase target — and dyld may only rebase inside a writable segment.
    const bool positionIndependent = !isShared && architecture->requiresPositionIndependentExecutable;
    const bool dynamic = isShared || positionIndependent || !importLib.empty();
    const bool slidImage = isShared || positionIndependent;
    const bool writableConstSegment = slidImage && architecture->requiresPositionIndependentExecutable;

    std::vector<std::string> importNames;
    importNames.reserve(importLib.size());
    for (const auto &name : importLib | std::views::keys) {
        importNames.push_back(name);
    }
    std::ranges::sort(importNames);

    std::vector<std::string> neededLibs;
    for (const auto &name : importNames) {
        const std::string &library = importLib.at(name);
        if (std::ranges::find(neededLibs, library) == neededLibs.end()) {
            neededLibs.push_back(library);
        }
    }
    std::ranges::sort(neededLibs);
    // dyld requires every image it loads to link libSystem, so an import-free
    // program that is dynamic only because arm64 demands it still names it.
    if (dynamic && neededLibs.empty()) {
        neededLibs.emplace_back(kDefaultLib);
    }

    std::unordered_map<std::string, uint8_t> libraryOrdinal;
    for (size_t i = 0; i < neededLibs.size(); ++i) {
        if (i + 1 > 255) {
            Error("the Mach-O linker supports at most 255 imported libraries");
            return false;
        }
        libraryOrdinal[neededLibs[i]] = static_cast<uint8_t>(i + 1);
    }
    std::vector<MachO::LinkEditImport> linkEditImports;
    linkEditImports.reserve(importNames.size());
    for (const std::string &name : importNames) {
        linkEditImports.push_back({.name = name, .libraryOrdinal = libraryOrdinal.at(importLib.at(name))});
    }

    // Dynamic executables are entered as a normal function by dyld. The AArch64
    // shim preserves dyld's frame/link registers across the Rux call. Static
    // executables call Main and issue the native macOS exit syscall directly;
    // the AArch64 syscall number lives in X16.
    Buf textPrefix;
    size_t callMainDisp = 0;
    if (isShared) {
        // A dylib has no process entry stub. dyld calls only explicit
        // initializers; Rux 0.4.0 does not synthesize one.
    }
    else if (dynamic) {
        if (targetArch == Target::Arch::AArch64) {
            WriteU32(textPrefix, 0xA9BF'7BFD); // stp x29, x30, [sp, #-16]!
            WriteU32(textPrefix, 0x9100'03FD); // mov x29, sp
            callMainDisp = textPrefix.size();
            WriteU32(textPrefix, 0x9400'0000); // bl Main
            WriteU32(textPrefix, 0xA8C1'7BFD); // ldp x29, x30, [sp], #16
            WriteU32(textPrefix, 0xD65F'03C0); // ret to dyld
        }
        else {
            textPrefix.insert(textPrefix.end(), {0x48, 0x83, 0xEC, 0x28}); // sub rsp, 40
            callMainDisp = textPrefix.size() + 1;
            textPrefix.insert(textPrefix.end(), {0xE8, 0, 0, 0, 0});       // call Main
            textPrefix.insert(textPrefix.end(), {0x48, 0x83, 0xC4, 0x28}); // add rsp, 40
            textPrefix.push_back(0xC3);                                    // ret to dyld
        }
    }
    else if (targetArch == Target::Arch::AArch64) {
        callMainDisp = textPrefix.size();
        WriteU32(textPrefix, 0x9400'0000); // bl Main
        WriteU32(textPrefix, 0xD280'0030); // mov x16, #1 (SYS_exit)
        WriteU32(textPrefix, 0xD400'1001); // svc #0x80
    }
    else {
        textPrefix.insert(textPrefix.end(), {0x48, 0x83, 0xE4, 0xF0}); // and rsp, -16
        textPrefix.insert(textPrefix.end(), {0x48, 0x83, 0xEC, 0x08}); // sub rsp, 8
        callMainDisp = textPrefix.size() + 1;
        textPrefix.insert(textPrefix.end(), {0xE8, 0, 0, 0, 0});       // call Main
        textPrefix.insert(textPrefix.end(), {0x48, 0x83, 0xC4, 0x08}); // add rsp, 8
        textPrefix.insert(textPrefix.end(), {0x89, 0xC7});             // mov edi, eax
        textPrefix.insert(textPrefix.end(), {0xB8, 0x01, 0, 0, 0x02}); // SYS_exit
        textPrefix.insert(textPrefix.end(), {0x0F, 0x05});             // syscall
    }
    // Merge RCU sections after the format-owned entry prefix. The shared layout
    // owns per-object alignment and all typed symbol/relocation offsets.
    RcuLayoutPrefixes prefixes;
    prefixes.text = textPrefix;
    const RcuObjectLayout layout = RcuObjectLayout::Build(objects, prefixes);
    Buf textBuffer = layout.Data(RcuMergedSection::Text);
    Buf rodataBuffer = layout.Data(RcuMergedSection::RoData);
    Buf dataBuffer = layout.Data(RcuMergedSection::Data);

    // The architecture owns the instruction sequence and its fixed size. An
    // x86-64 stub jumps through a RIP-relative pointer. An Apple ARM64 stub uses
    // X16 for ADRP/LDR/BR. dyld eagerly fills the matching non-lazy pointer
    // before transferring control.
    Buf stubs;
    for (size_t i = 0; i < importNames.size(); ++i) {
        if (targetArch == Target::Arch::AArch64) {
            WriteU32(stubs, 0x9000'0010); // adrp x16, pointer@page
            WriteU32(stubs, 0xF940'0210); // ldr x16, [x16, pointer@pageoff]
            WriteU32(stubs, 0xD61F'0200); // br x16
        }
        else {
            stubs.insert(stubs.end(), {0xFF, 0x25, 0, 0, 0, 0});
        }
    }
    if (stubs.size() != importNames.size() * architecture->instructionStubSize) {
        Error("internal: Mach-O instruction stub does not match its architecture profile");
        return false;
    }
    Buf importPointers(importNames.size() * 8, 0);

    const MachOEntryStrategy entryStrategy =
        dynamic ? architecture->dynamicEntryStrategy : architecture->staticEntryStrategy;
    if ((!isShared && dynamic && entryStrategy != MachOEntryStrategy::Main) ||
        (!dynamic && entryStrategy != MachOEntryStrategy::UnixThread)) {
        Error("internal: Mach-O entry strategy is not implemented for " +
              std::string(Target::ToDisplayString(architecture->architecture)));
        return false;
    }
    const std::string installName = isShared ? "@rpath/" + outputPath.filename().string() : std::string{};
    const MachO::ImageLayoutRequest imageLayoutRequest{
        .neededLibraries = neededLibs,
        .installName = installName,
        .imageBase = imageBase,
        .vmPageAlignment = architecture->vmPageAlignment,
        .fileAlignment = architecture->fileAlignment,
        .instructionStubAlignment = architecture->instructionStubAlignment,
        .instructionStubSize = architecture->instructionStubSize,
        .cpuType = architecture->cpuType,
        .cpuSubtype = architecture->cpuSubtype,
        .threadStateFlavor = architecture->threadStateFlavor,
        .threadStateCount = architecture->threadStateCount,
        .threadProgramCounterIndex = architecture->threadProgramCounterIndex,
        .textSize = textBuffer.size(),
        .stubSize = stubs.size(),
        .constantDataSize = rodataBuffer.size(),
        .importPointerSize = importPointers.size(),
        .writableDataSize = dataBuffer.size(),
        .importCount = importNames.size(),
        .dynamic = dynamic,
        .shared = isShared,
        .positionIndependent = positionIndependent,
        .writableConstantSegment = writableConstSegment,
        .emitBuildVersion = architecture->emitBuildVersion,
    };
    const MachO::ImageLayout imageLayout = MachO::ImageLayoutBuilder::Plan(imageLayoutRequest);
    for (const std::string &diagnostic : imageLayout.diagnostics) {
        Error(diagnostic);
    }
    if (imageLayout.HasErrors()) {
        return false;
    }
    const RcuSectionBases &sectionBases = imageLayout.sectionBases;
    const auto symbolAddress = [&](const RcuSymbolLocation location) -> std::optional<uint64_t> {
        const auto placement = layout.Symbol(location);
        return placement && placement->section != RcuMergedSection::Bss
                 ? RcuObjectLayout::Address(*placement, sectionBases)
                 : std::nullopt;
    };

    const MachO::LinkEditLayout linkEdit = MachO::LinkEditBuilder::Build({
        .graph = *graph,
        .objects = objects,
        .objectLayout = layout,
        .imports = linkEditImports,
        .segments =
            {
                .sectionBases = sectionBases,
                .imageBase = imageBase,
                .constantSegmentAddress = imageLayout.constantSegmentAddress,
                .dataSegmentAddress = imageLayout.dataSegmentAddress,
                .linkEditOffset = imageLayout.linkEditOffset,
                .vmPageAlignment = architecture->vmPageAlignment,
                .textSegmentIndex = imageLayout.textSegmentIndex,
                .constantSegmentIndex = imageLayout.constantSegmentIndex,
                .dataSegmentIndex = imageLayout.dataSegmentIndex,
            },
        .codeSignatureIdentifier = packageName,
        .dynamic = dynamic,
        .shared = isShared,
        .slid = slidImage,
        .writableConstantSegment = writableConstSegment,
    });
    for (const std::string &diagnostic : linkEdit.diagnostics) {
        Error(diagnostic);
    }
    if (linkEdit.HasErrors()) {
        return false;
    }

    // Patch each stub to its corresponding pointer slot.
    for (size_t i = 0; i < importNames.size(); ++i) {
        const uint64_t stubOffset = i * architecture->instructionStubSize;
        const uint64_t pointerAddress = imageLayout.importPointerAddress + i * 8;
        if (targetArch == Target::Arch::AArch64) {
            std::string relocationError;
            const uint64_t adrpVA = imageLayout.stubAddress + stubOffset;
            if (!ApplyAArch64Relocation(stubs, stubOffset, RcuRelType::AArch64AdrPrelPgHi21, pointerAddress, 0, adrpVA,
                                        importNames[i], "Mach-O AArch64 instruction stub", relocationError) ||
                !ApplyAArch64Relocation(stubs, stubOffset + 4, RcuRelType::AArch64LdstAbsLo12Nc, pointerAddress, 0,
                                        adrpVA + 4, importNames[i], "Mach-O AArch64 instruction stub",
                                        relocationError)) {
                Error(std::move(relocationError));
                return false;
            }
        }
        else {
            const uint64_t stubNextInstruction =
                imageLayout.stubAddress + stubOffset + architecture->instructionStubSize;
            const uint64_t displacement = pointerAddress - stubNextInstruction;
            if (displacement > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
                Error("Mach-O instruction stub for '" + importNames[i] + "' cannot reach its pointer slot");
                return false;
            }
            Patch32(stubs, stubOffset + 2, static_cast<uint32_t>(displacement));
        }
    }

    std::unordered_map<std::string, uint64_t> importStubs;
    for (size_t i = 0; i < importNames.size(); ++i) {
        importStubs[importNames[i]] = imageLayout.stubAddress + i * architecture->instructionStubSize;
    }

    if (!isShared) {
        const std::optional<uint64_t> entryAddress =
            graph->EntryRoot() ? symbolAddress(*graph->EntryRoot()) : std::nullopt;
        if (!entryAddress) {
            Error("internal: Mach-O entry symbol has no merged-section placement");
            return false;
        }
        if (targetArch == Target::Arch::AArch64) {
            std::string relocationError;
            if (!ApplyAArch64Relocation(textBuffer, callMainDisp, RcuRelType::AArch64Call26, *entryAddress, 0,
                                        imageLayout.textAddress + callMainDisp, "Main", "Mach-O AArch64 entry stub",
                                        relocationError)) {
                Error(std::move(relocationError));
                return false;
            }
        }
        else {
            const uint64_t callMainNextInstruction = imageLayout.textAddress + callMainDisp + 4;
            const uint64_t magnitude = *entryAddress >= callMainNextInstruction
                                         ? *entryAddress - callMainNextInstruction
                                         : callMainNextInstruction - *entryAddress;
            if ((*entryAddress >= callMainNextInstruction &&
                 magnitude > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) ||
                (*entryAddress < callMainNextInstruction && magnitude > uint64_t{1} << 31U)) {
                Error("Mach-O entry stub cannot reach 'Main'");
                return false;
            }
            const int64_t displacement = *entryAddress >= callMainNextInstruction ? static_cast<int64_t>(magnitude)
                                                                                  : -static_cast<int64_t>(magnitude);
            Patch32(textBuffer, callMainDisp, static_cast<uint32_t>(static_cast<int32_t>(displacement)));
        }
    }

    // Resolve graph references against definitions or format-owned import stubs.
    for (const RcuLinkReference &reference : graph->References()) {
        const RcuFile &object = objects[reference.objectIndex];
        const RcuReloc &relocation = object.sections[reference.sectionIndex].relocs[reference.relocationIndex];
        const RcuSymbol &symbol = object.symbols[reference.symbolIndex];
        const auto sitePlacement = layout.Relocation(reference);
        const auto siteAddress = sitePlacement ? RcuObjectLayout::Address(*sitePlacement, sectionBases) : std::nullopt;
        if (!sitePlacement) {
            if (!SupportsRelocation(*architecture, relocation.type)) {
                Error("relocation " + std::string(RcuRelTypeName(relocation.type)) + " against '" + symbol.name +
                      "' is not supported by the Mach-O " + std::string(Target::ToDisplayString(targetArch)) +
                      " profile");
            }
            else {
                Error(std::string(RcuRelTypeName(relocation.type)) + " relocation against '" + symbol.name +
                      "' is outside its Mach-O section");
            }
            continue;
        }
        if (!siteAddress) {
            continue;
        }

        Buf *buffer = nullptr;
        switch (sitePlacement->section) {
        case RcuMergedSection::Text:
            buffer = &textBuffer;
            break;
        case RcuMergedSection::RoData:
            buffer = &rodataBuffer;
            break;
        case RcuMergedSection::Data:
            buffer = &dataBuffer;
            break;
        case RcuMergedSection::Bss:
            continue;
        }

        std::optional<uint64_t> targetAddress;
        if (reference.resolution == RcuLinkResolution::External) {
            const auto import = importStubs.find(symbol.name);
            if (import != importStubs.end()) {
                targetAddress = import->second;
            }
        }
        else if (reference.definition) {
            targetAddress = symbolAddress(*reference.definition);
        }
        if (!targetAddress) {
            if (reference.resolution == RcuLinkResolution::External) {
                Error("undefined external symbol '" + symbol.name + "'");
            }
            else if (!symbol.name.empty()) {
                Error("undefined symbol '" + symbol.name + "'");
            }
            continue;
        }

        std::string relocationError;
        if (!ApplyMachORelocation(*architecture, *buffer, static_cast<size_t>(sitePlacement->offset), *siteAddress,
                                  *targetAddress, relocation.addend, relocation.type, symbol.name, relocationError)) {
            auto notes = RelocationNotes(reference, relocationError);
            Error(std::move(relocationError), std::move(notes));
        }
    }
    if (!errors.empty()) {
        return false;
    }

    MachO::LoadCommandLayout loadCommands =
        MachO::ImageLayoutBuilder::BuildLoadCommands(imageLayoutRequest, imageLayout, linkEdit);
    for (const std::string &diagnostic : loadCommands.diagnostics) {
        Error(diagnostic);
    }
    if (loadCommands.HasErrors()) {
        return false;
    }

    Buf image = std::move(loadCommands.header);
    image.insert(image.end(), loadCommands.commands.begin(), loadCommands.commands.end());
    const auto appendAt = [&](const uint64_t offset, const Buf &buffer) {
        image.resize(static_cast<std::size_t>(offset), 0);
        image.insert(image.end(), buffer.begin(), buffer.end());
    };
    appendAt(imageLayout.textOffset, textBuffer);
    appendAt(imageLayout.stubOffset, stubs);
    appendAt(imageLayout.constantDataOffset, rodataBuffer);
    appendAt(imageLayout.importPointerOffset, importPointers);
    appendAt(imageLayout.writableDataOffset, dataBuffer);
    appendAt(imageLayout.linkEditOffset, linkEdit.contents);
    image.resize(static_cast<std::size_t>(linkEdit.codeSignatureOffset), 0);

    // Identify the image by a digest of itself, taken while the UUID field is
    // still zero. Two links of the same input therefore agree, and the value
    // has to be in place before the signature covers it.
    const std::size_t uuidOffset = loadCommands.uuidOffset;
    if (uuidOffset + kMachUuidSize > image.size()) {
        Error("internal: Mach-O UUID load command falls outside the image");
        return false;
    }
    const Crypto::Sha256Digest imageDigest = Crypto::Sha256(image);
    std::copy_n(imageDigest.begin(), kMachUuidSize, image.begin() + static_cast<std::ptrdiff_t>(uuidOffset));
    // Shape the digest into a version-4 variant-1 UUID, the way a linker-
    // generated identifier is expected to read.
    image[uuidOffset + 6] = static_cast<uint8_t>((image[uuidOffset + 6] & 0x0F) | 0x40);
    image[uuidOffset + 8] = static_cast<uint8_t>((image[uuidOffset + 8] & 0x3F) | 0x80);

    Buf codeSignature;
    std::string signatureError;
    if (!MachO::BuildAdHocCodeSignature(image, packageName, 0, imageLayout.textSegmentFileEnd, isShared ? 0 : 1,
                                        codeSignature, signatureError)) {
        Error(std::move(signatureError));
        return false;
    }
    if (codeSignature.size() != linkEdit.codeSignatureSize) {
        Error("internal: Mach-O code-signature layout changed while writing the image");
        return false;
    }
    image.insert(image.end(), codeSignature.begin(), codeSignature.end());

    std::filesystem::create_directories(outputPath.parent_path());
    // Unlink before writing so the image lands on a fresh inode. The Darwin
    // kernel caches code-signature validity per vnode: truncating a signed
    // binary that has already been executed in place can leave the stale
    // verdict attached, and the next exec of the rewritten file is then
    // killed on arm64. Removing the old file first sidesteps the cache.
    std::error_code removeError;
    std::filesystem::remove(outputPath, removeError);
    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        Error("cannot open output file: " + outputPath.string());
        return false;
    }
    output.write(reinterpret_cast<const char *>(image.data()), static_cast<std::streamsize>(image.size()));

    output.close();
    if (!output) {
        Error("cannot write output file: " + outputPath.string());
        return false;
    }

    std::error_code errorCode;
    if (!isShared) {
        std::filesystem::permissions(outputPath,
                                     std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
                                         std::filesystem::perms::others_exec,
                                     std::filesystem::perm_options::add, errorCode);
        if (errorCode) {
            Error("cannot mark output executable: " + errorCode.message());
            return false;
        }
    }

    return true;
}
} // namespace Rux
