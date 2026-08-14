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
constexpr const char *kDyldPath = "/usr/lib/dyld";
constexpr std::size_t kMachUuidSize = 16;
constexpr std::size_t kMachHeaderSize = 32;

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

const MachOArchitectureProfile *ArchitectureProfile(const Target::Arch architecture) {
    if (architecture == kX86_64Profile.architecture) {
        return &kX86_64Profile;
    }
    if (architecture == kAArch64Profile.architecture) {
        return &kAArch64Profile;
    }
    return nullptr;
}

std::optional<uint64_t> AlignUp64(const uint64_t value, const uint64_t alignment) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0 ||
        value > std::numeric_limits<uint64_t>::max() - (alignment - 1)) {
        return std::nullopt;
    }
    return (value + alignment - 1) & ~(alignment - 1);
}

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

uint32_t AlignmentPower(const uint32_t alignment) {
    uint32_t power = 0;
    for (uint32_t value = alignment; value > 1; value >>= 1U) {
        ++power;
    }
    return power;
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

void WriteMachName(Buf &buffer, const char *name) {
    char field[16] = {};
    for (size_t i = 0; i < sizeof(field) && name[i] != '\0'; ++i) {
        field[i] = name[i];
    }
    for (const char byte : field) {
        WriteU8(buffer, static_cast<uint8_t>(byte));
    }
}

uint32_t StringCommandSize(const uint32_t headerSize, const std::string &value) {
    const auto size = AlignUp64(headerSize + value.size() + 1, 8);
    return size && *size <= std::numeric_limits<uint32_t>::max() ? static_cast<uint32_t>(*size) : 0;
}

std::string NormalizeDylibName(const std::string &name) {
    if (name == kSystemLibName) {
        return kDefaultLib;
    }
    return name;
}

void WriteDylinkerCommand(Buf &commands) {
    const std::string path = kDyldPath;
    const uint32_t commandSize = StringCommandSize(12, path);
    WriteU32(commands, 0x0E); // LC_LOAD_DYLINKER
    WriteU32(commands, commandSize);
    WriteU32(commands, 12); // path offset in command
    for (const char byte : path) {
        WriteU8(commands, static_cast<uint8_t>(byte));
    }
    WriteU8(commands, 0);
    while (commands.size() % 8 != 0) {
        WriteU8(commands, 0);
    }
}

void WriteDylibCommand(Buf &commands, const std::string &path) {
    const uint32_t commandSize = StringCommandSize(24, path);
    WriteU32(commands, 0x0C); // LC_LOAD_DYLIB
    WriteU32(commands, commandSize);
    WriteU32(commands, 24);      // path offset in command
    WriteU32(commands, 2);       // timestamp (conventional ld64 value)
    WriteU32(commands, 0x10000); // current version 1.0.0
    WriteU32(commands, 0x10000); // compatibility version 1.0.0
    for (const char byte : path) {
        WriteU8(commands, static_cast<uint8_t>(byte));
    }
    WriteU8(commands, 0);
    while (commands.size() % 8 != 0) {
        WriteU8(commands, 0);
    }
}

void WriteIdDylibCommand(Buf &commands, const std::string &path) {
    const uint32_t commandSize = StringCommandSize(24, path);
    WriteU32(commands, 0x0D); // LC_ID_DYLIB
    WriteU32(commands, commandSize);
    WriteU32(commands, 24);
    WriteU32(commands, 0);       // deterministic timestamp
    WriteU32(commands, 0x10000); // current version 1.0.0
    WriteU32(commands, 0x10000); // compatibility version 1.0.0
    for (const char byte : path) {
        WriteU8(commands, static_cast<uint8_t>(byte));
    }
    WriteU8(commands, 0);
    while (commands.size() % 8 != 0) {
        WriteU8(commands, 0);
    }
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

    // Segment order is [__PAGEZERO], __TEXT, [__DATA_CONST], __DATA, __LINKEDIT.
    // Rebase and bind opcodes address segments by that index.
    const uint8_t textSegmentIndex = isShared ? 0 : 1;
    const uint8_t constSegmentIndex = static_cast<uint8_t>(textSegmentIndex + (writableConstSegment ? 1 : 0));
    const uint8_t dataSegmentIndex = static_cast<uint8_t>(constSegmentIndex + 1);

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

    constexpr uint32_t segmentCommandSize = 72;
    constexpr uint32_t sectionSize = 80;
    const uint32_t threadCommandSize = 16 + architecture->threadStateCount * 4;
    // __const is a __TEXT section unless it needs to be rebased, in which case
    // it becomes the only section of a writable __DATA_CONST segment.
    const uint32_t textSectionCount = (dynamic ? 2 : 1) + (writableConstSegment ? 0 : 1);
    const uint32_t dataSectionCount = dynamic ? 2 : 1;
    const uint32_t textCommandSize = segmentCommandSize + textSectionCount * sectionSize;
    const uint32_t constCommandSize = segmentCommandSize + sectionSize;
    const uint32_t dataCommandSize = segmentCommandSize + dataSectionCount * sectionSize;

    uint32_t commandCount = isShared ? 3 : 4; // [PAGEZERO], TEXT, DATA, LINKEDIT
    uint32_t commandsSize = textCommandSize + dataCommandSize + segmentCommandSize;
    if (!isShared) {
        commandsSize += segmentCommandSize;
    }
    if (writableConstSegment) {
        ++commandCount;
        commandsSize += constCommandSize;
    }
    if (dynamic) {
        commandCount += (isShared ? 4 : 5) + static_cast<uint32_t>(neededLibs.size());
        commandsSize += 48; // LC_DYLD_INFO_ONLY
        commandsSize += 24; // LC_SYMTAB
        commandsSize += 80; // LC_DYSYMTAB
        if (isShared) {
            const std::string installName = "@rpath/" + outputPath.filename().string();
            commandsSize += StringCommandSize(24, installName); // LC_ID_DYLIB
        }
        else {
            commandsSize += StringCommandSize(12, kDyldPath);
            commandsSize += 24; // LC_MAIN
        }
        for (const auto &library : neededLibs) {
            commandsSize += StringCommandSize(24, library);
        }
    }
    else {
        ++commandCount;
        commandsSize += threadCommandSize;
    }
    // dyld refuses an image it links without an LC_UUID to identify it by, so
    // every image carries one.
    ++commandCount;
    commandsSize += 24; // LC_UUID
    if (architecture->emitBuildVersion) {
        ++commandCount;
        commandsSize += 24; // LC_BUILD_VERSION
    }
    ++commandCount;
    commandsSize += 16; // LC_CODE_SIGNATURE

    const auto alignLayout = [&](const uint64_t value, const uint64_t alignment,
                                 const std::string_view description) -> std::optional<uint64_t> {
        const auto aligned = AlignUp64(value, alignment);
        if (!aligned) {
            Error("Mach-O " + std::string(description) + " alignment overflows the image layout");
        }
        return aligned;
    };
    const auto checkedAdd = [&](const uint64_t left, const uint64_t right,
                                const std::string_view description) -> std::optional<uint64_t> {
        if (left > std::numeric_limits<uint64_t>::max() - right) {
            Error("Mach-O " + std::string(description) + " overflows the image layout");
            return std::nullopt;
        }
        return left + right;
    };

    const uint64_t headerSize = 32 + static_cast<uint64_t>(commandsSize);
    const auto textOffsetValue = alignLayout(headerSize, architecture->fileAlignment, "text offset");
    if (!textOffsetValue) {
        return false;
    }
    const uint64_t textOffset = *textOffsetValue;
    const uint64_t textVA = imageBase + textOffset;
    const auto textEnd = checkedAdd(textOffset, textBuffer.size(), "text size");
    const auto stubsOffsetValue =
        textEnd ? alignLayout(*textEnd, architecture->instructionStubAlignment, "instruction-stub offset")
                : std::nullopt;
    if (!stubsOffsetValue) {
        return false;
    }
    const uint64_t stubsOffset = *stubsOffsetValue;
    const uint64_t stubsVA = imageBase + stubsOffset;
    const auto stubsEnd = checkedAdd(stubsOffset, stubs.size(), "instruction-stub size");
    const uint64_t rodataAlignment = writableConstSegment ? architecture->vmPageAlignment : 16;
    const auto rodataOffsetValue =
        stubsEnd ? alignLayout(*stubsEnd, rodataAlignment, "constant-data offset") : std::nullopt;
    if (!rodataOffsetValue) {
        return false;
    }
    const uint64_t rodataOffset = *rodataOffsetValue;
    const uint64_t rodataVA = imageBase + rodataOffset;
    const auto textSegmentFileEndValue =
        writableConstSegment ? stubsEnd : checkedAdd(rodataOffset, rodataBuffer.size(), "text segment size");
    const auto textSegmentVMSizeValue =
        textSegmentFileEndValue ? alignLayout(*textSegmentFileEndValue, architecture->vmPageAlignment, "text segment")
                                : std::nullopt;
    if (!textSegmentFileEndValue || !textSegmentVMSizeValue) {
        return false;
    }
    const uint64_t textSegmentFileEnd = *textSegmentFileEndValue;
    const uint64_t textSegmentVMSize = *textSegmentVMSizeValue;

    // A rebased __const owns the pages between __TEXT and __DATA; otherwise the
    // writable data follows __TEXT directly.
    const uint64_t constSegmentFileSize = writableConstSegment ? rodataBuffer.size() : 0;
    const auto constSegmentVMSizeValue = writableConstSegment
                                           ? alignLayout(std::max<uint64_t>(constSegmentFileSize, 1),
                                                         architecture->vmPageAlignment, "constant segment")
                                           : std::optional<uint64_t>{0};
    if (!constSegmentVMSizeValue) {
        return false;
    }
    const uint64_t constSegmentVMSize = *constSegmentVMSizeValue;
    const auto dataSegmentOffsetValue =
        writableConstSegment ? checkedAdd(rodataOffset, constSegmentVMSize, "data segment")
                             : alignLayout(textSegmentFileEnd, architecture->vmPageAlignment, "data segment");
    if (!dataSegmentOffsetValue) {
        return false;
    }
    const uint64_t dataSegmentOffset = *dataSegmentOffsetValue;
    const uint64_t dataSegmentVA = imageBase + dataSegmentOffset;
    const uint64_t pointersOffset = dataSegmentOffset;
    const uint64_t pointersVA = dataSegmentVA;
    const auto pointersEnd = checkedAdd(pointersOffset, importPointers.size(), "import-pointer size");
    const auto dataOffsetValue = pointersEnd ? alignLayout(*pointersEnd, 8, "writable-data offset") : std::nullopt;
    if (!dataOffsetValue) {
        return false;
    }
    const uint64_t dataOffset = *dataOffsetValue;
    const uint64_t dataVA = imageBase + dataOffset;
    const auto dataEnd = checkedAdd(dataOffset, dataBuffer.size(), "writable-data size");
    if (!dataEnd) {
        return false;
    }
    const uint64_t dataSegmentFileSize = *dataEnd - dataSegmentOffset;
    const auto dataSegmentVMSizeValue =
        alignLayout(std::max<uint64_t>(dataSegmentFileSize, 1), architecture->vmPageAlignment, "data segment");
    if (!dataSegmentVMSizeValue) {
        return false;
    }
    const uint64_t dataSegmentVMSize = *dataSegmentVMSizeValue;

    const auto linkeditOffsetValue = checkedAdd(dataSegmentOffset, dataSegmentVMSize, "link-edit offset");
    if (!linkeditOffsetValue) {
        return false;
    }
    const uint64_t linkeditOffset = *linkeditOffsetValue;
    const uint64_t linkeditVA = imageBase + linkeditOffset;
    const RcuSectionBases sectionBases{
        .text = textVA,
        .rodata = rodataVA,
        .data = dataVA,
    };
    const auto symbolAddress = [&](const RcuSymbolLocation location) -> std::optional<uint64_t> {
        const auto placement = layout.Symbol(location);
        return placement && placement->section != RcuMergedSection::Bss
                 ? RcuObjectLayout::Address(*placement, sectionBases)
                 : std::nullopt;
    };

    const uint64_t constSegmentVA = writableConstSegment ? rodataVA : imageBase;
    const MachO::LinkEditLayout linkEdit = MachO::LinkEditBuilder::Build({
        .graph = *graph,
        .objects = objects,
        .objectLayout = layout,
        .imports = linkEditImports,
        .segments =
            {
                .sectionBases = sectionBases,
                .imageBase = imageBase,
                .constantSegmentAddress = constSegmentVA,
                .dataSegmentAddress = dataSegmentVA,
                .linkEditOffset = linkeditOffset,
                .vmPageAlignment = architecture->vmPageAlignment,
                .textSegmentIndex = textSegmentIndex,
                .constantSegmentIndex = constSegmentIndex,
                .dataSegmentIndex = dataSegmentIndex,
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

    const auto requireU32 = [&](const uint64_t value, const std::string_view description) {
        if (value > std::numeric_limits<uint32_t>::max()) {
            Error("Mach-O " + std::string(description) + " does not fit in its 32-bit load-command field");
            return false;
        }
        return true;
    };
    if (!requireU32(textOffset, "text file offset") || !requireU32(stubsOffset, "instruction-stub file offset") ||
        !requireU32(rodataOffset, "constant-data file offset") ||
        !requireU32(pointersOffset, "import-pointer file offset") ||
        !requireU32(dataOffset, "writable-data file offset") || !requireU32(linkeditOffset, "link-edit file offset")) {
        return false;
    }

    // Patch each stub to its corresponding pointer slot.
    for (size_t i = 0; i < importNames.size(); ++i) {
        const uint64_t stubOffset = i * architecture->instructionStubSize;
        const uint64_t pointerAddress = pointersVA + i * 8;
        if (targetArch == Target::Arch::AArch64) {
            std::string relocationError;
            const uint64_t adrpVA = stubsVA + stubOffset;
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
            const uint64_t stubNextInstruction = stubsVA + stubOffset + architecture->instructionStubSize;
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
        importStubs[importNames[i]] = stubsVA + i * architecture->instructionStubSize;
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
                                        textVA + callMainDisp, "Main", "Mach-O AArch64 entry stub", relocationError)) {
                Error(std::move(relocationError));
                return false;
            }
        }
        else {
            const uint64_t callMainNextInstruction = textVA + callMainDisp + 4;
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
            Error(std::move(relocationError));
        }
    }
    if (!errors.empty()) {
        return false;
    }

    Buf loadCommands;

    if (!isShared) {
        // __PAGEZERO
        WriteU32(loadCommands, 0x19);
        WriteU32(loadCommands, segmentCommandSize);
        WriteMachName(loadCommands, "__PAGEZERO");
        WriteU64(loadCommands, 0);
        WriteU64(loadCommands, kExecutableBase);
        WriteU64(loadCommands, 0);
        WriteU64(loadCommands, 0);
        WriteU32(loadCommands, 0);
        WriteU32(loadCommands, 0);
        WriteU32(loadCommands, 0);
        WriteU32(loadCommands, 0);
    }

    // __TEXT: __text, [__stubs], __const
    WriteU32(loadCommands, 0x19);
    WriteU32(loadCommands, textCommandSize);
    WriteMachName(loadCommands, "__TEXT");
    WriteU64(loadCommands, imageBase);
    WriteU64(loadCommands, textSegmentVMSize);
    WriteU64(loadCommands, 0);
    WriteU64(loadCommands, textSegmentFileEnd);
    WriteU32(loadCommands, 0x05);
    WriteU32(loadCommands, 0x05);
    WriteU32(loadCommands, textSectionCount);
    WriteU32(loadCommands, 0);

    WriteMachName(loadCommands, "__text");
    WriteMachName(loadCommands, "__TEXT");
    WriteU64(loadCommands, textVA);
    WriteU64(loadCommands, textBuffer.size());
    WriteU32(loadCommands, static_cast<uint32_t>(textOffset));
    WriteU32(loadCommands, 4);
    WriteU32(loadCommands, 0);
    WriteU32(loadCommands, 0);
    WriteU32(loadCommands, 0x8000'0400);
    WriteU32(loadCommands, 0);
    WriteU32(loadCommands, 0);
    WriteU32(loadCommands, 0);

    if (dynamic) {
        WriteMachName(loadCommands, "__stubs");
        WriteMachName(loadCommands, "__TEXT");
        WriteU64(loadCommands, stubsVA);
        WriteU64(loadCommands, stubs.size());
        WriteU32(loadCommands, static_cast<uint32_t>(stubsOffset));
        WriteU32(loadCommands, AlignmentPower(architecture->instructionStubAlignment));
        WriteU32(loadCommands, 0);
        WriteU32(loadCommands, 0);
        WriteU32(loadCommands, 0x8000'0408); // instructions | S_SYMBOL_STUBS
        WriteU32(loadCommands, 0);
        WriteU32(loadCommands, architecture->instructionStubSize);
        WriteU32(loadCommands, 0);
    }

    // __DATA_CONST holds constant data that dyld rebases; a fixed-address image
    // keeps it read-only inside __TEXT.
    const char *const constSegmentName = writableConstSegment ? "__DATA_CONST" : "__TEXT";
    if (writableConstSegment) {
        WriteU32(loadCommands, 0x19);
        WriteU32(loadCommands, constCommandSize);
        WriteMachName(loadCommands, constSegmentName);
        WriteU64(loadCommands, constSegmentVA);
        WriteU64(loadCommands, constSegmentVMSize);
        WriteU64(loadCommands, rodataOffset);
        WriteU64(loadCommands, constSegmentFileSize);
        WriteU32(loadCommands, 0x03);
        WriteU32(loadCommands, 0x03);
        WriteU32(loadCommands, 1);
        // SG_READ_ONLY: dyld maps the segment writable, applies its rebases,
        // and re-protects it read-only afterwards. dyld refuses to load a
        // __DATA_CONST segment that does not carry the flag.
        WriteU32(loadCommands, 0x10);
    }

    WriteMachName(loadCommands, "__const");
    WriteMachName(loadCommands, constSegmentName);
    WriteU64(loadCommands, rodataVA);
    WriteU64(loadCommands, rodataBuffer.size());
    WriteU32(loadCommands, static_cast<uint32_t>(rodataOffset));
    WriteU32(loadCommands, 4);
    WriteU32(loadCommands, 0);
    WriteU32(loadCommands, 0);
    WriteU32(loadCommands, 0);
    WriteU32(loadCommands, 0);
    WriteU32(loadCommands, 0);
    WriteU32(loadCommands, 0);

    // __DATA: [__nl_symbol_ptr], __data
    WriteU32(loadCommands, 0x19);
    WriteU32(loadCommands, dataCommandSize);
    WriteMachName(loadCommands, "__DATA");
    WriteU64(loadCommands, dataSegmentVA);
    WriteU64(loadCommands, dataSegmentVMSize);
    WriteU64(loadCommands, dataSegmentOffset);
    WriteU64(loadCommands, dataSegmentFileSize);
    WriteU32(loadCommands, 0x03);
    WriteU32(loadCommands, 0x03);
    WriteU32(loadCommands, dataSectionCount);
    WriteU32(loadCommands, 0);

    if (dynamic) {
        WriteMachName(loadCommands, "__nl_symbol_ptr");
        WriteMachName(loadCommands, "__DATA");
        WriteU64(loadCommands, pointersVA);
        WriteU64(loadCommands, importPointers.size());
        WriteU32(loadCommands, static_cast<uint32_t>(pointersOffset));
        WriteU32(loadCommands, 3);
        WriteU32(loadCommands, 0);
        WriteU32(loadCommands, 0);
        WriteU32(loadCommands, 0x06); // S_NON_LAZY_SYMBOL_POINTERS
        WriteU32(loadCommands, static_cast<uint32_t>(importNames.size()));
        WriteU32(loadCommands, 0);
        WriteU32(loadCommands, 0);
    }

    WriteMachName(loadCommands, "__data");
    WriteMachName(loadCommands, "__DATA");
    WriteU64(loadCommands, dataVA);
    WriteU64(loadCommands, dataBuffer.size());
    WriteU32(loadCommands, static_cast<uint32_t>(dataOffset));
    WriteU32(loadCommands, 0);
    WriteU32(loadCommands, 0);
    WriteU32(loadCommands, 0);
    WriteU32(loadCommands, 0);
    WriteU32(loadCommands, 0);
    WriteU32(loadCommands, 0);
    WriteU32(loadCommands, 0);

    // __LINKEDIT contains dyld metadata followed by the deterministic
    // in-process ad-hoc signature.
    WriteU32(loadCommands, 0x19);
    WriteU32(loadCommands, segmentCommandSize);
    WriteMachName(loadCommands, "__LINKEDIT");
    WriteU64(loadCommands, linkeditVA);
    WriteU64(loadCommands, linkEdit.vmSize);
    WriteU64(loadCommands, linkeditOffset);
    WriteU64(loadCommands, linkEdit.fileSize);
    WriteU32(loadCommands, 0x01);
    WriteU32(loadCommands, 0x01);
    WriteU32(loadCommands, 0);
    WriteU32(loadCommands, 0);

    if (dynamic) {
        WriteU32(loadCommands, 0x8000'0022); // LC_DYLD_INFO_ONLY
        WriteU32(loadCommands, 48);
        WriteU32(loadCommands, linkEdit.rebaseOpcodes.empty() ? 0 : static_cast<uint32_t>(linkeditOffset));
        WriteU32(loadCommands, static_cast<uint32_t>(linkEdit.rebaseOpcodes.size()));
        WriteU32(loadCommands, linkEdit.bindOpcodes.empty() ? 0 : static_cast<uint32_t>(linkEdit.bindOffset));
        WriteU32(loadCommands, static_cast<uint32_t>(linkEdit.bindOpcodes.size()));
        WriteU32(loadCommands, 0); // weak_bind_off
        WriteU32(loadCommands, 0); // weak_bind_size
        WriteU32(loadCommands, 0); // lazy_bind_off
        WriteU32(loadCommands, 0); // lazy_bind_size
        WriteU32(loadCommands, linkEdit.exportTrie.empty() ? 0 : static_cast<uint32_t>(linkEdit.exportTrieOffset));
        WriteU32(loadCommands, static_cast<uint32_t>(linkEdit.exportTrie.size()));

        WriteU32(loadCommands, 0x02); // LC_SYMTAB
        WriteU32(loadCommands, 24);
        WriteU32(loadCommands, static_cast<uint32_t>(linkEdit.symbolTableOffset));
        WriteU32(loadCommands, linkEdit.symbolCount);
        WriteU32(loadCommands, static_cast<uint32_t>(linkEdit.stringTableOffset));
        WriteU32(loadCommands, linkEdit.stringTableSize);

        WriteU32(loadCommands, 0x0B); // LC_DYSYMTAB
        WriteU32(loadCommands, 80);
        WriteU32(loadCommands, 0);                    // ilocalsym
        WriteU32(loadCommands, 0);                    // nlocalsym
        WriteU32(loadCommands, 0);                    // iextdefsym
        WriteU32(loadCommands, linkEdit.exportCount); // nextdefsym
        WriteU32(loadCommands, linkEdit.exportCount); // iundefsym
        WriteU32(loadCommands, linkEdit.importCount);
        WriteU32(loadCommands, 0); // tocoff
        WriteU32(loadCommands, 0); // ntoc
        WriteU32(loadCommands, 0); // modtaboff
        WriteU32(loadCommands, 0); // nmodtab
        WriteU32(loadCommands, 0); // extrefsymoff
        WriteU32(loadCommands, 0); // nextrefsyms
        WriteU32(loadCommands, static_cast<uint32_t>(linkEdit.indirectSymbolsOffset));
        WriteU32(loadCommands, linkEdit.importCount * 2);
        WriteU32(loadCommands, 0); // extreloff
        WriteU32(loadCommands, 0); // nextrel
        WriteU32(loadCommands, 0); // locreloff
        WriteU32(loadCommands, 0); // nlocrel

        if (isShared) {
            WriteIdDylibCommand(loadCommands, "@rpath/" + outputPath.filename().string());
        }
        else {
            WriteDylinkerCommand(loadCommands);

            if (architecture->dynamicEntryStrategy != MachOEntryStrategy::Main) {
                Error("internal: Mach-O dynamic entry strategy is not implemented for " +
                      std::string(Target::ToDisplayString(architecture->architecture)));
                return false;
            }
            WriteU32(loadCommands, 0x8000'0028); // LC_MAIN
            WriteU32(loadCommands, 24);
            WriteU64(loadCommands, textOffset); // entryoff from start of __TEXT/file
            WriteU64(loadCommands, 0);          // stacksize
        }

        for (const auto &library : neededLibs) {
            WriteDylibCommand(loadCommands, library);
        }
    }
    else {
        if (architecture->staticEntryStrategy != MachOEntryStrategy::UnixThread) {
            Error("internal: Mach-O static entry strategy is not implemented for " +
                  std::string(Target::ToDisplayString(architecture->architecture)));
            return false;
        }
        WriteU32(loadCommands, 0x05); // LC_UNIXTHREAD
        WriteU32(loadCommands, threadCommandSize);
        WriteU32(loadCommands, architecture->threadStateFlavor);
        WriteU32(loadCommands, architecture->threadStateCount);
        for (uint32_t reg = 0; reg < architecture->threadStateCount / 2; ++reg) {
            WriteU64(loadCommands, reg == architecture->threadProgramCounterIndex ? textVA : 0);
        }
    }

    // The identifier itself is a digest of the finished image, so it is filled
    // in once the rest of the file exists. Remember where its bytes land.
    WriteU32(loadCommands, 0x1B); // LC_UUID
    WriteU32(loadCommands, 24);
    const std::size_t uuidCommandPayload = loadCommands.size();
    loadCommands.insert(loadCommands.end(), kMachUuidSize, 0);

    if (architecture->emitBuildVersion) {
        WriteU32(loadCommands, 0x32); // LC_BUILD_VERSION
        WriteU32(loadCommands, 24);
        WriteU32(loadCommands, 1);           // PLATFORM_MACOS
        WriteU32(loadCommands, 0x001A'0000); // macOS 26.0 deployment target
        WriteU32(loadCommands, 0x001A'0000); // macOS 26.0 SDK baseline
        WriteU32(loadCommands, 0);           // no build-tool records
    }

    WriteU32(loadCommands, 0x1D); // LC_CODE_SIGNATURE
    WriteU32(loadCommands, 16);
    WriteU32(loadCommands, static_cast<uint32_t>(linkEdit.codeSignatureOffset));
    WriteU32(loadCommands, static_cast<uint32_t>(linkEdit.codeSignatureSize));

    if (loadCommands.size() != commandsSize) {
        Error("internal: Mach-O load-command size mismatch");
        return false;
    }

    Buf header;
    WriteU32(header, 0xFEED'FACF); // MH_MAGIC_64
    WriteU32(header, architecture->cpuType);
    WriteU32(header, architecture->cpuSubtype);
    WriteU32(header, isShared ? 6 : 2); // MH_DYLIB or MH_EXECUTE
    WriteU32(header, commandCount);
    WriteU32(header, commandsSize);
    // MH_NOUNDEFS | [MH_DYLDLINK] | [MH_PIE]
    WriteU32(header, (dynamic ? 0x0000'0005U : 0x0000'0001U) | (positionIndependent ? 0x0020'0000U : 0U));
    WriteU32(header, 0);

    Buf image = std::move(header);
    image.insert(image.end(), loadCommands.begin(), loadCommands.end());
    const auto appendAt = [&](const uint64_t offset, const Buf &buffer) {
        image.resize(static_cast<std::size_t>(offset), 0);
        image.insert(image.end(), buffer.begin(), buffer.end());
    };
    appendAt(textOffset, textBuffer);
    appendAt(stubsOffset, stubs);
    appendAt(rodataOffset, rodataBuffer);
    appendAt(pointersOffset, importPointers);
    appendAt(dataOffset, dataBuffer);
    appendAt(linkeditOffset, linkEdit.contents);
    image.resize(static_cast<std::size_t>(linkEdit.codeSignatureOffset), 0);

    // Identify the image by a digest of itself, taken while the UUID field is
    // still zero. Two links of the same input therefore agree, and the value
    // has to be in place before the signature covers it.
    const std::size_t uuidOffset = kMachHeaderSize + uuidCommandPayload;
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
    if (!MachO::BuildAdHocCodeSignature(image, packageName, 0, textSegmentFileEnd, isShared ? 0 : 1, codeSignature,
                                        signatureError)) {
        Error(std::move(signatureError));
        return false;
    }
    if (codeSignature.size() != linkEdit.codeSignatureSize) {
        Error("internal: Mach-O code-signature layout changed while writing the image");
        return false;
    }
    image.insert(image.end(), codeSignature.begin(), codeSignature.end());

    std::filesystem::create_directories(outputPath.parent_path());
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
