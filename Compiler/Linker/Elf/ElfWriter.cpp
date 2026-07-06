// ELF64 executable writer for Linux, the BSDs, Solaris, and illumos.
//
// Programs that reference no external symbols are emitted as a static
// ET_EXEC (the kernel jumps straight to our entry). Programs that import
// functions from shared libraries (libc.so.6 and friends) are emitted as a
// dynamically linked ET_EXEC with a PT_INTERP/PT_DYNAMIC and a standard
// PLT/GOT: each imported call goes through a PLT stub that the dynamic
// linker binds to the real libc routine. Imported calls use the SysV ABI
// directly — the linker adds no register-shuffle glue.

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <map>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Linker/Linker.h"
#include "Linker/LinkerInternal.h"
#include "Target/Platform.h"

namespace Rux {

// Dynamic-linker interpreter path and default C library for the host ELF OS.
#if RUX_OS_LINUX
static constexpr const char *kElfInterp = "/lib64/ld-linux-x86-64.so.2";
static constexpr const char *kDefaultLib = "libc.so.6";
#elif RUX_OS_FREEBSD
static constexpr const char *kElfInterp = "/libexec/ld-elf.so.1";
static constexpr const char *kDefaultLib = "libc.so.7";
#elif RUX_OS_DRAGONFLY
static constexpr const char *kElfInterp = "/libexec/ld-elf.so.2";
static constexpr const char *kDefaultLib = "libc.so.8";
#elif RUX_OS_OPENBSD
static constexpr const char *kElfInterp = "/usr/libexec/ld.so";
static constexpr const char *kDefaultLib = "libc.so";
#elif RUX_OS_NETBSD
static constexpr const char *kElfInterp = "/libexec/ld.elf_so";
static constexpr const char *kDefaultLib = "libc.so.12";
#elif RUX_IS_SUNOS
static constexpr const char *kElfInterp = "/lib/amd64/ld.so.1";
static constexpr const char *kDefaultLib = "libc.so.1";
#else
static constexpr const char *kElfInterp = "/lib64/ld-linux-x86-64.so.2";
static constexpr const char *kDefaultLib = "libc.so.6";
#endif

// Classic SysV ELF symbol hash (used by DT_HASH).
static uint32_t ElfHash(const std::string &name) {
    uint32_t h = 0;
    for (const unsigned char c : name) {
        h = (h << 4) + c;
        const uint32_t g = h & 0xF0000000u;
        if (g) {
            h ^= g >> 24;
        }
        h &= ~g;
    }
    return h;
}

// Builds the OS-identification ELF note some BSDs require in order to
// execute the binary. Empty on OSes that need none.
static Buf BuildOsNote() {
    Buf n;
#if RUX_OS_NETBSD
    WriteU32(n, 7);                                     // name size ("NetBSD\0")
    WriteU32(n, 4);                                     // desc size
    WriteU32(n, 1);                                     // type (OS version)
    for (const char c : std::string("NetBSD\0\0", 8)) { // padded to 8
        WriteU8(n, static_cast<uint8_t>(c));
    }
    WriteU32(n, 1000000000u);
#elif RUX_OS_OPENBSD
    WriteU32(n, 8); // name size ("OpenBSD\0")
    WriteU32(n, 4); // desc size
    WriteU32(n, 1); // type (NT_OPENBSD_IDENT)
    for (const char c : std::string("OpenBSD\0", 8)) {
        WriteU8(n, static_cast<uint8_t>(c));
    }
    WriteU32(n, 0); // any version
#elif RUX_OS_DRAGONFLY
    WriteU32(n, 10);                                          // name size ("DragonFly\0")
    WriteU32(n, 4);                                           // desc size
    WriteU32(n, 1);                                           // type
    for (const char c : std::string("DragonFly\0\0\0", 12)) { // padded to 12
        WriteU8(n, static_cast<uint8_t>(c));
    }
    WriteU32(n, 0);
#endif
    return n;
}

bool Linker::LinkElf64(const std::filesystem::path &outputPath) {
    static constexpr uint64_t kBase = 0x400000;
    static constexpr uint64_t kPage = 0x1000;
    static constexpr uint32_t kPfX = 0x1;
    static constexpr uint32_t kPfW = 0x2;
    static constexpr uint32_t kPfR = 0x4;

    const auto alignUp = [](const uint64_t v, const uint64_t a) { return (v + a - 1) & ~(a - 1); };

    // 1. Collect the names defined across all objects (non-extern). A
    //    cross-module call produces an ExternFunc relocation whose target is
    //    defined in another object; those are not dynamic-library imports.
    std::unordered_set<std::string> definedSymbols;
    for (const auto &obj : objects) {
        for (const auto &sym : obj.symbols) {
            if (sym.kind != RcuSymKind::ExternFunc && sym.kind != RcuSymKind::ExternData && !sym.name.empty()) {
                definedSymbols.insert(sym.name);
            }
        }
    }

    // 2. Collect explicit library assignments from declarations first. A
    //    call and its extern declaration may live in different RCU objects;
    //    the call-site symbol then has an empty typeName while the declaration
    //    carries the library name.
    std::unordered_map<std::string, std::string> explicitImportLib;
    for (const auto &obj : objects) {
        for (const auto &sym : obj.symbols) {
            if (sym.kind != RcuSymKind::ExternFunc || sym.name.empty() || sym.typeName.empty()) {
                continue;
            }
            const auto [it, inserted] = explicitImportLib.try_emplace(sym.name, sym.typeName);
            if (!inserted && it->second != sym.typeName) {
                Error("external symbol '" + sym.name + "' is assigned to both '" + it->second + "' and '" +
                      sym.typeName + "'");
            }
        }
    }

    // Collect the functions actually referenced by relocations. Prefer the
    // library recorded by an extern declaration in any object, then the
    // call-site metadata, and finally the host's default libc.
    // Imported data is not supported: a direct RIP-relative reference to a
    // library datum would require a copy relocation, which needs the object's
    // size.
    std::unordered_map<std::string, std::string> importLib; // func -> library
    for (const auto &obj : objects) {
        for (const auto &sec : obj.sections) {
            for (const auto &reloc : sec.relocs) {
                if (reloc.symbolIndex >= obj.symbols.size()) {
                    continue;
                }
                const auto &sym = obj.symbols[reloc.symbolIndex];
                if (definedSymbols.contains(sym.name)) {
                    continue;
                }
                if (sym.kind == RcuSymKind::ExternFunc) {
                    const auto explicitIt = explicitImportLib.find(sym.name);
                    const std::string &lib = explicitIt != explicitImportLib.end()
                                               ? explicitIt->second
                                               : (sym.typeName.empty() ? kDefaultLib : sym.typeName);
                    const auto [it, inserted] = importLib.try_emplace(sym.name, lib);
                    if (!inserted && it->second != lib) {
                        Error("external symbol '" + sym.name + "' is referenced from both '" + it->second + "' and '" +
                              lib + "'");
                    }
                }
                else if (sym.kind == RcuSymKind::ExternData) {
                    Error("external data symbol '" + sym.name + "' cannot be imported by the ELF linker");
                }
            }
        }
    }
    if (!errors.empty()) {
        return false;
    }

    // A program that imports from a shared library is linked dynamically and
    // runs against libc; one that imports nothing stays a freestanding static
    // executable. A dynamic program must terminate through libc's exit() so
    // buffered stdio is flushed — a raw exit syscall would discard it — so we
    // pull in exit() as an implicit import (mirroring the PE writer's use of
    // ExitProcess for its entry stub).
    const bool dynamic = !importLib.empty();
    if (dynamic) {
        importLib.try_emplace("exit", kDefaultLib);
    }

    // 3. Entry preamble (__rux_start): align the stack and call Main. A static
    //    program then exits with a raw syscall (no libc); a dynamic one tail
    //    calls libc exit() with Main's return value so stdio is flushed.
    Buf textPre;
    textPre.insert(textPre.end(), {0x48, 0x83, 0xE4, 0xF0}); // and rsp, -16 (align stack)
    const size_t kCallMainDisp = textPre.size() + 1;
    textPre.insert(textPre.end(), {0xE8, 0x00, 0x00, 0x00, 0x00}); // call Main
    textPre.insert(textPre.end(), {0x89, 0xC7});                   // mov edi, eax (exit code)
    size_t kCallExitDisp = 0;
    if (dynamic) {
        kCallExitDisp = textPre.size() + 1;
        textPre.insert(textPre.end(), {0xE8, 0x00, 0x00, 0x00, 0x00}); // call exit@plt
        textPre.insert(textPre.end(), {0xCC});                         // int3 (unreachable)
    }
    else {
#if RUX_IS_BSD || RUX_IS_SUNOS
        textPre.insert(textPre.end(), {0xB8, 0x01, 0x00, 0x00, 0x00}); // mov eax, 1  (BSD/Illumos exit)
#else
        textPre.insert(textPre.end(), {0xB8, 0x3C, 0x00, 0x00, 0x00}); // mov eax, 60 (Linux exit)
#endif
        textPre.insert(textPre.end(), {0x0F, 0x05}); // syscall
    }
    const auto preambleSize = static_cast<uint32_t>(textPre.size());

    // 4. Merge per-object sections.
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

    Buf textBuf;
    textBuf.insert(textBuf.end(), textPre.begin(), textPre.end());
    textBuf.insert(textBuf.end(), mergedText.begin(), mergedText.end());

    // Maps each defined (non-extern) symbol to its virtual address, given the
    // final segment placement. Local data/constant labels are intentionally
    // skipped: generated labels such as __f64_0 recur per object and must
    // resolve relative to their owning object via the section-index path.
    const auto buildDefinedSymMap = [&](uint64_t textVA, uint64_t roVA, uint64_t dataVA) {
        std::unordered_map<std::string, uint64_t> m;
        for (size_t i = 0; i < objects.size(); ++i) {
            const auto &obj = objects[i];
            const auto &lay = layouts[i];
            for (const auto &sym : obj.symbols) {
                if (sym.name.empty() || sym.kind == RcuSymKind::ExternFunc || sym.kind == RcuSymKind::ExternData) {
                    continue;
                }
                if (sym.visibility == RcuSymVis::Local && sym.kind != RcuSymKind::Func && sym.name != "Main") {
                    continue;
                }
                uint64_t va = 0;
                if (sym.sectionIdx == RCU_TEXT_IDX) {
                    va = textVA + preambleSize + lay.textOff + sym.value;
                }
                else if (sym.sectionIdx == RCU_RODATA_IDX) {
                    va = roVA + lay.rodataOff + sym.value;
                }
                else if (sym.sectionIdx == RCU_DATA_IDX) {
                    va = dataVA + lay.dataOff + sym.value;
                }
                else {
                    continue;
                }
                m.try_emplace(sym.name, va);
            }
        }
        return m;
    };

    // Applies every object's relocations against the resolved symbol map.
    const auto applyRelocs = [&](const std::unordered_map<std::string, uint64_t> &symMap, Buf &txt, Buf &ro, Buf &dat,
                                 uint64_t textVA, uint64_t roVA, uint64_t dataVA) {
        for (size_t i = 0; i < objects.size(); ++i) {
            const auto &obj = objects[i];
            const auto &lay = layouts[i];
            for (const auto &sec : obj.sections) {
                Buf *buf = nullptr;
                uint32_t baseInBuf = 0;
                uint64_t secBaseVA = 0;
                if (sec.type == RcuSecType::Text) {
                    buf = &txt;
                    baseInBuf = preambleSize + lay.textOff;
                    secBaseVA = textVA + preambleSize + lay.textOff;
                }
                else if (sec.type == RcuSecType::RoData) {
                    buf = &ro;
                    baseInBuf = lay.rodataOff;
                    secBaseVA = roVA + lay.rodataOff;
                }
                else if (sec.type == RcuSecType::Data) {
                    buf = &dat;
                    baseInBuf = lay.dataOff;
                    secBaseVA = dataVA + lay.dataOff;
                }
                else {
                    continue;
                }

                for (const auto &reloc : sec.relocs) {
                    if (reloc.symbolIndex >= obj.symbols.size()) {
                        continue;
                    }
                    const auto &sym = obj.symbols[reloc.symbolIndex];
                    uint64_t targetVA = 0;
                    if (sym.kind == RcuSymKind::ExternFunc || sym.kind == RcuSymKind::ExternData) {
                        auto it = symMap.find(sym.name);
                        if (it == symMap.end()) {
                            Error("undefined external symbol '" + sym.name + "'");
                            continue;
                        }
                        targetVA = it->second;
                    }
                    else if (sym.visibility != RcuSymVis::Local && !sym.name.empty() && symMap.contains(sym.name)) {
                        targetVA = symMap.at(sym.name);
                    }
                    else if (sym.sectionIdx == RCU_TEXT_IDX) {
                        targetVA = textVA + preambleSize + lay.textOff + sym.value;
                    }
                    else if (sym.sectionIdx == RCU_RODATA_IDX) {
                        targetVA = roVA + lay.rodataOff + sym.value;
                    }
                    else if (sym.sectionIdx == RCU_DATA_IDX) {
                        targetVA = dataVA + lay.dataOff + sym.value;
                    }
                    else {
                        continue;
                    }

                    const size_t patchAt = baseInBuf + reloc.sectionOffset;
                    const uint64_t siteVA = secBaseVA + reloc.sectionOffset;
                    if (reloc.type == RcuRelType::Rel32) {
                        if (patchAt + 4 > buf->size()) {
                            continue;
                        }
                        const auto disp = static_cast<int32_t>(targetVA + reloc.addend - (siteVA + 4));
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
    };

    // Sorted, deterministic list of imported function names.
    std::vector<std::string> importNames;
    importNames.reserve(importLib.size());
    for (const auto &name : importLib | std::views::keys) {
        importNames.push_back(name);
    }
    std::ranges::sort(importNames);

    const Buf osNote = BuildOsNote();
    const bool hasNote = !osNote.empty();

    // Common file emitter shared by both paths.
    const auto emitFile = [&](uint16_t phnum, const Buf &phdrs,
                              const std::vector<std::pair<uint64_t, const Buf *>> &segments, uint64_t entryVA,
                              uint64_t phoff) -> bool {
        std::filesystem::create_directories(outputPath.parent_path());
        std::ofstream out(outputPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            Error("cannot open output file: " + outputPath.string());
            return false;
        }
        const auto writeRaw = [&](const void *d, size_t n) {
            out.write(static_cast<const char *>(d), static_cast<std::streamsize>(n));
        };
        const auto padToOffset = [&](uint64_t offset) {
            static constexpr uint8_t zeros[4096] = {};
            while (static_cast<uint64_t>(out.tellp()) < offset) {
                const uint64_t remaining = offset - static_cast<uint64_t>(out.tellp());
                writeRaw(zeros, static_cast<size_t>(std::min<uint64_t>(remaining, sizeof(zeros))));
            }
        };

        Buf hdr;
        const uint8_t osabi =
#if RUX_OS_FREEBSD
            9; // FreeBSD
#elif RUX_OS_OPENBSD
            12; // OpenBSD
#elif RUX_OS_NETBSD
            2; // NetBSD
#elif RUX_IS_SUNOS
            6; // Solaris/Illumos
#else
            0; // System V (Linux, DragonFly)
#endif
        static constexpr uint8_t kIdent[7] = {0x7F, 'E', 'L', 'F', 2, 1, 1};
        for (const uint8_t b : kIdent) {
            WriteU8(hdr, b);
        }
        WriteU8(hdr, osabi);
        for (int i = 0; i < 8; ++i) {
            WriteU8(hdr, 0);
        }
        WriteU16(hdr, 2);    // ET_EXEC
        WriteU16(hdr, 0x3E); // EM_X86_64
        WriteU32(hdr, 1);    // e_version
        WriteU64(hdr, entryVA);
        WriteU64(hdr, phoff);
        WriteU64(hdr, 0);  // e_shoff
        WriteU32(hdr, 0);  // e_flags
        WriteU16(hdr, 64); // e_ehsize
        WriteU16(hdr, 56); // e_phentsize
        WriteU16(hdr, phnum);
        WriteU16(hdr, 0); // e_shentsize
        WriteU16(hdr, 0); // e_shnum
        WriteU16(hdr, 0); // e_shstrndx
        writeRaw(hdr.data(), hdr.size());
        writeRaw(phdrs.data(), phdrs.size());

        for (const auto &[off, buf] : segments) {
            if (buf->empty()) {
                continue;
            }
            padToOffset(off);
            writeRaw(buf->data(), buf->size());
        }

        out.close();
        if (!out) {
            Error("cannot write output file: " + outputPath.string());
            return false;
        }

        std::error_code ec;
        std::filesystem::permissions(outputPath,
                                     std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
                                         std::filesystem::perms::others_exec,
                                     std::filesystem::perm_options::add, ec);
        if (ec) {
            Error("cannot mark output executable: " + ec.message());
            return false;
        }
        return true;
    };

    const auto writePhdr = [](Buf &b, uint32_t type, uint32_t flags, uint64_t off, uint64_t vaddr, uint64_t fileSize,
                              uint64_t memSize, uint64_t align) {
        WriteU32(b, type);
        WriteU32(b, flags);
        WriteU64(b, off);
        WriteU64(b, vaddr);
        WriteU64(b, vaddr); // p_paddr
        WriteU64(b, fileSize);
        WriteU64(b, memSize);
        WriteU64(b, align);
    };

    if (!dynamic) {
        // --- Static executable: no imports, no interpreter. ---
        const auto phnum = static_cast<uint16_t>(2 + (!mergedData.empty() ? 1 : 0) + (hasNote ? 1 : 0));
        constexpr uint64_t phoff = 64;
        const uint64_t textOff = alignUp(phoff + static_cast<uint64_t>(phnum) * 56, kPage);
        const uint64_t textVA = kBase + textOff;

        // The OS note (when present) leads .rodata so PT_NOTE can point at it.
        Buf rodataBuf = osNote;
        rodataBuf.insert(rodataBuf.end(), mergedRodata.begin(), mergedRodata.end());
        const uint64_t noteRodataDelta = osNote.size();

        const uint64_t rdataOff = alignUp(textOff + textBuf.size(), kPage);
        const uint64_t rdataVA = kBase + rdataOff;
        const uint64_t dataOff = alignUp(rdataOff + rodataBuf.size(), kPage);
        const uint64_t dataVA = kBase + dataOff;

        auto symMap = buildDefinedSymMap(textVA, rdataVA + noteRodataDelta, dataVA);
        auto it = symMap.find("Main");
        if (it == symMap.end()) {
            Error("undefined symbol 'Main' — no entry point found");
            return false;
        }
        const uint64_t nextInst = textVA + kCallMainDisp + 4;
        Patch32(textBuf, kCallMainDisp, static_cast<uint32_t>(it->second - nextInst));

        applyRelocs(symMap, textBuf, rodataBuf, mergedData, textVA, rdataVA + noteRodataDelta, dataVA);
        if (!errors.empty()) {
            return false;
        }

        Buf phdrs;
        writePhdr(phdrs, 1, kPfR | kPfX, textOff, textVA, textBuf.size(), textBuf.size(), kPage);
        writePhdr(phdrs, 1, kPfR, rdataOff, rdataVA, rodataBuf.size(), rodataBuf.size(), kPage);
        if (hasNote) {
            writePhdr(phdrs, 4, kPfR, rdataOff, rdataVA, osNote.size(), osNote.size(), 4);
        }
        if (!mergedData.empty()) {
            writePhdr(phdrs, 1, kPfR | kPfW, dataOff, dataVA, mergedData.size(), mergedData.size(), kPage);
        }

        return emitFile(phnum, phdrs, {{textOff, &textBuf}, {rdataOff, &rodataBuf}, {dataOff, &mergedData}}, textVA,
                        phoff);
    }

    // --- Dynamically linked executable importing from shared libraries. ---
    const size_t n = importNames.size();
    std::unordered_map<std::string, size_t> importIdx;
    for (size_t i = 0; i < n; ++i) {
        importIdx[importNames[i]] = i;
    }

    // Deterministic set of needed libraries.
    std::vector<std::string> neededLibs;
    for (const auto &name : importNames) {
        const std::string &lib = importLib.at(name);
        if (std::ranges::find(neededLibs, lib) == neededLibs.end()) {
            neededLibs.push_back(lib);
        }
    }
    std::ranges::sort(neededLibs);

    // .dynstr: index 0 is the empty string; then each import name and library.
    Buf dynstr;
    dynstr.push_back(0);
    std::unordered_map<std::string, uint32_t> strOff;
    const auto internStr = [&](const std::string &s) -> uint32_t {
        if (const auto it = strOff.find(s); it != strOff.end()) {
            return it->second;
        }
        const auto off = static_cast<uint32_t>(dynstr.size());
        dynstr.insert(dynstr.end(), s.begin(), s.end());
        dynstr.push_back(0);
        strOff[s] = off;
        return off;
    };
    std::vector<uint32_t> nameStrOff(n);
    for (size_t i = 0; i < n; ++i) {
        nameStrOff[i] = internStr(importNames[i]);
    }
    std::vector<uint32_t> libStrOff(neededLibs.size());
    for (size_t i = 0; i < neededLibs.size(); ++i) {
        libStrOff[i] = internStr(neededLibs[i]);
    }

    // .dynsym: index 0 is STN_UNDEF; then one undefined STT_FUNC per import.
    const size_t nsym = n + 1;
    Buf dynsym;
    WriteZeros(dynsym, 24); // null symbol
    for (size_t i = 0; i < n; ++i) {
        WriteU32(dynsym, nameStrOff[i]); // st_name
        WriteU8(dynsym, 0x12);           // st_info: STB_GLOBAL | STT_FUNC
        WriteU8(dynsym, 0);              // st_other
        WriteU16(dynsym, 0);             // st_shndx: SHN_UNDEF
        WriteU64(dynsym, 0);             // st_value
        WriteU64(dynsym, 0);             // st_size
    }

    // .hash (SysV): distribute all dynsym entries across buckets by ElfHash.
    const uint32_t nbucket = static_cast<uint32_t>(std::max<size_t>(1, nsym));
    std::vector<uint32_t> bucket(nbucket, 0);
    std::vector<uint32_t> chain(nsym, 0);
    for (size_t i = n; i >= 1; --i) { // walk backwards so lower indices head the chain
        const uint32_t b = ElfHash(importNames[i - 1]) % nbucket;
        chain[i] = bucket[b];
        bucket[b] = static_cast<uint32_t>(i);
    }
    Buf hash;
    WriteU32(hash, nbucket);
    WriteU32(hash, static_cast<uint32_t>(nsym));
    for (const uint32_t v : bucket) {
        WriteU32(hash, v);
    }
    for (const uint32_t v : chain) {
        WriteU32(hash, v);
    }

    // Interp string.
    Buf interp;
    WriteCStr(interp, kElfInterp);

    // Fixed-size buffers whose bytes are patched once addresses are known.
    const size_t relaSz = n * 24;
    const size_t pltSz = (n + 1) * 16;
    const size_t gotSz = (3 + n) * 8;
    const size_t dynSz = (neededLibs.size() + 11) * 16;

    // 5. Assign file offsets; every section's VA is kBase + its file offset.
    uint64_t off = 64 + static_cast<uint64_t>([&] {
                       // PT_PHDR, PT_INTERP, PT_LOAD(rx), PT_LOAD(rw), PT_DYNAMIC, [PT_NOTE]
                       return (5 + (hasNote ? 1 : 0)) * 56;
                   }());
    constexpr uint64_t phoff = 64;
    const auto phnum = static_cast<uint16_t>(5 + (hasNote ? 1 : 0));

    const uint64_t interpOff = off;
    off += interp.size();
    uint64_t noteOff = 0;
    if (hasNote) {
        noteOff = alignUp(off, 4);
        off = noteOff + osNote.size();
    }
    const uint64_t hashOff = alignUp(off, 8);
    off = hashOff + hash.size();
    const uint64_t dynsymOff = alignUp(off, 8);
    off = dynsymOff + dynsym.size();
    const uint64_t dynstrOff = off;
    off += dynstr.size();
    const uint64_t relaOff = alignUp(off, 8);
    off = relaOff + relaSz;
    const uint64_t pltOff = alignUp(off, 16);
    off = pltOff + pltSz;
    const uint64_t textOff = alignUp(off, 16);
    off = textOff + textBuf.size();
    const uint64_t rodataOff = alignUp(off, 8);
    off = rodataOff + mergedRodata.size();
    const uint64_t rxFileEnd = off;

    // Read/write segment starts on a fresh page.
    const uint64_t dynamicOff = alignUp(rxFileEnd, kPage);
    off = dynamicOff + dynSz;
    const uint64_t gotOff = alignUp(off, 8);
    off = gotOff + gotSz;
    const uint64_t dataOff = alignUp(off, 8);
    // The RW segment's file size must not claim bytes past the last one we
    // actually write (an empty .data would leave the trailing alignment gap
    // unwritten).
    const uint64_t rwFileEnd = mergedData.empty() ? (gotOff + gotSz) : (dataOff + mergedData.size());

    const uint64_t interpVA = kBase + interpOff;
    const uint64_t hashVA = kBase + hashOff;
    const uint64_t dynsymVA = kBase + dynsymOff;
    const uint64_t dynstrVA = kBase + dynstrOff;
    const uint64_t relaVA = kBase + relaOff;
    const uint64_t pltVA = kBase + pltOff;
    const uint64_t textVA = kBase + textOff;
    const uint64_t rodataVA = kBase + rodataOff;
    const uint64_t dynamicVA = kBase + dynamicOff;
    const uint64_t gotVA = kBase + gotOff;
    const uint64_t dataVA = kBase + dataOff;

    // 6. .plt — PLT[0] is the resolver trampoline; PLT[k] (k>=1) binds import
    //    k-1 lazily through its GOT slot.
    Buf plt;
    // PLT[0]: push [rip+GOT+8]; jmp [rip+GOT+16]; pad to 16.
    WriteU8(plt, 0xFF);
    WriteU8(plt, 0x35);
    WriteU32(plt, static_cast<uint32_t>((gotVA + 8) - (pltVA + 6)));
    WriteU8(plt, 0xFF);
    WriteU8(plt, 0x25);
    WriteU32(plt, static_cast<uint32_t>((gotVA + 16) - (pltVA + 12)));
    for (const uint8_t b : {0x0F, 0x1F, 0x40, 0x00}) { // nop dword [rax+0]
        WriteU8(plt, b);
    }
    for (size_t i = 0; i < n; ++i) {
        const uint64_t entryVA = pltVA + (i + 1) * 16;
        const uint64_t gotEntryVA = gotVA + (3 + i) * 8;
        WriteU8(plt, 0xFF); // jmp [rip+got]
        WriteU8(plt, 0x25);
        WriteU32(plt, static_cast<uint32_t>(gotEntryVA - (entryVA + 6)));
        WriteU8(plt, 0x68); // push i
        WriteU32(plt, static_cast<uint32_t>(i));
        WriteU8(plt, 0xE9); // jmp PLT[0]
        WriteU32(plt, static_cast<uint32_t>(pltVA - (entryVA + 16)));
    }

    // 7. .got.plt — [0]=&_DYNAMIC, [1]/[2] filled by the loader, then one slot
    //    per import initialised to its PLT push sequence for lazy binding.
    Buf got;
    WriteU64(got, dynamicVA);
    WriteU64(got, 0);
    WriteU64(got, 0);
    for (size_t i = 0; i < n; ++i) {
        WriteU64(got, pltVA + (i + 1) * 16 + 6); // address of `push i`
    }

    // 8. .rela.plt — one R_X86_64_JUMP_SLOT per import.
    Buf rela;
    for (size_t i = 0; i < n; ++i) {
        WriteU64(rela, gotVA + (3 + i) * 8);                         // r_offset
        WriteU64(rela, (static_cast<uint64_t>(i + 1) << 32) | 7ull); // r_info: sym (i+1), R_X86_64_JUMP_SLOT
        WriteU64(rela, 0);                                           // r_addend
    }

    // 9. .dynamic
    Buf dyn;
    const auto writeDyn = [&](int64_t tag, uint64_t val) {
        WriteU64(dyn, static_cast<uint64_t>(tag));
        WriteU64(dyn, val);
    };
    for (size_t i = 0; i < neededLibs.size(); ++i) {
        writeDyn(1, libStrOff[i]); // DT_NEEDED
    }
    writeDyn(4, hashVA);         // DT_HASH
    writeDyn(5, dynstrVA);       // DT_STRTAB
    writeDyn(6, dynsymVA);       // DT_SYMTAB
    writeDyn(10, dynstr.size()); // DT_STRSZ
    writeDyn(11, 24);            // DT_SYMENT
    writeDyn(3, gotVA);          // DT_PLTGOT
    writeDyn(2, relaSz);         // DT_PLTRELSZ
    writeDyn(20, 7);             // DT_PLTREL = DT_RELA
    writeDyn(23, relaVA);        // DT_JMPREL
    writeDyn(21, 0);             // DT_DEBUG
    writeDyn(0, 0);              // DT_NULL
    if (dyn.size() != dynSz) {
        Error("internal: ELF .dynamic size mismatch");
        return false;
    }

    // 10. Resolve symbols: defined symbols plus each import at its PLT stub.
    auto symMap = buildDefinedSymMap(textVA, rodataVA, dataVA);
    for (size_t i = 0; i < n; ++i) {
        symMap[importNames[i]] = pltVA + (i + 1) * 16;
    }
    auto mainIt = symMap.find("Main");
    if (mainIt == symMap.end()) {
        Error("undefined symbol 'Main' — no entry point found");
        return false;
    }
    const uint64_t nextInst = textVA + kCallMainDisp + 4;
    Patch32(textBuf, kCallMainDisp, static_cast<uint32_t>(mainIt->second - nextInst));

    // Wire the entry stub's `call exit@plt` to libc exit's PLT stub.
    const auto exitIt = symMap.find("exit");
    if (exitIt == symMap.end()) {
        Error("internal: implicit libc 'exit' import was not resolved");
        return false;
    }
    const uint64_t exitNext = textVA + kCallExitDisp + 4;
    Patch32(textBuf, kCallExitDisp, static_cast<uint32_t>(exitIt->second - exitNext));

    applyRelocs(symMap, textBuf, mergedRodata, mergedData, textVA, rodataVA, dataVA);
    if (!errors.empty()) {
        return false;
    }

    // 11. Program headers.
    Buf phdrs;
    writePhdr(phdrs, 6, kPfR, phoff, kBase + phoff, static_cast<uint64_t>(phnum) * 56,
              static_cast<uint64_t>(phnum) * 56, 8);                                 // PT_PHDR
    writePhdr(phdrs, 3, kPfR, interpOff, interpVA, interp.size(), interp.size(), 1); // PT_INTERP
    writePhdr(phdrs, 1, kPfR | kPfX, 0, kBase, rxFileEnd, rxFileEnd, kPage);         // PT_LOAD (r-x)
    writePhdr(phdrs, 1, kPfR | kPfW, dynamicOff, dynamicVA, rwFileEnd - dynamicOff, rwFileEnd - dynamicOff,
              kPage);                                                         // PT_LOAD (rw-)
    writePhdr(phdrs, 2, kPfR | kPfW, dynamicOff, dynamicVA, dynSz, dynSz, 8); // PT_DYNAMIC
    if (hasNote) {
        writePhdr(phdrs, 4, kPfR, noteOff, kBase + noteOff, osNote.size(), osNote.size(), 4); // PT_NOTE
    }

    return emitFile(phnum, phdrs,
                    {{interpOff, &interp},
                     {noteOff, &osNote},
                     {hashOff, &hash},
                     {dynsymOff, &dynsym},
                     {dynstrOff, &dynstr},
                     {relaOff, &rela},
                     {pltOff, &plt},
                     {textOff, &textBuf},
                     {rodataOff, &mergedRodata},
                     {dynamicOff, &dyn},
                     {gotOff, &got},
                     {dataOff, &mergedData}},
                    textVA, phoff);
}

} // namespace Rux
