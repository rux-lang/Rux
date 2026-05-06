/*
    Rux Compiler
    Copyright © 2026 Ivan Muzyka
    Licensed under the MIT License
*/

#include "Rux/Linker.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace Rux
{
    // PE32+ layout constants
    static constexpr uint64_t kImageBase = 0x140000000ULL;
    static constexpr uint32_t kSecAlign = 0x1000; // 4 KB section alignment
    static constexpr uint32_t kFileAlign = 0x200; // 512 B file alignment
    static constexpr uint16_t kMachineAmd64 = 0x8664;
    static constexpr uint16_t kMagicPE32P = 0x020B;
    static constexpr uint16_t kSubsystemCUI = 3; // console

    // IMAGE_SCN_ characteristics
    static constexpr uint32_t kScnText = 0x60000020u; // CNT_CODE | MEM_EXECUTE | MEM_READ
    static constexpr uint32_t kScnRData = 0x40000040u; // CNT_INITIALIZED_DATA | MEM_READ
    static constexpr uint32_t kScnData = 0xC0000040u; // CNT_INITIALIZED_DATA | MEM_READ | MEM_WRITE

    // DllCharacteristics: HIGH_ENTROPY_VA | DYNAMIC_BASE | NX_COMPAT | TERMINAL_SERVER_AWARE
    static constexpr uint16_t kDllChars = 0x8160u;

    // Buffer helpers
    using Buf = std::vector<uint8_t>;
    static void WriteU8(Buf& b, uint8_t v) { b.push_back(v); }

    static void WriteU16(Buf& b, uint16_t v)
    {
        b.push_back(v & 0xFF);
        b.push_back(v >> 8);
    }

    static void WriteU32(Buf& b, uint32_t v)
    {
        b.push_back(v & 0xFF);
        b.push_back((v >> 8) & 0xFF);
        b.push_back((v >> 16) & 0xFF);
        b.push_back((v >> 24) & 0xFF);
    }

    static void WriteU64(Buf& b, uint64_t v)
    {
        for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>(v >> (i * 8)));
    }

    static void WriteZeros(Buf& b, size_t n) { b.insert(b.end(), n, 0); }

    static void WriteCStr(Buf& b, const char* s)
    {
        while (*s) b.push_back(*s++);
        b.push_back(0);
    }

    static void WriteName8(Buf& b, const char* s)
    {
        size_t len = std::strlen(s);
        for (size_t i = 0; i < 8; ++i) b.push_back(i < len ? static_cast<uint8_t>(s[i]) : 0);
    }

    static void PadTo(Buf& b, size_t align, uint8_t fill = 0)
    {
        while (b.size() % align) b.push_back(fill);
    }

    static uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

    static void Patch32(Buf& b, size_t off, uint32_t v)
    {
        b[off] = v & 0xFF;
        b[off + 1] = (v >> 8) & 0xFF;
        b[off + 2] = (v >> 16) & 0xFF;
        b[off + 3] = (v >> 24) & 0xFF;
    }

    static void Patch64(Buf& b, size_t off, uint64_t v)
    {
        for (int i = 0; i < 8; ++i) b[off + i] = static_cast<uint8_t>(v >> (i * 8));
    }

    static bool FileExists(const std::filesystem::path& path)
    {
        std::error_code ec;
        return std::filesystem::is_regular_file(path, ec);
    }

    static std::optional<Buf> ReadFileBytes(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) return std::nullopt;
        const auto size = in.tellg();
        if (size < 0) return std::nullopt;
        Buf data(static_cast<size_t>(size));
        in.seekg(0);
        if (!data.empty())
            in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!in && !in.eof()) return std::nullopt;
        return data;
    }

    static bool ReadU16At(const Buf& b, size_t off, uint16_t& out)
    {
        if (off + 2 > b.size()) return false;
        out = static_cast<uint16_t>(b[off] | (b[off + 1] << 8));
        return true;
    }

    static bool ReadU32At(const Buf& b, size_t off, uint32_t& out)
    {
        if (off + 4 > b.size()) return false;
        out = static_cast<uint32_t>(b[off]) |
            (static_cast<uint32_t>(b[off + 1]) << 8) |
            (static_cast<uint32_t>(b[off + 2]) << 16) |
            (static_cast<uint32_t>(b[off + 3]) << 24);
        return true;
    }

    static std::optional<size_t> PeRvaToOffset(const Buf& pe,
                                               uint32_t rva,
                                               size_t sectionTable,
                                               uint16_t sectionCount)
    {
        for (uint16_t i = 0; i < sectionCount; ++i)
        {
            const size_t sec = sectionTable + static_cast<size_t>(i) * 40;
            uint32_t virtualSize = 0, virtualAddress = 0, rawSize = 0, rawPtr = 0;
            if (!ReadU32At(pe, sec + 8, virtualSize) ||
                !ReadU32At(pe, sec + 12, virtualAddress) ||
                !ReadU32At(pe, sec + 16, rawSize) ||
                !ReadU32At(pe, sec + 20, rawPtr))
                return std::nullopt;

            const uint32_t span = std::max(virtualSize, rawSize);
            if (rva >= virtualAddress && rva < virtualAddress + span)
            {
                const size_t off = static_cast<size_t>(rawPtr) + (rva - virtualAddress);
                if (off < pe.size()) return off;
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    static bool ReadPeCString(const Buf& pe, size_t off, std::string& out)
    {
        if (off >= pe.size()) return false;
        out.clear();
        while (off < pe.size() && pe[off] != 0)
            out.push_back(static_cast<char>(pe[off++]));
        return off < pe.size();
    }

    static std::optional<std::unordered_set<std::string>> ReadDllExports(const std::filesystem::path& path)
    {
        auto peData = ReadFileBytes(path);
        if (!peData) return std::nullopt;
        const Buf& pe = *peData;

        uint32_t peOff32 = 0;
        if (pe.size() < 0x40 || !ReadU32At(pe, 0x3C, peOff32)) return std::nullopt;
        const size_t peOff = peOff32;
        if (peOff + 24 > pe.size() ||
            pe[peOff] != 'P' || pe[peOff + 1] != 'E' || pe[peOff + 2] != 0 || pe[peOff + 3] != 0)
            return std::nullopt;

        uint16_t sectionCount = 0, optionalSize = 0, magic = 0;
        if (!ReadU16At(pe, peOff + 6, sectionCount) ||
            !ReadU16At(pe, peOff + 20, optionalSize) ||
            !ReadU16At(pe, peOff + 24, magic))
            return std::nullopt;

        const size_t optionalOff = peOff + 24;
        const size_t dataDirOff = magic == 0x020B ? optionalOff + 112 : optionalOff + 96;
        uint32_t exportRva = 0, exportSize = 0;
        if (dataDirOff + 8 > optionalOff + optionalSize ||
            !ReadU32At(pe, dataDirOff, exportRva) ||
            !ReadU32At(pe, dataDirOff + 4, exportSize))
            return std::nullopt;

        std::unordered_set<std::string> exports;
        if (exportRva == 0 || exportSize == 0)
            return exports;

        const size_t sectionTable = optionalOff + optionalSize;
        auto exportOff = PeRvaToOffset(pe, exportRva, sectionTable, sectionCount);
        if (!exportOff || *exportOff + 40 > pe.size()) return std::nullopt;

        uint32_t nameCount = 0, namesRva = 0;
        if (!ReadU32At(pe, *exportOff + 24, nameCount) ||
            !ReadU32At(pe, *exportOff + 32, namesRva))
            return std::nullopt;

        auto namesOff = PeRvaToOffset(pe, namesRva, sectionTable, sectionCount);
        if (!namesOff) return std::nullopt;

        for (uint32_t i = 0; i < nameCount; ++i)
        {
            uint32_t nameRva = 0;
            if (!ReadU32At(pe, *namesOff + static_cast<size_t>(i) * 4, nameRva))
                return std::nullopt;

            auto nameOff = PeRvaToOffset(pe, nameRva, sectionTable, sectionCount);
            if (!nameOff) return std::nullopt;

            std::string name;
            if (!ReadPeCString(pe, *nameOff, name))
                return std::nullopt;
            exports.insert(std::move(name));
        }

        return exports;
    }

    static std::string GetPathEnv()
    {
#if defined(_MSC_VER)
        char* value = nullptr;
        size_t size = 0;
        if (_dupenv_s(&value, &size, "PATH") != 0 || value == nullptr)
            return {};
        std::unique_ptr<char, decltype(&std::free)> owned(value, &std::free);
        return std::string(value, size > 0 ? size - 1 : 0);
#else
        const char* value = std::getenv("PATH");
        return value ? std::string(value) : std::string();
#endif
    }

    static std::optional<std::filesystem::path> FindDllFile(
        const std::string& dll,
        const std::vector<std::filesystem::path>& searchDirs,
        const std::filesystem::path& outputDir)
    {
        const std::filesystem::path dllPath(dll);
        if (dllPath.is_absolute())
            return FileExists(dllPath) ? std::optional<std::filesystem::path>(dllPath) : std::nullopt;

        if (!outputDir.empty() && FileExists(outputDir / dllPath))
            return outputDir / dllPath;

        for (const auto& dir : searchDirs)
        {
            if (!dir.empty() && FileExists(dir / dllPath))
                return dir / dllPath;
        }

        if (FileExists(std::filesystem::current_path() / dllPath))
            return std::filesystem::current_path() / dllPath;

        const std::string pathEnv = GetPathEnv();
        if (pathEnv.empty()) return std::nullopt;

        std::stringstream ss(pathEnv);
        std::string dir;
        while (std::getline(ss, dir, ';'))
        {
            if (!dir.empty() && FileExists(std::filesystem::path(dir) / dllPath))
                return std::filesystem::path(dir) / dllPath;
        }

        return std::nullopt;
    }

    Linker::Linker(std::vector<RcuFile> objects,
                   std::string packageName,
                   std::vector<std::filesystem::path> importSearchDirs)
        : objects(std::move(objects)),
          packageName(std::move(packageName)),
          importSearchDirs(std::move(importSearchDirs))
    {
    }

    void Linker::Error(std::string msg) { errors.push_back({std::move(msg)}); }

    bool Linker::Link(const std::filesystem::path& outputPath)
    {
        // 1. Collect imported external function names

        // Always need ExitProcess for the entry thunk
        std::unordered_map<std::string, std::string> importDll;
        std::unordered_set<std::string> explicitImportDlls;
        std::unordered_map<std::string, std::vector<std::string>> explicitImportFuncsByDll;
        importDll["ExitProcess"] = "KERNEL32.DLL";

        // First pass: collect explicit DLL assignments from symbol declarations.
        // This handles the case where a call site and its declaration are in
        // different translation units — the declaration carries the DLL name.
        for (const auto& obj : objects)
        {
            for (const auto& sym : obj.symbols)
            {
                if (sym.kind == RcuSymKind::ExternFunc && !sym.typeName.empty())
                {
                    importDll[sym.name] = sym.typeName;
                    explicitImportDlls.insert(sym.typeName);
                    explicitImportFuncsByDll[sym.typeName].push_back(sym.name);
                }
            }
        }

        // Collect all symbol names that are defined (non-extern) across all objects.
        // Cross-module calls produce ExternFunc relocations but the callee is defined
        // in another RcuFile — those must NOT be treated as OS DLL imports.
        std::unordered_set<std::string> definedSymbols;
        for (const auto& obj : objects)
            for (const auto& sym : obj.symbols)
                if (sym.kind != RcuSymKind::ExternFunc && sym.kind != RcuSymKind::ExternData
                    && !sym.name.empty())
                    definedSymbols.insert(sym.name);

        // Second pass: collect imports from relocations. For compiler-generated
        // extern symbols (e.g. runtime helpers) that carry no explicit DLL, fall
        // back to KERNEL32.DLL so existing behaviour is preserved.
        // Skip symbols that are defined locally (cross-module references, not DLL imports).
        for (const auto& obj : objects)
        {
            for (const auto& sec : obj.sections)
            {
                for (const auto& reloc : sec.relocs)
                {
                    if (reloc.symbolIndex >= obj.symbols.size()) continue;
                    const auto& sym = obj.symbols[reloc.symbolIndex];
                    if (sym.kind == RcuSymKind::ExternFunc
                        && !definedSymbols.count(sym.name))
                        importDll.try_emplace(sym.name, "KERNEL32.DLL");
                }
            }
        }

        // Sorted for determinism
        std::vector<std::string> importNames;
        importNames.reserve(importDll.size());
        for (const auto& [n, _] : importDll) importNames.push_back(n);
        std::sort(importNames.begin(), importNames.end());

        std::unordered_map<std::string, size_t> importIdx;
        for (size_t i = 0; i < importNames.size(); ++i) importIdx[importNames[i]] = i;
        const size_t numImports = importNames.size();

        const auto outputDir = outputPath.parent_path();
        for (const auto& dll : explicitImportDlls)
        {
            auto dllPath = FindDllFile(dll, importSearchDirs, outputDir);
            if (!dllPath)
            {
                Error("import DLL '" + dll + "' was not found");
                continue;
            }

            auto exports = ReadDllExports(*dllPath);
            if (!exports)
            {
                Error("could not read export table from import DLL '" + dll + "'");
                continue;
            }

            for (const auto& func : explicitImportFuncsByDll[dll])
            {
                if (!exports->contains(func))
                    Error("import function '" + func + "' was not found in DLL '" + dll + "'");
            }
        }
        if (!errors.empty()) return false;

        // 2. Build .text preamble (entry thunk + import thunks)

        Buf textPre;

        // __rux_start entry thunk:
        //   sub rsp, 0x28       ; 48 83 EC 28
        //   call Main           ; E8 xx xx xx xx
        //   mov ecx, eax        ; 89 C1~
        //   call ExitProcess    ; E8 xx xx xx xx
        //   int3                ; CC
        textPre.insert(textPre.end(), {0x48, 0x83, 0xEC, 0x28});
        const size_t kCallMainDisp = textPre.size() + 1; // offset of 4-byte disp field
        textPre.insert(textPre.end(), {0xE8, 0x00, 0x00, 0x00, 0x00});
        textPre.insert(textPre.end(), {0x89, 0xC1});
        const size_t kCallExitDisp = textPre.size() + 1;
        textPre.insert(textPre.end(), {0xE8, 0x00, 0x00, 0x00, 0x00});
        textPre.push_back(0xCC);

        // Import thunks: jmp qword ptr [rip+disp32] = FF 25 xx xx xx xx
        std::vector<size_t> thunkOff(numImports);
        for (size_t i = 0; i < numImports; ++i)
        {
            thunkOff[i] = textPre.size();
            textPre.insert(textPre.end(), {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});
        }

        const uint32_t preambleSize = static_cast<uint32_t>(textPre.size());

        // 3. Merge RCU sections

        struct ObjLayout
        {
            uint32_t textOff, rodataOff, dataOff;
        };
        std::vector<ObjLayout> layouts(objects.size());
        Buf mergedText, mergedRodata, mergedData;

        for (size_t i = 0; i < objects.size(); ++i)
        {
            const auto& obj = objects[i];
            layouts[i] = {
                static_cast<uint32_t>(mergedText.size()),
                static_cast<uint32_t>(mergedRodata.size()),
                static_cast<uint32_t>(mergedData.size())
            };
            for (const auto& sec : obj.sections)
            {
                if (sec.type == RcuSecType::Text)
                    mergedText.insert(mergedText.end(), sec.data.begin(), sec.data.end());
                else if (sec.type == RcuSecType::RoData)
                    mergedRodata.insert(mergedRodata.end(), sec.data.begin(), sec.data.end());
                else if (sec.type == RcuSecType::Data)
                    mergedData.insert(mergedData.end(), sec.data.begin(), sec.data.end());
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
        for (size_t i = 0; i < numImports; ++i)
            importsByDll[importDll[importNames[i]]].push_back(i);
        const uint32_t importDirOff = static_cast<uint32_t>(rdataBuf.size());
        const size_t importDirPos = rdataBuf.size();
        WriteZeros(rdataBuf, (importsByDll.size() + 1) * 20);
        std::vector<std::string> importDllNames;
        std::vector<std::vector<size_t>> importDllMembers;
        importDllNames.reserve(importsByDll.size());
        importDllMembers.reserve(importsByDll.size());
        for (auto& [dll, members] : importsByDll)
        {
            importDllNames.push_back(dll);
            importDllMembers.push_back(std::move(members));
        }
        std::vector<uint32_t> intOff(importDllNames.size());
        std::vector<size_t> intPos(importDllNames.size());
        for (size_t g = 0; g < importDllNames.size(); ++g)
        {
            intOff[g] = static_cast<uint32_t>(rdataBuf.size());
            intPos[g] = rdataBuf.size();
            WriteZeros(rdataBuf, (importDllMembers[g].size() + 1) * 8);
        }
        const uint32_t iatOff = static_cast<uint32_t>(rdataBuf.size());
        std::vector<uint32_t> iatGroupOff(importDllNames.size());
        std::vector<size_t> iatPos(importDllNames.size());
        std::vector<uint32_t> iatEntryOff(numImports);
        for (size_t g = 0; g < importDllNames.size(); ++g)
        {
            iatGroupOff[g] = static_cast<uint32_t>(rdataBuf.size());
            iatPos[g] = rdataBuf.size();
            for (size_t j = 0; j < importDllMembers[g].size(); ++j)
                iatEntryOff[importDllMembers[g][j]] = iatGroupOff[g] + static_cast<uint32_t>(j * 8);
            WriteZeros(rdataBuf, (importDllMembers[g].size() + 1) * 8);
        }
        const uint32_t iatSize = static_cast<uint32_t>(rdataBuf.size()) - iatOff;
        std::vector<uint32_t> dllNameOff(importDllNames.size());
        for (size_t g = 0; g < importDllNames.size(); ++g)
        {
            dllNameOff[g] = static_cast<uint32_t>(rdataBuf.size());
            WriteCStr(rdataBuf, importDllNames[g].c_str());
            PadTo(rdataBuf, 2);
        }
        std::vector<uint32_t> hintNameOff(numImports);
        for (size_t i = 0; i < numImports; ++i)
        {
            hintNameOff[i] = static_cast<uint32_t>(rdataBuf.size());
            WriteU16(rdataBuf, 0); // hint
            for (char c : importNames[i]) rdataBuf.push_back(static_cast<uint8_t>(c));
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
        const uint32_t rdataVirtSize = static_cast<uint32_t>(rdataBuf.size());
        const uint32_t rdataFileSize = AlignUp(rdataVirtSize, kFileAlign);
        const uint32_t rdataFileOff = textFileOff + textFileSize;
        uint32_t dataRva = 0, dataVirtSize = 0, dataFileSize = 0, dataFileOff = 0;
        if (!mergedData.empty())
        {
            dataRva = rdataRva + AlignUp(rdataVirtSize, kSecAlign);
            dataVirtSize = static_cast<uint32_t>(mergedData.size());
            dataFileSize = AlignUp(dataVirtSize, kFileAlign);
            dataFileOff = rdataFileOff + rdataFileSize;
        }
        const uint32_t sizeOfImage = !mergedData.empty()
                                         ? dataRva + AlignUp(dataVirtSize, kSecAlign)
                                         : rdataRva + AlignUp(rdataVirtSize, kSecAlign);

        // 6. Patch .rdata import table with real RVAs
        for (size_t g = 0; g < importDllNames.size(); ++g)
        {
            for (size_t j = 0; j < importDllMembers[g].size(); ++j)
            {
                const size_t importIndex = importDllMembers[g][j];
                const uint64_t hnRva = rdataRva + hintNameOff[importIndex];
                Patch64(rdataBuf, intPos[g] + j * 8, hnRva); // INT entry
                Patch64(rdataBuf, iatPos[g] + j * 8, hnRva); // IAT entry (pre-bind)
            }
            // Patch IMAGE_IMPORT_DESCRIPTOR
            const size_t descPos = importDirPos + g * 20;
            Patch32(rdataBuf, descPos + 0, rdataRva + intOff[g]); // OriginalFirstThunk
            Patch32(rdataBuf, descPos + 4, 0); // TimeDateStamp
            Patch32(rdataBuf, descPos + 8, 0xFFFFFFFFu); // ForwarderChain
            Patch32(rdataBuf, descPos + 12, rdataRva + dllNameOff[g]); // Name
            Patch32(rdataBuf, descPos + 16, rdataRva + iatGroupOff[g]); // FirstThunk (IAT)
        }
        // null descriptor and null thunk terminators already zeroed

        // 7. Build global symbol map (name → VA)

        std::unordered_map<std::string, uint64_t> symMap;

        // Add all imported function thunks first
        for (size_t i = 0; i < numImports; ++i)
            symMap[importNames[i]] = kImageBase + textRva + thunkOff[i];

        // Add symbols defined in each RCU file
        for (size_t i = 0; i < objects.size(); ++i)
        {
            const auto& obj = objects[i];
            const auto& lay = layouts[i];
            for (const auto& sym : obj.symbols)
            {
                if (sym.name.empty()) continue;
                if (sym.kind == RcuSymKind::ExternFunc || sym.kind == RcuSymKind::ExternData)
                    continue; // already handled via thunks
                uint64_t va = 0;
                if (sym.sectionIdx == RCU_TEXT_IDX)
                    va = kImageBase + textRva + preambleSize + lay.textOff + sym.value;
                else if (sym.sectionIdx == RCU_RODATA_IDX)
                    va = kImageBase + rdataRva + lay.rodataOff + sym.value;
                else if (sym.sectionIdx == RCU_DATA_IDX)
                    va = kImageBase + dataRva + lay.dataOff + sym.value;
                else
                    continue;
                symMap.try_emplace(sym.name, va); // first definition wins
            }
        }

        // 8. Build final .text (preamble + user code)
        Buf textBuf;
        textBuf.insert(textBuf.end(), textPre.begin(), textPre.end());
        textBuf.insert(textBuf.end(), mergedText.begin(), mergedText.end());

        // Patch import thunks: jmp [rip + disp32] → IAT entry
        for (size_t i = 0; i < numImports; ++i)
        {
            uint64_t thunkVA = kImageBase + textRva + thunkOff[i];
            uint64_t iatEntryVA = kImageBase + rdataRva + iatEntryOff[i];
            int32_t disp = static_cast<int32_t>(iatEntryVA - (thunkVA + 6));
            Patch32(textBuf, thunkOff[i] + 2, static_cast<uint32_t>(disp));
        }

        // Patch entry thunk: call Main
        {
            auto it = symMap.find("Main");
            if (it == symMap.end())
            {
                Error("undefined symbol 'Main' — no entry point found");
                return false;
            }
            uint64_t mainVA = it->second;
            uint64_t nextInst = kImageBase + textRva + kCallMainDisp + 4;
            Patch32(textBuf, kCallMainDisp, static_cast<uint32_t>(mainVA - nextInst));
        }

        // Patch entry thunk: call ExitProcess thunk
        {
            uint64_t exitVA = kImageBase + textRva + thunkOff[importIdx["ExitProcess"]];
            uint64_t nextInst = kImageBase + textRva + kCallExitDisp + 4;
            Patch32(textBuf, kCallExitDisp, static_cast<uint32_t>(exitVA - nextInst));
        }

        // 9. Patch user code relocations

        for (size_t i = 0; i < objects.size(); ++i)
        {
            const auto& obj = objects[i];
            const auto& lay = layouts[i];
            for (const auto& sec : obj.sections)
            {
                Buf* buf = nullptr;
                uint32_t baseInBuf = 0;
                uint64_t secBaseVA = 0;
                if (sec.type == RcuSecType::Text)
                {
                    buf = &textBuf;
                    baseInBuf = preambleSize + lay.textOff;
                    secBaseVA = kImageBase + textRva + preambleSize + lay.textOff;
                }
                else if (sec.type == RcuSecType::RoData)
                {
                    buf = &rdataBuf;
                    baseInBuf = lay.rodataOff;
                    secBaseVA = kImageBase + rdataRva + lay.rodataOff;
                }
                else if (sec.type == RcuSecType::Data)
                {
                    buf = &mergedData;
                    baseInBuf = lay.dataOff;
                    secBaseVA = kImageBase + dataRva + lay.dataOff;
                }
                else
                {
                    continue;
                }

                for (const auto& reloc : sec.relocs)
                {
                    if (reloc.symbolIndex >= obj.symbols.size()) continue;
                    const auto& sym = obj.symbols[reloc.symbolIndex];

                    // Resolve target VA
                    uint64_t targetVA = 0;
                    if (sym.kind == RcuSymKind::ExternFunc)
                    {
                        // OS import: resolved via thunk
                        auto it = symMap.find(sym.name);
                        if (it == symMap.end())
                        {
                            Error("undefined external symbol '" + sym.name + "'");
                            continue;
                        }
                        targetVA = it->second;
                    }
                    else if (!sym.name.empty() && symMap.count(sym.name))
                    {
                        // Named symbol (cross-module or local with entry in symMap)
                        targetVA = symMap[sym.name];
                    }
                    else
                    {
                        // Unnamed or purely local — compute from section index
                        if (sym.sectionIdx == RCU_TEXT_IDX)
                            targetVA = kImageBase + textRva + preambleSize + lay.textOff + sym.value;
                        else if (sym.sectionIdx == RCU_RODATA_IDX)
                            targetVA = kImageBase + rdataRva + lay.rodataOff + sym.value;
                        else if (sym.sectionIdx == RCU_DATA_IDX)
                            targetVA = kImageBase + dataRva + lay.dataOff + sym.value;
                        else
                            continue;
                    }
                    const size_t patchAt = baseInBuf + reloc.sectionOffset;
                    const uint64_t siteVA = secBaseVA + reloc.sectionOffset;
                    if (reloc.type == RcuRelType::Rel32)
                    {
                        if (patchAt + 4 > buf->size()) continue;
                        int32_t disp = static_cast<int32_t>(targetVA + reloc.addend - (siteVA + 4));
                        Patch32(*buf, patchAt, static_cast<uint32_t>(disp));
                    }
                    else if (reloc.type == RcuRelType::Abs64)
                    {
                        if (patchAt + 8 > buf->size()) continue;
                        Patch64(*buf, patchAt, targetVA + static_cast<uint64_t>(reloc.addend));
                    }
                    else if (reloc.type == RcuRelType::Abs32)
                    {
                        if (patchAt + 4 > buf->size()) continue;
                        Patch32(*buf, patchAt, static_cast<uint32_t>(targetVA + reloc.addend));
                    }
                }
            }
        }

        if (!errors.empty()) return false;

        // 10. Emit PE32+ file
        std::filesystem::create_directories(outputPath.parent_path());
        std::ofstream out(outputPath, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            Error("cannot open output file: " + outputPath.string());
            return false;
        }
        const auto writeRaw = [&](const void* d, size_t n) { out.write(static_cast<const char*>(d), n); };
        const auto wU16 = [&](uint16_t v) { writeRaw(&v, 2); };
        const auto wU32 = [&](uint32_t v) { writeRaw(&v, 4); };
        const auto wU64 = [&](uint64_t v) { writeRaw(&v, 8); };
        const auto wU8 = [&](uint8_t v) { writeRaw(&v, 1); };
        const auto wBuf = [&](const Buf& b) { writeRaw(b.data(), b.size()); };
        const auto padTo = [&](uint32_t align)
        {
            auto pos = static_cast<uint32_t>(out.tellp());
            uint32_t pad = AlignUp(pos, align) - pos;
            static constexpr uint8_t Z[kFileAlign] = {};
            writeRaw(Z, pad);
        };
        const auto wDir = [&](uint32_t rva, uint32_t sz)
        {
            wU32(rva);
            wU32(sz);
        };
        const auto wSec8 = [&](const char* s)
        {
            char buf8[8] = {};
            size_t len = std::strlen(s);
            for (size_t k = 0; k < 8 && k < len; ++k) buf8[k] = s[k];
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
        wU32(0); // no COFF symbol table
        wU16(240); // SizeOfOptionalHeader for PE32+
        wU16(0x0022); // Characteristics: EXECUTABLE | LARGE_ADDRESS_AWARE

        // Optional Header PE32+ (240 bytes)
        wU16(kMagicPE32P);
        wU8(14);
        wU8(0); // Linker version 14.0
        wU32(textFileSize); // SizeOfCode
        wU32(rdataFileSize + dataFileSize); // SizeOfInitializedData
        wU32(0); // SizeOfUninitializedData
        wU32(textRva); // AddressOfEntryPoint (__rux_start at start of .text)
        wU32(textRva); // BaseOfCode
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
        wU16(kSubsystemCUI);
        wU16(kDllChars);
        wU64(0x100000ULL); // SizeOfStackReserve (1 MB)
        wU64(0x1000ULL); // SizeOfStackCommit  (4 KB)
        wU64(0x100000ULL); // SizeOfHeapReserve  (1 MB)
        wU64(0x1000ULL); // SizeOfHeapCommit   (4 KB)
        wU32(0); // LoaderFlags
        wU32(16); // NumberOfRvaAndSizes
        // DataDirectory[16]
        wDir(0, 0); // [0]  Export
        wDir(rdataRva + importDirOff, static_cast<uint32_t>((importDllNames.size() + 1) * 20)); // [1]  Import
        wDir(0, 0);
        wDir(0, 0);
        wDir(0, 0);
        wDir(0, 0);
        wDir(0, 0);
        wDir(0, 0); // [2..7]
        wDir(0, 0);
        wDir(0, 0);
        wDir(0, 0);
        wDir(0, 0); // [8..11]
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
        if (!mergedData.empty())
        {
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
        if (!mergedData.empty())
        {
            wBuf(mergedData);
            padTo(kFileAlign);
        }
        return errors.empty();
    }
}
