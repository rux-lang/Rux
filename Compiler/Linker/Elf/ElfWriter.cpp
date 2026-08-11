// ELF64 executable writer for Linux, the BSDs, Solaris, and illumos.
//
// Programs that reference no external symbols are emitted as a static
// ET_EXEC (the kernel jumps straight to our entry). Programs that import
// functions from shared libraries (libc.so.6 and friends) are emitted as a
// dynamically linked ET_EXEC with a PT_INTERP/PT_DYNAMIC and a standard
// PLT/GOT: each imported call goes through a PLT stub that the dynamic
// linker binds to the real libc routine. Imported calls use the SysV ABI
// directly — the linker adds no register-shuffle glue.

#include "Linker/Linker.h"
#include "Linker/LinkerInternal.h"
#include "Target/Platform.h"

#include <algorithm>
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
static constexpr const char *kDefaultLib = "libc.so.103.0";
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

// Linux names its loader after the architecture it runs on, so the path above
// is the x86-64 one and AArch64 needs its own. The BSDs and illumos use a
// single path for every architecture they support, so there the two agree.
#if RUX_OS_LINUX
static constexpr const char *kElfInterpAArch64 = "/lib/ld-linux-aarch64.so.1";
#else
static constexpr const char *kElfInterpAArch64 = kElfInterp;
#endif

// The interpreter the image being linked names in its PT_INTERP.
[[nodiscard]] static constexpr const char *ElfInterpFor(const Target::Arch arch) noexcept {
    return arch == Target::Arch::AArch64 ? kElfInterpAArch64 : kElfInterp;
}

// The machine code of an AArch64 entry stub. Written as words rather than
// assembled, because the linker owns no code generator: the sequence is
// mov x9, sp / and x9, x9, #-16 / mov sp, x9 / bl Main / mov x8, #nr / svc #0.
namespace A64Entry {
constexpr uint32_t MovX9Sp = 0x910003E9;
constexpr uint32_t AndX9Neg16 = 0x927CED29;
constexpr uint32_t MovSpX9 = 0x9100013F;
constexpr uint32_t Bl = 0x94000000;
constexpr uint32_t MovzX8 = 0xD2800008;
constexpr uint32_t Svc0 = 0xD4000001;
} // namespace A64Entry

// Reads back a little-endian instruction word an AArch64 relocation is about
// to rewrite a field of.
[[nodiscard]] static uint32_t ReadU32(const Buf &b, const size_t off) {
    return static_cast<uint32_t>(b[off]) | static_cast<uint32_t>(b[off + 1]) << 8U |
           static_cast<uint32_t>(b[off + 2]) << 16U | static_cast<uint32_t>(b[off + 3]) << 24U;
}

// True when `value` fits in `bits` bits read as two's complement.
[[nodiscard]] static bool FitsSigned(const int64_t value, const unsigned bits) {
    const int64_t limit = int64_t{1} << (bits - 1);
    return value >= -limit && value < limit;
}

// Replaces `width` bits of `word` starting at `lsb` with `value`.
[[nodiscard]] static uint32_t WithField(const uint32_t word, const unsigned lsb, const unsigned width,
                                        const uint32_t value) {
    const uint32_t mask = width == 32 ? ~0U : ((1U << width) - 1U) << lsb;
    return (word & ~mask) | ((value << lsb) & mask);
}

// The number of low bits an LDR / STR immediate drops, which is the log2 of
// its access width. The width lives in the instruction rather than in the
// relocation, split between the two-bit size field and, for the 128-bit vector
// forms, the opc bit that extends it.
[[nodiscard]] static unsigned LoadStoreScale(const uint32_t word) {
    const unsigned size = word >> 30U & 3U;
    const bool vector = (word >> 26U & 1U) != 0;
    if (vector && size == 0 && (word >> 23U & 1U) != 0) {
        return 4; // Q form: 16 bytes
    }
    return size;
}

// Applies one AArch64 relocation, whose defining property is that it names a
// whole instruction and rewrites an immediate field inside it rather than
// overwriting a displacement laid out in the byte stream. Every kind therefore
// reads the word back, replaces its field and writes it again, and a value the
// field cannot hold is reported rather than truncated.
//
// `siteVA` is P, `targetVA + addend` is S + A, in the notation of the ELF for
// the Arm 64-bit Architecture document that names these relocations.
[[nodiscard]] static bool ApplyAArch64Reloc(Buf &buf, const size_t patchAt, const uint16_t type,
                                            const uint64_t targetVA, const int64_t addend, const uint64_t siteVA,
                                            const std::string &symbolName, std::string &error) {
    const uint64_t value = targetVA + static_cast<uint64_t>(addend);
    const auto delta = static_cast<int64_t>(value - siteVA);
    const auto fail = [&](const std::string_view what) {
        error = std::format("{} relocation against '{}' is out of range: {}", RcuRelTypeName(type), symbolName, what);
        return false;
    };

    if (type == RcuRelType::AArch64Prel32 || type == RcuRelType::AArch64Prel64) {
        const size_t width = type == RcuRelType::AArch64Prel64 ? 8 : 4;
        if (patchAt + width > buf.size()) {
            return true;
        }
        if (width == 8) {
            Patch64(buf, patchAt, static_cast<uint64_t>(delta));
        }
        else {
            if (!FitsSigned(delta, 32)) {
                return fail("the displacement does not fit in 32 bits");
            }
            Patch32(buf, patchAt, static_cast<uint32_t>(delta));
        }
        return true;
    }

    if (patchAt + 4 > buf.size()) {
        return true;
    }
    const uint32_t word = ReadU32(buf, patchAt);
    const auto patch = [&](const uint32_t patched) {
        Patch32(buf, patchAt, patched);
        return true;
    };
    switch (type) {
    case RcuRelType::AArch64Call26:
    case RcuRelType::AArch64Jump26:
        if (delta % 4 != 0 || !FitsSigned(delta >> 2, 26)) {
            return fail("a branch reaches 128 MB either way");
        }
        return patch(WithField(word, 0, 26, static_cast<uint32_t>(delta >> 2)));
    case RcuRelType::AArch64CondBr19:
        if (delta % 4 != 0 || !FitsSigned(delta >> 2, 19)) {
            return fail("a conditional branch reaches 1 MB either way");
        }
        return patch(WithField(word, 5, 19, static_cast<uint32_t>(delta >> 2)));
    case RcuRelType::AArch64TstBr14:
        if (delta % 4 != 0 || !FitsSigned(delta >> 2, 14)) {
            return fail("a test-and-branch reaches 32 KB either way");
        }
        return patch(WithField(word, 5, 14, static_cast<uint32_t>(delta >> 2)));
    case RcuRelType::AArch64AdrPrelPgHi21: {
        // ADRP names the 4 KB page the symbol sits on, relative to the page
        // the instruction itself sits on, and the 21-bit result is split with
        // its low two bits high in the word.
        const int64_t pages =
            (static_cast<int64_t>(value & ~0xFFFull) - static_cast<int64_t>(siteVA & ~0xFFFull)) >> 12;
        if (!FitsSigned(pages, 21)) {
            return fail("an ADRP reaches 4 GB either way");
        }
        const auto immediate = static_cast<uint32_t>(pages) & 0x1FFFFFU;
        return patch(WithField(WithField(word, 29, 2, immediate & 3U), 5, 19, immediate >> 2U));
    }
    case RcuRelType::AArch64AddAbsLo12Nc:
        return patch(WithField(word, 10, 12, static_cast<uint32_t>(value & 0xFFFU)));
    case RcuRelType::AArch64LdstAbsLo12Nc: {
        const unsigned scale = LoadStoreScale(word);
        if ((value & ((1ull << scale) - 1)) != 0) {
            return fail("the symbol is not aligned to the access width");
        }
        return patch(WithField(word, 10, 12, static_cast<uint32_t>((value & 0xFFFU) >> scale)));
    }
    case RcuRelType::AArch64MovwUabsG0:
    case RcuRelType::AArch64MovwUabsG1:
    case RcuRelType::AArch64MovwUabsG2:
    case RcuRelType::AArch64MovwUabsG3: {
        // The four together carry the whole 64-bit value, one halfword each,
        // so no single one of them checks for the bits the others take.
        const unsigned shift = 16U * (type - RcuRelType::AArch64MovwUabsG0);
        return patch(WithField(word, 5, 16, static_cast<uint32_t>(value >> shift & 0xFFFFU)));
    }
    default:
        error = std::format("relocation {} against '{}' is not supported by the ELF writer", RcuRelTypeName(type),
                            symbolName);
        return false;
    }
}

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
    WriteU32(n, 7); // name size ("NetBSD\0")
    WriteU32(n, 4); // desc size
    WriteU32(n, 1); // type (OS version)
    for (const char c : std::string("NetBSD\0\0", 8)) {
        // padded to 8
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
    WriteU32(n, 10); // name size ("DragonFly\0")
    WriteU32(n, 4);  // desc size
    WriteU32(n, 1);  // type
    for (const char c : std::string("DragonFly\0\0\0", 12)) {
        // padded to 12
        WriteU8(n, static_cast<uint8_t>(c));
    }
    WriteU32(n, 0);
#endif
    return n;
}

bool Linker::LinkElf64(const std::filesystem::path &outputPath) {
    const bool isShared = artifactKind == ArtifactKind::SharedLibrary;
    const uint64_t imageBase = isShared ? 0 : 0x400000;
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
    const bool dynamic = isShared || !importLib.empty();
    const bool aarch64 = targetArch == Target::Arch::AArch64;
    if (dynamic && aarch64) {
        // Task 29 brings the AArch64 PLT and GOT. Until it does, the AArch64
        // path lays out freestanding static images only, and a program that
        // reaches for a shared library is told which import took it there
        // rather than being handed an image with unbound calls in it. A shared
        // library is refused earlier still, by Linker::CheckArchitecture.
        const auto names = importLib | std::views::keys;
        const auto first = std::ranges::min_element(names);
        Error(first == names.end()
                  ? std::string("dynamic linking for AArch64 is not implemented yet")
                  : std::format("dynamic linking for AArch64 is not implemented yet: '{}' is imported from '{}'",
                                *first, importLib.at(*first)));
        return false;
    }
    if (dynamic && !isShared) {
        importLib.try_emplace("exit", kDefaultLib);
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
            // Linux and Android number exit 93 on AArch64; the BSDs kept the
            // traditional 1. Task 29 replaces this tail with a call to libc
            // exit() for a dynamic image.
            const uint32_t exitSyscall = targetOs == Target::OS::Linux || targetOs == Target::OS::Android ? 93U : 1U;
            WriteU32(textPre, A64Entry::MovzX8 | exitSyscall << 5U); // mov x8, #nr
            WriteU32(textPre, A64Entry::Svc0);                       // svc #0
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
#if RUX_IS_BSD || RUX_IS_SUNOS
                textPre.insert(textPre.end(), {0xB8, 0x01, 0x00, 0x00, 0x00}); // mov eax, 1  (BSD/Illumos exit)
#else
                textPre.insert(textPre.end(), {0xB8, 0x3C, 0x00, 0x00, 0x00}); // mov eax, 60 (Linux exit)
#endif
                textPre.insert(textPre.end(), {0x0F, 0x05}); // syscall
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
        if (!ApplyAArch64Reloc(textBuf, instrOffset, RcuRelType::AArch64Call26, targetVA, 0, siteVA, symbolName,
                               error)) {
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
                        Patch64(*buf, patchAt, value);
                        if (dynamicRelocations != nullptr) {
                            WriteU64(*dynamicRelocations, siteVA); // r_offset
                            WriteU64(*dynamicRelocations, 8);      // R_X86_64_RELATIVE
                            WriteU64(*dynamicRelocations, value);  // r_addend
                        }
                    }
                    else if (reloc.type == RcuRelType::Abs32) {
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
                        if (!ApplyAArch64Reloc(*buf, patchAt, reloc.type, targetVA, reloc.addend, siteVA, sym.name,
                                               error)) {
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
        WriteU16(hdr, isShared ? 3 : 2);      // ET_DYN or ET_EXEC
        WriteU16(hdr, aarch64 ? 0xB7 : 0x3E); // EM_AARCH64 or EM_X86_64
        WriteU32(hdr, 1);                     // e_version
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

#if RUX_IS_BSD
    const auto exportPointerDataSymbol = [&](std::string name) {
        while (mergedData.size() % 8) {
            mergedData.push_back(0);
        }
        const auto dataOffset = static_cast<uint32_t>(mergedData.size());
        WriteU64(mergedData, 0);
        exportedDataSymbols.push_back({std::move(name), dataOffset, 0});
    };
    // BSD libc is normally entered through crt1, which provides these
    // process globals. Rux dynamic ET_EXEC output starts directly at Main, so
    // export null-initialized definitions for libc to bind against.
    exportPointerDataSymbol("environ");
    exportPointerDataSymbol("__progname");
#endif

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
        WriteCStr(interp, ElfInterpFor(targetArch));
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
    const size_t pltSz = (n + 1) * 16;
    const size_t gotSz = (3 + n) * 8;
    const size_t dynSz = (neededLibs.size() + 11 + (isShared ? 1 : 0) + (relaDynSz != 0 ? 3 : 0)) * 16;

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
    for (const uint8_t b : {0x0F, 0x1F, 0x40, 0x00}) {
        // nop dword [rax+0]
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
    if (isShared) {
        writeDyn(14, sonameStrOff); // DT_SONAME
    }
    writeDyn(4, hashVA);         // DT_HASH
    writeDyn(5, dynstrVA);       // DT_STRTAB
    writeDyn(6, dynsymVA);       // DT_SYMTAB
    writeDyn(10, dynstr.size()); // DT_STRSZ
    writeDyn(11, 24);            // DT_SYMENT
    if (relaDynSz != 0) {
        writeDyn(7, relaDynVA); // DT_RELA
        writeDyn(8, relaDynSz); // DT_RELASZ
        writeDyn(9, 24);        // DT_RELAENT
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
        symMap[importNames[i]] = pltVA + (i + 1) * 16;
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
