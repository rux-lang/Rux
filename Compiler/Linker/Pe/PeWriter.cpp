// PE32+ object writer and Windows DLL import resolution.

#include "Linker/AArch64Relocation.h"
#include "Linker/Linker.h"
#include "Linker/LinkerInternal.h"
#include "System/Os.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace Rux {
[[maybe_unused]] static constexpr uint64_t kImageBase = 0x1'4000'0000ULL;
[[maybe_unused]] static constexpr uint32_t kSecAlign = 0x1000; // 4 KB section alignment
[[maybe_unused]] static constexpr uint32_t kFileAlign = 0x200; // 512 B file alignment
[[maybe_unused]] static constexpr uint16_t kMagicPE32P = 0x020B;
[[maybe_unused]] static constexpr uint16_t kSubsystemCUI = 3; // console

// DLL-specific
[[maybe_unused]] static constexpr uint16_t kSubsystemGUI = 2; // windows GUI (used for DLLs)
// IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE. The .reloc
// table below describes every absolute fixup, so IMAGE_FILE_RELOCS_STRIPPED
// must stay clear: the ARM64 loader refuses an image it cannot relocate with
// "%1 is not a valid Win32 application".
[[maybe_unused]] static constexpr uint16_t kCharacteristicsExecutable = 0x0022u;
// The same flags plus IMAGE_FILE_DLL.
[[maybe_unused]] static constexpr uint16_t kCharacteristicsDll = 0x2022u;

// IMAGE_SCN_ characteristics
[[maybe_unused]] static constexpr uint32_t kScnText = 0x6000'0020u;  // CNT_CODE | MEM_EXECUTE | MEM_READ
[[maybe_unused]] static constexpr uint32_t kScnRData = 0x4000'0040u; // CNT_INITIALIZED_DATA | MEM_READ
[[maybe_unused]] static constexpr uint32_t kScnData = 0xC000'0040u;  // CNT_INITIALIZED_DATA | MEM_READ | MEM_WRITE
// CNT_INITIALIZED_DATA | MEM_DISCARDABLE | MEM_READ — the loader reads .reloc
// once while mapping and never needs it again.
[[maybe_unused]] static constexpr uint32_t kScnReloc = 0x4200'0040u;

// DllCharacteristics: HIGH_ENTROPY_VA | DYNAMIC_BASE | NX_COMPAT |
// TERMINAL_SERVER_AWARE. The .reloc table lets the loader place the image
// anywhere, and Windows on ARM64 only loads images that opt into ASLR.
[[maybe_unused]] static constexpr uint16_t kDllChars = 0x8160u;

struct PeStubPatch {
    size_t stubOffset = 0;
    size_t fieldOffset = 0;
    uint16_t type = RcuRelType::Rel32;
};

struct PeImportThunk {
    size_t stubOffset = 0;
    std::vector<PeStubPatch> patches;
};

struct PeMissingEntryPatch {
    size_t offset = 0;
    Buf bytes;
};

struct PeStubs {
    Buf bytes;
    PeStubPatch userEntry;
    std::optional<PeStubPatch> exitProcess;
    std::vector<PeImportThunk> imports;
    std::optional<PeMissingEntryPatch> missingDllEntry;
};

using BuildPeEntryStub = PeStubs (*)(bool);
using AppendPeImportThunk = void (*)(PeStubs &);
using ApplyPeRelocation = bool (*)(Buf &, size_t, uint64_t, uint64_t, int64_t, uint16_t, std::string_view,
                                   std::string &);

struct PeArchitectureConfig {
    uint16_t machine = 0;
    uint8_t codePadding = 0;
    // Operating-system and subsystem version stamped into the image, matching
    // the platform linker's default for the machine. The values are load
    // gates, not documentation: the ARM64 loader terminates a process whose
    // image claims a version other than 6.2 (verified on Windows 11 ARM64 —
    // 10.0 dies during initialization with STATUS_INVALID_IMAGE_FORMAT, and
    // anything below 6.2 is rejected outright).
    uint16_t minimumOsVersionMajor = 0;
    uint16_t minimumOsVersionMinor = 0;
    BuildPeEntryStub buildEntryStub = nullptr;
    AppendPeImportThunk appendImportThunk = nullptr;
    ApplyPeRelocation applyRelocation = nullptr;
};

static PeStubs BuildX86_64EntryStub(const bool isDll) {
    PeStubs stubs;
    if (isDll) {
        // sub rsp, 0x28; call DllMain; add rsp, 0x28; ret
        stubs.bytes.insert(stubs.bytes.end(), {0x48, 0x83, 0xEC, 0x28, 0xE8, 0, 0, 0, 0, 0x48, 0x83, 0xC4, 0x28, 0xC3});
        stubs.userEntry = {0, 5, RcuRelType::Rel32};
        stubs.missingDllEntry = PeMissingEntryPatch{4, {0xB8, 1, 0, 0, 0}}; // mov eax, TRUE
    }
    else {
        // sub rsp, 0x28; call Main; mov ecx, eax; call ExitProcess; int3
        stubs.bytes.insert(stubs.bytes.end(),
                           {0x48, 0x83, 0xEC, 0x28, 0xE8, 0, 0, 0, 0, 0x89, 0xC1, 0xE8, 0, 0, 0, 0, 0xCC});
        stubs.userEntry = {0, 5, RcuRelType::Rel32};
        stubs.exitProcess = PeStubPatch{0, 12, RcuRelType::Rel32};
    }

    return stubs;
}

static void AppendX86_64ImportThunk(PeStubs &stubs) {
    const size_t offset = stubs.bytes.size();
    stubs.bytes.insert(stubs.bytes.end(), {0xFF, 0x25, 0, 0, 0, 0}); // jmp qword ptr [rip+disp32]
    stubs.imports.push_back({offset, {{offset, offset + 2, RcuRelType::Rel32}}});
}

static bool ApplyX86_64PeRelocation(Buf &buffer, const size_t patchAt, const uint64_t siteVa, const uint64_t targetVa,
                                    const int64_t addend, const uint16_t type, const std::string_view symbolName,
                                    std::string &error) {
    if (type == RcuRelType::Rel32) {
        if (patchAt + 4 > buffer.size()) {
            return false;
        }
        const auto displacement = static_cast<int32_t>(targetVa + addend - (siteVa + 4));
        Patch32(buffer, patchAt, static_cast<uint32_t>(displacement));
        return true;
    }
    if (type == RcuRelType::Abs64) {
        if (patchAt + 8 > buffer.size()) {
            return false;
        }
        Patch64(buffer, patchAt, targetVa + static_cast<uint64_t>(addend));
        return true;
    }
    if (type == RcuRelType::Abs32) {
        if (patchAt + 4 > buffer.size()) {
            return false;
        }
        Patch32(buffer, patchAt, static_cast<uint32_t>(targetVa + addend));
        return true;
    }
    error = std::format("relocation {} against '{}' is not supported by the PE32+ writer", RcuRelTypeName(type),
                        symbolName);
    return false;
}

static PeStubs BuildAArch64EntryStub(const bool isDll) {
    PeStubs stubs;
    if (isDll) {
        // stp x29, x30, [sp, #-16]!; mov x29, sp; bl DllMain;
        // ldp x29, x30, [sp], #16; ret
        //
        // The loader's x0-x2 arguments reach DllMain untouched. The frame
        // saves the loader's return address across BL and keeps SP aligned.
        for (const uint32_t word : {0xA9BF7BFDu, 0x910003FDu, 0x94000000u, 0xA8C17BFDu, 0xD65F03C0u}) {
            WriteU32(stubs.bytes, word);
        }
        stubs.userEntry = {0, 8, RcuRelType::AArch64Call26};

        Buf returnTrue;
        WriteU32(returnTrue, 0x52800020u); // mov w0, #TRUE
        stubs.missingDllEntry = PeMissingEntryPatch{8, std::move(returnTrue)};
        return stubs;
    }

    // stp x29, x30, [sp, #-16]!; mov x29, sp; bl Main; bl ExitProcess; brk #0
    // The loader supplies a 16-byte-aligned SP. Preserve that alignment and a
    // conventional frame chain, leave Main's result in w0 for ExitProcess,
    // and trap if the no-return import unexpectedly returns.
    for (const uint32_t word : {0xA9BF7BFDu, 0x910003FDu, 0x94000000u, 0x94000000u, 0xD4200000u}) {
        WriteU32(stubs.bytes, word);
    }
    stubs.userEntry = {0, 8, RcuRelType::AArch64Call26};
    stubs.exitProcess = PeStubPatch{0, 12, RcuRelType::AArch64Call26};
    return stubs;
}

static void AppendAArch64ImportThunk(PeStubs &stubs) {
    const size_t offset = stubs.bytes.size();
    // adrp x16, IAT-page; ldr x16, [x16, IAT-pageoff]; br x16
    // x16 is AAPCS64's intra-procedure-call scratch register, so the thunk
    // leaves x0-x7 (and therefore every argument register) untouched.
    for (const uint32_t word : {0x90000010u, 0xF9400210u, 0xD61F0200u}) {
        WriteU32(stubs.bytes, word);
    }
    stubs.imports.push_back(
        {offset,
         {{offset, offset, RcuRelType::AArch64AdrPrelPgHi21}, {offset, offset + 4, RcuRelType::AArch64LdstAbsLo12Nc}}});
}

static bool ApplyAArch64PeRelocation(Buf &buffer, const size_t patchAt, const uint64_t siteVa, const uint64_t targetVa,
                                     const int64_t addend, const uint16_t type, const std::string_view symbolName,
                                     std::string &error) {
    const size_t width = type == RcuRelType::Abs64 || type == RcuRelType::AArch64Prel64 ? 8 : 4;
    if (patchAt > buffer.size() || width > buffer.size() - patchAt) {
        error = std::format("{} relocation against '{}' has a patch outside its PE section", RcuRelTypeName(type),
                            symbolName);
        return false;
    }
    return ApplyAArch64Relocation(buffer, patchAt, type, targetVa, addend, siteVa, symbolName, "PE32+ writer", error);
}

static const PeArchitectureConfig *PeArchitectureFor(const Target::Arch arch) {
    static constexpr PeArchitectureConfig x86_64{
        .machine = 0x8664, // IMAGE_FILE_MACHINE_AMD64
        .codePadding = 0xCC,
        .minimumOsVersionMajor = 6, // Windows Vista
        .minimumOsVersionMinor = 0,
        .buildEntryStub = BuildX86_64EntryStub,
        .appendImportThunk = AppendX86_64ImportThunk,
        .applyRelocation = ApplyX86_64PeRelocation,
    };
    static constexpr PeArchitectureConfig aarch64{
        .machine = 0xAA64, // IMAGE_FILE_MACHINE_ARM64
        .codePadding = 0,
        .minimumOsVersionMajor = 6, // 6.2, the ARM64 baseline MSVC stamps
        .minimumOsVersionMinor = 2,
        .buildEntryStub = BuildAArch64EntryStub,
        .appendImportThunk = AppendAArch64ImportThunk,
        .applyRelocation = ApplyAArch64PeRelocation,
    };
    switch (arch) {
    case Target::Arch::X86_64:
        return &x86_64;
    case Target::Arch::AArch64:
        return &aarch64;
    default:
        return nullptr;
    }
}

static PeStubs BuildPeStubs(const PeArchitectureConfig &architecture, const bool isDll, const size_t importCount) {
    PeStubs stubs = architecture.buildEntryStub(isDll);
    stubs.imports.reserve(importCount);
    for (size_t i = 0; i < importCount; ++i) {
        architecture.appendImportThunk(stubs);
    }
    return stubs;
}

static bool PatchPeStubs(const PeArchitectureConfig &architecture, const PeStubs &stubs, Buf &text,
                         const uint64_t textVa, const uint64_t rdataVa, const std::vector<uint32_t> &iatEntryOffsets,
                         const std::vector<std::string> &importNames,
                         const std::unordered_map<std::string, size_t> &importIndexes,
                         const std::unordered_map<std::string, uint64_t> &symbols, const bool isDll,
                         std::string &error) {
    for (size_t i = 0; i < stubs.imports.size(); ++i) {
        for (const auto &patch : stubs.imports[i].patches) {
            if (!architecture.applyRelocation(text, patch.fieldOffset, textVa + patch.fieldOffset,
                                              rdataVa + iatEntryOffsets[i], 0, patch.type, importNames[i], error)) {
                return false;
            }
        }
    }

    const std::string_view entryName = isDll ? "DllMain" : "Main";
    const auto entry = symbols.find(std::string(entryName));
    if (entry != symbols.end()) {
        const auto &patch = stubs.userEntry;
        if (!architecture.applyRelocation(text, patch.fieldOffset, textVa + patch.fieldOffset, entry->second, 0,
                                          patch.type, entryName, error)) {
            return false;
        }
    }
    else if (stubs.missingDllEntry) {
        const auto &replacement = *stubs.missingDllEntry;
        std::ranges::copy(replacement.bytes, text.begin() + static_cast<std::ptrdiff_t>(replacement.offset));
    }
    else {
        error = "undefined symbol 'Main' — no entry point found";
        return false;
    }

    if (stubs.exitProcess) {
        const auto exitIndex = importIndexes.find("ExitProcess");
        if (exitIndex == importIndexes.end()) {
            error = "internal: ExitProcess import is missing from PE entry stub";
            return false;
        }
        const auto &patch = *stubs.exitProcess;
        const uint64_t exitThunkVa = textVa + stubs.imports[exitIndex->second].stubOffset;
        if (!architecture.applyRelocation(text, patch.fieldOffset, textVa + patch.fieldOffset, exitThunkVa, 0,
                                          patch.type, "ExitProcess", error)) {
            return false;
        }
    }
    return true;
}

static Buf BuildPeCoffHeader(const PeArchitectureConfig &architecture, const uint16_t sectionCount,
                             const uint32_t timestamp, const bool isDll) {
    Buf header;
    WriteU16(header, architecture.machine);
    WriteU16(header, sectionCount);
    WriteU32(header, timestamp);
    WriteU32(header, 0);
    WriteU32(header, 0);   // no COFF symbol table
    WriteU16(header, 240); // SizeOfOptionalHeader for PE32+
    WriteU16(header, isDll ? kCharacteristicsDll : kCharacteristicsExecutable);
    return header;
}

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
    // commonly declared without an extension (e.g. #Link("kernel32"));
    // mirror the OS loader and also try the name with ".dll" appended.
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

bool Linker::LinkPe32Plus(const std::filesystem::path &outputPath) {
    const PeArchitectureConfig *architecture = PeArchitectureFor(targetArch);
    if (architecture == nullptr) {
        Error("internal: no PE32+ architecture configuration for target");
        return false;
    }
    const bool isDll = artifactKind == ArtifactKind::SharedLibrary;
    // Preferred bases follow x86-64 linker convention — 0x140000000 for
    // executables, 0x180000000 for DLLs — but the .reloc table lets the
    // loader place either anywhere.
    const uint64_t imageBase = isDll ? 0x1'8000'0000ULL : kImageBase;
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

    // 2. Build the architecture-owned entry and import stubs. Their patch
    // metadata keeps the shared PE layout independent of instruction sizes.
    PeStubs stubs = BuildPeStubs(*architecture, isDll, numImports);
    Buf &textPre = stubs.bytes;

    // The entry and import thunks have a variable length. Keep the first RCU
    // text section aligned even when the number of imports changes: codegen
    // records that alignment in the object, and the image writer must not
    // discard it merely because it prepends linker-generated code.
    uint16_t textAlignment = 1;
    for (const auto &obj : objects) {
        for (const auto &sec : obj.sections) {
            if (sec.type == RcuSecType::Text) {
                textAlignment = std::max(textAlignment, sec.alignment);
            }
        }
    }
    PadTo(textPre, textAlignment, architecture->codePadding);

    const auto preambleSize = static_cast<uint32_t>(textPre.size());

    // 3. Merge RCU sections

    struct ObjLayout {
        uint32_t textOff, rodataOff, dataOff;
    };

    std::vector<ObjLayout> layouts(objects.size());
    Buf mergedText, mergedRodata, mergedData;

    for (size_t i = 0; i < objects.size(); ++i) {
        const auto &obj = objects[i];

        // RCU section offsets are relative to a section whose alignment is
        // part of the object contract. Preserve it between input objects for
        // all three PE sections; otherwise a preceding object's odd size can
        // misalign every symbol in the next object.
        uint16_t objectTextAlignment = 1;
        uint16_t objectRodataAlignment = 1;
        uint16_t objectDataAlignment = 1;
        for (const auto &sec : obj.sections) {
            if (sec.type == RcuSecType::Text) {
                objectTextAlignment = std::max(objectTextAlignment, sec.alignment);
            }
            else if (sec.type == RcuSecType::RoData) {
                objectRodataAlignment = std::max(objectRodataAlignment, sec.alignment);
            }
            else if (sec.type == RcuSecType::Data) {
                objectDataAlignment = std::max(objectDataAlignment, sec.alignment);
            }
        }
        PadTo(mergedText, objectTextAlignment, architecture->codePadding);
        PadTo(mergedRodata, objectRodataAlignment);
        PadTo(mergedData, objectDataAlignment);
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
    // IMAGE_IMPORT_DESCRIPTOR is 20 bytes, so an even number of DLL groups
    // leaves the directory plus its null descriptor only four-byte aligned.
    // Each PE32+ thunk is a uint64_t and an AArch64 import stub addresses its
    // IAT entry with a scaled 64-bit LDR; align both thunk-table families.
    PadTo(rdataBuf, 8);
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
    PadTo(rdataBuf, 8);
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

    // Reserve the complete export directory before assigning section RVAs.
    // Appending it after layout could move .data to another page and leave
    // already-resolved data symbols pointing at the old address.
    std::vector<std::string> exportNames;
    if (isDll) {
        for (const auto &obj : objects) {
            for (const auto &sym : obj.symbols) {
                if (sym.kind == RcuSymKind::Func && sym.visibility != RcuSymVis::Local && !sym.name.empty() &&
                    sym.name != "DllMain") {
                    exportNames.push_back(sym.name);
                }
            }
        }
        std::ranges::sort(exportNames);
        exportNames.erase(std::ranges::unique(exportNames).begin(), exportNames.end());
    }

    uint32_t exportDirOff = 0;
    uint32_t exportDirSize = 0;
    size_t exportDirectoryPosition = 0;
    uint32_t exportFunctionArrayOff = 0;
    uint32_t exportNameArrayOff = 0;
    uint32_t exportOrdinalArrayOff = 0;
    uint32_t exportDllNameOff = 0;
    std::vector<uint32_t> exportNameStringOffsets;
    if (!exportNames.empty()) {
        exportDirOff = static_cast<uint32_t>(rdataBuf.size());
        exportDirectoryPosition = rdataBuf.size();
        WriteZeros(rdataBuf, 40);

        exportFunctionArrayOff = static_cast<uint32_t>(rdataBuf.size());
        WriteZeros(rdataBuf, exportNames.size() * 4);
        exportNameArrayOff = static_cast<uint32_t>(rdataBuf.size());
        WriteZeros(rdataBuf, exportNames.size() * 4);
        exportOrdinalArrayOff = static_cast<uint32_t>(rdataBuf.size());
        for (size_t i = 0; i < exportNames.size(); ++i) {
            WriteU16(rdataBuf, static_cast<uint16_t>(i));
        }

        exportDllNameOff = static_cast<uint32_t>(rdataBuf.size());
        WriteCStr(rdataBuf, System::SharedLibraryFileName(packageName, Target::OS::Windows).c_str());
        PadTo(rdataBuf, 2);
        exportNameStringOffsets.reserve(exportNames.size());
        for (const auto &name : exportNames) {
            exportNameStringOffsets.push_back(static_cast<uint32_t>(rdataBuf.size()));
            WriteCStr(rdataBuf, name.c_str());
            PadTo(rdataBuf, 2);
        }
        exportDirSize = static_cast<uint32_t>(rdataBuf.size()) - exportDirOff;
    }

    // 5. Compute section layout (RVAs and file offsets)
    // Every Abs64 fixup becomes an IMAGE_REL_BASED_DIR64 entry, so the .reloc
    // section exists exactly when the merged objects contain one.
    const bool hasAbsoluteFixups = std::ranges::any_of(objects, [](const auto &obj) {
        return std::ranges::any_of(obj.sections, [](const auto &sec) {
            if (sec.type != RcuSecType::Text && sec.type != RcuSecType::RoData && sec.type != RcuSecType::Data) {
                return false;
            }
            return std::ranges::any_of(sec.relocs, [](const auto &reloc) { return reloc.type == RcuRelType::Abs64; });
        });
    });
    const uint32_t numSections = (mergedData.empty() ? 2u : 3u) + (hasAbsoluteFixups ? 1u : 0u);
    const uint32_t rawHdrBytes = 64 + 4 + 20 + 240 + numSections * 40;
    const uint32_t sizeOfHeaders = AlignUp(rawHdrBytes, kFileAlign);
    const uint32_t textRva = AlignUp(sizeOfHeaders, kSecAlign);
    const uint32_t textVirtSize = preambleSize + static_cast<uint32_t>(mergedText.size());
    const uint32_t textFileSize = AlignUp(textVirtSize, kFileAlign);
    const uint32_t textFileOff = sizeOfHeaders;
    const uint32_t rdataRva = textRva + AlignUp(textVirtSize, kSecAlign);
    auto rdataVirtSize = static_cast<uint32_t>(rdataBuf.size());
    uint32_t rdataFileSize = AlignUp(rdataVirtSize, kFileAlign);
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
        symMap[importNames[i]] = imageBase + textRva + stubs.imports[i].stubOffset;
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
                va = imageBase + textRva + preambleSize + lay.textOff + sym.value;
            }
            else if (sym.sectionIdx == RCU_RODATA_IDX) {
                va = imageBase + rdataRva + lay.rodataOff + sym.value;
            }
            else if (sym.sectionIdx == RCU_DATA_IDX) {
                va = imageBase + dataRva + lay.dataOff + sym.value;
            }
            else {
                continue;
            }
            symMap.try_emplace(sym.name, va); // first definition wins
        }
    }

    if (!exportNames.empty()) {
        const auto numberOfExports = static_cast<uint32_t>(exportNames.size());
        for (uint32_t i = 0; i < numberOfExports; ++i) {
            const auto symbol = symMap.find(exportNames[i]);
            if (symbol == symMap.end()) {
                Error("internal: exported PE symbol '" + exportNames[i] + "' was not resolved");
                continue;
            }
            Patch32(rdataBuf, exportFunctionArrayOff + i * 4, static_cast<uint32_t>(symbol->second - imageBase));
            Patch32(rdataBuf, exportNameArrayOff + i * 4, rdataRva + exportNameStringOffsets[i]);
        }
        Patch32(rdataBuf, exportDirectoryPosition + 0, 0);                            // Characteristics
        Patch32(rdataBuf, exportDirectoryPosition + 4, 0);                            // deterministic timestamp
        Patch32(rdataBuf, exportDirectoryPosition + 12, rdataRva + exportDllNameOff); // Name
        Patch32(rdataBuf, exportDirectoryPosition + 16, 1);                           // ordinal base
        Patch32(rdataBuf, exportDirectoryPosition + 20, numberOfExports);
        Patch32(rdataBuf, exportDirectoryPosition + 24, numberOfExports);
        Patch32(rdataBuf, exportDirectoryPosition + 28, rdataRva + exportFunctionArrayOff);
        Patch32(rdataBuf, exportDirectoryPosition + 32, rdataRva + exportNameArrayOff);
        Patch32(rdataBuf, exportDirectoryPosition + 36, rdataRva + exportOrdinalArrayOff);
    }
    if (!errors.empty()) {
        return false;
    }

    // 8. Build final .text (preamble + user code)
    Buf textBuf;
    textBuf.insert(textBuf.end(), textPre.begin(), textPre.end());
    textBuf.insert(textBuf.end(), mergedText.begin(), mergedText.end());

    std::string stubError;
    if (!PatchPeStubs(*architecture, stubs, textBuf, imageBase + textRva, imageBase + rdataRva, iatEntryOff,
                      importNames, importIdx, symMap, isDll, stubError)) {
        Error(std::move(stubError));
        return false;
    }

    // 9. Patch user code relocations

    std::vector<uint32_t> baseRelocationRvas;
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
                secBaseVA = imageBase + textRva + preambleSize + lay.textOff;
            }
            else if (sec.type == RcuSecType::RoData) {
                buf = &rdataBuf;
                baseInBuf = lay.rodataOff;
                secBaseVA = imageBase + rdataRva + lay.rodataOff;
            }
            else if (sec.type == RcuSecType::Data) {
                buf = &mergedData;
                baseInBuf = lay.dataOff;
                secBaseVA = imageBase + dataRva + lay.dataOff;
            }
            else {
                continue;
            }

            for (const auto &reloc : sec.relocs) {
                if (reloc.type == RcuRelType::None) {
                    continue;
                }
                if (reloc.symbolIndex >= obj.symbols.size()) {
                    Error(std::format("relocation in object {} refers to missing symbol index {}", obj.sourcePath,
                                      reloc.symbolIndex));
                    continue;
                }
                const auto &sym = obj.symbols[reloc.symbolIndex];

                // A 64-bit absolute slot moves with the image; record it for
                // the base-relocation table before resolving its target.
                if (reloc.type == RcuRelType::Abs64) {
                    baseRelocationRvas.push_back(static_cast<uint32_t>(secBaseVA + reloc.sectionOffset - imageBase));
                }

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
                        targetVA = imageBase + textRva + preambleSize + lay.textOff + sym.value;
                    }
                    else if (sym.sectionIdx == RCU_RODATA_IDX) {
                        targetVA = imageBase + rdataRva + lay.rodataOff + sym.value;
                    }
                    else if (sym.sectionIdx == RCU_DATA_IDX) {
                        targetVA = imageBase + dataRva + lay.dataOff + sym.value;
                    }
                    else {
                        continue;
                    }
                }
                const size_t patchAt = baseInBuf + reloc.sectionOffset;
                const uint64_t siteVA = secBaseVA + reloc.sectionOffset;
                std::string relocationError;
                if (!architecture->applyRelocation(*buf, patchAt, siteVA, targetVA, reloc.addend, reloc.type, sym.name,
                                                   relocationError)) {
                    Error(std::move(relocationError));
                }
            }
        }
    }

    if (!errors.empty()) {
        return false;
    }

    // 10. Build the base-relocation table: one block per 4 KB page holding an
    // IMAGE_REL_BASED_DIR64 entry per fixup, padded to a four-byte block size
    // with an IMAGE_REL_BASED_ABSOLUTE no-op.
    std::ranges::sort(baseRelocationRvas);
    Buf relocBuf;
    for (size_t first = 0; first < baseRelocationRvas.size();) {
        const uint32_t page = baseRelocationRvas[first] & ~0xFFFu;
        size_t last = first;
        while (last < baseRelocationRvas.size() && (baseRelocationRvas[last] & ~0xFFFu) == page) {
            ++last;
        }
        const size_t entryCount = last - first;
        WriteU32(relocBuf, page);
        WriteU32(relocBuf, static_cast<uint32_t>(8 + (entryCount + entryCount % 2) * 2));
        for (; first < last; ++first) {
            WriteU16(relocBuf, static_cast<uint16_t>(0xA000u | (baseRelocationRvas[first] & 0xFFFu)));
        }
        if (entryCount % 2 != 0) {
            WriteU16(relocBuf, 0);
        }
    }
    // .reloc follows the last mapped section; numSections and sizeOfHeaders
    // already account for it whenever the objects contain an Abs64 fixup.
    const auto relocVirtSize = static_cast<uint32_t>(relocBuf.size());
    const uint32_t relocRva = sizeOfImage;
    const uint32_t relocFileSize = AlignUp(relocVirtSize, kFileAlign);
    const uint32_t relocFileOff = mergedData.empty() ? rdataFileOff + rdataFileSize : dataFileOff + dataFileSize;
    const uint32_t finalSizeOfImage = relocBuf.empty() ? sizeOfImage : relocRva + AlignUp(relocVirtSize, kSecAlign);

    // 11. Emit PE32+ file
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
    wBuf(BuildPeCoffHeader(*architecture, static_cast<uint16_t>(numSections), static_cast<uint32_t>(std::time(nullptr)),
                           isDll));

    // Optional Header PE32+ (240 bytes)
    wU16(kMagicPE32P);
    wU8(14);
    wU8(0);                                             // Linker version 14.0
    wU32(textFileSize);                                 // SizeOfCode
    wU32(rdataFileSize + dataFileSize + relocFileSize); // SizeOfInitializedData
    wU32(0);                                            // SizeOfUninitializedData
    wU32(textRva);                                      // AddressOfEntryPoint (__rux_start at start of .text)
    wU32(textRva);                                      // BaseOfCode
    wU64(imageBase);
    wU32(kSecAlign);
    wU32(kFileAlign);
    wU16(architecture->minimumOsVersionMajor);
    wU16(architecture->minimumOsVersionMinor); // MajorOSVersion / MinorOSVersion
    wU16(0);
    wU16(0); // MajorImageVersion / MinorImageVersion
    wU16(architecture->minimumOsVersionMajor);
    wU16(architecture->minimumOsVersionMinor); // MajorSubsystemVersion / MinorSubsystemVersion
    wU32(0);                                   // Win32VersionValue
    wU32(finalSizeOfImage);
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
    wDir(importDllNames.empty() ? 0 : rdataRva + importDirOff,
         importDllNames.empty() ? 0 : static_cast<uint32_t>((importDllNames.size() + 1) * 20)); // [1] Import
    wDir(0, 0);
    wDir(0, 0);
    wDir(0, 0);                                           // [2..4]
    wDir(relocBuf.empty() ? 0 : relocRva, relocVirtSize); // [5] Base relocation
    wDir(0, 0);
    wDir(0, 0); // [6..7]
    wDir(0, 0);
    wDir(0, 0);
    wDir(0, 0);
    wDir(0, 0);                                          // [8..11]
    wDir(iatSize == 0 ? 0 : rdataRva + iatOff, iatSize); // [12] IAT
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
    if (!relocBuf.empty()) {
        wSec8(".reloc");
        wU32(relocVirtSize);
        wU32(relocRva);
        wU32(relocFileSize);
        wU32(relocFileOff);
        wU32(0);
        wU32(0);
        wU16(0);
        wU16(0);
        wU32(kScnReloc);
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
    if (!relocBuf.empty()) {
        wBuf(relocBuf);
        padTo(kFileAlign);
    }
    return errors.empty();
}
} // namespace Rux
