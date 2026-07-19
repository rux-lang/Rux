// Mach-O executable writer for macOS x86-64. Freestanding programs retain a
// static LC_UNIXTHREAD entry point; programs that reference #Link externs use
// dyld, eager symbol binding, and standard symbol stubs.

#include "Linker/Linker.h"
#include "Linker/LinkerInternal.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Rux {
namespace {
constexpr uint64_t kBase = 0x1'0000'0000ULL;
constexpr uint64_t kPage = 0x1000;
constexpr const char *kSystemLibName = "libSystem.B.dylib";
constexpr const char *kDefaultLib = "/usr/lib/libSystem.B.dylib";
constexpr const char *kDyldPath = "/usr/lib/dyld";

uint64_t AlignUp64(const uint64_t value, const uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
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
    return static_cast<uint32_t>(AlignUp64(headerSize + value.size() + 1, 8));
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
} // namespace

bool Linker::LinkMachO64(const std::filesystem::path &outputPath) {
    // Collect definitions first so cross-module references are resolved as
    // ordinary Rux symbols instead of dynamic imports.
    std::unordered_set<std::string> definedSymbols;
    for (const auto &object : objects) {
        for (const auto &symbol : object.symbols) {
            if (symbol.kind != RcuSymKind::ExternFunc && symbol.kind != RcuSymKind::ExternData &&
                !symbol.name.empty()) {
                definedSymbols.insert(symbol.name);
            }
        }
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
                    Error("external data symbol '" + symbol.name + "' cannot be imported by the Mach-O linker");
                }
            }
        }
    }
    if (!errors.empty()) {
        return false;
    }

    const bool dynamic = !importLib.empty();

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

    std::unordered_map<std::string, uint8_t> libraryOrdinal;
    for (size_t i = 0; i < neededLibs.size(); ++i) {
        if (i + 1 > 255) {
            Error("the Mach-O linker supports at most 255 imported libraries");
            return false;
        }
        libraryOrdinal[neededLibs[i]] = static_cast<uint8_t>(i + 1);
    }

    // Entry point. Dynamic executables are entered as a normal function by
    // dyld, so preserve its return address and provide Win64 shadow space for
    // Rux's current default macOS calling convention. Static executables keep
    // the raw exit syscall path.
    Buf textPrefix;
    size_t callMainDisp = 0;
    if (dynamic) {
        textPrefix.insert(textPrefix.end(), {0x48, 0x83, 0xEC, 0x28}); // sub rsp, 40
        callMainDisp = textPrefix.size() + 1;
        textPrefix.insert(textPrefix.end(), {0xE8, 0, 0, 0, 0});       // call Main
        textPrefix.insert(textPrefix.end(), {0x48, 0x83, 0xC4, 0x28}); // add rsp, 40
        textPrefix.push_back(0xC3);                                    // ret to dyld
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
    const auto prefixSize = static_cast<uint32_t>(textPrefix.size());

    struct ObjectLayout {
        uint32_t textOffset;
        uint32_t rodataOffset;
        uint32_t dataOffset;
    };

    std::vector<ObjectLayout> layouts(objects.size());
    Buf mergedText;
    Buf mergedRodata;
    Buf mergedData;
    for (size_t i = 0; i < objects.size(); ++i) {
        const auto &object = objects[i];
        layouts[i] = {static_cast<uint32_t>(mergedText.size()), static_cast<uint32_t>(mergedRodata.size()),
                      static_cast<uint32_t>(mergedData.size())};
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

    // Each function stub is `jmp qword ptr [rip + disp32]`; dyld eagerly
    // fills the matching non-lazy pointer before transferring control.
    Buf stubs;
    for (size_t i = 0; i < importNames.size(); ++i) {
        stubs.insert(stubs.end(), {0xFF, 0x25, 0, 0, 0, 0});
    }
    Buf importPointers(importNames.size() * 8, 0);

    // The eager bind stream points each slot in __DATA,__nl_symbol_ptr at its
    // underscored Mach-O C symbol in the requested LC_LOAD_DYLIB ordinal.
    Buf bindStream;
    if (dynamic) {
        WriteU8(bindStream, 0x51); // SET_TYPE_IMM | POINTER
        WriteU8(bindStream, 0x72); // SET_SEGMENT_AND_OFFSET_ULEB | __DATA index
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
        for (size_t i = 0; i < importNames.size(); ++i) {
            const auto &name = importNames[i];
            const uint32_t stringIndex = static_cast<uint32_t>(stringTable.size());
            WriteU8(stringTable, '_');
            for (const char byte : name) {
                WriteU8(stringTable, static_cast<uint8_t>(byte));
            }
            WriteU8(stringTable, 0);

            WriteU32(dynamicSymbols, stringIndex);
            WriteU8(dynamicSymbols, 0x01); // N_UNDF | N_EXT
            WriteU8(dynamicSymbols, 0);    // NO_SECT
            WriteU16(dynamicSymbols, static_cast<uint16_t>(libraryOrdinal.at(importLib.at(name))) << 8);
            WriteU64(dynamicSymbols, 0);
        }
        for (size_t section = 0; section < 2; ++section) {
            for (size_t i = 0; i < importNames.size(); ++i) {
                WriteU32(indirectSymbols, static_cast<uint32_t>(i));
            }
        }
    }

    constexpr uint32_t segmentCommandSize = 72;
    constexpr uint32_t sectionSize = 80;
    constexpr uint32_t threadCommandSize = 184;
    const uint32_t textSectionCount = dynamic ? 3 : 2;
    const uint32_t dataSectionCount = dynamic ? 2 : 1;
    const uint32_t textCommandSize = segmentCommandSize + textSectionCount * sectionSize;
    const uint32_t dataCommandSize = segmentCommandSize + dataSectionCount * sectionSize;

    uint32_t commandCount = 4; // PAGEZERO, TEXT, DATA, LINKEDIT
    uint32_t commandsSize = segmentCommandSize + textCommandSize + dataCommandSize + segmentCommandSize;
    if (dynamic) {
        commandCount += 5 + static_cast<uint32_t>(neededLibs.size()); // DYLD_INFO, SYMTAB, DYSYMTAB,
                                                                      // DYLINKER, MAIN, dylibs
        commandsSize += 48;                                           // LC_DYLD_INFO_ONLY
        commandsSize += 24;                                           // LC_SYMTAB
        commandsSize += 80;                                           // LC_DYSYMTAB
        commandsSize += StringCommandSize(12, kDyldPath);
        commandsSize += 24; // LC_MAIN
        for (const auto &library : neededLibs) {
            commandsSize += StringCommandSize(24, library);
        }
    }
    else {
        ++commandCount;
        commandsSize += threadCommandSize;
    }

    const uint64_t headerSize = 32 + commandsSize;
    constexpr uint64_t codeSignatureCommandSlack = 32;
    const uint64_t textOffset = AlignUp64(headerSize + codeSignatureCommandSlack, 16);
    const uint64_t textVA = kBase + textOffset;
    const uint64_t stubsOffset = AlignUp64(textOffset + textBuffer.size(), 2);
    const uint64_t stubsVA = kBase + stubsOffset;
    const uint64_t rodataOffset = AlignUp64(stubsOffset + stubs.size(), 16);
    const uint64_t rodataVA = kBase + rodataOffset;
    const uint64_t textSegmentFileEnd = rodataOffset + mergedRodata.size();
    const uint64_t textSegmentVMSize = AlignUp64(textSegmentFileEnd, kPage);

    const uint64_t dataSegmentOffset = AlignUp64(textSegmentFileEnd, kPage);
    const uint64_t dataSegmentVA = kBase + dataSegmentOffset;
    const uint64_t pointersOffset = dataSegmentOffset;
    const uint64_t pointersVA = dataSegmentVA;
    const uint64_t dataOffset = AlignUp64(pointersOffset + importPointers.size(), 8);
    const uint64_t dataVA = kBase + dataOffset;
    const uint64_t dataSegmentFileSize = dataOffset + mergedData.size() - dataSegmentOffset;
    const uint64_t dataSegmentVMSize = AlignUp64(std::max<uint64_t>(dataSegmentFileSize, 1), kPage);

    const uint64_t linkeditOffset = dataSegmentOffset + dataSegmentVMSize;
    const uint64_t linkeditVA = kBase + linkeditOffset;
    Buf linkeditBuffer = bindStream;
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
    const uint64_t linkeditVMSize = AlignUp64(std::max<uint64_t>(linkeditBuffer.size(), 1), kPage);

    // Patch each stub to its corresponding pointer slot.
    for (size_t i = 0; i < importNames.size(); ++i) {
        const uint64_t stubNextInstruction = stubsVA + i * 6 + 6;
        const uint64_t pointerAddress = pointersVA + i * 8;
        const auto displacement = static_cast<int32_t>(pointerAddress - stubNextInstruction);
        Patch32(stubs, i * 6 + 2, static_cast<uint32_t>(displacement));
    }

    std::unordered_map<std::string, uint64_t> symbolMap;
    for (size_t i = 0; i < importNames.size(); ++i) {
        symbolMap[importNames[i]] = stubsVA + i * 6;
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

    const auto mainIt = symbolMap.find("Main");
    if (mainIt == symbolMap.end()) {
        Error("undefined symbol 'Main' — no entry point found");
        return false;
    }
    const uint64_t callMainNextInstruction = textVA + callMainDisp + 4;
    Patch32(textBuffer, callMainDisp,
            static_cast<uint32_t>(static_cast<int32_t>(mainIt->second - callMainNextInstruction)));

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
                    continue;
                }

                const size_t patchOffset = baseInBuffer + relocation.sectionOffset;
                const uint64_t relocationVA = sectionBaseVA + relocation.sectionOffset;
                if (relocation.type == RcuRelType::Rel32) {
                    if (patchOffset + 4 <= buffer->size()) {
                        const auto displacement =
                            static_cast<int32_t>(targetVA + relocation.addend - (relocationVA + 4));
                        Patch32(*buffer, patchOffset, static_cast<uint32_t>(displacement));
                    }
                }
                else if (relocation.type == RcuRelType::Abs64) {
                    if (patchOffset + 8 <= buffer->size()) {
                        Patch64(*buffer, patchOffset, targetVA + static_cast<uint64_t>(relocation.addend));
                    }
                }
                else if (relocation.type == RcuRelType::Abs32) {
                    if (patchOffset + 4 <= buffer->size()) {
                        Patch32(*buffer, patchOffset,
                                static_cast<uint32_t>(targetVA + static_cast<uint64_t>(relocation.addend)));
                    }
                }
            }
        }
    }
    if (!errors.empty()) {
        return false;
    }

    Buf loadCommands;

    // __PAGEZERO
    WriteU32(loadCommands, 0x19);
    WriteU32(loadCommands, segmentCommandSize);
    WriteMachName(loadCommands, "__PAGEZERO");
    WriteU64(loadCommands, 0);
    WriteU64(loadCommands, kBase);
    WriteU64(loadCommands, 0);
    WriteU64(loadCommands, 0);
    WriteU32(loadCommands, 0);
    WriteU32(loadCommands, 0);
    WriteU32(loadCommands, 0);
    WriteU32(loadCommands, 0);

    // __TEXT: __text, [__stubs], __const
    WriteU32(loadCommands, 0x19);
    WriteU32(loadCommands, textCommandSize);
    WriteMachName(loadCommands, "__TEXT");
    WriteU64(loadCommands, kBase);
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
        WriteU32(loadCommands, 1);
        WriteU32(loadCommands, 0);
        WriteU32(loadCommands, 0);
        WriteU32(loadCommands, 0x8000'0408); // instructions | S_SYMBOL_STUBS
        WriteU32(loadCommands, 0);
        WriteU32(loadCommands, 6); // stub size
        WriteU32(loadCommands, 0);
    }

    WriteMachName(loadCommands, "__const");
    WriteMachName(loadCommands, "__TEXT");
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

    // __LINKEDIT contains dyld bind opcodes and the dynamic symbol metadata;
    // codesign appends its signature here and updates this segment.
    WriteU32(loadCommands, 0x19);
    WriteU32(loadCommands, segmentCommandSize);
    WriteMachName(loadCommands, "__LINKEDIT");
    WriteU64(loadCommands, linkeditVA);
    WriteU64(loadCommands, linkeditVMSize);
    WriteU64(loadCommands, linkeditOffset);
    WriteU64(loadCommands, linkeditBuffer.size());
    WriteU32(loadCommands, 0x01);
    WriteU32(loadCommands, 0x01);
    WriteU32(loadCommands, 0);
    WriteU32(loadCommands, 0);

    if (dynamic) {
        WriteU32(loadCommands, 0x8000'0022); // LC_DYLD_INFO_ONLY
        WriteU32(loadCommands, 48);
        WriteU32(loadCommands, 0); // rebase_off
        WriteU32(loadCommands, 0); // rebase_size
        WriteU32(loadCommands, static_cast<uint32_t>(linkeditOffset));
        WriteU32(loadCommands, static_cast<uint32_t>(bindStream.size()));
        WriteU32(loadCommands, 0); // weak_bind_off
        WriteU32(loadCommands, 0); // weak_bind_size
        WriteU32(loadCommands, 0); // lazy_bind_off
        WriteU32(loadCommands, 0); // lazy_bind_size
        WriteU32(loadCommands, 0); // export_off
        WriteU32(loadCommands, 0); // export_size

        WriteU32(loadCommands, 0x02); // LC_SYMTAB
        WriteU32(loadCommands, 24);
        WriteU32(loadCommands, static_cast<uint32_t>(symbolTableOffset));
        WriteU32(loadCommands, static_cast<uint32_t>(importNames.size()));
        WriteU32(loadCommands, static_cast<uint32_t>(stringTableOffset));
        WriteU32(loadCommands, static_cast<uint32_t>(stringTable.size()));

        WriteU32(loadCommands, 0x0B); // LC_DYSYMTAB
        WriteU32(loadCommands, 80);
        WriteU32(loadCommands, 0); // ilocalsym
        WriteU32(loadCommands, 0); // nlocalsym
        WriteU32(loadCommands, 0); // iextdefsym
        WriteU32(loadCommands, 0); // nextdefsym
        WriteU32(loadCommands, 0); // iundefsym
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

        WriteDylinkerCommand(loadCommands);

        WriteU32(loadCommands, 0x8000'0028); // LC_MAIN
        WriteU32(loadCommands, 24);
        WriteU64(loadCommands, textOffset); // entryoff from start of __TEXT/file
        WriteU64(loadCommands, 0);          // stacksize

        for (const auto &library : neededLibs) {
            WriteDylibCommand(loadCommands, library);
        }
    }
    else {
        WriteU32(loadCommands, 0x05); // LC_UNIXTHREAD
        WriteU32(loadCommands, threadCommandSize);
        WriteU32(loadCommands, 4);  // x86_THREAD_STATE64
        WriteU32(loadCommands, 42); // 21 uint64 registers
        for (int reg = 0; reg < 21; ++reg) {
            WriteU64(loadCommands, reg == 16 ? textVA : 0);
        }
    }

    if (loadCommands.size() != commandsSize) {
        Error("internal: Mach-O load-command size mismatch");
        return false;
    }

    std::filesystem::create_directories(outputPath.parent_path());
    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        Error("cannot open output file: " + outputPath.string());
        return false;
    }

    const auto writeBuffer = [&](const Buf &buffer) {
        if (!buffer.empty()) {
            output.write(reinterpret_cast<const char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        }
    };
    const auto padToOffset = [&](const uint64_t offset) {
        static constexpr uint8_t zeros[4096] = {};
        while (static_cast<uint64_t>(output.tellp()) < offset) {
            const uint64_t remaining = offset - static_cast<uint64_t>(output.tellp());
            output.write(reinterpret_cast<const char *>(zeros),
                         static_cast<std::streamsize>(std::min<uint64_t>(remaining, sizeof(zeros))));
        }
    };

    Buf header;
    WriteU32(header, 0xFEED'FACF); // MH_MAGIC_64
    WriteU32(header, 0x0100'0007); // CPU_TYPE_X86_64
    WriteU32(header, 0x0000'0003); // CPU_SUBTYPE_X86_64_ALL
    WriteU32(header, 2);           // MH_EXECUTE
    WriteU32(header, commandCount);
    WriteU32(header, commandsSize);
    WriteU32(header, dynamic ? 0x0000'0005 : 0x0000'0001); // MH_DYLDLINK | MH_NOUNDEFS
    WriteU32(header, 0);

    writeBuffer(header);
    writeBuffer(loadCommands);
    padToOffset(textOffset);
    writeBuffer(textBuffer);
    padToOffset(stubsOffset);
    writeBuffer(stubs);
    padToOffset(rodataOffset);
    writeBuffer(mergedRodata);
    padToOffset(pointersOffset);
    writeBuffer(importPointers);
    padToOffset(dataOffset);
    writeBuffer(mergedData);
    padToOffset(linkeditOffset);
    writeBuffer(linkeditBuffer);

    output.close();
    if (!output) {
        Error("cannot write output file: " + outputPath.string());
        return false;
    }

    std::error_code errorCode;
    std::filesystem::permissions(outputPath,
                                 std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
                                     std::filesystem::perms::others_exec,
                                 std::filesystem::perm_options::add, errorCode);
    if (errorCode) {
        Error("cannot mark output executable: " + errorCode.message());
        return false;
    }

    const std::string signCommand = "codesign --force --sign - \"" + outputPath.string() + "\" 2>/dev/null";
    if (std::system(signCommand.c_str()) != 0) {
        Error("ad-hoc codesign failed (need Xcode command line tools); binary will not run on Apple Silicon");
        return false;
    }

    return true;
}
} // namespace Rux
