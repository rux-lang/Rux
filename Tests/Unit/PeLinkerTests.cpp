#include "Linker/Linker.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace Rux;

namespace {
struct PeSection {
    std::string name;
    uint32_t virtualSize = 0;
    uint32_t rva = 0;
    uint32_t rawSize = 0;
    uint32_t rawOffset = 0;
};

uint16_t Read16(const std::vector<uint8_t> &image, const size_t offset) {
    return static_cast<uint16_t>(image.at(offset)) | static_cast<uint16_t>(image.at(offset + 1)) << 8U;
}

uint32_t Read32(const std::vector<uint8_t> &image, const size_t offset) {
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) {
        value |= static_cast<uint32_t>(image.at(offset + i)) << (i * 8U);
    }
    return value;
}

uint32_t ReadBig32(const std::vector<uint8_t> &bytes, const size_t offset) {
    return static_cast<uint32_t>(bytes.at(offset)) << 24U | static_cast<uint32_t>(bytes.at(offset + 1)) << 16U |
           static_cast<uint32_t>(bytes.at(offset + 2)) << 8U | static_cast<uint32_t>(bytes.at(offset + 3));
}

uint64_t Read64(const std::vector<uint8_t> &image, const size_t offset) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(image.at(offset + i)) << (i * 8U);
    }
    return value;
}

void AppendWord(std::vector<uint8_t> &bytes, const uint32_t word) {
    for (unsigned i = 0; i < 4; ++i) {
        bytes.push_back(static_cast<uint8_t>(word >> (i * 8U)));
    }
}

std::string ReadString(const std::vector<uint8_t> &image, size_t offset) {
    std::string value;
    while (offset < image.size() && image[offset] != 0) {
        value.push_back(static_cast<char>(image[offset++]));
    }
    return value;
}

std::filesystem::path PeTestPath(const std::string_view extension = ".exe") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("rux-pe-baseline-" + std::to_string(nonce) + std::string(extension));
}

std::vector<uint8_t> ReadFile(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    return {(std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>()};
}

// Collects the RVA of every IMAGE_REL_BASED_DIR64 entry in the .reloc
// section, in file order.
std::vector<uint32_t> ReadDir64Fixups(const std::vector<uint8_t> &image, const PeSection &relocSection,
                                      const uint32_t directorySize) {
    std::vector<uint32_t> fixups;
    for (uint32_t cursor = 0; cursor < directorySize;) {
        const size_t block = relocSection.rawOffset + cursor;
        const uint32_t pageRva = Read32(image, block);
        const uint32_t blockSize = Read32(image, block + 4);
        for (uint32_t entry = 8; entry < blockSize; entry += 2) {
            const uint16_t value = Read16(image, block + entry);
            if (value >> 12U == 10U) {
                fixups.push_back(pageRva + (value & 0xFFFU));
            }
        }
        cursor += blockSize;
    }
    return fixups;
}

struct ArchiveMember {
    size_t header = 0;
    size_t data = 0;
    size_t size = 0;
};

std::vector<ArchiveMember> ReadArchiveMembers(const std::vector<uint8_t> &archive) {
    std::vector<ArchiveMember> members;
    size_t cursor = 8;
    while (cursor + 60 <= archive.size()) {
        const std::string sizeText(archive.begin() + static_cast<std::ptrdiff_t>(cursor + 48),
                                   archive.begin() + static_cast<std::ptrdiff_t>(cursor + 58));
        const size_t size = std::stoull(sizeText);
        members.push_back({cursor, cursor + 60, size});
        cursor += 60 + size + (size & 1U);
    }
    return members;
}
} // namespace

TEST_CASE("PE linker preserves the x86-64 executable layout and patched targets") {
    // Main calls one local function, one imported function, and one function
    // from a second object. The deliberately awkward object sizes protect the
    // alignment padding between the linker preamble and both input objects.
    RcuFile first;
    first.sourcePath = "Main.rux";
    RcuSection text;
    text.name = ".text";
    text.type = RcuSecType::Text;
    text.flags = RcuSecFlag::Alloc | RcuSecFlag::Exec | RcuSecFlag::Read;
    text.alignment = 16;
    text.data = {
        0xE8, 0,  0, 0, 0, // call Helper
        0xE8, 0,  0, 0, 0, // call GetCurrentProcessId
        0xE8, 0,  0, 0, 0, // call Second
        0xB8, 42, 0, 0, 0, // mov eax, 42
        0xC3,              // ret
        0xC3,              // Helper: ret
    };
    text.relocs.push_back({1, 1, RcuRelType::Rel32, 0});
    text.relocs.push_back({6, 2, RcuRelType::Rel32, 0});
    text.relocs.push_back({11, 3, RcuRelType::Rel32, 0});
    first.sections.push_back(std::move(text));

    RcuSection rodata;
    rodata.name = ".rdata";
    rodata.type = RcuSecType::RoData;
    rodata.flags = RcuSecFlag::Alloc | RcuSecFlag::Read;
    rodata.alignment = 16;
    rodata.data.resize(8);
    rodata.relocs.push_back({0, 1, RcuRelType::Abs64, 0});
    first.sections.push_back(std::move(rodata));

    first.symbols.push_back({"Main", "int", 0, 21, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});
    first.symbols.push_back({"Helper", "", 21, 1, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Local});
    first.symbols.push_back(
        {"GetCurrentProcessId", "", 0, 0, RCU_SEC_EXTERNAL, RcuSymKind::ExternFunc, RcuSymVis::Global});
    first.symbols.push_back({"Second", "", 0, 0, RCU_SEC_EXTERNAL, RcuSymKind::ExternFunc, RcuSymVis::Global});

    RcuFile second;
    second.sourcePath = "Second.rux";
    RcuSection secondText;
    secondText.name = ".text";
    secondText.type = RcuSecType::Text;
    secondText.flags = RcuSecFlag::Alloc | RcuSecFlag::Exec | RcuSecFlag::Read;
    secondText.alignment = 32;
    secondText.data = {0xC3};
    second.sections.push_back(std::move(secondText));
    RcuSection data;
    data.name = ".data";
    data.type = RcuSecType::Data;
    data.flags = RcuSecFlag::Alloc | RcuSecFlag::Read | RcuSecFlag::Write;
    data.alignment = 32;
    data.data.resize(8);
    data.relocs.push_back({0, 0, RcuRelType::Abs64, 0});
    second.sections.push_back(std::move(data));
    second.symbols.push_back({"Second", "", 0, 1, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});

    const auto output = PeTestPath();
    std::error_code error;
    std::filesystem::remove(output, error);
    Linker linker({std::move(first), std::move(second)}, "PeBaseline", {}, ArtifactKind::Executable,
                  Target::OS::Windows, Target::Arch::X86_64);
    REQUIRE(linker.Link(output));

    std::ifstream stream(output, std::ios::binary);
    REQUIRE(stream.is_open());
    const std::vector<uint8_t> image((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    stream.close();
    std::filesystem::remove(output, error);

    REQUIRE(image.size() >= 0x200);
    REQUIRE(Read16(image, 0) == 0x5A4D); // MZ
    const size_t peOffset = Read32(image, 0x3C);
    REQUIRE(peOffset + 24 <= image.size());
    CHECK(Read32(image, peOffset) == 0x0000'4550);       // PE\0\0
    CHECK(Read16(image, peOffset + 4) == 0x8664);        // IMAGE_FILE_MACHINE_AMD64
    CHECK((Read16(image, peOffset + 22) & 0x0001) == 0); // relocations kept for ASLR
    const uint16_t sectionCount = Read16(image, peOffset + 6);
    REQUIRE(sectionCount == 4);
    CHECK(Read16(image, peOffset + 20) == 240);

    const size_t optional = peOffset + 24;
    CHECK(Read16(image, optional) == 0x020B); // PE32+
    const uint32_t entryRva = Read32(image, optional + 16);
    const uint64_t imageBase = Read64(image, optional + 24);
    const uint32_t sectionAlignment = Read32(image, optional + 32);
    const uint32_t fileAlignment = Read32(image, optional + 36);
    const uint32_t sizeOfImage = Read32(image, optional + 56);
    const uint32_t sizeOfHeaders = Read32(image, optional + 60);
    CHECK(entryRva == 0x1000);
    CHECK(imageBase == 0x1'4000'0000ULL);
    CHECK(sectionAlignment == 0x1000);
    CHECK(fileAlignment == 0x200);
    CHECK(sizeOfHeaders % fileAlignment == 0);
    // x86-64 keeps the Vista baseline; ARM64 stamps the 6.2 baseline below.
    CHECK(Read16(image, optional + 40) == 6); // MajorOperatingSystemVersion
    CHECK(Read16(image, optional + 48) == 6); // MajorSubsystemVersion
    // HIGH_ENTROPY_VA | DYNAMIC_BASE | NX_COMPAT | TERMINAL_SERVER_AWARE
    CHECK(Read16(image, optional + 70) == 0x8160);

    const uint32_t importRva = Read32(image, optional + 112 + 8);
    const uint32_t importSize = Read32(image, optional + 112 + 12);
    const uint32_t iatRva = Read32(image, optional + 112 + 12 * 8);
    const uint32_t iatSize = Read32(image, optional + 112 + 12 * 8 + 4);
    CHECK(importRva != 0);
    CHECK(importSize == 40); // one descriptor plus the null descriptor
    CHECK(iatRva != 0);
    CHECK(iatSize == 24); // two imports plus the null thunk

    std::vector<PeSection> sections;
    const size_t sectionTable = optional + 240;
    for (uint16_t i = 0; i < sectionCount; ++i) {
        const size_t offset = sectionTable + static_cast<size_t>(i) * 40;
        REQUIRE(offset + 40 <= image.size());
        PeSection section;
        for (size_t c = 0; c < 8 && image[offset + c] != 0; ++c) {
            section.name.push_back(static_cast<char>(image[offset + c]));
        }
        section.virtualSize = Read32(image, offset + 8);
        section.rva = Read32(image, offset + 12);
        section.rawSize = Read32(image, offset + 16);
        section.rawOffset = Read32(image, offset + 20);
        CHECK(section.rva % sectionAlignment == 0);
        CHECK(section.rawOffset % fileAlignment == 0);
        CHECK(static_cast<uint64_t>(section.rawOffset) + section.rawSize <= image.size());
        CHECK(static_cast<uint64_t>(section.rva) + section.virtualSize <= sizeOfImage);
        sections.push_back(std::move(section));
    }

    const auto findSection = [&](const std::string_view name) -> const PeSection & {
        return *std::ranges::find_if(sections, [&](const PeSection &section) { return section.name == name; });
    };
    const auto &textSection = findSection(".text");
    const auto &rdataSection = findSection(".rdata");
    const auto &dataSection = findSection(".data");
    CHECK(entryRva >= textSection.rva);
    CHECK(entryRva < textSection.rva + textSection.virtualSize);
    CHECK(importRva >= rdataSection.rva);
    CHECK(importRva + importSize <= rdataSection.rva + rdataSection.virtualSize);
    CHECK(iatRva >= rdataSection.rva);
    CHECK(iatRva + iatSize <= rdataSection.rva + rdataSection.virtualSize);

    const auto rvaToOffset = [&](const uint32_t rva) -> size_t {
        for (const auto &section : sections) {
            if (rva >= section.rva && rva < section.rva + std::max(section.virtualSize, section.rawSize)) {
                return section.rawOffset + rva - section.rva;
            }
        }
        return image.size();
    };
    const auto rel32Target = [&](const uint32_t fieldRva) {
        const int32_t displacement = static_cast<int32_t>(Read32(image, rvaToOffset(fieldRva)));
        return static_cast<uint32_t>(fieldRva + 4 + displacement);
    };

    const size_t importOffset = rvaToOffset(importRva);
    REQUIRE(importOffset + 40 <= image.size());
    const uint32_t lookupRva = Read32(image, importOffset);
    const uint32_t dllNameRva = Read32(image, importOffset + 12);
    const uint32_t firstThunkRva = Read32(image, importOffset + 16);
    CHECK(ReadString(image, rvaToOffset(dllNameRva)) == "KERNEL32.DLL");
    CHECK(firstThunkRva == iatRva);
    CHECK(std::ranges::all_of(image | std::views::drop(importOffset + 20) | std::views::take(20),
                              [](const uint8_t byte) { return byte == 0; }));

    std::unordered_map<std::string, uint32_t> iatEntries;
    for (uint32_t index = 0;; ++index) {
        const uint64_t nameRva = Read64(image, rvaToOffset(lookupRva) + index * 8);
        const uint64_t iatValue = Read64(image, rvaToOffset(firstThunkRva) + index * 8);
        CHECK(iatValue == nameRva);
        if (nameRva == 0) {
            break;
        }
        const std::string name = ReadString(image, rvaToOffset(static_cast<uint32_t>(nameRva)) + 2);
        iatEntries[name] = firstThunkRva + index * 8;
    }
    REQUIRE(iatEntries.size() == 2);
    REQUIRE(iatEntries.contains("ExitProcess"));
    REQUIRE(iatEntries.contains("GetCurrentProcessId"));

    const size_t entryOffset = rvaToOffset(entryRva);
    REQUIRE(entryOffset + 32 <= image.size());
    CHECK(std::vector<uint8_t>(image.begin() + static_cast<std::ptrdiff_t>(entryOffset),
                               image.begin() + static_cast<std::ptrdiff_t>(entryOffset + 4)) ==
          std::vector<uint8_t>{0x48, 0x83, 0xEC, 0x28});
    CHECK(image[entryOffset + 4] == 0xE8);  // call Main
    CHECK(image[entryOffset + 9] == 0x89);  // mov ecx, eax
    CHECK(image[entryOffset + 11] == 0xE8); // call ExitProcess thunk
    CHECK(image[entryOffset + 16] == 0xCC); // cannot fall through

    const uint32_t mainRva = rel32Target(entryRva + 5);
    CHECK(mainRva % 16 == 0);
    const uint32_t exitThunkRva = rel32Target(entryRva + 12);
    CHECK(image[rvaToOffset(exitThunkRva)] == 0xFF);
    CHECK(image[rvaToOffset(exitThunkRva) + 1] == 0x25);
    CHECK(rel32Target(exitThunkRva + 2) == iatEntries["ExitProcess"]);

    const size_t mainOffset = rvaToOffset(mainRva);
    REQUIRE(mainOffset + 21 <= image.size());
    CHECK(image[mainOffset] == 0xE8);
    CHECK(image[mainOffset + 5] == 0xE8);
    CHECK(image[mainOffset + 10] == 0xE8);
    CHECK(rel32Target(mainRva + 1) == mainRva + 21); // local Helper
    const uint32_t importedThunkRva = rel32Target(mainRva + 6);
    CHECK(rel32Target(importedThunkRva + 2) == iatEntries["GetCurrentProcessId"]);
    const uint32_t secondRva = rel32Target(mainRva + 11);
    CHECK(secondRva % 32 == 0);
    CHECK(image[rvaToOffset(secondRva)] == 0xC3);

    // Absolute relocations use the same aligned symbol addresses as calls.
    CHECK(Read64(image, rdataSection.rawOffset) == imageBase + mainRva + 21);
    CHECK(dataSection.rva % 32 == 0);
    CHECK(Read64(image, dataSection.rawOffset) == imageBase + secondRva);

    // Both absolute slots appear as DIR64 base relocations so the loader can
    // slide the image away from its preferred base.
    const uint32_t relocDirRva = Read32(image, optional + 112 + 5 * 8);
    const uint32_t relocDirSize = Read32(image, optional + 112 + 5 * 8 + 4);
    const auto &relocSection = findSection(".reloc");
    CHECK(relocDirRva == relocSection.rva);
    CHECK(relocDirSize == relocSection.virtualSize);
    CHECK(ReadDir64Fixups(image, relocSection, relocDirSize) ==
          std::vector<uint32_t>{rdataSection.rva, dataSection.rva});
}

TEST_CASE("PE linker preserves both x86-64 DLL entry stub forms") {
    RcuFile library;
    library.sourcePath = "Library.rux";
    RcuSection text;
    text.name = ".text";
    text.type = RcuSecType::Text;
    text.flags = RcuSecFlag::Alloc | RcuSecFlag::Exec | RcuSecFlag::Read;
    text.alignment = 16;

    SUBCASE("calls DllMain when it is defined") {
        text.data = {0xC3, 0xC3};
        library.symbols.push_back({"DllMain", "", 0, 1, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});
        library.symbols.push_back({"Exported", "", 1, 1, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});
    }
    SUBCASE("returns TRUE when DllMain is absent") {
        text.data = {0xC3};
        library.symbols.push_back({"Exported", "", 0, 1, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});
    }
    library.sections.push_back(std::move(text));

    const bool definesDllMain =
        std::ranges::any_of(library.symbols, [](const RcuSymbol &symbol) { return symbol.name == "DllMain"; });
    const auto output = PeTestPath(".dll");
    auto importLibrary = output;
    importLibrary.replace_extension(".lib");
    std::error_code error;
    std::filesystem::remove(output, error);
    std::filesystem::remove(importLibrary, error);

    Linker linker({std::move(library)}, "PeBaseline", {}, ArtifactKind::SharedLibrary, Target::OS::Windows,
                  Target::Arch::X86_64);
    REQUIRE(linker.Link(output));

    std::ifstream stream(output, std::ios::binary);
    REQUIRE(stream.is_open());
    const std::vector<uint8_t> image((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    stream.close();
    std::filesystem::remove(output, error);
    std::filesystem::remove(importLibrary, error);

    const size_t peOffset = Read32(image, 0x3C);
    REQUIRE(Read16(image, peOffset + 4) == 0x8664);      // IMAGE_FILE_MACHINE_AMD64
    CHECK((Read16(image, peOffset + 22) & 0x0001) == 0); // relocations kept for ASLR
    const size_t optional = peOffset + 24;
    REQUIRE(Read16(image, optional) == 0x020B); // PE32+
    REQUIRE(Read16(image, optional + 68) == 2); // IMAGE_SUBSYSTEM_WINDOWS_GUI
    // A DLL without absolute slots needs no fixups, so it carries no .reloc
    // section, but stays relocatable and ASLR-capable.
    CHECK(Read32(image, optional + 112 + 5 * 8) == 0);
    CHECK((Read16(image, optional + 70) & 0x40) != 0); // DYNAMIC_BASE
    const uint32_t entryRva = Read32(image, optional + 16);
    const size_t sectionTable = optional + Read16(image, peOffset + 20);
    const uint32_t textRva = Read32(image, sectionTable + 12);
    const uint32_t textOffset = Read32(image, sectionTable + 20);
    const size_t entryOffset = textOffset + entryRva - textRva;
    REQUIRE(entryOffset + 16 <= image.size());

    CHECK(std::vector<uint8_t>(image.begin() + static_cast<std::ptrdiff_t>(entryOffset),
                               image.begin() + static_cast<std::ptrdiff_t>(entryOffset + 4)) ==
          std::vector<uint8_t>{0x48, 0x83, 0xEC, 0x28}); // sub rsp, 0x28
    CHECK(std::vector<uint8_t>(image.begin() + static_cast<std::ptrdiff_t>(entryOffset + 9),
                               image.begin() + static_cast<std::ptrdiff_t>(entryOffset + 14)) ==
          std::vector<uint8_t>{0x48, 0x83, 0xC4, 0x28, 0xC3}); // add rsp, 0x28; ret
    CHECK(image[entryOffset + 14] == 0xCC);                    // x86-64 alignment padding

    if (definesDllMain) {
        CHECK(image[entryOffset + 4] == 0xE8); // call DllMain
        const int32_t displacement = static_cast<int32_t>(Read32(image, entryOffset + 5));
        CHECK(entryRva + 9 + displacement == entryRva + 16);
    }
    else {
        CHECK(std::vector<uint8_t>(image.begin() + static_cast<std::ptrdiff_t>(entryOffset + 4),
                                   image.begin() + static_cast<std::ptrdiff_t>(entryOffset + 9)) ==
              std::vector<uint8_t>{0xB8, 1, 0, 0, 0}); // mov eax, TRUE
    }
}

TEST_CASE("PE linker preserves the missing x86-64 entry diagnostic") {
    RcuFile object;
    object.sourcePath = "NoMain.rux";
    const auto output = PeTestPath();
    std::error_code error;
    std::filesystem::remove(output, error);

    Linker linker({std::move(object)}, "PeBaseline", {}, ArtifactKind::Executable, Target::OS::Windows,
                  Target::Arch::X86_64);
    CHECK_FALSE(linker.Link(output));
    REQUIRE(linker.Errors().size() == 1);
    CHECK(linker.Errors().front().message == "undefined symbol 'Main' — no entry point found");
    CHECK_FALSE(std::filesystem::exists(output));
}

namespace {
int64_t SignExtend(const uint64_t value, const unsigned bits) {
    const uint64_t sign = uint64_t{1} << (bits - 1);
    return static_cast<int64_t>((value ^ sign) - sign);
}

uint32_t AArch64BranchTarget(const std::vector<uint8_t> &image, const size_t instructionOffset,
                             const uint32_t instructionRva) {
    const uint32_t word = Read32(image, instructionOffset);
    return static_cast<uint32_t>(static_cast<int64_t>(instructionRva) + SignExtend(word & 0x03FF'FFFFU, 26) * 4);
}

uint32_t AArch64AddressTarget(const std::vector<uint8_t> &image, const size_t adrpOffset, const uint32_t adrpRva) {
    const uint32_t adrp = Read32(image, adrpOffset);
    const uint32_t ldrOrAdd = Read32(image, adrpOffset + 4);
    const uint32_t immediate = (adrp >> 29U & 3U) | (adrp >> 5U & 0x7FFFFU) << 2U;
    const int64_t page = static_cast<int64_t>(adrpRva & ~0xFFFU) + SignExtend(immediate, 21) * 0x1000;
    const bool load = (ldrOrAdd & 0xFFC0'0000U) == 0xF940'0000U;
    const uint32_t low = ldrOrAdd >> 10U & 0xFFFU;
    return static_cast<uint32_t>(page + (load ? low << 3U : low));
}
} // namespace

TEST_CASE("PE linker emits and resolves a Windows AArch64 executable") {
    RcuFile first;
    first.arch = RcuArch::AArch64;
    first.sourcePath = "Main.rux";

    RcuSection text;
    text.name = ".text";
    text.type = RcuSecType::Text;
    text.flags = RcuSecFlag::Alloc | RcuSecFlag::Exec | RcuSecFlag::Read;
    text.alignment = 16;
    for (const uint32_t word : {
             0x94000000u, // bl Helper
             0x94000000u, // bl GetCurrentProcessId
             0x94000000u, // bl Second
             0x90000000u, // adrp x0, Constant
             0x91000000u, // add x0, x0, Constant@lo12
             0x90000001u, // adrp x1, Constant
             0xF9400021u, // ldr x1, [x1, Constant@lo12]
             0x52800920u, // mov w0, #73
             0xD65F03C0u, // ret
             0xD65F03C0u, // Helper: ret
         }) {
        AppendWord(text.data, word);
    }
    text.relocs = {
        {0, 1, RcuRelType::AArch64Call26, 0},         {4, 2, RcuRelType::AArch64Call26, 0},
        {8, 3, RcuRelType::AArch64Call26, 0},         {12, 4, RcuRelType::AArch64AdrPrelPgHi21, 0},
        {16, 4, RcuRelType::AArch64AddAbsLo12Nc, 0},  {20, 4, RcuRelType::AArch64AdrPrelPgHi21, 0},
        {24, 4, RcuRelType::AArch64LdstAbsLo12Nc, 0},
    };
    first.sections.push_back(std::move(text));

    RcuSection rodata;
    rodata.name = ".rdata";
    rodata.type = RcuSecType::RoData;
    rodata.flags = RcuSecFlag::Alloc | RcuSecFlag::Read;
    rodata.alignment = 16;
    rodata.data = {0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0, 0, 0, 0, 0, 0, 0, 0};
    rodata.relocs.push_back({8, 1, RcuRelType::Abs64, 0}); // vtable-like function pointer
    first.sections.push_back(std::move(rodata));

    first.symbols.push_back({"Main", "int", 0, 36, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});
    first.symbols.push_back({"Helper", "", 36, 4, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Local});
    first.symbols.push_back(
        {"GetCurrentProcessId", "", 0, 0, RCU_SEC_EXTERNAL, RcuSymKind::ExternFunc, RcuSymVis::Global});
    first.symbols.push_back({"Second", "", 0, 0, RCU_SEC_EXTERNAL, RcuSymKind::ExternFunc, RcuSymVis::Global});
    first.symbols.push_back({"Constant", "", 0, 8, RCU_RODATA_IDX, RcuSymKind::Const, RcuSymVis::Local});

    RcuFile second;
    second.arch = RcuArch::AArch64;
    second.sourcePath = "Second.rux";
    RcuSection secondText;
    secondText.name = ".text";
    secondText.type = RcuSecType::Text;
    secondText.flags = RcuSecFlag::Alloc | RcuSecFlag::Exec | RcuSecFlag::Read;
    secondText.alignment = 32;
    AppendWord(secondText.data, 0xD65F03C0); // ret
    second.sections.push_back(std::move(secondText));
    RcuSection data;
    data.name = ".data";
    data.type = RcuSecType::Data;
    data.flags = RcuSecFlag::Alloc | RcuSecFlag::Read | RcuSecFlag::Write;
    data.alignment = 8;
    data.data.resize(8);
    data.relocs.push_back({0, 0, RcuRelType::Abs64, 0});
    second.sections.push_back(std::move(data));
    second.symbols.push_back({"Second", "", 0, 4, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});

    const auto output = PeTestPath();
    std::error_code error;
    std::filesystem::remove(output, error);
    Linker linker({std::move(first), std::move(second)}, "AArch64Pe", {}, ArtifactKind::Executable, Target::OS::Windows,
                  Target::Arch::AArch64);
    REQUIRE(linker.Link(output));

    std::ifstream stream(output, std::ios::binary);
    REQUIRE(stream.is_open());
    const std::vector<uint8_t> image((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    stream.close();
    std::filesystem::remove(output, error);

    const size_t peOffset = Read32(image, 0x3C);
    REQUIRE(Read32(image, peOffset) == 0x0000'4550);
    CHECK(Read16(image, peOffset + 4) == 0xAA64);        // IMAGE_FILE_MACHINE_ARM64
    CHECK((Read16(image, peOffset + 22) & 0x0001) == 0); // relocations kept for ASLR
    REQUIRE(Read16(image, peOffset + 20) == 240);
    const size_t optional = peOffset + 24;
    CHECK(Read16(image, optional) == 0x020B); // PE32+
    const uint32_t entryRva = Read32(image, optional + 16);
    const uint64_t imageBase = Read64(image, optional + 24);
    CHECK(imageBase == 0x1'4000'0000ULL);
    CHECK(Read32(image, optional + 32) == 0x1000); // section alignment
    CHECK(Read32(image, optional + 36) == 0x200);  // file alignment
    // The ARM64 loader accepts exactly the 6.2 version baseline MSVC stamps:
    // 10.0 kills the process during initialization with
    // STATUS_INVALID_IMAGE_FORMAT, and lower values are rejected outright.
    CHECK(Read16(image, optional + 40) == 6); // MajorOperatingSystemVersion
    CHECK(Read16(image, optional + 42) == 2); // MinorOperatingSystemVersion
    CHECK(Read16(image, optional + 48) == 6); // MajorSubsystemVersion
    CHECK(Read16(image, optional + 50) == 2); // MinorSubsystemVersion
    CHECK(Read16(image, optional + 68) == 3); // console subsystem
    // Windows on ARM64 only loads relocatable, ASLR-capable images.
    CHECK((Read16(image, optional + 70) & 0x40) != 0); // DYNAMIC_BASE
    CHECK(Read64(image, optional + 72) == 0x100000);   // stack reserve
    CHECK(Read64(image, optional + 80) == 0x1000);     // stack commit
    CHECK(Read64(image, optional + 88) == 0x100000);   // heap reserve
    CHECK(Read64(image, optional + 96) == 0x1000);     // heap commit

    const uint16_t sectionCount = Read16(image, peOffset + 6);
    REQUIRE(sectionCount == 4);
    std::vector<PeSection> sections;
    const size_t sectionTable = optional + 240;
    for (uint16_t i = 0; i < sectionCount; ++i) {
        const size_t offset = sectionTable + static_cast<size_t>(i) * 40;
        PeSection section;
        for (size_t c = 0; c < 8 && image[offset + c] != 0; ++c) {
            section.name.push_back(static_cast<char>(image[offset + c]));
        }
        section.virtualSize = Read32(image, offset + 8);
        section.rva = Read32(image, offset + 12);
        section.rawSize = Read32(image, offset + 16);
        section.rawOffset = Read32(image, offset + 20);
        CHECK(section.rva % 0x1000 == 0);
        CHECK(section.rawOffset % 0x200 == 0);
        CHECK(static_cast<uint64_t>(section.rawOffset) + section.rawSize <= image.size());
        CHECK(static_cast<uint64_t>(section.rva) + section.virtualSize <= Read32(image, optional + 56));
        sections.push_back(std::move(section));
    }
    const auto findSection = [&](const std::string_view name) -> const PeSection & {
        return *std::ranges::find_if(sections, [&](const PeSection &section) { return section.name == name; });
    };
    const auto &textSection = findSection(".text");
    const auto &rdataSection = findSection(".rdata");
    const auto &dataSection = findSection(".data");
    CHECK(entryRva >= textSection.rva);
    CHECK(entryRva < textSection.rva + textSection.virtualSize);
    const auto rvaToOffset = [&](const uint32_t rva) -> size_t {
        for (const auto &section : sections) {
            if (rva >= section.rva && rva < section.rva + std::max(section.virtualSize, section.rawSize)) {
                return section.rawOffset + rva - section.rva;
            }
        }
        return image.size();
    };

    const uint32_t importRva = Read32(image, optional + 112 + 8);
    const uint32_t iatRva = Read32(image, optional + 112 + 12 * 8);
    CHECK(importRva >= rdataSection.rva);
    CHECK(iatRva >= rdataSection.rva);
    const size_t importOffset = rvaToOffset(importRva);
    const uint32_t lookupRva = Read32(image, importOffset);
    const uint32_t dllNameRva = Read32(image, importOffset + 12);
    const uint32_t firstThunkRva = Read32(image, importOffset + 16);
    CHECK(ReadString(image, rvaToOffset(dllNameRva)) == "KERNEL32.DLL");
    CHECK(firstThunkRva == iatRva);
    std::unordered_map<std::string, uint32_t> iatEntries;
    for (uint32_t index = 0;; ++index) {
        const uint64_t nameRva = Read64(image, rvaToOffset(lookupRva) + index * 8);
        if (nameRva == 0) {
            break;
        }
        iatEntries[ReadString(image, rvaToOffset(static_cast<uint32_t>(nameRva)) + 2)] = iatRva + index * 8;
    }
    REQUIRE(iatEntries.size() == 2);

    const size_t entryOffset = rvaToOffset(entryRva);
    CHECK(Read32(image, entryOffset) == 0xA9BF7BFD);      // aligned frame
    CHECK(Read32(image, entryOffset + 4) == 0x910003FD);  // mov x29, sp
    CHECK(Read32(image, entryOffset + 16) == 0xD4200000); // cannot fall through
    const uint32_t mainRva = AArch64BranchTarget(image, entryOffset + 8, entryRva + 8);
    const uint32_t exitThunkRva = AArch64BranchTarget(image, entryOffset + 12, entryRva + 12);
    CHECK(mainRva % 16 == 0);
    CHECK(AArch64AddressTarget(image, rvaToOffset(exitThunkRva), exitThunkRva) == iatEntries["ExitProcess"]);
    CHECK(Read32(image, rvaToOffset(exitThunkRva) + 8) == 0xD61F0200); // br x16

    const size_t mainOffset = rvaToOffset(mainRva);
    const uint32_t helperRva = AArch64BranchTarget(image, mainOffset, mainRva);
    const uint32_t importedThunkRva = AArch64BranchTarget(image, mainOffset + 4, mainRva + 4);
    const uint32_t secondRva = AArch64BranchTarget(image, mainOffset + 8, mainRva + 8);
    CHECK(helperRva == mainRva + 36);
    CHECK(secondRva % 32 == 0);
    CHECK(AArch64AddressTarget(image, rvaToOffset(importedThunkRva), importedThunkRva) ==
          iatEntries["GetCurrentProcessId"]);
    CHECK(Read32(image, rvaToOffset(importedThunkRva) + 8) == 0xD61F0200);
    CHECK(AArch64AddressTarget(image, mainOffset + 12, mainRva + 12) == rdataSection.rva);
    CHECK(AArch64AddressTarget(image, mainOffset + 20, mainRva + 20) == rdataSection.rva);
    CHECK(Read32(image, mainOffset + 28) == 0x52800920); // distinctive exit code 73
    CHECK(Read64(image, rdataSection.rawOffset + 8) == imageBase + helperRva);
    CHECK(Read64(image, dataSection.rawOffset) == imageBase + secondRva);

    // Both absolute slots appear as DIR64 base relocations; the ARM64 loader
    // refuses an image without them.
    const uint32_t relocDirRva = Read32(image, optional + 112 + 5 * 8);
    const uint32_t relocDirSize = Read32(image, optional + 112 + 5 * 8 + 4);
    const auto &relocSection = findSection(".reloc");
    CHECK(relocDirRva == relocSection.rva);
    CHECK(relocDirSize == relocSection.virtualSize);
    CHECK(ReadDir64Fixups(image, relocSection, relocDirSize) ==
          std::vector<uint32_t>{rdataSection.rva + 8, dataSection.rva});
}

TEST_CASE("PE linker eight-byte aligns Windows AArch64 thunk tables across DLL groups") {
    const auto output = PeTestPath();
    auto importedLibrary = output;
    importedLibrary.replace_filename(output.stem().string() + "-imports.dll");
    auto importLibrary = importedLibrary;
    importLibrary.replace_extension(".lib");
    std::error_code filesystemError;
    std::filesystem::remove(output, filesystemError);
    std::filesystem::remove(importedLibrary, filesystemError);
    std::filesystem::remove(importLibrary, filesystemError);

    RcuFile library;
    library.arch = RcuArch::AArch64;
    library.sourcePath = "Imported.rux";
    RcuSection libraryText;
    libraryText.name = ".text";
    libraryText.type = RcuSecType::Text;
    libraryText.flags = RcuSecFlag::Alloc | RcuSecFlag::Exec | RcuSecFlag::Read;
    libraryText.alignment = 4;
    AppendWord(libraryText.data, 0xD65F03C0); // ret
    library.sections.push_back(std::move(libraryText));
    library.symbols.push_back({"Imported", "int", 0, 4, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});

    Linker libraryLinker({std::move(library)}, importedLibrary.stem().string(), {}, ArtifactKind::SharedLibrary,
                         Target::OS::Windows, Target::Arch::AArch64);
    REQUIRE(libraryLinker.Link(importedLibrary));

    RcuFile executable;
    executable.arch = RcuArch::AArch64;
    executable.sourcePath = "Main.rux";
    RcuSection executableText;
    executableText.name = ".text";
    executableText.type = RcuSecType::Text;
    executableText.flags = RcuSecFlag::Alloc | RcuSecFlag::Exec | RcuSecFlag::Read;
    executableText.alignment = 4;
    AppendWord(executableText.data, 0x94000000); // bl Imported
    AppendWord(executableText.data, 0x52800000); // mov w0, #0
    AppendWord(executableText.data, 0xD65F03C0); // ret
    executableText.relocs.push_back({0, 1, RcuRelType::AArch64Call26, 0});
    executable.sections.push_back(std::move(executableText));
    executable.symbols.push_back({"Main", "int", 0, 12, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});
    executable.symbols.push_back({"Imported", importedLibrary.filename().string(), 0, 0, RCU_SEC_EXTERNAL,
                                  RcuSymKind::ExternFunc, RcuSymVis::Global});

    Linker executableLinker({std::move(executable)}, "AlignedImports", {importedLibrary.parent_path()},
                            ArtifactKind::Executable, Target::OS::Windows, Target::Arch::AArch64);
    REQUIRE(executableLinker.Link(output));
    const auto image = ReadFile(output);
    std::filesystem::remove(output, filesystemError);
    std::filesystem::remove(importedLibrary, filesystemError);
    std::filesystem::remove(importLibrary, filesystemError);

    const size_t peOffset = Read32(image, 0x3C);
    const size_t optional = peOffset + 24;
    const uint32_t importRva = Read32(image, optional + 112 + 8);
    const uint32_t importSize = Read32(image, optional + 112 + 12);
    const uint32_t iatRva = Read32(image, optional + 112 + 12 * 8);
    CHECK(importSize == 60); // two DLL descriptors plus the null descriptor
    CHECK(iatRva % 8 == 0);

    const size_t sectionTable = optional + Read16(image, peOffset + 20);
    const uint16_t sectionCount = Read16(image, peOffset + 6);
    size_t importOffset = image.size();
    for (uint16_t i = 0; i < sectionCount; ++i) {
        const size_t section = sectionTable + static_cast<size_t>(i) * 40;
        const uint32_t sectionRva = Read32(image, section + 12);
        const uint32_t sectionSize = std::max(Read32(image, section + 8), Read32(image, section + 16));
        if (importRva >= sectionRva && importRva < sectionRva + sectionSize) {
            importOffset = Read32(image, section + 20) + importRva - sectionRva;
            break;
        }
    }
    REQUIRE(importOffset != image.size());
    for (size_t descriptor = 0; descriptor < 2; ++descriptor) {
        CHECK(Read32(image, importOffset + descriptor * 20) % 8 == 0);      // OriginalFirstThunk
        CHECK(Read32(image, importOffset + descriptor * 20 + 16) % 8 == 0); // FirstThunk
    }
}

TEST_CASE("PE linker emits Windows AArch64 DLL entries exports and import libraries") {
    for (const bool definesDllMain : {false, true}) {
        CAPTURE(definesDllMain);
        RcuFile library;
        library.arch = RcuArch::AArch64;
        library.sourcePath = "Library.rux";

        RcuSection text;
        text.name = ".text";
        text.type = RcuSecType::Text;
        text.flags = RcuSecFlag::Alloc | RcuSecFlag::Exec | RcuSecFlag::Read;
        text.alignment = 16;
        if (definesDllMain) {
            AppendWord(text.data, 0xD65F03C0); // DllMain: ret
            library.symbols.push_back({"DllMain", "", 0, 4, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});
        }
        const uint32_t exportedOffset = static_cast<uint32_t>(text.data.size());
        AppendWord(text.data, 0x52800540); // Exported: mov w0, #42
        AppendWord(text.data, 0xD65F03C0); // ret
        library.symbols.push_back(
            {"Exported", "int", exportedOffset, 8, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});
        library.sections.push_back(std::move(text));

        const auto output = PeTestPath(".dll");
        auto importLibrary = output;
        importLibrary.replace_extension(".lib");
        std::error_code filesystemError;
        std::filesystem::remove(output, filesystemError);
        std::filesystem::remove(importLibrary, filesystemError);

        Linker linker({std::move(library)}, "AArch64Library", {}, ArtifactKind::SharedLibrary, Target::OS::Windows,
                      Target::Arch::AArch64);
        REQUIRE(linker.Link(output));
        REQUIRE(std::filesystem::is_regular_file(importLibrary));
        const auto image = ReadFile(output);
        const auto archive = ReadFile(importLibrary);
        std::filesystem::remove(output, filesystemError);
        std::filesystem::remove(importLibrary, filesystemError);

        const size_t peOffset = Read32(image, 0x3C);
        REQUIRE(Read16(image, peOffset + 4) == 0xAA64);      // IMAGE_FILE_MACHINE_ARM64
        CHECK((Read16(image, peOffset + 22) & 0x2000) != 0); // IMAGE_FILE_DLL
        CHECK((Read16(image, peOffset + 22) & 0x0001) == 0); // relocations kept for ASLR
        const size_t optional = peOffset + 24;
        CHECK(Read64(image, optional + 24) == 0x1'8000'0000ULL);
        CHECK(Read16(image, optional + 68) == 2);          // GUI subsystem
        CHECK((Read16(image, optional + 70) & 0x40) != 0); // DYNAMIC_BASE
        const uint32_t entryRva = Read32(image, optional + 16);
        const uint32_t exportRva = Read32(image, optional + 112);
        REQUIRE(exportRva != 0);

        const size_t sectionTable = optional + Read16(image, peOffset + 20);
        const uint16_t sectionCount = Read16(image, peOffset + 6);
        std::vector<PeSection> sections;
        for (uint16_t i = 0; i < sectionCount; ++i) {
            const size_t offset = sectionTable + static_cast<size_t>(i) * 40;
            PeSection section;
            for (size_t c = 0; c < 8 && image[offset + c] != 0; ++c) {
                section.name.push_back(static_cast<char>(image[offset + c]));
            }
            section.virtualSize = Read32(image, offset + 8);
            section.rva = Read32(image, offset + 12);
            section.rawSize = Read32(image, offset + 16);
            section.rawOffset = Read32(image, offset + 20);
            sections.push_back(std::move(section));
        }
        const auto rvaToOffset = [&](const uint32_t rva) -> size_t {
            const auto section = std::ranges::find_if(sections, [&](const PeSection &candidate) {
                return rva >= candidate.rva && rva < candidate.rva + std::max(candidate.virtualSize, candidate.rawSize);
            });
            REQUIRE(section != sections.end());
            return section->rawOffset + rva - section->rva;
        };

        const size_t entryOffset = rvaToOffset(entryRva);
        CHECK(Read32(image, entryOffset) == 0xA9BF7BFD);      // save x29/x30 and align
        CHECK(Read32(image, entryOffset + 4) == 0x910003FD);  // mov x29, sp
        CHECK(Read32(image, entryOffset + 12) == 0xA8C17BFD); // restore x29/x30
        CHECK(Read32(image, entryOffset + 16) == 0xD65F03C0); // ret to the loader
        if (definesDllMain) {
            CHECK(AArch64BranchTarget(image, entryOffset + 8, entryRva + 8) == entryRva + 32);
        }
        else {
            CHECK(Read32(image, entryOffset + 8) == 0x52800020); // mov w0, #TRUE
        }

        const size_t exportOffset = rvaToOffset(exportRva);
        CHECK(Read32(image, exportOffset + 20) == 1);
        CHECK(Read32(image, exportOffset + 24) == 1);
        const uint32_t functionArrayRva = Read32(image, exportOffset + 28);
        const uint32_t nameArrayRva = Read32(image, exportOffset + 32);
        const uint32_t exportedRva = Read32(image, rvaToOffset(functionArrayRva));
        CHECK(ReadString(image, rvaToOffset(Read32(image, rvaToOffset(nameArrayRva)))) == "Exported");
        CHECK(Read32(image, rvaToOffset(exportedRva)) == 0x52800540); // ARM64 exported function body
        CHECK(exportedRva == entryRva + 32 + exportedOffset);

        REQUIRE(std::string(archive.begin(), archive.begin() + 8) == "!<arch>\n");
        const auto members = ReadArchiveMembers(archive);
        REQUIRE(members.size() == 3);
        CHECK(ReadBig32(archive, members[0].data) == 2); // Exported and __imp_Exported
        CHECK(ReadBig32(archive, members[0].data + 4) == members[2].header);
        CHECK(ReadBig32(archive, members[0].data + 8) == members[2].header);
        CHECK(Read32(archive, members[1].data) == 1); // one object member
        CHECK(Read32(archive, members[1].data + 4) == members[2].header);
        CHECK(Read32(archive, members[1].data + 8) == 2); // two indexed symbols
        CHECK(Read16(archive, members[1].data + 12) == 1);
        CHECK(Read16(archive, members[1].data + 14) == 1);
        CHECK(ReadString(archive, members[1].data + 16) == "Exported");
        CHECK(ReadString(archive, members[1].data + 25) == "__imp_Exported");
        CHECK(Read16(archive, members[2].data + 6) == 0xAA64); // short import member machine
        CHECK(ReadString(archive, members[2].data + 20) == "Exported");
    }
}

TEST_CASE("PE linker keeps Windows AArch64 static-library members architecture-stamped") {
    RcuFile library;
    library.arch = RcuArch::AArch64;
    library.sourcePath = "Static.rux";
    RcuSection text;
    text.name = ".text";
    text.type = RcuSecType::Text;
    text.flags = RcuSecFlag::Alloc | RcuSecFlag::Exec | RcuSecFlag::Read;
    text.alignment = 4;
    AppendWord(text.data, 0xD65F03C0);
    library.sections.push_back(std::move(text));
    library.symbols.push_back({"Answer", "int", 0, 4, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});

    const auto output = PeTestPath(".lib");
    std::error_code filesystemError;
    std::filesystem::remove(output, filesystemError);
    Linker linker({std::move(library)}, "Static", {}, ArtifactKind::StaticLibrary, Target::OS::Windows,
                  Target::Arch::AArch64);
    REQUIRE(linker.Link(output));
    const auto archive = ReadFile(output);
    std::filesystem::remove(output, filesystemError);
    const auto members = ReadArchiveMembers(archive);
    REQUIRE(members.size() == 3);
    CHECK(Read16(archive, members[2].data) == 0xAA64); // normal COFF object machine
}

TEST_CASE("PE linker diagnoses invalid Windows AArch64 relocations") {
    RcuFile object;
    object.arch = RcuArch::AArch64;
    object.sourcePath = "Invalid.rux";
    RcuSection text;
    text.name = ".text";
    text.type = RcuSecType::Text;
    text.flags = RcuSecFlag::Alloc | RcuSecFlag::Exec | RcuSecFlag::Read;
    text.alignment = 4;
    AppendWord(text.data, 0x94000000);
    object.sections.push_back(std::move(text));
    object.symbols.push_back({"Main", "int", 0, 4, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});

    SUBCASE("out-of-range branch") {
        object.sections[0].relocs.push_back({0, 0, RcuRelType::AArch64Call26, 1 << 27});
    }
    SUBCASE("x86-64 relocation in an AArch64 object") {
        object.sections[0].relocs.push_back({0, 0, RcuRelType::Rel32, 0});
    }

    const auto output = PeTestPath();
    std::error_code error;
    std::filesystem::remove(output, error);
    Linker linker({std::move(object)}, "Invalid", {}, ArtifactKind::Executable, Target::OS::Windows,
                  Target::Arch::AArch64);
    CHECK_FALSE(linker.Link(output));
    REQUIRE(linker.Errors().size() == 1);
    if (linker.Errors().front().message.starts_with("AARCH64_CALL26")) {
        CHECK(linker.Errors().front().message.contains("out of range"));
        CHECK(linker.Errors().front().message.contains("128 MB"));
    }
    else {
        CHECK(linker.Errors().front().message ==
              "relocation REL_32 against 'Main' is not supported by the PE32+ writer");
    }
    CHECK_FALSE(std::filesystem::exists(output));
}
