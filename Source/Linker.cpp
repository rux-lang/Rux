/*
    Rux Compiler
    Copyright © 2026 Ivan Muzyka
    Licensed under the MIT License
*/

#include "Rux/Linker.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <fstream>
#include <unordered_map>

namespace Rux {
    // ── PE32+ layout constants ────────────────────────────────────────────────────

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

    // ── Buffer helpers ────────────────────────────────────────────────────────────

    using Buf = std::vector<uint8_t>;

    static void WriteU8(Buf &b, uint8_t v) { b.push_back(v); }

    static void WriteU16(Buf &b, uint16_t v) {
        b.push_back(v & 0xFF);
        b.push_back(v >> 8);
    }

    static void WriteU32(Buf &b, uint32_t v) {
        b.push_back(v & 0xFF);
        b.push_back((v >> 8) & 0xFF);
        b.push_back((v >> 16) & 0xFF);
        b.push_back((v >> 24) & 0xFF);
    }

    static void WriteU64(Buf &b, uint64_t v) {
        for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>(v >> (i * 8)));
    }

    static void WriteZeros(Buf &b, size_t n) { b.insert(b.end(), n, 0); }

    static void WriteCStr(Buf &b, const char *s) {
        while (*s) b.push_back(*s++);
        b.push_back(0);
    }

    static void WriteName8(Buf &b, const char *s) {
        size_t len = std::strlen(s);
        for (size_t i = 0; i < 8; ++i) b.push_back(i < len ? static_cast<uint8_t>(s[i]) : 0);
    }

    static void PadTo(Buf &b, size_t align, uint8_t fill = 0) {
        while (b.size() % align) b.push_back(fill);
    }

    static uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

    static void Patch32(Buf &b, size_t off, uint32_t v) {
        b[off] = v & 0xFF;
        b[off + 1] = (v >> 8) & 0xFF;
        b[off + 2] = (v >> 16) & 0xFF;
        b[off + 3] = (v >> 24) & 0xFF;
    }

    static void Patch64(Buf &b, size_t off, uint64_t v) {
        for (int i = 0; i < 8; ++i) b[off + i] = static_cast<uint8_t>(v >> (i * 8));
    }

    // ── Linker ────────────────────────────────────────────────────────────────────

    Linker::Linker(std::vector<RcuFile> objects, std::string packageName)
        : objects_(std::move(objects)), packageName_(std::move(packageName)) {
    }

    void Linker::Error(std::string msg) { errors_.push_back({std::move(msg)}); }

    bool Linker::Link(const std::filesystem::path &outputPath) {
        // ── 1. Collect imported external function names ───────────────────────────

        // Always need ExitProcess for the entry thunk
        std::unordered_map<std::string, std::string> importDll;
        importDll["ExitProcess"] = "KERNEL32.DLL";

        for (const auto &obj: objects_) {
            for (const auto &sec: obj.sections) {
                for (const auto &reloc: sec.relocs) {
                    if (reloc.symbolIndex >= obj.symbols.size()) continue;
                    const auto &sym = obj.symbols[reloc.symbolIndex];
                    if (sym.kind == RcuSymKind::ExternFunc)
                        importDll.try_emplace(sym.name, "KERNEL32.DLL");
                }
            }
        }

        // Sorted for determinism
        std::vector<std::string> importNames;
        importNames.reserve(importDll.size());
        for (const auto &[n, _]: importDll) importNames.push_back(n);
        std::sort(importNames.begin(), importNames.end());

        std::unordered_map<std::string, size_t> importIdx;
        for (size_t i = 0; i < importNames.size(); ++i) importIdx[importNames[i]] = i;
        const size_t numImports = importNames.size();

        // ── 2. Build .text preamble (entry thunk + import thunks) ────────────────

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
        for (size_t i = 0; i < numImports; ++i) {
            thunkOff[i] = textPre.size();
            textPre.insert(textPre.end(), {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});
        }

        const uint32_t preambleSize = static_cast<uint32_t>(textPre.size());

        // ── 3. Merge RCU sections ─────────────────────────────────────────────────

        struct ObjLayout {
            uint32_t textOff, rodataOff, dataOff;
        };
        std::vector<ObjLayout> layouts(objects_.size());
        Buf mergedText, mergedRodata, mergedData;

        for (size_t i = 0; i < objects_.size(); ++i) {
            const auto &obj = objects_[i];
            layouts[i] = {
                static_cast<uint32_t>(mergedText.size()),
                static_cast<uint32_t>(mergedRodata.size()),
                static_cast<uint32_t>(mergedData.size())
            };
            for (const auto &sec: obj.sections) {
                if (sec.type == RcuSecType::Text)
                    mergedText.insert(mergedText.end(), sec.data.begin(), sec.data.end());
                else if (sec.type == RcuSecType::RoData)
                    mergedRodata.insert(mergedRodata.end(), sec.data.begin(), sec.data.end());
                else if (sec.type == RcuSecType::Data)
                    mergedData.insert(mergedData.end(), sec.data.begin(), sec.data.end());
            }
        }

        // ── 4. Build import table appended to .rdata ──────────────────────────────
        // Layout within .rdata (after user rodata):
        //   [aligned pad]
        //   [Import Directory Table: 1 descriptor + 1 null = 40 bytes]
        //   [INT: (numImports+1) × 8 bytes]
        //   [IAT: (numImports+1) × 8 bytes]
        //   [DLL name string]
        //   [IMAGE_IMPORT_BY_NAME entries per function]

        Buf rdataBuf;
        rdataBuf.insert(rdataBuf.end(), mergedRodata.begin(), mergedRodata.end());
        PadTo(rdataBuf, 8);

        const uint32_t importDirOff = static_cast<uint32_t>(rdataBuf.size());
        const size_t importDirPos = rdataBuf.size();
        WriteZeros(rdataBuf, 40); // 1 descriptor + null terminator

        const uint32_t intOff = static_cast<uint32_t>(rdataBuf.size());
        const size_t intPos = rdataBuf.size();
        WriteZeros(rdataBuf, (numImports + 1) * 8);

        const uint32_t iatOff = static_cast<uint32_t>(rdataBuf.size());
        const size_t iatPos = rdataBuf.size();
        WriteZeros(rdataBuf, (numImports + 1) * 8);

        const uint32_t dllNameOff = static_cast<uint32_t>(rdataBuf.size());
        WriteCStr(rdataBuf, "KERNEL32.DLL");
        PadTo(rdataBuf, 2);

        std::vector<uint32_t> hintNameOff(numImports);
        for (size_t i = 0; i < numImports; ++i) {
            hintNameOff[i] = static_cast<uint32_t>(rdataBuf.size());
            WriteU16(rdataBuf, 0); // hint
            for (char c: importNames[i]) rdataBuf.push_back(static_cast<uint8_t>(c));
            rdataBuf.push_back(0);
            PadTo(rdataBuf, 2);
        }

        // ── 5. Compute section layout (RVAs and file offsets) ─────────────────────

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
        if (!mergedData.empty()) {
            dataRva = rdataRva + AlignUp(rdataVirtSize, kSecAlign);
            dataVirtSize = static_cast<uint32_t>(mergedData.size());
            dataFileSize = AlignUp(dataVirtSize, kFileAlign);
            dataFileOff = rdataFileOff + rdataFileSize;
        }

        const uint32_t sizeOfImage = !mergedData.empty()
                                         ? dataRva + AlignUp(dataVirtSize, kSecAlign)
                                         : rdataRva + AlignUp(rdataVirtSize, kSecAlign);

        // ── 6. Patch .rdata import table with real RVAs ───────────────────────────

        for (size_t i = 0; i < numImports; ++i) {
            uint64_t hnRva = rdataRva + hintNameOff[i];
            Patch64(rdataBuf, intPos + i * 8, hnRva); // INT entry
            Patch64(rdataBuf, iatPos + i * 8, hnRva); // IAT entry (pre-bind)
        }
        // Patch IMAGE_IMPORT_DESCRIPTOR
        Patch32(rdataBuf, importDirPos + 0, rdataRva + intOff); // OriginalFirstThunk
        Patch32(rdataBuf, importDirPos + 4, 0); // TimeDateStamp
        Patch32(rdataBuf, importDirPos + 8, 0xFFFFFFFFu); // ForwarderChain
        Patch32(rdataBuf, importDirPos + 12, rdataRva + dllNameOff); // Name
        Patch32(rdataBuf, importDirPos + 16, rdataRva + iatOff); // FirstThunk (IAT)
        // null terminator already zeroed

        // ── 7. Build global symbol map (name → VA) ────────────────────────────────

        std::unordered_map<std::string, uint64_t> symMap;

        // Add all imported function thunks first
        for (size_t i = 0; i < numImports; ++i)
            symMap[importNames[i]] = kImageBase + textRva + thunkOff[i];

        // Add symbols defined in each RCU file
        for (size_t i = 0; i < objects_.size(); ++i) {
            const auto &obj = objects_[i];
            const auto &lay = layouts[i];
            for (const auto &sym: obj.symbols) {
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

        // ── 8. Build final .text (preamble + user code) ───────────────────────────

        Buf textBuf;
        textBuf.insert(textBuf.end(), textPre.begin(), textPre.end());
        textBuf.insert(textBuf.end(), mergedText.begin(), mergedText.end());

        // Patch import thunks: jmp [rip + disp32] → IAT entry
        for (size_t i = 0; i < numImports; ++i) {
            uint64_t thunkVA = kImageBase + textRva + thunkOff[i];
            uint64_t iatEntryVA = kImageBase + rdataRva + iatOff + i * 8;
            int32_t disp = static_cast<int32_t>(iatEntryVA - (thunkVA + 6));
            Patch32(textBuf, thunkOff[i] + 2, static_cast<uint32_t>(disp));
        }

        // Patch entry thunk: call Main
        {
            auto it = symMap.find("Main");
            if (it == symMap.end()) {
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

        // ── 9. Patch user code relocations ───────────────────────────────────────

        for (size_t i = 0; i < objects_.size(); ++i) {
            const auto &obj = objects_[i];
            const auto &lay = layouts[i];

            for (const auto &sec: obj.sections) {
                Buf *buf = nullptr;
                uint32_t baseInBuf = 0;
                uint64_t secBaseVA = 0;

                if (sec.type == RcuSecType::Text) {
                    buf = &textBuf;
                    baseInBuf = preambleSize + lay.textOff;
                    secBaseVA = kImageBase + textRva + preambleSize + lay.textOff;
                } else if (sec.type == RcuSecType::RoData) {
                    buf = &rdataBuf;
                    baseInBuf = lay.rodataOff;
                    secBaseVA = kImageBase + rdataRva + lay.rodataOff;
                } else if (sec.type == RcuSecType::Data) {
                    buf = &mergedData;
                    baseInBuf = lay.dataOff;
                    secBaseVA = kImageBase + dataRva + lay.dataOff;
                } else {
                    continue;
                }

                for (const auto &reloc: sec.relocs) {
                    if (reloc.symbolIndex >= obj.symbols.size()) continue;
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
                    } else if (!sym.name.empty() && symMap.count(sym.name)) {
                        // Named symbol (cross-module or local with entry in symMap)
                        targetVA = symMap[sym.name];
                    } else {
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

                    if (reloc.type == RcuRelType::Rel32) {
                        if (patchAt + 4 > buf->size()) continue;
                        int32_t disp = static_cast<int32_t>(targetVA + reloc.addend - (siteVA + 4));
                        Patch32(*buf, patchAt, static_cast<uint32_t>(disp));
                    } else if (reloc.type == RcuRelType::Abs64) {
                        if (patchAt + 8 > buf->size()) continue;
                        Patch64(*buf, patchAt, targetVA + static_cast<uint64_t>(reloc.addend));
                    } else if (reloc.type == RcuRelType::Abs32) {
                        if (patchAt + 4 > buf->size()) continue;
                        Patch32(*buf, patchAt, static_cast<uint32_t>(targetVA + reloc.addend));
                    }
                }
            }
        }

        if (!errors_.empty()) return false;

        // ── 10. Emit PE32+ file ───────────────────────────────────────────────────

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
        const auto wU8 = [&](uint8_t v) { writeRaw(&v, 1); };
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
        wDir(rdataRva + importDirOff, 40); // [1]  Import
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
        wDir(rdataRva + iatOff, static_cast<uint32_t>((numImports + 1) * 8)); // [12] IAT
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

        return errors_.empty();
    }
}
