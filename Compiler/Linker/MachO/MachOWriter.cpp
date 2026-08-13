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

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

void WriteUleb128(Buf &buffer, uint64_t value) {
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7F);
        value >>= 7;
        if (value != 0) {
            byte |= 0x80;
        }
        WriteU8(buffer, byte);
    }
    while (value != 0);
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

Buf BuildExportTrie(const std::vector<std::pair<std::string, uint64_t>> &exports) {
    if (exports.empty()) {
        return {};
    }

    struct TrieNode {
        std::map<uint8_t, size_t> children;
        bool terminal = false;
        uint64_t address = 0;
    };

    std::vector<TrieNode> nodes(1);
    for (const auto &[name, address] : exports) {
        size_t nodeIndex = 0;
        const std::string nativeName = "_" + name;
        for (const unsigned char character : nativeName) {
            const auto child = nodes[nodeIndex].children.find(character);
            if (child != nodes[nodeIndex].children.end()) {
                nodeIndex = child->second;
                continue;
            }
            const size_t childIndex = nodes.size();
            nodes.push_back({});
            nodes[nodeIndex].children.emplace(character, childIndex);
            nodeIndex = childIndex;
        }
        nodes[nodeIndex].terminal = true;
        nodes[nodeIndex].address = address;
    }

    std::vector<size_t> offsets(nodes.size());
    std::vector<Buf> encoded(nodes.size());
    for (;;) {
        for (size_t i = 0; i < nodes.size(); ++i) {
            encoded[i].clear();
            if (nodes[i].terminal) {
                Buf terminal;
                WriteUleb128(terminal, 0);                // flags
                WriteUleb128(terminal, nodes[i].address); // image-relative address
                WriteUleb128(encoded[i], terminal.size());
                encoded[i].insert(encoded[i].end(), terminal.begin(), terminal.end());
            }
            else {
                WriteU8(encoded[i], 0);
            }
            WriteU8(encoded[i], static_cast<uint8_t>(nodes[i].children.size()));
            for (const auto &[character, childIndex] : nodes[i].children) {
                WriteU8(encoded[i], character);
                WriteU8(encoded[i], 0);
                WriteUleb128(encoded[i], offsets[childIndex]);
            }
        }

        std::vector<size_t> nextOffsets(nodes.size());
        size_t cursor = 0;
        for (size_t i = 0; i < encoded.size(); ++i) {
            nextOffsets[i] = cursor;
            cursor += encoded[i].size();
        }
        if (nextOffsets == offsets) {
            break;
        }
        offsets = std::move(nextOffsets);
    }

    Buf trie;
    for (const auto &node : encoded) {
        trie.insert(trie.end(), node.begin(), node.end());
    }
    return trie;
}
} // namespace

bool Linker::LinkMachO64(const std::filesystem::path &outputPath) {
    const MachOArchitectureProfile *architecture = ArchitectureProfile(targetArch);
    if (architecture == nullptr) {
        Error("internal: no Mach-O architecture profile for " + std::string(Target::ToDisplayString(targetArch)));
        return false;
    }
    const bool isShared = artifactKind == ArtifactKind::SharedLibrary;
    const uint64_t imageBase = isShared ? 0 : kExecutableBase;
    // Collect definitions first so cross-module references are resolved as
    // ordinary Rux symbols instead of dynamic imports.
    std::unordered_set<std::string> definedSymbols;
    std::unordered_set<std::string> externalDefinitions;
    for (const auto &object : objects) {
        for (const auto &symbol : object.symbols) {
            if (symbol.kind == RcuSymKind::ExternFunc || symbol.kind == RcuSymKind::ExternData ||
                symbol.sectionIdx == RCU_SEC_EXTERNAL || symbol.name.empty()) {
                continue;
            }
            // Private Rux functions are local in their defining RCU object but
            // are still referenced as externs from other source-file objects.
            // Generated local data and constant labels remain object-relative.
            if (symbol.visibility != RcuSymVis::Local || symbol.kind == RcuSymKind::Func) {
                definedSymbols.insert(symbol.name);
            }
            if (symbol.visibility != RcuSymVis::Local && !externalDefinitions.insert(symbol.name).second) {
                Error("duplicate definition of symbol '" + symbol.name + "'");
            }
        }
    }
    if (!errors.empty()) {
        return false;
    }

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

    // Only referenced externs become imports. Function addresses resolve to
    // generated stubs. Imported data still needs GOT-aware lowering and is
    // rejected explicitly rather than producing an invalid direct reference.
    std::unordered_map<std::string, std::string> importLib;
    for (const auto &object : objects) {
        for (const auto &section : object.sections) {
            for (const auto &relocation : section.relocs) {
                if (relocation.symbolIndex >= object.symbols.size()) {
                    continue;
                }
                const auto &symbol = object.symbols[relocation.symbolIndex];
                if (definedSymbols.contains(symbol.name)) {
                    continue;
                }
                if (symbol.kind == RcuSymKind::ExternFunc) {
                    const auto explicitIt = explicitImportLib.find(symbol.name);
                    const std::string library =
                        explicitIt != explicitImportLib.end()
                            ? explicitIt->second
                            : NormalizeDylibName(symbol.typeName.empty() ? kDefaultLib : symbol.typeName);
                    const auto [it, inserted] = importLib.try_emplace(symbol.name, library);
                    if (!inserted && it->second != library) {
                        Error("external symbol '" + symbol.name + "' is referenced from both '" + it->second +
                              "' and '" + library + "'");
                    }
                }
                else if (symbol.kind == RcuSymKind::ExternData) {
                    Error("external data symbol '" + symbol.name + "' cannot be imported by the Mach-O " +
                          std::string(Target::ToDisplayString(targetArch)) +
                          " linker because GOT-aware lowering is not implemented");
                }
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

    struct SharedExportSource {
        std::string name;
        size_t objectIndex;
        RcuSymbol symbol;
    };

    std::vector<SharedExportSource> sharedExportSources;
    if (isShared) {
        std::map<std::string, SharedExportSource> exportsByName;
        for (size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex) {
            for (const auto &symbol : objects[objectIndex].symbols) {
                if (symbol.visibility == RcuSymVis::Local || symbol.name.empty() ||
                    symbol.sectionIdx == RCU_SEC_EXTERNAL) {
                    continue;
                }
                exportsByName.try_emplace(symbol.name, SharedExportSource{symbol.name, objectIndex, symbol});
            }
        }
        for (auto &[name, source] : exportsByName) {
            (void)name;
            sharedExportSources.push_back(std::move(source));
        }
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
    const uint64_t prefixSize = textPrefix.size();

    struct ObjectLayout {
        uint64_t textOffset;
        uint64_t rodataOffset;
        uint64_t dataOffset;
    };

    std::vector<ObjectLayout> layouts(objects.size());
    Buf mergedText;
    Buf mergedRodata;
    Buf mergedData;
    for (size_t i = 0; i < objects.size(); ++i) {
        const auto &object = objects[i];

        // RCU symbol values are relative to section starts whose alignment is
        // part of the object contract. Preserve that alignment when input
        // sections are concatenated; otherwise an odd-sized section from one
        // object can make a scaled AArch64 load from the next object
        // impossible to encode.
        uint16_t textAlignment = 1;
        uint16_t rodataAlignment = 1;
        uint16_t dataAlignment = 1;
        for (const auto &section : object.sections) {
            if (section.type == RcuSecType::Text) {
                textAlignment = std::max(textAlignment, section.alignment);
            }
            else if (section.type == RcuSecType::RoData) {
                rodataAlignment = std::max(rodataAlignment, section.alignment);
            }
            else if (section.type == RcuSecType::Data) {
                dataAlignment = std::max(dataAlignment, section.alignment);
            }
        }
        const auto padToAlignment = [](Buf &buffer, const uint16_t alignment, const uint64_t prefixSize = 0) {
            while ((prefixSize + buffer.size()) % alignment != 0) {
                buffer.push_back(0);
            }
        };
        padToAlignment(mergedText, textAlignment, prefixSize);
        padToAlignment(mergedRodata, rodataAlignment);
        padToAlignment(mergedData, dataAlignment);
        layouts[i] = {mergedText.size(), mergedRodata.size(), mergedData.size()};
        for (const auto &section : object.sections) {
            if (section.type == RcuSecType::Text) {
                mergedText.insert(mergedText.end(), section.data.begin(), section.data.end());
            }
            else if (section.type == RcuSecType::RoData) {
                mergedRodata.insert(mergedRodata.end(), section.data.begin(), section.data.end());
            }
            else if (section.type == RcuSecType::Data) {
                mergedData.insert(mergedData.end(), section.data.begin(), section.data.end());
            }
        }
    }

    Buf textBuffer = textPrefix;
    textBuffer.insert(textBuffer.end(), mergedText.begin(), mergedText.end());

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

    // The eager bind stream points each slot in __DATA,__nl_symbol_ptr at its
    // underscored Mach-O C symbol in the requested LC_LOAD_DYLIB ordinal.
    Buf bindStream;
    if (dynamic && !importNames.empty()) {
        WriteU8(bindStream, 0x51); // SET_TYPE_IMM | POINTER
        WriteU8(bindStream,
                static_cast<uint8_t>(0x70 | dataSegmentIndex)); // SET_SEGMENT_AND_OFFSET_ULEB | __DATA index
        WriteUleb128(bindStream, 0);
        for (const auto &name : importNames) {
            const uint8_t ordinal = libraryOrdinal.at(importLib.at(name));
            if (ordinal <= 15) {
                WriteU8(bindStream, static_cast<uint8_t>(0x10 | ordinal)); // SET_DYLIB_ORDINAL_IMM
            }
            else {
                WriteU8(bindStream, 0x20); // SET_DYLIB_ORDINAL_ULEB
                WriteUleb128(bindStream, ordinal);
            }
            WriteU8(bindStream, 0x40); // SET_SYMBOL_TRAILING_FLAGS_IMM
            WriteU8(bindStream, '_');
            for (const char byte : name) {
                WriteU8(bindStream, static_cast<uint8_t>(byte));
            }
            WriteU8(bindStream, 0);
            WriteU8(bindStream, 0x90); // DO_BIND (also advances by pointer size)
        }
        WriteU8(bindStream, 0x00); // DONE
    }

    // Publish conventional undefined nlist entries and indirect-symbol table
    // indexes as well as bind opcodes. dyld performs the binding from the
    // opcodes; these tables make __stubs/__nl_symbol_ptr complete Mach-O
    // sections and keep inspection/debugging tools aware of the imports.
    Buf dynamicSymbols;
    Buf indirectSymbols;
    Buf stringTable = {0};
    if (dynamic) {
        for (size_t section = 0; section < 2; ++section) {
            for (size_t i = 0; i < importNames.size(); ++i) {
                WriteU32(indirectSymbols, static_cast<uint32_t>(sharedExportSources.size() + i));
            }
        }
    }

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
        writableConstSegment ? stubsEnd : checkedAdd(rodataOffset, mergedRodata.size(), "text segment size");
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
    const uint64_t constSegmentFileSize = writableConstSegment ? mergedRodata.size() : 0;
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
    const auto dataEnd = checkedAdd(dataOffset, mergedData.size(), "writable-data size");
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

    // Every slid image has to tell dyld which absolute pointers to adjust.
    const uint64_t constSegmentVA = writableConstSegment ? rodataVA : imageBase;
    Buf rebaseStream;
    if (slidImage) {
        WriteU8(rebaseStream, 0x11); // SET_TYPE_IMM | REBASE_TYPE_POINTER
        for (size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex) {
            const auto &object = objects[objectIndex];
            const auto &layout = layouts[objectIndex];
            for (const auto &section : object.sections) {
                uint8_t segmentIndex = 0;
                uint64_t segmentOffset = 0;
                if (section.type == RcuSecType::Text) {
                    segmentIndex = textSegmentIndex;
                    segmentOffset = textVA + prefixSize + layout.textOffset - imageBase;
                }
                else if (section.type == RcuSecType::RoData) {
                    segmentIndex = constSegmentIndex;
                    segmentOffset = rodataVA + layout.rodataOffset - constSegmentVA;
                }
                else if (section.type == RcuSecType::Data) {
                    segmentIndex = dataSegmentIndex;
                    segmentOffset = dataVA + layout.dataOffset - dataSegmentVA;
                }
                else {
                    continue;
                }
                for (const auto &relocation : section.relocs) {
                    if (relocation.type != RcuRelType::Abs64) {
                        continue;
                    }
                    if (writableConstSegment && section.type == RcuSecType::Text) {
                        Error("Mach-O code cannot hold an absolute address in a position-independent image; "
                              "'" +
                              object.symbols[relocation.symbolIndex].name + "' must be reached PC-relatively");
                        continue;
                    }
                    WriteU8(rebaseStream, static_cast<uint8_t>(0x20 | segmentIndex));
                    WriteUleb128(rebaseStream, segmentOffset + relocation.sectionOffset);
                    WriteU8(rebaseStream, 0x51); // DO_REBASE_IMM_TIMES | 1
                }
            }
        }
        WriteU8(rebaseStream, 0); // DONE
    }

    std::vector<std::pair<std::string, uint64_t>> sharedExports;
    for (const auto &source : sharedExportSources) {
        const auto &layout = layouts[source.objectIndex];
        uint64_t address = 0;
        uint8_t sectionOrdinal = 0;
        if (source.symbol.sectionIdx == RCU_TEXT_IDX) {
            address = textVA + prefixSize + layout.textOffset + source.symbol.value;
            sectionOrdinal = 1;
        }
        else if (source.symbol.sectionIdx == RCU_RODATA_IDX) {
            address = rodataVA + layout.rodataOffset + source.symbol.value;
            sectionOrdinal = 3;
        }
        else if (source.symbol.sectionIdx == RCU_DATA_IDX) {
            address = dataVA + layout.dataOffset + source.symbol.value;
            sectionOrdinal = 5;
        }
        else {
            continue;
        }

        const uint32_t stringIndex = static_cast<uint32_t>(stringTable.size());
        WriteU8(stringTable, '_');
        for (const char byte : source.name) {
            WriteU8(stringTable, static_cast<uint8_t>(byte));
        }
        WriteU8(stringTable, 0);
        WriteU32(dynamicSymbols, stringIndex);
        WriteU8(dynamicSymbols, 0x0F); // N_SECT | N_EXT
        WriteU8(dynamicSymbols, sectionOrdinal);
        WriteU16(dynamicSymbols, 0);
        WriteU64(dynamicSymbols, address);
        sharedExports.emplace_back(source.name, address);
    }
    for (const auto &name : importNames) {
        const uint32_t stringIndex = static_cast<uint32_t>(stringTable.size());
        WriteU8(stringTable, '_');
        for (const char byte : name) {
            WriteU8(stringTable, static_cast<uint8_t>(byte));
        }
        WriteU8(stringTable, 0);
        WriteU32(dynamicSymbols, stringIndex);
        WriteU8(dynamicSymbols, 0x01); // N_UNDF | N_EXT
        WriteU8(dynamicSymbols, 0);
        WriteU16(dynamicSymbols, static_cast<uint16_t>(libraryOrdinal.at(importLib.at(name))) << 8);
        WriteU64(dynamicSymbols, 0);
    }

    const Buf exportTrie = BuildExportTrie(sharedExports);
    Buf linkeditBuffer = rebaseStream;
    const uint64_t bindOffset = linkeditOffset + linkeditBuffer.size();
    linkeditBuffer.insert(linkeditBuffer.end(), bindStream.begin(), bindStream.end());
    const uint64_t exportTrieOffset = linkeditOffset + linkeditBuffer.size();
    linkeditBuffer.insert(linkeditBuffer.end(), exportTrie.begin(), exportTrie.end());
    while ((linkeditOffset + linkeditBuffer.size()) % 8 != 0) {
        WriteU8(linkeditBuffer, 0);
    }
    const uint64_t symbolTableOffset = linkeditOffset + linkeditBuffer.size();
    linkeditBuffer.insert(linkeditBuffer.end(), dynamicSymbols.begin(), dynamicSymbols.end());
    while ((linkeditOffset + linkeditBuffer.size()) % 4 != 0) {
        WriteU8(linkeditBuffer, 0);
    }
    const uint64_t indirectSymbolsOffset = linkeditOffset + linkeditBuffer.size();
    linkeditBuffer.insert(linkeditBuffer.end(), indirectSymbols.begin(), indirectSymbols.end());
    const uint64_t stringTableOffset = linkeditOffset + linkeditBuffer.size();
    linkeditBuffer.insert(linkeditBuffer.end(), stringTable.begin(), stringTable.end());
    const auto codeSignatureOffsetValue =
        alignLayout(linkeditOffset + linkeditBuffer.size(), 16, "code-signature offset");
    if (!codeSignatureOffsetValue) {
        return false;
    }
    const uint64_t codeSignatureOffset = *codeSignatureOffsetValue;
    std::string signatureError;
    const uint64_t codeSignatureSize = MachO::AdHocCodeSignatureSize(codeSignatureOffset, packageName, signatureError);
    if (codeSignatureSize == 0) {
        Error(std::move(signatureError));
        return false;
    }
    const auto signedFileEnd = checkedAdd(codeSignatureOffset, codeSignatureSize, "code-signature size");
    if (!signedFileEnd) {
        return false;
    }
    const uint64_t linkeditFileSize = *signedFileEnd - linkeditOffset;
    const auto linkeditVMSizeValue =
        alignLayout(std::max<uint64_t>(linkeditFileSize, 1), architecture->vmPageAlignment, "link-edit segment");
    if (!linkeditVMSizeValue) {
        return false;
    }
    const uint64_t linkeditVMSize = *linkeditVMSizeValue;

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
        !requireU32(dataOffset, "writable-data file offset") || !requireU32(linkeditOffset, "link-edit file offset") ||
        !requireU32(bindOffset, "bind metadata offset") || !requireU32(exportTrieOffset, "export metadata offset") ||
        !requireU32(symbolTableOffset, "symbol-table offset") ||
        !requireU32(indirectSymbolsOffset, "indirect-symbol-table offset") ||
        !requireU32(stringTableOffset, "string-table offset") || !requireU32(bindStream.size(), "bind metadata size") ||
        !requireU32(exportTrie.size(), "export metadata size") ||
        !requireU32(dynamicSymbols.size() / 16, "symbol count") ||
        !requireU32(stringTable.size(), "string-table size") ||
        !requireU32(codeSignatureOffset, "code-signature offset") ||
        !requireU32(codeSignatureSize, "code-signature size")) {
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

    std::unordered_map<std::string, uint64_t> symbolMap;
    for (size_t i = 0; i < importNames.size(); ++i) {
        symbolMap[importNames[i]] = stubsVA + i * architecture->instructionStubSize;
    }
    for (size_t i = 0; i < objects.size(); ++i) {
        const auto &object = objects[i];
        const auto &layout = layouts[i];
        for (const auto &symbol : object.symbols) {
            if (symbol.name.empty() || symbol.kind == RcuSymKind::ExternFunc || symbol.kind == RcuSymKind::ExternData) {
                continue;
            }
            if (symbol.visibility == RcuSymVis::Local && symbol.kind != RcuSymKind::Func && symbol.name != "Main") {
                continue;
            }

            uint64_t address = 0;
            if (symbol.sectionIdx == RCU_TEXT_IDX) {
                address = textVA + prefixSize + layout.textOffset + symbol.value;
            }
            else if (symbol.sectionIdx == RCU_RODATA_IDX) {
                address = rodataVA + layout.rodataOffset + symbol.value;
            }
            else if (symbol.sectionIdx == RCU_DATA_IDX) {
                address = dataVA + layout.dataOffset + symbol.value;
            }
            else {
                continue;
            }
            symbolMap.try_emplace(symbol.name, address);
        }
    }

    if (!isShared) {
        const auto mainIt = symbolMap.find("Main");
        if (mainIt == symbolMap.end()) {
            Error("undefined symbol 'Main' — no entry point found");
            return false;
        }
        if (targetArch == Target::Arch::AArch64) {
            std::string relocationError;
            if (!ApplyAArch64Relocation(textBuffer, callMainDisp, RcuRelType::AArch64Call26, mainIt->second, 0,
                                        textVA + callMainDisp, "Main", "Mach-O AArch64 entry stub", relocationError)) {
                Error(std::move(relocationError));
                return false;
            }
        }
        else {
            const uint64_t callMainNextInstruction = textVA + callMainDisp + 4;
            const uint64_t magnitude = mainIt->second >= callMainNextInstruction
                                         ? mainIt->second - callMainNextInstruction
                                         : callMainNextInstruction - mainIt->second;
            if ((mainIt->second >= callMainNextInstruction &&
                 magnitude > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) ||
                (mainIt->second < callMainNextInstruction && magnitude > uint64_t{1} << 31U)) {
                Error("Mach-O entry stub cannot reach 'Main'");
                return false;
            }
            const int64_t displacement = mainIt->second >= callMainNextInstruction ? static_cast<int64_t>(magnitude)
                                                                                   : -static_cast<int64_t>(magnitude);
            Patch32(textBuffer, callMainDisp, static_cast<uint32_t>(static_cast<int32_t>(displacement)));
        }
    }

    // Resolve object relocations against defined symbols or import stubs.
    for (size_t i = 0; i < objects.size(); ++i) {
        const auto &object = objects[i];
        const auto &layout = layouts[i];
        for (const auto &section : object.sections) {
            Buf *buffer = nullptr;
            uint32_t baseInBuffer = 0;
            uint64_t sectionBaseVA = 0;
            if (section.type == RcuSecType::Text) {
                buffer = &textBuffer;
                baseInBuffer = prefixSize + layout.textOffset;
                sectionBaseVA = textVA + prefixSize + layout.textOffset;
            }
            else if (section.type == RcuSecType::RoData) {
                buffer = &mergedRodata;
                baseInBuffer = layout.rodataOffset;
                sectionBaseVA = rodataVA + layout.rodataOffset;
            }
            else if (section.type == RcuSecType::Data) {
                buffer = &mergedData;
                baseInBuffer = layout.dataOffset;
                sectionBaseVA = dataVA + layout.dataOffset;
            }
            else {
                continue;
            }

            for (const auto &relocation : section.relocs) {
                if (relocation.symbolIndex >= object.symbols.size()) {
                    Error("relocation in Mach-O section '" + section.name + "' refers to missing symbol index " +
                          std::to_string(relocation.symbolIndex));
                    continue;
                }
                const auto &symbol = object.symbols[relocation.symbolIndex];
                uint64_t targetVA = 0;
                if (symbol.kind == RcuSymKind::ExternFunc || symbol.kind == RcuSymKind::ExternData) {
                    const auto it = symbolMap.find(symbol.name);
                    if (it == symbolMap.end()) {
                        Error("undefined external symbol '" + symbol.name + "'");
                        continue;
                    }
                    targetVA = it->second;
                }
                else if (symbol.visibility != RcuSymVis::Local && !symbol.name.empty() &&
                         symbolMap.contains(symbol.name)) {
                    targetVA = symbolMap.at(symbol.name);
                }
                else if (symbol.sectionIdx == RCU_TEXT_IDX) {
                    targetVA = textVA + prefixSize + layout.textOffset + symbol.value;
                }
                else if (symbol.sectionIdx == RCU_RODATA_IDX) {
                    targetVA = rodataVA + layout.rodataOffset + symbol.value;
                }
                else if (symbol.sectionIdx == RCU_DATA_IDX) {
                    targetVA = dataVA + layout.dataOffset + symbol.value;
                }
                else {
                    if (!symbol.name.empty()) {
                        Error("undefined symbol '" + symbol.name + "'");
                    }
                    continue;
                }

                const size_t patchOffset = baseInBuffer + relocation.sectionOffset;
                const uint64_t relocationVA = sectionBaseVA + relocation.sectionOffset;
                if (relocation.type == RcuRelType::None) {
                    continue;
                }
                std::string relocationError;
                if (!ApplyMachORelocation(*architecture, *buffer, patchOffset, relocationVA, targetVA,
                                          relocation.addend, relocation.type, symbol.name, relocationError)) {
                    Error(std::move(relocationError));
                }
            }
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
    WriteU64(loadCommands, mergedRodata.size());
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
    WriteU64(loadCommands, mergedData.size());
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
    WriteU64(loadCommands, linkeditVMSize);
    WriteU64(loadCommands, linkeditOffset);
    WriteU64(loadCommands, linkeditFileSize);
    WriteU32(loadCommands, 0x01);
    WriteU32(loadCommands, 0x01);
    WriteU32(loadCommands, 0);
    WriteU32(loadCommands, 0);

    if (dynamic) {
        WriteU32(loadCommands, 0x8000'0022); // LC_DYLD_INFO_ONLY
        WriteU32(loadCommands, 48);
        WriteU32(loadCommands, rebaseStream.empty() ? 0 : static_cast<uint32_t>(linkeditOffset));
        WriteU32(loadCommands, static_cast<uint32_t>(rebaseStream.size()));
        WriteU32(loadCommands, bindStream.empty() ? 0 : static_cast<uint32_t>(bindOffset));
        WriteU32(loadCommands, static_cast<uint32_t>(bindStream.size()));
        WriteU32(loadCommands, 0); // weak_bind_off
        WriteU32(loadCommands, 0); // weak_bind_size
        WriteU32(loadCommands, 0); // lazy_bind_off
        WriteU32(loadCommands, 0); // lazy_bind_size
        WriteU32(loadCommands, exportTrie.empty() ? 0 : static_cast<uint32_t>(exportTrieOffset));
        WriteU32(loadCommands, static_cast<uint32_t>(exportTrie.size()));

        WriteU32(loadCommands, 0x02); // LC_SYMTAB
        WriteU32(loadCommands, 24);
        WriteU32(loadCommands, static_cast<uint32_t>(symbolTableOffset));
        WriteU32(loadCommands, static_cast<uint32_t>(sharedExports.size() + importNames.size()));
        WriteU32(loadCommands, static_cast<uint32_t>(stringTableOffset));
        WriteU32(loadCommands, static_cast<uint32_t>(stringTable.size()));

        WriteU32(loadCommands, 0x0B); // LC_DYSYMTAB
        WriteU32(loadCommands, 80);
        WriteU32(loadCommands, 0);                                           // ilocalsym
        WriteU32(loadCommands, 0);                                           // nlocalsym
        WriteU32(loadCommands, 0);                                           // iextdefsym
        WriteU32(loadCommands, static_cast<uint32_t>(sharedExports.size())); // nextdefsym
        WriteU32(loadCommands, static_cast<uint32_t>(sharedExports.size())); // iundefsym
        WriteU32(loadCommands, static_cast<uint32_t>(importNames.size()));
        WriteU32(loadCommands, 0); // tocoff
        WriteU32(loadCommands, 0); // ntoc
        WriteU32(loadCommands, 0); // modtaboff
        WriteU32(loadCommands, 0); // nmodtab
        WriteU32(loadCommands, 0); // extrefsymoff
        WriteU32(loadCommands, 0); // nextrefsyms
        WriteU32(loadCommands, static_cast<uint32_t>(indirectSymbolsOffset));
        WriteU32(loadCommands, static_cast<uint32_t>(importNames.size() * 2));
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
    WriteU32(loadCommands, static_cast<uint32_t>(codeSignatureOffset));
    WriteU32(loadCommands, static_cast<uint32_t>(codeSignatureSize));

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
    appendAt(rodataOffset, mergedRodata);
    appendAt(pointersOffset, importPointers);
    appendAt(dataOffset, mergedData);
    appendAt(linkeditOffset, linkeditBuffer);
    image.resize(static_cast<std::size_t>(codeSignatureOffset), 0);

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
    if (!MachO::BuildAdHocCodeSignature(image, packageName, 0, textSegmentFileEnd, isShared ? 0 : 1, codeSignature,
                                        signatureError)) {
        Error(std::move(signatureError));
        return false;
    }
    if (codeSignature.size() != codeSignatureSize) {
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
