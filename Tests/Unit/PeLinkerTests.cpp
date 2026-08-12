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

uint64_t Read64(const std::vector<uint8_t> &image, const size_t offset) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(image.at(offset + i)) << (i * 8U);
    }
    return value;
}

std::string ReadString(const std::vector<uint8_t> &image, size_t offset) {
    std::string value;
    while (offset < image.size() && image[offset] != 0) {
        value.push_back(static_cast<char>(image[offset++]));
    }
    return value;
}

std::filesystem::path PeTestPath() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() / ("rux-pe-baseline-" + std::to_string(nonce) + ".exe");
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
    CHECK(Read32(image, peOffset) == 0x0000'4550); // PE\0\0
    CHECK(Read16(image, peOffset + 4) == 0x8664);  // IMAGE_FILE_MACHINE_AMD64
    const uint16_t sectionCount = Read16(image, peOffset + 6);
    REQUIRE(sectionCount == 3);
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
}
