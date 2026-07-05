// PE32+ object writer and Windows DLL import resolution.

#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "Linker/Linker.h"
#include "Linker/LinkerInternal.h"
#include "System/Os.h"

namespace Rux {

[[maybe_unused]] static constexpr uint64_t kImageBase = 0x1'4000'0000ULL;
[[maybe_unused]] static constexpr uint32_t kSecAlign = 0x1000; // 4 KB section alignment
[[maybe_unused]] static constexpr uint32_t kFileAlign = 0x200; // 512 B file alignment
[[maybe_unused]] static constexpr uint16_t kMachineAmd64 = 0x8664;
[[maybe_unused]] static constexpr uint16_t kMagicPE32P = 0x020B;
[[maybe_unused]] static constexpr uint16_t kSubsystemCUI = 3; // console

// DLL-specific
[[maybe_unused]] static constexpr uint16_t kSubsystemGUI = 2; // windows GUI (used for DLLs)
// IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE |
// IMAGE_FILE_DLL
[[maybe_unused]] static constexpr uint16_t kCharacteristicsDll = 0x2022u;

// IMAGE_SCN_ characteristics
[[maybe_unused]] static constexpr uint32_t kScnText = 0x6000'0020u;  // CNT_CODE | MEM_EXECUTE | MEM_READ
[[maybe_unused]] static constexpr uint32_t kScnRData = 0x4000'0040u; // CNT_INITIALIZED_DATA | MEM_READ
[[maybe_unused]] static constexpr uint32_t kScnData = 0xC000'0040u;  // CNT_INITIALIZED_DATA | MEM_READ | MEM_WRITE

// DllCharacteristics: NX_COMPAT | TERMINAL_SERVER_AWARE.
// The linker currently does not emit a .reloc table, so do not opt into
// ASLR. Absolute relocations such as vtable function pointers must remain
// valid at the preferred image base.
[[maybe_unused]] static constexpr uint16_t kDllChars = 0x8100u;

static bool FileExists(const std::filesystem::path &path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

static std::optional<Buf> ReadFileBytes(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return std::nullopt;
    }
    const auto size = in.tellg();
    if (size < 0) {
        return std::nullopt;
    }
    Buf data(static_cast<size_t>(size));
    in.seekg(0);
    if (!data.empty()) {
        in.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    if (!in && !in.eof()) {
        return std::nullopt;
    }
    return data;
}

static bool ReadU16At(const Buf &b, size_t off, uint16_t &out) {
    if (off + 2 > b.size()) {
        return false;
    }
    out = static_cast<uint16_t>(b[off] | (b[off + 1] << 8));
    return true;
}

static bool ReadU32At(const Buf &b, size_t off, uint32_t &out) {
    if (off + 4 > b.size()) {
        return false;
    }
    out = static_cast<uint32_t>(b[off]) | (static_cast<uint32_t>(b[off + 1]) << 8) |
          (static_cast<uint32_t>(b[off + 2]) << 16) | (static_cast<uint32_t>(b[off + 3]) << 24);
    return true;
}

static std::optional<size_t> PeRvaToOffset(const Buf &pe, uint32_t rva, size_t sectionTable, uint16_t sectionCount) {
    for (uint16_t i = 0; i < sectionCount; ++i) {
        const size_t sec = sectionTable + static_cast<size_t>(i) * 40;
        uint32_t virtualSize = 0, virtualAddress = 0, rawSize = 0, rawPtr = 0;
        if (!ReadU32At(pe, sec + 8, virtualSize) || !ReadU32At(pe, sec + 12, virtualAddress) ||
            !ReadU32At(pe, sec + 16, rawSize) || !ReadU32At(pe, sec + 20, rawPtr)) {
            return std::nullopt;
        }

        const uint32_t span = std::max(virtualSize, rawSize);
        if (rva >= virtualAddress && rva < virtualAddress + span) {
            const size_t off = static_cast<size_t>(rawPtr) + (rva - virtualAddress);
            if (off < pe.size()) {
                return off;
            }
            return std::nullopt;
        }
    }
    return std::nullopt;
}

static bool ReadPeCString(const Buf &pe, size_t off, std::string &out) {
    if (off >= pe.size()) {
        return false;
    }
    out.clear();
    while (off < pe.size() && pe[off] != 0) {
        out.push_back(static_cast<char>(pe[off++]));
    }
    return off < pe.size();
}

[[maybe_unused]] static std::optional<std::unordered_set<std::string>>
ReadDllExports(const std::filesystem::path &path) {
    auto peData = ReadFileBytes(path);
    if (!peData) {
        return std::nullopt;
    }
    const Buf &pe = *peData;

    uint32_t peOff32 = 0;
    if (pe.size() < 0x40 || !ReadU32At(pe, 0x3C, peOff32)) {
        return std::nullopt;
    }
    const size_t peOff = peOff32;
    if (peOff + 24 > pe.size() || pe[peOff] != 'P' || pe[peOff + 1] != 'E' || pe[peOff + 2] != 0 ||
        pe[peOff + 3] != 0) {
        return std::nullopt;
    }

    uint16_t sectionCount = 0, optionalSize = 0, magic = 0;
    if (!ReadU16At(pe, peOff + 6, sectionCount) || !ReadU16At(pe, peOff + 20, optionalSize) ||
        !ReadU16At(pe, peOff + 24, magic)) {
        return std::nullopt;
    }

    const size_t optionalOff = peOff + 24;
    const size_t dataDirOff = magic == 0x020B ? optionalOff + 112 : optionalOff + 96;
    uint32_t exportRva = 0, exportSize = 0;
    if (dataDirOff + 8 > optionalOff + optionalSize || !ReadU32At(pe, dataDirOff, exportRva) ||
        !ReadU32At(pe, dataDirOff + 4, exportSize)) {
        return std::nullopt;
    }

    std::unordered_set<std::string> exports;
    if (exportRva == 0 || exportSize == 0) {
        return exports;
    }

    const size_t sectionTable = optionalOff + optionalSize;
    auto exportOff = PeRvaToOffset(pe, exportRva, sectionTable, sectionCount);
    if (!exportOff || *exportOff + 40 > pe.size()) {
        return std::nullopt;
    }

    uint32_t nameCount = 0, namesRva = 0;
    if (!ReadU32At(pe, *exportOff + 24, nameCount) || !ReadU32At(pe, *exportOff + 32, namesRva)) {
        return std::nullopt;
    }

    auto namesOff = PeRvaToOffset(pe, namesRva, sectionTable, sectionCount);
    if (!namesOff) {
        return std::nullopt;
    }

    for (uint32_t i = 0; i < nameCount; ++i) {
        uint32_t nameRva = 0;
        if (!ReadU32At(pe, *namesOff + static_cast<size_t>(i) * 4, nameRva)) {
            return std::nullopt;
        }

        auto nameOff = PeRvaToOffset(pe, nameRva, sectionTable, sectionCount);
        if (!nameOff) {
            return std::nullopt;
        }

        std::string name;
        if (!ReadPeCString(pe, *nameOff, name)) {
            return std::nullopt;
        }
        exports.insert(std::move(name));
    }

    return exports;
}

static std::string GetPathEnv() {
    return System::GetEnv("PATH").value_or(std::string{});
}

[[maybe_unused]] static std::optional<std::filesystem::path>
FindDllFile(const std::string &dll, const std::vector<std::filesystem::path> &searchDirs,
            const std::filesystem::path &outputDir) {
    const std::filesystem::path dllPath(dll);
    if (dllPath.is_absolute()) {
        return FileExists(dllPath) ? std::optional<std::filesystem::path>(dllPath) : std::nullopt;
    }

    // Candidate file names to probe in each search location. Imports are
    // commonly declared without an extension (e.g. @[Import(lib:
    // "kernel32")]); mirror the OS loader and also try the name with ".dll"
    // appended.
    std::vector<std::filesystem::path> candidates{dllPath};
    if (dllPath.extension().empty()) {
        candidates.emplace_back(dll + ".dll");
    }

    const auto probe = [&](const std::filesystem::path &dir) -> std::optional<std::filesystem::path> {
        if (dir.empty()) {
            return std::nullopt;
        }
        for (const auto &name : candidates) {
            if (FileExists(dir / name)) {
                return dir / name;
            }
        }
        return std::nullopt;
    };

    if (auto hit = probe(outputDir)) {
        return hit;
    }

    for (const auto &dir : searchDirs) {
        if (auto hit = probe(dir)) {
            return hit;
        }
    }

    if (auto hit = probe(std::filesystem::current_path())) {
        return hit;
    }

    // System DLLs (kernel32, user32, ...) live in the Windows system
    // directory, which is the authoritative source for them — don't rely on
    // it happening to be on PATH (it isn't under some shells, e.g. Git
    // Bash). Empty (and skipped by probe) on non-Windows hosts.
    if (auto hit = probe(System::WindowsSystemDirectory())) {
        return hit;
    }

    const std::string pathEnv = GetPathEnv();
    if (pathEnv.empty()) {
        return std::nullopt;
    }

    std::stringstream ss(pathEnv);
    std::string dir;
    while (std::getline(ss, dir, ';')) {
        if (auto hit = probe(std::filesystem::path(dir))) {
            return hit;
        }
    }

    return std::nullopt;
}

bool Linker::LinkPe64(const std::filesystem::path &outputPath) {
    // 1. Collect imported external function names

    // EXEs always need ExitProcess for the entry thunk; DLLs do not.
    std::unordered_map<std::string, std::string> importDll;
    std::unordered_set<std::string> explicitImportDlls;
    std::unordered_map<std::string, std::vector<std::string>> explicitImportFuncsByDll;
    if (!isDll) {
        importDll["ExitProcess"] = "KERNEL32.DLL";
    }

    // First pass: collect explicit DLL assignments from symbol
    // declarations. This handles the case where a call site and its
    // declaration are in different translation units — the declaration
    // carries the DLL name.
    for (const auto &obj : objects) {
        for (const auto &sym : obj.symbols) {
            if (sym.kind == RcuSymKind::ExternFunc && !sym.typeName.empty()) {
                importDll[sym.name] = sym.typeName;
                explicitImportDlls.insert(sym.typeName);
                explicitImportFuncsByDll[sym.typeName].push_back(sym.name);
            }
        }
    }

    // Collect all symbol names that are defined (non-extern) across all
    // objects. Cross-module calls produce ExternFunc relocations but the
    // callee is defined in another RcuFile — those must NOT be treated as
    // OS DLL imports.
    std::unordered_set<std::string> definedSymbols;
    for (const auto &obj : objects) {
        for (const auto &sym : obj.symbols) {
            if (sym.kind != RcuSymKind::ExternFunc && sym.kind != RcuSymKind::ExternData && !sym.name.empty()) {
                definedSymbols.insert(sym.name);
            }
        }
    }

    // Second pass: collect imports from relocations. For compiler-generated
    // extern symbols (e.g. runtime helpers) that carry no explicit DLL,
    // fall back to KERNEL32.DLL so existing behavior is preserved. Skip
    // symbols that are defined locally (cross-module references, not DLL
    // imports).
    for (const auto &obj : objects) {
        for (const auto &sec : obj.sections) {
            for (const auto &reloc : sec.relocs) {
                if (reloc.symbolIndex >= obj.symbols.size()) {
                    continue;
                }
                const auto &sym = obj.symbols[reloc.symbolIndex];
                if (sym.kind == RcuSymKind::ExternFunc && !definedSymbols.contains(sym.name)) {
                    importDll.try_emplace(sym.name, "KERNEL32.DLL");
                }
            }
        }
    }

    // Sorted for determinism
    std::vector<std::string> importNames;
    importNames.reserve(importDll.size());
    for (const auto &n : importDll | std::views::keys) {
        importNames.push_back(n);
    }
    std::ranges::sort(importNames);

    std::unordered_map<std::string, size_t> importIdx;
    for (size_t i = 0; i < importNames.size(); ++i) {
        importIdx[importNames[i]] = i;
    }
    const size_t numImports = importNames.size();

    const auto outputDir = outputPath.parent_path();
    for (const auto &dll : explicitImportDlls) {
        auto dllPath = FindDllFile(dll, importSearchDirs, outputDir);
        if (!dllPath) {
            Error("import DLL '" + dll + "' was not found");
            continue;
        }

        auto exports = ReadDllExports(*dllPath);
        if (!exports) {
            Error("could not read export table from import DLL '" + dll + "'");
            continue;
        }

        for (const auto &func : explicitImportFuncsByDll[dll]) {
            if (!exports->contains(func)) {
                Error("import function '" + func + "' was not found in DLL '" + dll + "'");
            }
        }
    }
    if (!errors.empty()) {
        return false;
    }

    // 2. Build .text preamble (entry thunk + import thunks)

    Buf textPre;

    if (isDll) {
        // DLL entry point: _DllMainCRTStartup / DllMain proxy
        // Win64 DLL entry: BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID)
        // args: rcx=hModule, rdx=fdwReason, r8=lpvReserved
        // We call user's DllMain if it exists, otherwise just return TRUE
        // (1).
        //
        // Thunk layout:
        //   sub  rsp, 0x28
        //   call DllMain       ; E8 disp32  (patched; if DllMain defined)
        //   mov  eax, 1        ; return TRUE if DllMain missing or returned
        //   0 add  rsp, 0x28 ret
        //
        // If DllMain is not defined in user code we emit a minimal stub
        // that just returns TRUE — standard DLL behaviour when no
        // initialisation needed.
        textPre.insert(textPre.end(), {0x48, 0x83, 0xEC, 0x28}); // sub rsp, 0x28
        const size_t kCallDllMainDisp = textPre.size() + 1;
        textPre.insert(textPre.end(), {0xE8, 0x00, 0x00, 0x00, 0x00}); // call DllMain
        // If DllMain returned 0 (init failed), still propagate it;
        // otherwise keep eax. For simplicity, we trust DllMain's return value directly.
        textPre.insert(textPre.end(), {0x48, 0x83, 0xC4, 0x28}); // add rsp, 0x28
        textPre.push_back(0xC3);                                 // ret
        (void)kCallDllMainDisp;                                  // used below during patching
    }
    else {
        // EXE entry thunk (__rux_start):
        //   sub rsp, 0x28       ; 48 83 EC 28
        //   call Main           ; E8 xx xx xx xx
        //   mov ecx, eax        ; 89 C1
        //   call ExitProcess    ; E8 xx xx xx xx
        //   int3                ; CC
        textPre.insert(textPre.end(), {0x48, 0x83, 0xEC, 0x28});
    }
    const size_t kCallMainDisp = isDll ? 5 : textPre.size() + 1; // offset of 4-byte disp field
    if (!isDll) {
        textPre.insert(textPre.end(), {0xE8, 0x00, 0x00, 0x00, 0x00});
        textPre.insert(textPre.end(), {0x89, 0xC1});
    }
    const size_t kCallExitDisp = textPre.size() + 1;
    if (!isDll) {
        textPre.insert(textPre.end(), {0xE8, 0x00, 0x00, 0x00, 0x00});
        textPre.push_back(0xCC);
    }

    // Import thunks: jmp qword ptr [rip+disp32] = FF 25 xx xx xx xx
    std::vector<size_t> thunkOff(numImports);
    for (size_t i = 0; i < numImports; ++i) {
        thunkOff[i] = textPre.size();
        textPre.insert(textPre.end(), {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});
    }

    const auto preambleSize = static_cast<uint32_t>(textPre.size());

    // 3. Merge RCU sections

    struct ObjLayout {
        uint32_t textOff, rodataOff, dataOff;
    };

    std::vector<ObjLayout> layouts(objects.size());
    Buf mergedText, mergedRodata, mergedData;

    for (size_t i = 0; i < objects.size(); ++i) {
        const auto &obj = objects[i];
        layouts[i] = {static_cast<uint32_t>(mergedText.size()), static_cast<uint32_t>(mergedRodata.size()),
                      static_cast<uint32_t>(mergedData.size())};
        for (const auto &sec : obj.sections) {
            if (sec.type == RcuSecType::Text) {
                mergedText.insert(mergedText.end(), sec.data.begin(), sec.data.end());
            }
            else if (sec.type == RcuSecType::RoData) {
                mergedRodata.insert(mergedRodata.end(), sec.data.begin(), sec.data.end());
            }
            else if (sec.type == RcuSecType::Data) {
                mergedData.insert(mergedData.end(), sec.data.begin(), sec.data.end());
            }
        }
    }

    // 4. Build import table appended to .rdata
    // Layout within .rdata (after user rodata):
    //   [aligned pad]
    //   [Import Directory Table: one descriptor per DLL + one null]
    //   [INT arrays: one null-terminated array per DLL]
    //   [IAT arrays: one null-terminated array per DLL]
    //   [DLL name strings]
    //   [IMAGE_IMPORT_BY_NAME entries per function]

    Buf rdataBuf;
    rdataBuf.insert(rdataBuf.end(), mergedRodata.begin(), mergedRodata.end());
    PadTo(rdataBuf, 8);
    std::map<std::string, std::vector<size_t>> importsByDll;
    for (size_t i = 0; i < numImports; ++i) {
        importsByDll[importDll[importNames[i]]].push_back(i);
    }
    const auto importDirOff = static_cast<uint32_t>(rdataBuf.size());
    const size_t importDirPos = rdataBuf.size();
    WriteZeros(rdataBuf, (importsByDll.size() + 1) * 20);
    std::vector<std::string> importDllNames;
    std::vector<std::vector<size_t>> importDllMembers;
    importDllNames.reserve(importsByDll.size());
    importDllMembers.reserve(importsByDll.size());
    for (auto &[dll, members] : importsByDll) {
        importDllNames.push_back(dll);
        importDllMembers.push_back(std::move(members));
    }
    std::vector<uint32_t> intOff(importDllNames.size());
    std::vector<size_t> intPos(importDllNames.size());
    for (size_t g = 0; g < importDllNames.size(); ++g) {
        intOff[g] = static_cast<uint32_t>(rdataBuf.size());
        intPos[g] = rdataBuf.size();
        WriteZeros(rdataBuf, (importDllMembers[g].size() + 1) * 8);
    }
    const auto iatOff = static_cast<uint32_t>(rdataBuf.size());
    std::vector<uint32_t> iatGroupOff(importDllNames.size());
    std::vector<size_t> iatPos(importDllNames.size());
    std::vector<uint32_t> iatEntryOff(numImports);
    for (size_t g = 0; g < importDllNames.size(); ++g) {
        iatGroupOff[g] = static_cast<uint32_t>(rdataBuf.size());
        iatPos[g] = rdataBuf.size();
        for (size_t j = 0; j < importDllMembers[g].size(); ++j) {
            iatEntryOff[importDllMembers[g][j]] = iatGroupOff[g] + static_cast<uint32_t>(j * 8);
        }
        WriteZeros(rdataBuf, (importDllMembers[g].size() + 1) * 8);
    }
    const uint32_t iatSize = static_cast<uint32_t>(rdataBuf.size()) - iatOff;
    std::vector<uint32_t> dllNameOff(importDllNames.size());
    for (size_t g = 0; g < importDllNames.size(); ++g) {
        dllNameOff[g] = static_cast<uint32_t>(rdataBuf.size());
        WriteCStr(rdataBuf, importDllNames[g].c_str());
        PadTo(rdataBuf, 2);
    }
    std::vector<uint32_t> hintNameOff(numImports);
    for (size_t i = 0; i < numImports; ++i) {
        hintNameOff[i] = static_cast<uint32_t>(rdataBuf.size());
        WriteU16(rdataBuf, 0); // hint
        for (char c : importNames[i]) {
            rdataBuf.push_back(static_cast<uint8_t>(c));
        }
        rdataBuf.push_back(0);
        PadTo(rdataBuf, 2);
    }

    // 5. Compute section layout (RVAs and file offsets)
    const uint32_t numSections = mergedData.empty() ? 2u : 3u;
    const uint32_t rawHdrBytes = 64 + 4 + 20 + 240 + numSections * 40;
    const uint32_t sizeOfHeaders = AlignUp(rawHdrBytes, kFileAlign);
    const uint32_t textRva = AlignUp(sizeOfHeaders, kSecAlign);
    const uint32_t textVirtSize = preambleSize + static_cast<uint32_t>(mergedText.size());
    const uint32_t textFileSize = AlignUp(textVirtSize, kFileAlign);
    const uint32_t textFileOff = sizeOfHeaders;
    const uint32_t rdataRva = textRva + AlignUp(textVirtSize, kSecAlign);
    const auto rdataVirtSize = static_cast<uint32_t>(rdataBuf.size());
    const uint32_t rdataFileSize = AlignUp(rdataVirtSize, kFileAlign);
    const uint32_t rdataFileOff = textFileOff + textFileSize;
    uint32_t dataRva = 0, dataVirtSize = 0, dataFileSize = 0, dataFileOff = 0;
    if (!mergedData.empty()) {
        dataRva = rdataRva + AlignUp(rdataVirtSize, kSecAlign);
        dataVirtSize = static_cast<uint32_t>(mergedData.size());
        dataFileSize = AlignUp(dataVirtSize, kFileAlign);
        dataFileOff = rdataFileOff + rdataFileSize;
    }
    const uint32_t sizeOfImage =
        !mergedData.empty() ? dataRva + AlignUp(dataVirtSize, kSecAlign) : rdataRva + AlignUp(rdataVirtSize, kSecAlign);

    // 6. Patch .rdata import table with real RVAs
    for (size_t g = 0; g < importDllNames.size(); ++g) {
        for (size_t j = 0; j < importDllMembers[g].size(); ++j) {
            const size_t importIndex = importDllMembers[g][j];
            const uint64_t hnRva = rdataRva + hintNameOff[importIndex];
            Patch64(rdataBuf, intPos[g] + j * 8, hnRva); // INT entry
            Patch64(rdataBuf, iatPos[g] + j * 8, hnRva); // IAT entry (pre-bind)
        }
        // Patch IMAGE_IMPORT_DESCRIPTOR
        const size_t descPos = importDirPos + g * 20;
        Patch32(rdataBuf, descPos + 0,
                rdataRva + intOff[g]);                             // OriginalFirstThunk
        Patch32(rdataBuf, descPos + 4, 0);                         // TimeDateStamp
        Patch32(rdataBuf, descPos + 8, 0xFFFF'FFFFu);              // ForwarderChain
        Patch32(rdataBuf, descPos + 12, rdataRva + dllNameOff[g]); // Name
        Patch32(rdataBuf, descPos + 16,
                rdataRva + iatGroupOff[g]); // FirstThunk (IAT)
    }
    // null descriptor and null thunk terminators already zeroed

    // 7. Build global symbol map (name → VA)

    std::unordered_map<std::string, uint64_t> symMap;

    // Add all imported function thunks first
    for (size_t i = 0; i < numImports; ++i) {
        symMap[importNames[i]] = kImageBase + textRva + thunkOff[i];
    }

    // Add symbols defined in each RCU file. Local data/constant symbols are
    // intentionally not added here: generated labels such as __f64_0 are
    // reused per object and must resolve relative to their owning object.
    for (size_t i = 0; i < objects.size(); ++i) {
        const auto &obj = objects[i];
        const auto &lay = layouts[i];
        for (const auto &sym : obj.symbols) {
            if (sym.name.empty()) {
                continue;
            }
            if (sym.kind == RcuSymKind::ExternFunc || sym.kind == RcuSymKind::ExternData) {
                continue; // already handled via thunks
            }
            if (sym.visibility == RcuSymVis::Local && sym.kind != RcuSymKind::Func && sym.name != "Main") {
                continue;
            }
            uint64_t va = 0;
            if (sym.sectionIdx == RCU_TEXT_IDX) {
                va = kImageBase + textRva + preambleSize + lay.textOff + sym.value;
            }
            else if (sym.sectionIdx == RCU_RODATA_IDX) {
                va = kImageBase + rdataRva + lay.rodataOff + sym.value;
            }
            else if (sym.sectionIdx == RCU_DATA_IDX) {
                va = kImageBase + dataRva + lay.dataOff + sym.value;
            }
            else {
                continue;
            }
            symMap.try_emplace(sym.name, va); // first definition wins
        }
    }

    // 8. Build final .text (preamble + user code)
    Buf textBuf;
    textBuf.insert(textBuf.end(), textPre.begin(), textPre.end());
    textBuf.insert(textBuf.end(), mergedText.begin(), mergedText.end());

    // Patch import thunks: jmp [rip + disp32] → IAT entry
    for (size_t i = 0; i < numImports; ++i) {
        uint64_t thunkVA = kImageBase + textRva + thunkOff[i];
        uint64_t iatEntryVA = kImageBase + rdataRva + iatEntryOff[i];
        auto disp = static_cast<int32_t>(iatEntryVA - (thunkVA + 6));
        Patch32(textBuf, thunkOff[i] + 2, static_cast<uint32_t>(disp));
    }

    // Patch entry thunk: call Main (EXE) or DllMain (DLL)
    if (isDll) {
        // DllMain is optional: if not defined, patch the call to target the
        // instruction immediately after it so it falls through to `ret`
        // returning whatever eax happened to hold (Windows
        // default-initialises to 0, but the sub/add rsp frame means the
        // caller sees TRUE from a fresh eax on many ABIs). We make it
        // explicit: if no DllMain, patch to call a tiny inline stub that
        // sets eax=1 then returns.
        //
        // Simple approach: if DllMain absent, replace the call+6-byte nop
        // with `mov eax, 1; nop` so the stub just returns TRUE.
        auto it = symMap.find("DllMain");
        if (it != symMap.end()) {
            uint64_t dllMainVA = it->second;
            uint64_t nextInst = kImageBase + textRva + kCallMainDisp + 4;
            Patch32(textBuf, kCallMainDisp, static_cast<uint32_t>(dllMainVA - nextInst));
        }
        else {
            // No DllMain: replace `E8 00 00 00 00` with `B8 01 00 00 00`
            // (mov eax, 1)
            textBuf[kCallMainDisp - 1] = 0xB8;  // change opcode from E8 (call) to B8 (mov eax,
                                                // imm32)
            Patch32(textBuf, kCallMainDisp, 1); // imm = 1 (TRUE)
        }
    }
    else {
        auto it = symMap.find("Main");
        if (it == symMap.end()) {
            Error("undefined symbol 'Main' — no entry point found");
            return false;
        }
        uint64_t mainVA = it->second;
        uint64_t nextInst = kImageBase + textRva + kCallMainDisp + 4;
        Patch32(textBuf, kCallMainDisp, static_cast<uint32_t>(mainVA - nextInst));
    }

    // Patch entry thunk: call ExitProcess thunk (EXE only)
    if (!isDll) {
        uint64_t exitVA = kImageBase + textRva + thunkOff[importIdx["ExitProcess"]];
        uint64_t nextInst = kImageBase + textRva + kCallExitDisp + 4;
        Patch32(textBuf, kCallExitDisp, static_cast<uint32_t>(exitVA - nextInst));
    }

    // 9. Patch user code relocations

    for (size_t i = 0; i < objects.size(); ++i) {
        const auto &obj = objects[i];
        const auto &lay = layouts[i];
        for (const auto &sec : obj.sections) {
            Buf *buf = nullptr;
            uint32_t baseInBuf = 0;
            uint64_t secBaseVA = 0;
            if (sec.type == RcuSecType::Text) {
                buf = &textBuf;
                baseInBuf = preambleSize + lay.textOff;
                secBaseVA = kImageBase + textRva + preambleSize + lay.textOff;
            }
            else if (sec.type == RcuSecType::RoData) {
                buf = &rdataBuf;
                baseInBuf = lay.rodataOff;
                secBaseVA = kImageBase + rdataRva + lay.rodataOff;
            }
            else if (sec.type == RcuSecType::Data) {
                buf = &mergedData;
                baseInBuf = lay.dataOff;
                secBaseVA = kImageBase + dataRva + lay.dataOff;
            }
            else {
                continue;
            }

            for (const auto &reloc : sec.relocs) {
                if (reloc.symbolIndex >= obj.symbols.size()) {
                    continue;
                }
                const auto &sym = obj.symbols[reloc.symbolIndex];

                // Resolve target VA
                uint64_t targetVA = 0;
                if (sym.kind == RcuSymKind::ExternFunc) {
                    // OS import: resolved via thunk
                    auto it = symMap.find(sym.name);
                    if (it == symMap.end()) {
                        Error("undefined external symbol '" + sym.name + "'");
                        continue;
                    }
                    targetVA = it->second;
                }
                else if (sym.visibility != RcuSymVis::Local && !sym.name.empty() && symMap.contains(sym.name)) {
                    // Named exported symbol, including cross-module
                    // references.
                    targetVA = symMap[sym.name];
                }
                else {
                    // Unnamed or purely local — compute from section index
                    if (sym.sectionIdx == RCU_TEXT_IDX) {
                        targetVA = kImageBase + textRva + preambleSize + lay.textOff + sym.value;
                    }
                    else if (sym.sectionIdx == RCU_RODATA_IDX) {
                        targetVA = kImageBase + rdataRva + lay.rodataOff + sym.value;
                    }
                    else if (sym.sectionIdx == RCU_DATA_IDX) {
                        targetVA = kImageBase + dataRva + lay.dataOff + sym.value;
                    }
                    else {
                        continue;
                    }
                }
                const size_t patchAt = baseInBuf + reloc.sectionOffset;
                const uint64_t siteVA = secBaseVA + reloc.sectionOffset;
                if (reloc.type == RcuRelType::Rel32) {
                    if (patchAt + 4 > buf->size()) {
                        continue;
                    }
                    auto disp = static_cast<int32_t>(targetVA + reloc.addend - (siteVA + 4));
                    Patch32(*buf, patchAt, static_cast<uint32_t>(disp));
                }
                else if (reloc.type == RcuRelType::Abs64) {
                    if (patchAt + 8 > buf->size()) {
                        continue;
                    }
                    Patch64(*buf, patchAt, targetVA + static_cast<uint64_t>(reloc.addend));
                }
                else if (reloc.type == RcuRelType::Abs32) {
                    if (patchAt + 4 > buf->size()) {
                        continue;
                    }
                    Patch32(*buf, patchAt, static_cast<uint32_t>(targetVA + reloc.addend));
                }
            }
        }
    }

    if (!errors.empty()) {
        return false;
    }

    // Build export directory for DLLs
    // We export all pub functions that are marked as exported symbols.
    // Collect exported function names (non-extern, non-local, Func kind).
    std::vector<std::string> exportNames;
    if (isDll) {
        for (const auto &obj : objects) {
            for (const auto &sym : obj.symbols) {
                if (sym.kind == RcuSymKind::Func && sym.visibility != RcuSymVis::Local && !sym.name.empty() &&
                    sym.name != "DllMain" && symMap.contains(sym.name)) {
                    exportNames.push_back(sym.name);
                }
            }
        }
        std::ranges::sort(exportNames);
        exportNames.erase(std::ranges::unique(exportNames).begin(), exportNames.end());
    }

    // Build export directory data (appended to .rdata)
    uint32_t exportDirOff = 0;
    uint32_t exportDirSize = 0;
    if (isDll && !exportNames.empty()) {
        exportDirOff = static_cast<uint32_t>(rdataBuf.size());
        const auto numExports = static_cast<uint32_t>(exportNames.size());

        // Reserve IMAGE_EXPORT_DIRECTORY (40 bytes)
        const size_t expDirPos = rdataBuf.size();
        WriteZeros(rdataBuf, 40);

        // AddressOfFunctions array (RVAs)
        const auto funcArrayOff = static_cast<uint32_t>(rdataBuf.size());
        for (uint32_t i = 0; i < numExports; ++i) {
            WriteU32(rdataBuf, 0); // patched below
        }

        // AddressOfNames array (RVAs to name strings)
        const auto nameArrayOff = static_cast<uint32_t>(rdataBuf.size());
        for (uint32_t i = 0; i < numExports; ++i) {
            WriteU32(rdataBuf, 0); // patched below
        }

        // AddressOfNameOrdinals array
        const auto ordArrayOff = static_cast<uint32_t>(rdataBuf.size());
        for (uint32_t i = 0; i < numExports; ++i) {
            WriteU16(rdataBuf, static_cast<uint16_t>(i));
        }

        // DLL name string
        const auto dllNameStrOff = static_cast<uint32_t>(rdataBuf.size());
        WriteCStr(rdataBuf, System::SharedLibraryFileName(packageName, Target::OS::Windows).c_str());
        PadTo(rdataBuf, 2);

        // Function name strings + patch name/func arrays
        for (uint32_t i = 0; i < numExports; ++i) {
            const auto nameStrOff = static_cast<uint32_t>(rdataBuf.size());
            WriteCStr(rdataBuf, exportNames[i].c_str());
            PadTo(rdataBuf, 2);
            // Patch name array entry
            Patch32(rdataBuf, nameArrayOff + i * 4, rdataRva + nameStrOff);
            // Patch function RVA
            auto it = symMap.find(exportNames[i]);
            if (it != symMap.end()) {
                auto funcRva = static_cast<uint32_t>(it->second - kImageBase);
                Patch32(rdataBuf, funcArrayOff + i * 4, funcRva);
            }
        }

        exportDirSize = static_cast<uint32_t>(rdataBuf.size()) - exportDirOff;

        // Patch IMAGE_EXPORT_DIRECTORY fields
        Patch32(rdataBuf, expDirPos + 0, 0); // Characteristics
        Patch32(rdataBuf, expDirPos + 4,
                static_cast<uint32_t>(std::time(nullptr)));          // TimeDateStamp
        Patch32(rdataBuf, expDirPos + 12, rdataRva + dllNameStrOff); // Name RVA
        Patch32(rdataBuf, expDirPos + 16, 1);                        // Base (ordinal base)
        Patch32(rdataBuf, expDirPos + 20, numExports);               // NumberOfFunctions
        Patch32(rdataBuf, expDirPos + 24, numExports);               // NumberOfNames
        Patch32(rdataBuf, expDirPos + 28,
                rdataRva + funcArrayOff); // AddressOfFunctions
        Patch32(rdataBuf, expDirPos + 32,
                rdataRva + nameArrayOff); // AddressOfNames
        Patch32(rdataBuf, expDirPos + 36,
                rdataRva + ordArrayOff); // AddressOfNameOrdinals
    }

    // 10. Emit PE32+ file
    std::filesystem::create_directories(outputPath.parent_path());
    std::ofstream out(outputPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        Error("cannot open output file: " + outputPath.string());
        return false;
    }
    const auto writeRaw = [&](const void *d, size_t n) { out.write(static_cast<const char *>(d), n); };
    const auto wU16 = [&](uint16_t v) { writeRaw(&v, 2); };
    const auto wU32 = [&](uint32_t v) { writeRaw(&v, 4); };
    const auto wU64 = [&](uint64_t v) { writeRaw(&v, 8); };
    [[maybe_unused]] const auto wU8 = [&](uint8_t v) { writeRaw(&v, 1); };
    const auto wBuf = [&](const Buf &b) { writeRaw(b.data(), b.size()); };
    const auto padTo = [&](uint32_t align) {
        auto pos = static_cast<uint32_t>(out.tellp());
        uint32_t pad = AlignUp(pos, align) - pos;
        static constexpr uint8_t Z[kFileAlign] = {};
        writeRaw(Z, pad);
    };
    const auto wDir = [&](uint32_t rva, uint32_t sz) {
        wU32(rva);
        wU32(sz);
    };
    const auto wSec8 = [&](const char *s) {
        char buf8[8] = {};
        size_t len = std::strlen(s);
        for (size_t k = 0; k < 8 && k < len; ++k) {
            buf8[k] = s[k];
        }
        writeRaw(buf8, 8);
    };

    // DOS header (e_lfanew = 0x40 so PE signature follows immediately)
    static const uint8_t kDosHdr[64] = {
        0x4D, 0x5A, 0x90, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00,
        0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00,
    };
    writeRaw(kDosHdr, 64);

    writeRaw("PE\0\0", 4); // PE signature

    // COFF File Header (20 bytes)
    wU16(kMachineAmd64);
    wU16(static_cast<uint16_t>(numSections));
    wU32(static_cast<uint32_t>(std::time(nullptr)));
    wU32(0);
    wU32(0);   // no COFF symbol table
    wU16(240); // SizeOfOptionalHeader for PE32+
    // EXE: EXECUTABLE | LARGE_ADDRESS_AWARE
    // DLL: EXECUTABLE | LARGE_ADDRESS_AWARE | DLL
    wU16(isDll ? kCharacteristicsDll : static_cast<uint16_t>(0x0022u));

    // Optional Header PE32+ (240 bytes)
    wU16(kMagicPE32P);
    wU8(14);
    wU8(0);                             // Linker version 14.0
    wU32(textFileSize);                 // SizeOfCode
    wU32(rdataFileSize + dataFileSize); // SizeOfInitializedData
    wU32(0);                            // SizeOfUninitializedData
    wU32(textRva);                      // AddressOfEntryPoint (__rux_start at start of .text)
    wU32(textRva);                      // BaseOfCode
    wU64(kImageBase);
    wU32(kSecAlign);
    wU32(kFileAlign);
    wU16(6);
    wU16(0); // MajorOSVersion / MinorOSVersion
    wU16(0);
    wU16(0); // MajorImageVersion / MinorImageVersion
    wU16(6);
    wU16(0); // MajorSubsystemVersion 6.0 (Vista+)
    wU32(0); // Win32VersionValue
    wU32(sizeOfImage);
    wU32(sizeOfHeaders);
    wU32(0); // CheckSum
    wU16(isDll ? kSubsystemGUI : kSubsystemCUI);
    wU16(kDllChars);
    wU64(0x100000ULL); // SizeOfStackReserve (1 MB)
    wU64(0x1000ULL);   // SizeOfStackCommit  (4 KB)
    wU64(0x100000ULL); // SizeOfHeapReserve  (1 MB)
    wU64(0x1000ULL);   // SizeOfHeapCommit   (4 KB)
    wU32(0);           // LoaderFlags
    wU32(16);          // NumberOfRvaAndSizes
    // DataDirectory[16]
    // [0] Export — filled for DLLs, empty for EXEs
    wDir(isDll && exportDirSize > 0 ? rdataRva + exportDirOff : 0, isDll && exportDirSize > 0 ? exportDirSize : 0);
    wDir(rdataRva + importDirOff,
         static_cast<uint32_t>((importDllNames.size() + 1) * 20)); // [1]  Import
    wDir(0, 0);
    wDir(0, 0);
    wDir(0, 0);
    wDir(0, 0);
    wDir(0, 0);
    wDir(0, 0); // [2..7]
    wDir(0, 0);
    wDir(0, 0);
    wDir(0, 0);
    wDir(0, 0);                       // [8..11]
    wDir(rdataRva + iatOff, iatSize); // [12] IAT
    wDir(0, 0);
    wDir(0, 0);
    wDir(0, 0); // [13..15]
    // Section Headers (40 bytes each)
    wSec8(".text");
    wU32(textVirtSize);
    wU32(textRva);
    wU32(textFileSize);
    wU32(textFileOff);
    wU32(0);
    wU32(0);
    wU16(0);
    wU16(0);
    wU32(kScnText);
    wSec8(".rdata");
    wU32(rdataVirtSize);
    wU32(rdataRva);
    wU32(rdataFileSize);
    wU32(rdataFileOff);
    wU32(0);
    wU32(0);
    wU16(0);
    wU16(0);
    wU32(kScnRData);
    if (!mergedData.empty()) {
        wSec8(".data");
        wU32(dataVirtSize);
        wU32(dataRva);
        wU32(dataFileSize);
        wU32(dataFileOff);
        wU32(0);
        wU32(0);
        wU16(0);
        wU16(0);
        wU32(kScnData);
    }
    padTo(kFileAlign);
    // Section data
    wBuf(textBuf);
    padTo(kFileAlign);
    wBuf(rdataBuf);
    padTo(kFileAlign);
    if (!mergedData.empty()) {
        wBuf(mergedData);
        padTo(kFileAlign);
    }
    return errors.empty();
}

} // namespace Rux
