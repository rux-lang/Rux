// ELF64 executable writer for Linux, the BSDs, Solaris, and illumos.
//
// Programs that reference no external symbols are emitted as a static
// ET_EXEC (the kernel jumps straight to our entry). Programs that import
// functions from shared libraries (libc.so.6 and friends) are emitted as a
// dynamically linked ET_EXEC with a PT_INTERP/PT_DYNAMIC and a standard
// PLT/GOT: each imported call goes through a PLT stub that the dynamic
// linker binds to the real libc routine. Imported calls use the SysV ABI
// directly — the linker adds no register-shuffle glue.

#include "Linker/AArch64Relocation.h"
#include "Linker/Linker.h"
#include "Linker/LinkerInternal.h"
#include "Target/ElfProfile.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <fstream>
#include <map>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Rux {

// The machine code of an AArch64 entry stub. Written as words rather than
// assembled, because the linker owns no code generator: the sequence is
// mov x9, sp / and x9, x9, #-16 / mov sp, x9 / bl Main, closed by either
// mov x8, #nr / svc #0 or bl exit / brk #0.
namespace A64Entry {
constexpr uint32_t MovX9Sp = 0x910003E9;
constexpr uint32_t AndX9Neg16 = 0x927CED29;
constexpr uint32_t MovSpX9 = 0x9100013F;
constexpr uint32_t Bl = 0x94000000;
constexpr uint32_t MovzX8 = 0xD2800008;
constexpr uint32_t Svc0 = 0xD4000001;
constexpr uint32_t Brk0 = 0xD4200000;
} // namespace A64Entry

// The machine code of an AArch64 PLT stub, whose immediates the writer fills in
// through the same relocation kinds the code generator emits. Both the header
// and a per-symbol stub reach their GOT slot with the ADRP / LDR / ADD / BR
// sequence AAELF64 prescribes; the header prepends the push of X16 and the
// return address that the dynamic resolver reads its arguments from, so it is
// two stubs long where a stub is four instructions.
namespace A64Plt {
constexpr uint32_t StpX16X30 = 0xA9BF7BF0; // stp x16, x30, [sp, #-16]!
constexpr uint32_t AdrpX16 = 0x90000010;   // adrp x16, page(&got)
constexpr uint32_t LdrX17X16 = 0xF9400211; // ldr x17, [x16, :lo12:&got]
constexpr uint32_t AddX16 = 0x91000210;    // add x16, x16, :lo12:&got
constexpr uint32_t BrX17 = 0xD61F0220;     // br x17
constexpr uint32_t Nop = 0xD503201F;
} // namespace A64Plt

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
// execute the binary, for the system the image is linked for. Empty on OSes
// that need none.
static Buf BuildOsNote(const Target::OS os) {
    Buf n;
    switch (os) {
    case Target::OS::NetBSD:
        WriteU32(n, 7); // name size ("NetBSD\0")
        WriteU32(n, 4); // desc size
        WriteU32(n, 1); // type (OS version)
        for (const char c : std::string("NetBSD\0\0", 8)) {
            // padded to 8
            WriteU8(n, static_cast<uint8_t>(c));
        }
        WriteU32(n, 1000000000u);
        break;
    case Target::OS::OpenBSD:
        WriteU32(n, 8); // name size ("OpenBSD\0")
        WriteU32(n, 4); // desc size
        WriteU32(n, 1); // type (NT_OPENBSD_IDENT)
        for (const char c : std::string("OpenBSD\0", 8)) {
            WriteU8(n, static_cast<uint8_t>(c));
        }
        WriteU32(n, 0); // any version
        break;
    case Target::OS::DragonFlyBSD:
        WriteU32(n, 10); // name size ("DragonFly\0")
        WriteU32(n, 4);  // desc size
        WriteU32(n, 1);  // type
        for (const char c : std::string("DragonFly\0\0\0", 12)) {
            // padded to 12
            WriteU8(n, static_cast<uint8_t>(c));
        }
        WriteU32(n, 0);
        break;
    default:
        break;
    }
    return n;
}

bool Linker::LinkElf64(const std::filesystem::path &outputPath) {
    const auto selectedProfile = Target::Elf64ProfileFor(targetOs, targetArch);
    if (!selectedProfile) {
        std::string targetName = std::format("{}-{}", Target::ToString(targetOs), Target::ToString(targetArch));
        std::ranges::transform(targetName, targetName.begin(),
                               [](const unsigned char character) { return std::tolower(character); });
        Error(std::format("ELF writer does not implement the complete target '{}'", targetName));
        return false;
    }
    const Target::Elf64Profile &profile = *selectedProfile;
    const bool isShared = artifactKind == ArtifactKind::SharedLibrary;
    const bool aarch64 = profile.arch == Target::Arch::AArch64;
    const bool aarch64Plt = profile.pltInstructionShape == Target::ElfPltInstructionShape::AArch64;
    const uint64_t imageBase = isShared ? profile.sharedImageBase : profile.executableImageBase;
    // The alignment every loadable segment starts on, which is the largest page
    // an image may be mapped with. Two segments sharing a page cannot carry
    // different permissions, so an AArch64 image — where the kernel may be
    // configured for 4 KB, 16 KB or 64 KB pages — separates them by the largest
    // of the three rather than by the one this kernel happens to use.
    const uint64_t kPage = profile.maximumLoadAlignment;
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
                    const std::string &lib =
                        explicitIt != explicitImportLib.end()
                            ? explicitIt->second
                            : (sym.typeName.empty() ? std::string(profile.defaultLibc) : sym.typeName);
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
    const bool dynamic = isShared || !importLib.empty();
    const Target::ElfDynamicRelocations dynRelocs = profile.dynamicRelocations;
    if (dynamic && !isShared) {
        importLib.try_emplace("exit", profile.defaultLibc);
    }

    // 3. Entry preamble (__rux_start): align the stack and call Main. A static
    //    program then exits with a raw syscall (no libc); a dynamic one tail
    //    calls libc exit() with Main's return value so stdio is flushed.
    //
    //    The two architectures differ in where Main leaves its result and in
    //    how they reach a syscall. x86-64 returns in EAX and has to move the
    //    value to EDI to pass it on; AArch64 returns in X0, which is already
    //    where both the exit syscall and a call to exit() read their argument
    //    from, so the value never moves. Both offsets below name the call
    //    instruction itself rather than a field inside it, since an AArch64
    //    relocation rewrites a whole word.
    Buf textPre;
    size_t callMainOffset = 0;
    size_t callExitOffset = 0;
    if (!isShared) {
        if (aarch64) {
            // The kernel enters a fresh process with SP already 16-byte
            // aligned, but AAPCS64 requires that alignment wherever SP
            // addresses memory, which in a Rux frame is everywhere, so the
            // stub establishes the invariant rather than inheriting it. AND
            // cannot read SP, hence the round trip through X9.
            WriteU32(textPre, A64Entry::MovX9Sp);
            WriteU32(textPre, A64Entry::AndX9Neg16);
            WriteU32(textPre, A64Entry::MovSpX9);
            callMainOffset = textPre.size();
            WriteU32(textPre, A64Entry::Bl); // bl Main
            if (dynamic) {
                callExitOffset = textPre.size();
                WriteU32(textPre, A64Entry::Bl);   // bl exit@plt
                WriteU32(textPre, A64Entry::Brk0); // unreachable
            }
            else {
                WriteU32(textPre, A64Entry::MovzX8 | profile.freestandingExitSyscall << 5U); // mov x8, #nr
                WriteU32(textPre, A64Entry::Svc0);                                           // svc #0
            }
        }
        else {
            textPre.insert(textPre.end(), {0x48, 0x83, 0xE4, 0xF0}); // and rsp, -16 (align stack)
            callMainOffset = textPre.size();
            textPre.insert(textPre.end(), {0xE8, 0x00, 0x00, 0x00, 0x00}); // call Main
            textPre.insert(textPre.end(), {0x89, 0xC7});                   // mov edi, eax (exit code)
            if (dynamic) {
                callExitOffset = textPre.size();
                textPre.insert(textPre.end(), {0xE8, 0x00, 0x00, 0x00, 0x00}); // call exit@plt
                textPre.insert(textPre.end(), {0xCC});                         // int3 (unreachable)
            }
            else {
                const auto exitSyscall = static_cast<uint8_t>(profile.freestandingExitSyscall);
                textPre.insert(textPre.end(), {0xB8, exitSyscall, 0x00, 0x00, 0x00}); // mov eax, #nr
                textPre.insert(textPre.end(), {0x0F, 0x05});                          // syscall
            }
        }
    }
    const auto preambleSize = static_cast<uint32_t>(textPre.size());

    // 4. Merge per-object sections.
    struct ObjLayout {
        uint32_t textOff, rodataOff, dataOff;
    };

    std::vector<ObjLayout> layouts(objects.size());
    Buf mergedText, mergedRodata, mergedData;
    // Each object's section starts on the boundary that object asked for. An
    // object aligns its own constants relative to the start of its section, so
    // a preceding object ending on an odd byte would carry the whole of the
    // next one off its alignment — which on AArch64 is not a matter of speed:
    // the low twelve bits of a symbol's address go into a scaled load's
    // immediate, and an address that does not divide by the access width has no
    // encoding at all.
    const auto padToAlignment = [](Buf &buf, const uint16_t alignment) {
        const size_t boundary = std::max<uint16_t>(alignment, 1);
        while (buf.size() % boundary != 0) {
            buf.push_back(0);
        }
    };
    for (size_t i = 0; i < objects.size(); ++i) {
        const auto &obj = objects[i];
        for (const auto &sec : obj.sections) {
            if (sec.type == RcuSecType::Text) {
                padToAlignment(mergedText, sec.alignment);
            }
            else if (sec.type == RcuSecType::RoData) {
                padToAlignment(mergedRodata, sec.alignment);
            }
            else if (sec.type == RcuSecType::Data) {
                padToAlignment(mergedData, sec.alignment);
            }
        }
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

    // Points one call in the entry stub at `targetVA`, in whichever form that
    // architecture's call takes: an x86-64 displacement measured from the
    // instruction after it, or an AArch64 BL whose imm26 counts instructions
    // from the branch itself.
    const auto patchEntryCall = [&](const size_t instrOffset, const uint64_t textVA, const uint64_t targetVA,
                                    const std::string &symbolName) {
        const uint64_t siteVA = textVA + instrOffset;
        if (!aarch64) {
            Patch32(textBuf, instrOffset + 1, static_cast<uint32_t>(targetVA - (siteVA + 5)));
            return true;
        }
        std::string error;
        if (!ApplyAArch64Relocation(textBuf, instrOffset, RcuRelType::AArch64Call26, targetVA, 0, siteVA, symbolName,
                                    "ELF writer", error)) {
            Error(std::move(error));
            return false;
        }
        return true;
    };

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

    // .dynsym index of each imported function, filled in once the dynamic
    // symbol table is laid out below. A pointer to an import stored in a data
    // section resolves to the local PLT stub, which is the right answer only
    // for the image that owns that stub, so in an ET_DYN the slot is left to
    // the loader through a GLOB_DAT naming the symbol instead.
    std::unordered_map<std::string, uint32_t> importDynsymIndex;

    // Applies every object's relocations against the resolved symbol map.
    const auto applyRelocs = [&](const std::unordered_map<std::string, uint64_t> &symMap, Buf &txt, Buf &ro, Buf &dat,
                                 uint64_t textVA, uint64_t roVA, uint64_t dataVA, Buf *dynamicRelocations) {
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
                    if (reloc.type == RcuRelType::None) {
                        continue;
                    }
                    if (!aarch64 && reloc.type == RcuRelType::Rel32) {
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
                        const uint64_t value = targetVA + static_cast<uint64_t>(reloc.addend);
                        if (aarch64) {
                            std::string error;
                            if (!ApplyAArch64Relocation(*buf, patchAt, reloc.type, targetVA, reloc.addend, siteVA,
                                                        sym.name, "ELF writer", error)) {
                                Error(std::move(error));
                            }
                        }
                        else {
                            Patch64(*buf, patchAt, value);
                        }
                        if (dynamicRelocations != nullptr) {
                            const auto importIt = importDynsymIndex.find(sym.name);
                            const bool imported =
                                importIt != importDynsymIndex.end() &&
                                (sym.kind == RcuSymKind::ExternFunc || sym.kind == RcuSymKind::ExternData);
                            WriteU64(*dynamicRelocations, siteVA); // r_offset
                            WriteU64(*dynamicRelocations,
                                     imported ? static_cast<uint64_t>(importIt->second) << 32 | dynRelocs.globDat
                                              : dynRelocs.relative);             // r_info
                            WriteU64(*dynamicRelocations, imported ? 0 : value); // r_addend
                        }
                    }
                    else if (!aarch64 && reloc.type == RcuRelType::Abs32) {
                        if (patchAt + 4 > buf->size()) {
                            continue;
                        }
                        Patch32(*buf, patchAt, static_cast<uint32_t>(targetVA + reloc.addend));
                    }
                    else {
                        // Everything left names an instruction and rewrites a
                        // field inside it. An x86-64 kind reaching here is an
                        // x86-64 displacement in an AArch64 object, which is a
                        // code-generator bug rather than something to patch.
                        std::string error;
                        if (!ApplyAArch64Relocation(*buf, patchAt, reloc.type, targetVA, reloc.addend, siteVA, sym.name,
                                                    "ELF writer", error)) {
                            Error(std::move(error));
                        }
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

    const Buf osNote = BuildOsNote(targetOs);
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
        const uint8_t osabi = profile.osAbi;
        static constexpr uint8_t kIdent[7] = {0x7F, 'E', 'L', 'F', 2, 1, 1};
        for (const uint8_t b : kIdent) {
            WriteU8(hdr, b);
        }
        WriteU8(hdr, osabi);
        for (int i = 0; i < 8; ++i) {
            WriteU8(hdr, 0);
        }
        WriteU16(hdr, isShared ? 3 : 2); // ET_DYN or ET_EXEC
        WriteU16(hdr, profile.machine);  // EM_AARCH64 or EM_X86_64
        WriteU32(hdr, 1);                // e_version
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
        if (!isShared) {
            std::filesystem::permissions(outputPath,
                                         std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
                                             std::filesystem::perms::others_exec,
                                         std::filesystem::perm_options::add, ec);
            if (ec) {
                Error("cannot mark output executable: " + ec.message());
                return false;
            }
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
        const uint64_t textVA = imageBase + textOff;

        // The OS note (when present) leads .rodata so PT_NOTE can point at it.
        Buf rodataBuf = osNote;
        rodataBuf.insert(rodataBuf.end(), mergedRodata.begin(), mergedRodata.end());
        const uint64_t noteRodataDelta = osNote.size();

        const uint64_t rdataOff = alignUp(textOff + textBuf.size(), kPage);
        const uint64_t rdataVA = imageBase + rdataOff;
        const uint64_t dataOff = alignUp(rdataOff + rodataBuf.size(), kPage);
        const uint64_t dataVA = imageBase + dataOff;

        auto symMap = buildDefinedSymMap(textVA, rdataVA + noteRodataDelta, dataVA);
        auto it = symMap.find("Main");
        if (it == symMap.end()) {
            Error("undefined symbol 'Main' — no entry point found");
            return false;
        }
        if (!patchEntryCall(callMainOffset, textVA, it->second, "Main")) {
            return false;
        }

        applyRelocs(symMap, textBuf, rodataBuf, mergedData, textVA, rdataVA + noteRodataDelta, dataVA, nullptr);
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

    // --- Dynamically linked executable or shared object. ---
    const size_t n = importNames.size();

    struct ExportedDataSymbol {
        std::string name;
        uint32_t dataOffset;
        uint32_t dynsymValueOffset;
    };

    std::vector<ExportedDataSymbol> exportedDataSymbols;

    struct SharedExport {
        std::string name;
        uint8_t kind;
        uint32_t size;
        uint32_t dynsymValueOffset;
    };

    std::vector<SharedExport> sharedExports;
    if (isShared) {
        std::map<std::string, SharedExport> exportsByName;
        for (const auto &object : objects) {
            for (const auto &symbol : object.symbols) {
                if (symbol.visibility == RcuSymVis::Local || symbol.name.empty() ||
                    symbol.sectionIdx == RCU_SEC_EXTERNAL) {
                    continue;
                }
                exportsByName.try_emplace(symbol.name, SharedExport{symbol.name, symbol.kind, symbol.size, 0});
            }
        }
        for (auto &[name, symbol] : exportsByName) {
            (void)name;
            sharedExports.push_back(std::move(symbol));
        }
    }

    if (profile.definesBsdProcessGlobals) {
        const auto exportPointerDataSymbol = [&](std::string name) {
            while (mergedData.size() % 8) {
                mergedData.push_back(0);
            }
            const auto dataOffset = static_cast<uint32_t>(mergedData.size());
            WriteU64(mergedData, 0);
            exportedDataSymbols.push_back({std::move(name), dataOffset, 0});
        };
        // BSD libc is normally entered through crt1, which provides these
        // process globals. Rux dynamic ET_EXEC output starts directly at Main,
        // so export null-initialized definitions for libc to bind against.
        exportPointerDataSymbol("environ");
        exportPointerDataSymbol("__progname");
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
    std::vector<uint32_t> exportedNameStrOff(exportedDataSymbols.size());
    for (size_t i = 0; i < exportedDataSymbols.size(); ++i) {
        exportedNameStrOff[i] = internStr(exportedDataSymbols[i].name);
    }
    std::vector<uint32_t> sharedExportNameStrOff(sharedExports.size());
    for (size_t i = 0; i < sharedExports.size(); ++i) {
        sharedExportNameStrOff[i] = internStr(sharedExports[i].name);
    }
    std::vector<uint32_t> libStrOff(neededLibs.size());
    for (size_t i = 0; i < neededLibs.size(); ++i) {
        libStrOff[i] = internStr(neededLibs[i]);
    }
    const uint32_t sonameStrOff = isShared ? internStr(outputPath.filename().string()) : 0;

    // .dynsym: index 0 is STN_UNDEF, followed by undefined STT_FUNC imports
    // and any executable-defined symbols needed by the runtime loader.
    const size_t nsym = n + 1 + exportedDataSymbols.size() + sharedExports.size();
    Buf dynsym;
    WriteZeros(dynsym, 24); // null symbol
    for (size_t i = 0; i < n; ++i) {
        importDynsymIndex[importNames[i]] = static_cast<uint32_t>(i + 1);
        WriteU32(dynsym, nameStrOff[i]); // st_name
        WriteU8(dynsym, 0x12);           // st_info: STB_GLOBAL | STT_FUNC
        WriteU8(dynsym, 0);              // st_other
        WriteU16(dynsym, 0);             // st_shndx: SHN_UNDEF
        WriteU64(dynsym, 0);             // st_value
        WriteU64(dynsym, 0);             // st_size
    }
    for (size_t i = 0; i < exportedDataSymbols.size(); ++i) {
        WriteU32(dynsym, exportedNameStrOff[i]); // st_name
        WriteU8(dynsym, 0x11);                   // st_info: STB_GLOBAL | STT_OBJECT
        WriteU8(dynsym, 0);                      // st_other
        WriteU16(dynsym, 0xFFF1);                // st_shndx: SHN_ABS
        exportedDataSymbols[i].dynsymValueOffset = static_cast<uint32_t>(dynsym.size());
        WriteU64(dynsym, 0); // st_value, patched once dataVA is known
        WriteU64(dynsym, 8); // st_size
    }
    for (size_t i = 0; i < sharedExports.size(); ++i) {
        const auto &symbol = sharedExports[i];
        WriteU32(dynsym, sharedExportNameStrOff[i]);
        WriteU8(dynsym, symbol.kind == RcuSymKind::Func ? 0x12 : 0x11); // global function or object
        WriteU8(dynsym, 0);
        WriteU16(dynsym, 0xFFF1); // SHN_ABS; the image has no section table
        sharedExports[i].dynsymValueOffset = static_cast<uint32_t>(dynsym.size());
        WriteU64(dynsym, 0); // patched after virtual addresses are assigned
        WriteU64(dynsym, symbol.size);
    }

    // .hash (SysV): distribute all dynsym entries across buckets by ElfHash.
    const uint32_t nbucket = static_cast<uint32_t>(std::max<size_t>(1, nsym));
    std::vector<uint32_t> bucket(nbucket, 0);
    std::vector<uint32_t> chain(nsym, 0);
    std::vector<std::string> dynSymbolNames = importNames;
    for (const auto &symbol : exportedDataSymbols) {
        dynSymbolNames.push_back(symbol.name);
    }
    for (const auto &symbol : sharedExports) {
        dynSymbolNames.push_back(symbol.name);
    }
    for (size_t i = dynSymbolNames.size(); i >= 1; --i) {
        // walk backwards so lower indices head the chain
        const uint32_t b = ElfHash(dynSymbolNames[i - 1]) % nbucket;
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
    if (!isShared) {
        WriteCStr(interp, profile.interpreter.data());
    }

    // Fixed-size buffers whose bytes are patched once addresses are known.
    const size_t relaSz = n * 24;
    size_t relativeRelocationCount = 0;
    if (isShared) {
        for (const auto &object : objects) {
            for (const auto &section : object.sections) {
                relativeRelocationCount += static_cast<size_t>(std::ranges::count_if(
                    section.relocs, [](const RcuReloc &relocation) { return relocation.type == RcuRelType::Abs64; }));
            }
        }
    }
    const size_t relaDynSz = relativeRelocationCount * 24;
    // x86-64 opens the PLT with one 16-byte resolver trampoline; the AArch64
    // header is twice that, since it pushes the stub's GOT pointer and the
    // return address before it reaches the resolver the way its own stubs do.
    const size_t pltHeaderSz = profile.pltHeaderSize;
    const size_t pltEntrySz = profile.pltEntrySize;
    const size_t pltSz = pltHeaderSz + n * pltEntrySz;
    const size_t gotSz = (3 + n) * 8;
    // The stub an import's calls are bound to, which is what its name resolves
    // to everywhere in the image.
    const auto pltStubOffset = [&](const size_t index) { return pltHeaderSz + index * pltEntrySz; };
    const size_t dynSz = (neededLibs.size() + 12 + (isShared ? 1 : 0) + (relaDynSz != 0 ? 2 : 0)) * 16;

    // 5. Assign file offsets; every section's VA is imageBase + its file offset.
    constexpr uint64_t phoff = 64;
    const auto phnum = static_cast<uint16_t>((isShared ? 4 : 5) + (hasNote ? 1 : 0));
    uint64_t off = 64 + static_cast<uint64_t>(phnum) * 56;

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
    const uint64_t relaDynOff = alignUp(off, 8);
    off = relaDynOff + relaDynSz;
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

    const uint64_t interpVA = imageBase + interpOff;
    const uint64_t hashVA = imageBase + hashOff;
    const uint64_t dynsymVA = imageBase + dynsymOff;
    const uint64_t dynstrVA = imageBase + dynstrOff;
    const uint64_t relaDynVA = imageBase + relaDynOff;
    const uint64_t relaVA = imageBase + relaOff;
    const uint64_t pltVA = imageBase + pltOff;
    const uint64_t textVA = imageBase + textOff;
    const uint64_t rodataVA = imageBase + rodataOff;
    const uint64_t dynamicVA = imageBase + dynamicOff;
    const uint64_t gotVA = imageBase + gotOff;
    const uint64_t dataVA = imageBase + dataOff;

    for (const auto &symbol : exportedDataSymbols) {
        Patch64(dynsym, symbol.dynsymValueOffset, dataVA + symbol.dataOffset);
    }

    // 6. .plt — the header is the resolver trampoline; the stub after it binds
    //    its import lazily through the import's own GOT slot.
    Buf plt;
    // An AArch64 stub reaches its GOT slot with the same ADRP / LDR / ADD trio
    // the code generator emits, so the three immediates are filled in by the
    // relocation applier rather than computed here. The LDR's scale comes out
    // of the instruction, which is why the slot has to be doubleword aligned —
    // and being a GOT entry, it is.
    const auto writeAArch64PltStub = [&](const uint64_t gotEntryVA) {
        const size_t at = plt.size();
        WriteU32(plt, A64Plt::AdrpX16);
        WriteU32(plt, A64Plt::LdrX17X16);
        WriteU32(plt, A64Plt::AddX16);
        WriteU32(plt, A64Plt::BrX17);
        static constexpr uint16_t kFields[3] = {RcuRelType::AArch64AdrPrelPgHi21, RcuRelType::AArch64LdstAbsLo12Nc,
                                                RcuRelType::AArch64AddAbsLo12Nc};
        for (size_t i = 0; i < 3; ++i) {
            std::string error;
            if (!ApplyAArch64Relocation(plt, at + i * 4, kFields[i], gotEntryVA, 0, pltVA + at + i * 4, "PLT",
                                        "ELF writer", error)) {
                Error(std::move(error));
                return false;
            }
        }
        return true;
    };

    if (aarch64Plt) {
        // PLT[0]: push the stub's GOT pointer and the return address where the
        // resolver reads them, then enter the resolver through .got.plt[2].
        WriteU32(plt, A64Plt::StpX16X30);
        if (!writeAArch64PltStub(gotVA + 16)) {
            return false;
        }
        WriteU32(plt, A64Plt::Nop);
        WriteU32(plt, A64Plt::Nop);
        WriteU32(plt, A64Plt::Nop);
        for (size_t i = 0; i < n; ++i) {
            if (!writeAArch64PltStub(gotVA + (3 + i) * 8)) {
                return false;
            }
        }
    }
    else {
        // PLT[0]: push [rip+GOT+8]; jmp [rip+GOT+16]; pad to 16.
        WriteU8(plt, 0xFF);
        WriteU8(plt, 0x35);
        WriteU32(plt, static_cast<uint32_t>((gotVA + 8) - (pltVA + 6)));
        WriteU8(plt, 0xFF);
        WriteU8(plt, 0x25);
        WriteU32(plt, static_cast<uint32_t>((gotVA + 16) - (pltVA + 12)));
        for (const uint8_t b : {0x0F, 0x1F, 0x40, 0x00}) {
            // nop dword [rax+0]
            WriteU8(plt, b);
        }
        for (size_t i = 0; i < n; ++i) {
            const uint64_t entryVA = pltVA + pltStubOffset(i);
            const uint64_t gotEntryVA = gotVA + (3 + i) * 8;
            WriteU8(plt, 0xFF); // jmp [rip+got]
            WriteU8(plt, 0x25);
            WriteU32(plt, static_cast<uint32_t>(gotEntryVA - (entryVA + 6)));
            WriteU8(plt, 0x68); // push i
            WriteU32(plt, static_cast<uint32_t>(i));
            WriteU8(plt, 0xE9); // jmp PLT[0]
            WriteU32(plt, static_cast<uint32_t>(pltVA - (entryVA + 16)));
        }
    }
    if (plt.size() != pltSz) {
        Error("internal: ELF .plt size mismatch");
        return false;
    }

    // 7. .got.plt — [0]=&_DYNAMIC, [1]/[2] filled by the loader, then one slot
    //    per import holding where an unbound call resumes. x86-64 resumes at
    //    the stub's own `push`; an AArch64 stub has nothing to push, so it
    //    resumes in the header, which pushed the GOT pointer for it.
    Buf got;
    WriteU64(got, dynamicVA);
    WriteU64(got, 0);
    WriteU64(got, 0);
    for (size_t i = 0; i < n; ++i) {
        WriteU64(got, aarch64Plt ? pltVA : pltVA + pltStubOffset(i) + 6);
    }

    // 8. .rela.plt — one JUMP_SLOT per import, in GOT order, which is also the
    //    order the resolver turns a stub's GOT slot back into an index in.
    Buf rela;
    for (size_t i = 0; i < n; ++i) {
        WriteU64(rela, gotVA + (3 + i) * 8);                                     // r_offset
        WriteU64(rela, static_cast<uint64_t>(i + 1) << 32 | dynRelocs.jumpSlot); // r_info
        WriteU64(rela, 0);                                                       // r_addend
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
    if (isShared) {
        writeDyn(14, sonameStrOff); // DT_SONAME
    }
    writeDyn(4, hashVA);         // DT_HASH
    writeDyn(5, dynstrVA);       // DT_STRTAB
    writeDyn(6, dynsymVA);       // DT_SYMTAB
    writeDyn(10, dynstr.size()); // DT_STRSZ
    writeDyn(11, 24);            // DT_SYMENT
    // DT_RELAENT covers .rela.plt as well as .rela.dyn, and DT_PLTREL says the
    // former is RELA, so the entry size is always meaningful even in an image
    // with no .rela.dyn at all.
    writeDyn(9, 24); // DT_RELAENT
    if (relaDynSz != 0) {
        writeDyn(7, relaDynVA); // DT_RELA
        writeDyn(8, relaDynSz); // DT_RELASZ
    }
    writeDyn(3, gotVA);   // DT_PLTGOT
    writeDyn(2, relaSz);  // DT_PLTRELSZ
    writeDyn(20, 7);      // DT_PLTREL = DT_RELA
    writeDyn(23, relaVA); // DT_JMPREL
    writeDyn(21, 0);      // DT_DEBUG
    writeDyn(0, 0);       // DT_NULL
    if (dyn.size() != dynSz) {
        Error("internal: ELF .dynamic size mismatch");
        return false;
    }

    // 10. Resolve symbols: defined symbols plus each import at its PLT stub.
    auto symMap = buildDefinedSymMap(textVA, rodataVA, dataVA);
    for (size_t i = 0; i < n; ++i) {
        symMap[importNames[i]] = pltVA + pltStubOffset(i);
    }
    for (const auto &symbol : sharedExports) {
        const auto it = symMap.find(symbol.name);
        if (it == symMap.end()) {
            Error("internal: exported ELF symbol '" + symbol.name + "' was not resolved");
            return false;
        }
        Patch64(dynsym, symbol.dynsymValueOffset, it->second);
    }
    if (!isShared) {
        const auto mainIt = symMap.find("Main");
        if (mainIt == symMap.end()) {
            Error("undefined symbol 'Main' — no entry point found");
            return false;
        }
        if (!patchEntryCall(callMainOffset, textVA, mainIt->second, "Main")) {
            return false;
        }

        // Wire the entry stub's `call exit@plt` to libc exit's PLT stub.
        const auto exitIt = symMap.find("exit");
        if (exitIt == symMap.end()) {
            Error("internal: implicit libc 'exit' import was not resolved");
            return false;
        }
        if (!patchEntryCall(callExitOffset, textVA, exitIt->second, "exit")) {
            return false;
        }
    }

    Buf relaDyn;
    applyRelocs(symMap, textBuf, mergedRodata, mergedData, textVA, rodataVA, dataVA, isShared ? &relaDyn : nullptr);
    if (!errors.empty()) {
        return false;
    }
    if (relaDyn.size() != relaDynSz) {
        Error("internal: ELF .rela.dyn size mismatch");
        return false;
    }

    // 11. Program headers.
    Buf phdrs;
    writePhdr(phdrs, 6, kPfR, phoff, imageBase + phoff, static_cast<uint64_t>(phnum) * 56,
              static_cast<uint64_t>(phnum) * 56, 8); // PT_PHDR
    if (!isShared) {
        writePhdr(phdrs, 3, kPfR, interpOff, interpVA, interp.size(), interp.size(), 1); // PT_INTERP
    }
    writePhdr(phdrs, 1, kPfR | kPfX, 0, imageBase, rxFileEnd, rxFileEnd, kPage); // PT_LOAD (r-x)
    writePhdr(phdrs, 1, kPfR | kPfW, dynamicOff, dynamicVA, rwFileEnd - dynamicOff, rwFileEnd - dynamicOff,
              kPage);                                                         // PT_LOAD (rw-)
    writePhdr(phdrs, 2, kPfR | kPfW, dynamicOff, dynamicVA, dynSz, dynSz, 8); // PT_DYNAMIC
    if (hasNote) {
        writePhdr(phdrs, 4, kPfR, noteOff, imageBase + noteOff, osNote.size(), osNote.size(), 4); // PT_NOTE
    }

    return emitFile(phnum, phdrs,
                    {{interpOff, &interp},
                     {noteOff, &osNote},
                     {hashOff, &hash},
                     {dynsymOff, &dynsym},
                     {dynstrOff, &dynstr},
                     {relaDynOff, &relaDyn},
                     {relaOff, &rela},
                     {pltOff, &plt},
                     {textOff, &textBuf},
                     {rodataOff, &mergedRodata},
                     {dynamicOff, &dyn},
                     {gotOff, &got},
                     {dataOff, &mergedData}},
                    isShared ? 0 : textVA, phoff);
}
} // namespace Rux
