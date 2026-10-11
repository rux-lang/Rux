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
#include "Linker/RcuObjectLayout.h"
#include "Target/ElfProfile.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <fstream>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <vector>

namespace Rux {
namespace {
/// A package without a namespace is identified by its manifest path ("local:/path/App/Rux.toml::Name"). Profilers
/// show its folder ("App::Name") instead, which is shorter and keeps the build machine's paths out of the image.
std::string ProfilerName(const std::string_view name) {
    static constexpr std::string_view kLocal = "local:";
    const auto separator = name.find("::");
    if (!name.starts_with(kLocal) || separator == std::string_view::npos)
        return std::string(name);
    const auto manifest = name.substr(kLocal.size(), separator - kLocal.size());
    const auto slash = manifest.rfind('/');
    auto folder = slash == std::string_view::npos ? std::string_view{} : manifest.substr(0, slash);
    folder = folder.substr(folder.rfind('/') + 1);
    return std::string(folder) + std::string(name.substr(separator));
}
} // namespace

// The machine code of an AArch64 entry stub. Written as words rather than
// assembled, because the linker owns no code generator: the sequence is
// mov x9, sp / adrp x10, frame / str x9, [x10, :lo12:frame] / and x9, x9, #-16 /
// mov sp, x9 / bl Main, closed by either mov x8, #nr / svc #0 or bl exit / brk #0.
namespace A64Entry {
constexpr uint32_t MovX9Sp = 0x910003E9;
constexpr uint32_t AdrpX10 = 0x9000000A;
constexpr uint32_t StrX9X10 = 0xF9000149;
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

/// Classic SysV ELF symbol hash (used by DT_HASH).
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
    if (!graph) {
        Error("internal: ELF64 linking requires an RCU link graph");
        return false;
    }
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

    // The ABI-tag note, when the target reads one. The kernel keys its compatibility shims on the release it
    // declares and takes an image without one for older than all of them, so every image carries it: four-byte
    // name and descriptor sizes and a type, the NUL-terminated vendor name padded to a word, then the release.
    Buf abiNote;
    if (!profile.abiNoteName.empty()) {
        WriteU32(abiNote, static_cast<uint32_t>(profile.abiNoteName.size() + 1)); // n_namesz
        WriteU32(abiNote, 4);                                                     // n_descsz
        WriteU32(abiNote, 1);                                                     // n_type: NT_*_ABI_TAG
        for (const char character : profile.abiNoteName) {
            WriteU8(abiNote, static_cast<uint8_t>(character));
        }
        WriteU8(abiNote, 0);
        while (abiNote.size() % 4 != 0) {
            WriteU8(abiNote, 0);
        }
        WriteU32(abiNote, profile.abiNoteOsRelease);
    }
    const bool hasAbiNote = !abiNote.empty();
    static constexpr uint32_t kPtNote = 4;

    // 1. Collect explicit library assignments from declarations first. A
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

    // Assign the graph's referenced externals to libraries. Prefer the library
    // recorded by an extern declaration in any object, then the call-site
    // metadata, and finally the target's default libc.
    // Imported data is not supported: direct lowering does not address the
    // datum through a GOT entry, and a copy relocation additionally needs the
    // defining object's size.
    std::unordered_map<std::string, std::string> importLib; // func -> library
    for (const RcuReferencedExternal &external : graph->ReferencedExternals()) {
        if (external.kind == RcuSymKind::ExternData) {
            Error("external data symbol '" + external.name + "' cannot be imported for target '" +
                  std::string(profile.targetName) +
                  "' because GOT-aware data lowering and symbol-size metadata are not implemented");
            continue;
        }
        if (external.kind != RcuSymKind::ExternFunc || external.referenceIndices.empty()) {
            continue;
        }
        const RcuLinkReference &reference = graph->References()[external.referenceIndices.front()];
        const RcuSymbol &callSite = objects[reference.objectIndex].symbols[reference.symbolIndex];
        const auto explicitIt = explicitImportLib.find(external.name);
        const std::string &library =
            explicitIt != explicitImportLib.end()
                ? explicitIt->second
                : (callSite.typeName.empty() ? std::string(profile.defaultLibc) : callSite.typeName);
        const auto [it, inserted] = importLib.try_emplace(external.name, library);
        if (!inserted && it->second != library) {
            Error("external symbol '" + external.name + "' is referenced from both '" + it->second + "' and '" +
                  library + "'");
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
    // Thread-local variables are reached at fixed offsets from the thread pointer, so the image describes their
    // template in PT_TLS and the dynamic loader builds every thread's block from it, the main thread's included. That
    // is why a program with thread-local variables is linked against libc, as a program that starts threads is anyway.
    // A shared library would need the dynamic TLS models, which are not implemented yet.
    const bool hasTls = std::ranges::any_of(objects, [](const RcuFile &object) {
        return std::ranges::any_of(object.sections, [](const RcuSection &section) {
            return section.type == RcuSecType::Tls && !section.data.empty();
        });
    });
    if (hasTls && isShared) {
        Error("thread-local variables are not supported in an ELF shared library yet");
        return false;
    }
    const bool dynamic = isShared || !importLib.empty() || hasTls;
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
    //    Before anything else, a program that reads its start frame keeps the
    //    stack pointer the kernel started it with, which addresses the
    //    argument count, the argument and environment pointers and the
    //    auxiliary vector, in the word the linker defines for
    //    `Core::ProcessStartFrame` to read.
    const bool keepsStartFrame = !isShared && graph->FindDefinition(kProcessStartFrameSymbol).has_value();
    Buf textPre;
    size_t callMainOffset = 0;
    size_t callExitOffset = 0;
    size_t storeFrameOffset = 0;
    if (!isShared) {
        if (aarch64) {
            // The kernel enters a fresh process with SP already 16-byte
            // aligned, but AAPCS64 requires that alignment wherever SP
            // addresses memory, which in a Rux frame is everywhere, so the
            // stub establishes the invariant rather than inheriting it. AND
            // cannot read SP, hence the round trip through X9.
            WriteU32(textPre, A64Entry::MovX9Sp);
            if (keepsStartFrame) {
                storeFrameOffset = textPre.size();
                WriteU32(textPre, A64Entry::AdrpX10);  // adrp x10, frame
                WriteU32(textPre, A64Entry::StrX9X10); // str x9, [x10, :lo12:frame]
            }
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
            if (keepsStartFrame) {
                storeFrameOffset = textPre.size();
                textPre.insert(textPre.end(), {0x48, 0x89, 0x25, 0x00, 0x00, 0x00, 0x00}); // mov [rip+frame], rsp
            }
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
    // 3. Merge RCU sections after the format-owned entry preamble. The shared
    // layout owns per-object alignment and all typed symbol/relocation offsets.
    RcuLayoutPrefixes prefixes;
    prefixes.text = textPre;
    const RcuObjectLayout layout = RcuObjectLayout::Build(objects, prefixes);
    Buf tlsBuf = layout.Data(RcuMergedSection::Tls);
    const uint64_t tlsAlign = std::max<uint64_t>(layout.Alignment(RcuMergedSection::Tls), 1);
    Buf textBuf = layout.Data(RcuMergedSection::Text);
    Buf rodataBuf = layout.Data(RcuMergedSection::RoData);
    Buf dataBuf = layout.Data(RcuMergedSection::Data);

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

    // .dynsym index of each imported function, filled in once the dynamic
    // symbol table is laid out below. A pointer to an import stored in a data
    // section resolves to the local PLT stub, which is the right answer only
    // for the image that owns that stub, so in an ET_DYN the slot is left to
    // the loader through a GLOB_DAT naming the symbol instead.
    std::unordered_map<std::string, uint32_t> importDynsymIndex;

    // Retain ELF's relocation-size diagnostics before graph references are
    // applied. The graph deliberately indexes only structurally valid symbol
    // references, while the format still owns instruction-field validation.
    for (const RcuFile &object : objects) {
        for (const RcuSection &section : object.sections) {
            for (const RcuReloc &relocation : section.relocs) {
                if (relocation.type == RcuRelType::None) {
                    continue;
                }
                if (relocation.symbolIndex >= object.symbols.size()) {
                    Error(std::format("relocation at offset {} in section '{}' has invalid symbol index {}",
                                      relocation.sectionOffset, section.name, relocation.symbolIndex));
                    continue;
                }
                const RcuSymbol &symbol = object.symbols[relocation.symbolIndex];
                const size_t relocationSize =
                    relocation.type == RcuRelType::Abs64 || relocation.type == RcuRelType::AArch64Prel64 ? 8 : 4;
                if (relocation.sectionOffset > section.data.size() ||
                    relocationSize > section.data.size() - relocation.sectionOffset) {
                    Error(std::format("{} relocation against '{}' at offset {} exceeds section '{}'",
                                      RcuRelTypeName(relocation.type), symbol.name, relocation.sectionOffset,
                                      section.name));
                }
            }
        }
    }
    if (!errors.empty()) {
        return false;
    }

    const auto symbolAddress = [&](const RcuSymbolLocation location,
                                   const RcuSectionBases &bases) -> std::optional<uint64_t> {
        const auto placement = layout.Symbol(location);
        return placement ? RcuObjectLayout::Address(*placement, bases) : std::nullopt;
    };

    // Points the stub's store of the initial stack pointer at the start-frame
    // word: a RIP-relative displacement on x86-64, and an ADRP page with a
    // scaled low-twelve offset on AArch64.
    const auto patchStartFrameStore = [&](const uint64_t textVA, const RcuSectionBases &bases) {
        const auto frame = graph->FindDefinition(kProcessStartFrameSymbol);
        const auto frameVA = frame ? symbolAddress(*frame, bases) : std::nullopt;
        if (!frameVA) {
            Error("internal: the process start frame word was not placed");
            return false;
        }
        const uint64_t siteVA = textVA + storeFrameOffset;
        if (!aarch64) {
            Patch32(textBuf, storeFrameOffset + 3, static_cast<uint32_t>(*frameVA - (siteVA + 7)));
            return true;
        }
        const std::string name(kProcessStartFrameSymbol);
        std::string error;
        if (!ApplyAArch64Relocation(textBuf, storeFrameOffset, RcuRelType::AArch64AdrPrelPgHi21, *frameVA, 0, siteVA,
                                    name, "ELF writer", error) ||
            !ApplyAArch64Relocation(textBuf, storeFrameOffset + 4, RcuRelType::AArch64LdstAbsLo12Nc, *frameVA, 0,
                                    siteVA + 4, name, "ELF writer", error)) {
            Error(std::move(error));
            return false;
        }
        return true;
    };

    // Apply graph references to their typed merged-section placements.
    const auto applyRelocs = [&](const std::unordered_map<std::string, uint64_t> &imports, Buf &txt, Buf &ro, Buf &dat,
                                 const RcuSectionBases &bases, Buf *dynamicRelocations) {
        for (const RcuLinkReference &reference : graph->References()) {
            const RcuFile &object = objects[reference.objectIndex];
            const RcuReloc &relocation = object.sections[reference.sectionIndex].relocs[reference.relocationIndex];
            const RcuSymbol &symbol = object.symbols[reference.symbolIndex];
            const auto sitePlacement = layout.Relocation(reference);
            const auto siteAddress = sitePlacement ? RcuObjectLayout::Address(*sitePlacement, bases) : std::nullopt;
            if (!sitePlacement || !siteAddress) {
                continue;
            }

            Buf *buffer = nullptr;
            switch (sitePlacement->section) {
            case RcuMergedSection::Text:
                buffer = &txt;
                break;
            case RcuMergedSection::RoData:
                buffer = &ro;
                break;
            case RcuMergedSection::Data:
                buffer = &dat;
                break;
            case RcuMergedSection::Tls:
                buffer = &tlsBuf;
                break;
            case RcuMergedSection::Bss:
                continue;
            }

            std::optional<uint64_t> targetAddress;
            if (reference.resolution == RcuLinkResolution::External) {
                const auto imported = imports.find(symbol.name);
                if (imported != imports.end()) {
                    targetAddress = imported->second;
                }
            }
            else if (reference.definition) {
                targetAddress = symbolAddress(*reference.definition, bases);
            }
            if (!targetAddress) {
                if (reference.resolution == RcuLinkResolution::External) {
                    Error("undefined external symbol '" + symbol.name + "'");
                }
                continue;
            }

            const size_t patchAt = static_cast<size_t>(sitePlacement->offset);
            if (relocation.type == RcuRelType::TlsOffset32 || relocation.type == RcuRelType::AArch64TlsHi12 ||
                relocation.type == RcuRelType::AArch64TlsLo12Nc) {
                // A thread-local variable's offset from the thread pointer. x86-64 puts the program's block just
                // below the thread pointer (TLS variant II), so the offset is negative; AArch64 puts it after a
                // sixteen-byte control block above the thread pointer (variant I).
                const auto target = reference.definition ? layout.Symbol(*reference.definition) : std::nullopt;
                if (!target || target->section != RcuMergedSection::Tls) {
                    const std::string error = std::format(
                        "thread-local relocation to '{}' names a variable that is not thread-local", symbol.name);
                    Error(error, RelocationNotes(reference, error));
                    continue;
                }
                if (!aarch64) {
                    const auto offset = static_cast<int64_t>(target->offset) + relocation.addend -
                                        static_cast<int64_t>(alignUp(tlsBuf.size(), tlsAlign));
                    Patch32(*buffer, patchAt, static_cast<uint32_t>(static_cast<int32_t>(offset)));
                    continue;
                }
                std::string error;
                if (!ApplyAArch64Relocation(*buffer, patchAt, relocation.type, alignUp(16, tlsAlign) + target->offset,
                                            relocation.addend, *siteAddress, symbol.name, "ELF writer", error)) {
                    auto notes = RelocationNotes(reference, error);
                    Error(std::move(error), std::move(notes));
                }
                continue;
            }
            if (!aarch64 && relocation.type == RcuRelType::Rel32) {
                uint64_t value = 0;
                int64_t displacement = 0;
                if (!AddSignedAddress(*targetAddress, relocation.addend, value) ||
                    !SignedAddressDelta(value, *siteAddress + 4, displacement) ||
                    displacement < std::numeric_limits<int32_t>::min() ||
                    displacement > std::numeric_limits<int32_t>::max()) {
                    const std::string error =
                        std::format("x86-64 PC-relative relocation to '{}' is out of range", symbol.name);
                    Error(error, RelocationNotes(reference, error));
                    continue;
                }
                Patch32(*buffer, patchAt, static_cast<uint32_t>(static_cast<int32_t>(displacement)));
            }
            else if (relocation.type == RcuRelType::Abs64) {
                uint64_t value = 0;
                if (!AddSignedAddress(*targetAddress, relocation.addend, value)) {
                    const std::string error =
                        std::format("relocation to '{}' overflows the 64-bit target address", symbol.name);
                    Error(error, RelocationNotes(reference, error));
                    continue;
                }
                if (aarch64) {
                    std::string error;
                    if (!ApplyAArch64Relocation(*buffer, patchAt, relocation.type, *targetAddress, relocation.addend,
                                                *siteAddress, symbol.name, "ELF writer", error)) {
                        auto notes = RelocationNotes(reference, error);
                        Error(std::move(error), std::move(notes));
                    }
                }
                else {
                    Patch64(*buffer, patchAt, value);
                }
                if (dynamicRelocations != nullptr) {
                    const auto import = importDynsymIndex.find(symbol.name);
                    const bool imported =
                        reference.resolution == RcuLinkResolution::External && import != importDynsymIndex.end();
                    WriteU64(*dynamicRelocations, *siteAddress);
                    WriteU64(*dynamicRelocations, imported
                                                      ? static_cast<uint64_t>(import->second) << 32 | dynRelocs.globDat
                                                      : dynRelocs.relative);
                    WriteU64(*dynamicRelocations, imported ? 0 : value);
                }
            }
            else if (!aarch64 && relocation.type == RcuRelType::Abs32) {
                uint64_t value = 0;
                if (!AddSignedAddress(*targetAddress, relocation.addend, value) ||
                    value > std::numeric_limits<uint32_t>::max()) {
                    const std::string error =
                        std::format("ABS_32 relocation to '{}' does not fit in 32 bits", symbol.name);
                    Error(error, RelocationNotes(reference, error));
                    continue;
                }
                Patch32(*buffer, patchAt, static_cast<uint32_t>(value));
            }
            else {
                std::string error;
                if (!ApplyAArch64Relocation(*buffer, patchAt, relocation.type, *targetAddress, relocation.addend,
                                            *siteAddress, symbol.name, "ELF writer", error)) {
                    auto notes = RelocationNotes(reference, error);
                    Error(std::move(error), std::move(notes));
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

    // Optional non-allocated .symtab naming every placed function (build --emit symbols), so profilers such as perf
    // attribute samples to functions. Section headers describe only .text and the symbol table; segments still own
    // every loaded byte.
    struct SymbolSections {
        uint64_t textOff = 0;
        uint64_t textVA = 0;
        uint64_t textSize = 0;
        uint32_t count = 0;
        Buf symtab;
        Buf strtab;
    };

    std::optional<SymbolSections> symbolSections;
    const auto prepareSymbols = [&](const uint64_t textOff, const uint64_t textVA, const RcuSectionBases &bases) {
        if (!emitSymbols) {
            return;
        }
        SymbolSections sections;
        sections.textOff = textOff;
        sections.textVA = textVA;
        sections.textSize = textBuf.size();
        std::vector<std::pair<uint64_t, const RcuLinkDefinition *>> functions;
        for (const auto &definition : graph->Definitions()) {
            if (definition.kind != RcuSymKind::Func || definition.name.empty()) {
                continue;
            }
            if (const auto address = symbolAddress(definition.location, bases)) {
                functions.emplace_back(*address, &definition);
            }
        }
        std::ranges::sort(functions, [](const auto &a, const auto &b) {
            return a.first != b.first ? a.first < b.first : a.second->name < b.second->name;
        });
        WriteU8(sections.strtab, 0);
        for (int i = 0; i < 24; ++i) {
            WriteU8(sections.symtab, 0);
        }
        for (const auto &[address, definition] : functions) {
            const RcuSymbol &symbol =
                objects[definition->location.objectIndex].symbols[definition->location.symbolIndex];
            WriteU32(sections.symtab, static_cast<uint32_t>(sections.strtab.size()));
            WriteU8(sections.symtab, 2);  // STB_LOCAL, STT_FUNC
            WriteU8(sections.symtab, 0);  // STV_DEFAULT
            WriteU16(sections.symtab, 1); // .text
            WriteU64(sections.symtab, address);
            WriteU64(sections.symtab, symbol.size);
            const auto name = ProfilerName(definition->name);
            sections.strtab.insert(sections.strtab.end(), name.begin(), name.end());
            WriteU8(sections.strtab, 0);
        }
        sections.count = static_cast<uint32_t>(functions.size() + 1);
        symbolSections = std::move(sections);
    };

    // Common file emitter shared by both paths.
    const auto emitFile = [&](uint16_t phnum, const Buf &phdrs,
                              const std::vector<std::pair<uint64_t, const Buf *>> &segments, uint64_t entryVA,
                              uint64_t phoff) -> bool {
        // Section data follows the last loaded byte; the header table follows it.
        uint64_t fileEnd = phoff + phdrs.size();
        for (const auto &[off, buf] : segments) {
            if (!buf->empty()) {
                fileEnd = std::max<uint64_t>(fileEnd, off + buf->size());
            }
        }
        static constexpr char kSectionNames[] = "\0.text\0.symtab\0.strtab\0.shstrtab";
        Buf sectionData;
        Buf sectionHeaders;
        uint64_t shoff = 0;
        if (symbolSections) {
            const uint64_t symtabOff = alignUp(fileEnd, 8);
            const uint64_t strtabOff = symtabOff + symbolSections->symtab.size();
            const uint64_t shstrtabOff = strtabOff + symbolSections->strtab.size();
            shoff = alignUp(shstrtabOff + sizeof(kSectionNames), 8);
            sectionData = symbolSections->symtab;
            sectionData.insert(sectionData.end(), symbolSections->strtab.begin(), symbolSections->strtab.end());
            sectionData.insert(sectionData.end(), kSectionNames, kSectionNames + sizeof(kSectionNames));
            const auto writeShdr = [&](uint32_t name, uint32_t type, uint64_t flags, uint64_t addr, uint64_t off,
                                       uint64_t size, uint32_t link, uint32_t info, uint64_t align, uint64_t entsize) {
                WriteU32(sectionHeaders, name);
                WriteU32(sectionHeaders, type);
                WriteU64(sectionHeaders, flags);
                WriteU64(sectionHeaders, addr);
                WriteU64(sectionHeaders, off);
                WriteU64(sectionHeaders, size);
                WriteU32(sectionHeaders, link);
                WriteU32(sectionHeaders, info);
                WriteU64(sectionHeaders, align);
                WriteU64(sectionHeaders, entsize);
            };
            writeShdr(0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
            writeShdr(1, 1, 0x6, symbolSections->textVA, symbolSections->textOff, symbolSections->textSize, 0, 0, 16,
                      0); // .text: SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR
            writeShdr(7, 2, 0, 0, symtabOff, symbolSections->symtab.size(), 3, symbolSections->count, 8,
                      24); // .symtab: SHT_SYMTAB, every entry local
            writeShdr(15, 3, 0, 0, strtabOff, symbolSections->strtab.size(), 0, 0, 1, 0); // .strtab
            writeShdr(23, 3, 0, 0, shstrtabOff, sizeof(kSectionNames), 0, 0, 1, 0);       // .shstrtab
            fileEnd = symtabOff;
        }

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
        WriteU64(hdr, shoff); // e_shoff
        WriteU32(hdr, 0);     // e_flags
        WriteU16(hdr, 64);    // e_ehsize
        WriteU16(hdr, 56);    // e_phentsize
        WriteU16(hdr, phnum);
        WriteU16(hdr, shoff != 0 ? 64 : 0); // e_shentsize
        WriteU16(hdr, shoff != 0 ? 5 : 0);  // e_shnum
        WriteU16(hdr, shoff != 0 ? 4 : 0);  // e_shstrndx
        writeRaw(hdr.data(), hdr.size());
        writeRaw(phdrs.data(), phdrs.size());

        for (const auto &[off, buf] : segments) {
            if (buf->empty()) {
                continue;
            }
            padToOffset(off);
            writeRaw(buf->data(), buf->size());
        }
        if (shoff != 0) {
            padToOffset(fileEnd);
            writeRaw(sectionData.data(), sectionData.size());
            padToOffset(shoff);
            writeRaw(sectionHeaders.data(), sectionHeaders.size());
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
        const bool hasRodata = !rodataBuf.empty();
        const bool hasWritable = !dataBuf.empty() || layout.BssSize() != 0;
        const auto phnum =
            static_cast<uint16_t>(1 + (hasRodata ? 1 : 0) + (hasWritable ? 1 : 0) + (hasAbiNote ? 1 : 0));
        constexpr uint64_t phoff = 64;
        // The note follows the program headers, inside the first page, which is where the kernel reads it from.
        const uint64_t noteOff = phoff + static_cast<uint64_t>(phnum) * 56;
        const uint64_t textOff = alignUp(noteOff + abiNote.size(), kPage);
        const uint64_t textVA = imageBase + textOff;

        const uint64_t rdataOff = alignUp(textOff + textBuf.size(), kPage);
        const uint64_t rdataVA = imageBase + rdataOff;
        const uint64_t afterReadOnly = hasRodata ? rdataOff + rodataBuf.size() : textOff + textBuf.size();
        const uint64_t dataOff = alignUp(afterReadOnly, kPage);
        const uint64_t dataVA = imageBase + dataOff;
        const uint64_t bssVA = alignUp(dataVA + dataBuf.size(), layout.Alignment(RcuMergedSection::Bss));
        const uint64_t writableMemorySize = bssVA + layout.BssSize() - dataVA;
        const RcuSectionBases sectionBases{.text = textVA, .rodata = rdataVA, .data = dataVA, .bss = bssVA};

        const auto entryAddress = graph->EntryRoot() ? symbolAddress(*graph->EntryRoot(), sectionBases) : std::nullopt;
        if (!entryAddress) {
            Error("internal: ELF entry symbol was not placed");
            return false;
        }
        if (!patchEntryCall(callMainOffset, textVA, *entryAddress, "Main")) {
            return false;
        }
        if (keepsStartFrame && !patchStartFrameStore(textVA, sectionBases)) {
            return false;
        }

        applyRelocs({}, textBuf, rodataBuf, dataBuf, sectionBases, nullptr);
        if (!errors.empty()) {
            return false;
        }

        Buf phdrs;
        writePhdr(phdrs, 1, kPfR | kPfX, textOff, textVA, textBuf.size(), textBuf.size(), kPage);
        if (hasRodata) {
            writePhdr(phdrs, 1, kPfR, rdataOff, rdataVA, rodataBuf.size(), rodataBuf.size(), kPage);
        }
        if (hasWritable) {
            writePhdr(phdrs, 1, kPfR | kPfW, dataOff, dataVA, dataBuf.size(), writableMemorySize, kPage);
        }
        if (hasAbiNote) {
            writePhdr(phdrs, kPtNote, kPfR, noteOff, imageBase + noteOff, abiNote.size(), abiNote.size(), 4);
        }

        prepareSymbols(textOff, textVA, sectionBases);
        return emitFile(phnum, phdrs,
                        {{noteOff, &abiNote}, {textOff, &textBuf}, {rdataOff, &rodataBuf}, {dataOff, &dataBuf}}, textVA,
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
        RcuSymbolLocation location;
    };

    std::vector<SharedExport> sharedExports;
    if (isShared) {
        for (const RcuSymbolLocation location : graph->ExportRoots()) {
            const RcuSymbol &symbol = objects[location.objectIndex].symbols[location.symbolIndex];
            sharedExports.push_back({symbol.name, symbol.kind, symbol.size, 0, location});
        }
    }

    if (profile.definesBsdProcessGlobals && !isShared) {
        const auto exportPointerDataSymbol = [&](std::string name) {
            while (dataBuf.size() % 8) {
                dataBuf.push_back(0);
            }
            const auto dataOffset = static_cast<uint32_t>(dataBuf.size());
            WriteU64(dataBuf, 0);
            exportedDataSymbols.push_back({std::move(name), dataOffset, 0});
        };
        // BSD startup normally provides these pointer definitions through
        // crt1. Rux dynamic ET_EXEC output starts directly at Main, so it owns
        // writable, null-initialized storage for them; FreeBSD rtld's
        // set_program_var also initializes the auxiliary-vector pointer.
        exportPointerDataSymbol("__progname");
        exportPointerDataSymbol("environ");
        if (profile.definesElfAuxVector) {
            exportPointerDataSymbol("__elf_aux_vector");
        }
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
        for (const RcuLinkReference &reference : graph->References()) {
            const RcuReloc &relocation =
                objects[reference.objectIndex].sections[reference.sectionIndex].relocs[reference.relocationIndex];
            relativeRelocationCount += relocation.type == RcuRelType::Abs64 && layout.Relocation(reference).has_value();
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
    const auto phnum = static_cast<uint16_t>((isShared ? 4 : 5) + (hasAbiNote ? 1 : 0) + (hasTls ? 1 : 0));
    uint64_t off = 64 + static_cast<uint64_t>(phnum) * 56;

    const uint64_t noteOff = off;
    off += abiNote.size();
    const uint64_t interpOff = off;
    off += interp.size();
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
    // Each merged section starts on the strictest alignment any input section
    // recorded: the layout padded every object's section to its own alignment
    // relative to the merged start, so that start has to honour all of them.
    const auto sectionAlignment = [&](const RcuMergedSection section, const uint64_t minimum) {
        return std::max<uint64_t>(layout.Alignment(section), minimum);
    };
    const uint64_t textOff = alignUp(off, sectionAlignment(RcuMergedSection::Text, 16));
    off = textOff + textBuf.size();
    const uint64_t rodataOff = alignUp(off, sectionAlignment(RcuMergedSection::RoData, 8));
    off = rodataOff + rodataBuf.size();
    const uint64_t rxFileEnd = off;

    // Read/write segment starts on a fresh page.
    const uint64_t dynamicOff = alignUp(rxFileEnd, kPage);
    off = dynamicOff + dynSz;
    const uint64_t gotOff = alignUp(off, 8);
    off = gotOff + gotSz;
    // The thread-local template sits at the start of its own alignment, so its first byte is aligned in every thread's
    // block the loader copies it to.
    const uint64_t tlsOff = alignUp(off, tlsAlign);
    if (hasTls) {
        off = tlsOff + tlsBuf.size();
    }
    const uint64_t dataOff = alignUp(off, sectionAlignment(RcuMergedSection::Data, 8));
    // The RW segment's file size must not claim bytes past the last one we
    // actually write (an empty .data would leave the trailing alignment gap
    // unwritten).
    const uint64_t rwFileEnd = !dataBuf.empty() ? (dataOff + dataBuf.size())
                             : hasTls           ? (tlsOff + tlsBuf.size())
                                                : (gotOff + gotSz);
    const uint64_t bssOff = alignUp(dataOff + dataBuf.size(), layout.Alignment(RcuMergedSection::Bss));
    const uint64_t rwMemoryEnd = bssOff + layout.BssSize();

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
    const uint64_t bssVA = imageBase + bssOff;
    const RcuSectionBases sectionBases{
        .text = textVA, .rodata = rodataVA, .data = dataVA, .bss = bssVA, .tls = imageBase + tlsOff};

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
        // FreeBSD's AArch64 _rtld_bind_start contract enters with X16 naming
        // GOT[2], X17 holding the resolver, and a stack pair containing the
        // calling stub's &GOT[index] followed by X30. The standard AAELF64
        // shape establishes the same state for Linux AArch64.
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

    // 10. Resolve graph locations and format-owned import stubs.
    std::unordered_map<std::string, uint64_t> importAddresses;
    for (size_t i = 0; i < n; ++i) {
        importAddresses[importNames[i]] = pltVA + pltStubOffset(i);
    }
    for (const auto &symbol : sharedExports) {
        const auto address = symbolAddress(symbol.location, sectionBases);
        if (!address) {
            Error("internal: exported ELF symbol '" + symbol.name + "' was not resolved");
            return false;
        }
        Patch64(dynsym, symbol.dynsymValueOffset, *address);
    }
    if (!isShared) {
        const auto entryAddress = graph->EntryRoot() ? symbolAddress(*graph->EntryRoot(), sectionBases) : std::nullopt;
        if (!entryAddress) {
            Error("internal: ELF entry symbol was not placed");
            return false;
        }
        if (!patchEntryCall(callMainOffset, textVA, *entryAddress, "Main")) {
            return false;
        }
        if (keepsStartFrame && !patchStartFrameStore(textVA, sectionBases)) {
            return false;
        }

        // Wire the entry stub's `call exit@plt` to libc exit's PLT stub.
        const auto exit = importAddresses.find("exit");
        if (exit == importAddresses.end()) {
            Error("internal: implicit libc 'exit' import was not resolved");
            return false;
        }
        if (!patchEntryCall(callExitOffset, textVA, exit->second, "exit")) {
            return false;
        }
    }

    Buf relaDyn;
    applyRelocs(importAddresses, textBuf, rodataBuf, dataBuf, sectionBases, isShared ? &relaDyn : nullptr);
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
    writePhdr(phdrs, 1, kPfR | kPfW, dynamicOff, dynamicVA, rwFileEnd - dynamicOff,
              std::max(rwFileEnd, rwMemoryEnd) - dynamicOff,
              kPage);                                                         // PT_LOAD (rw-)
    writePhdr(phdrs, 2, kPfR | kPfW, dynamicOff, dynamicVA, dynSz, dynSz, 8); // PT_DYNAMIC
    if (hasAbiNote) {
        writePhdr(phdrs, kPtNote, kPfR, noteOff, imageBase + noteOff, abiNote.size(), abiNote.size(), 4); // PT_NOTE
    }
    if (hasTls) {
        static constexpr uint32_t kPtTls = 7;
        writePhdr(phdrs, kPtTls, kPfR, tlsOff, imageBase + tlsOff, tlsBuf.size(), tlsBuf.size(), tlsAlign); // PT_TLS
    }

    prepareSymbols(textOff, textVA, sectionBases);
    return emitFile(phnum, phdrs,
                    {{noteOff, &abiNote},
                     {interpOff, &interp},
                     {hashOff, &hash},
                     {dynsymOff, &dynsym},
                     {dynstrOff, &dynstr},
                     {relaDynOff, &relaDyn},
                     {relaOff, &rela},
                     {pltOff, &plt},
                     {textOff, &textBuf},
                     {rodataOff, &rodataBuf},
                     {dynamicOff, &dyn},
                     {gotOff, &got},
                     {tlsOff, &tlsBuf},
                     {dataOff, &dataBuf}},
                    isShared ? 0 : textVA, phoff);
}
} // namespace Rux
