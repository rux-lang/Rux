#include "ElfReader.h"
#include "Linker/Linker.h"

#include <algorithm>
#include <cstdint>
#include <doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <ranges>
#include <string>
#include <vector>

using namespace Rux;
using Rux::Testing::ElfImage;

namespace {
constexpr std::uint64_t kPage = 0x10000;

void AppendWord(std::vector<std::uint8_t> &bytes, const std::uint32_t word) {
    for (unsigned byte = 0; byte < 4; ++byte) {
        bytes.push_back(static_cast<std::uint8_t>(word >> (byte * 8U)));
    }
}

RcuSection Section(std::string name, const std::uint32_t type, const std::uint32_t flags, const std::uint16_t alignment,
                   std::vector<std::uint8_t> data = {}) {
    return {std::move(name), type, flags, alignment, std::move(data), {}};
}

RcuFile MainObject(const std::uint16_t textAlignment = 4) {
    RcuFile object;
    object.arch = RcuArch::AArch64;
    auto text =
        Section(".text", RcuSecType::Text, RcuSecFlag::Alloc | RcuSecFlag::Read | RcuSecFlag::Exec, textAlignment);
    AppendWord(text.data, 0xD2800540); // mov x0, #42
    AppendWord(text.data, 0xD65F03C0); // ret
    object.sections.push_back(std::move(text));
    object.symbols.push_back({"Main", "int", 0, 8, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});
    return object;
}

std::vector<std::uint8_t> ReadFile(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream.is_open());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

ElfImage LinkImage(std::vector<RcuFile> objects, const std::filesystem::path &path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    Linker linker(std::move(objects), "FreeBsdAArch64Test", {}, ArtifactKind::Executable, Target::OS::FreeBSD,
                  Target::Arch::AArch64);
    REQUIRE(linker.Link(path));
    REQUIRE(linker.Errors().empty());
    ElfImage image{ReadFile(path)};
    std::filesystem::remove(path, ec);
    return image;
}

std::uint64_t BranchTarget(const ElfImage &image, const std::uint64_t address) {
    const auto displacement = static_cast<std::int32_t>((image.Word(address) & 0x03FFFFFFU) << 6U) >> 6;
    return address + 4 * static_cast<std::int64_t>(displacement);
}

std::vector<ElfImage::Segment> LoadSegments(const ElfImage &image) {
    std::vector<ElfImage::Segment> loads;
    std::ranges::copy_if(image.Segments(), std::back_inserter(loads),
                         [](const ElfImage::Segment &segment) { return segment.type == 1; });
    return loads;
}
} // namespace

TEST_SUITE("FreeBSD AArch64 freestanding ELF") {
    TEST_CASE("FreeBSD AArch64 freestanding executable has target-owned entry and segments") {
        RcuFile object = MainObject();
        object.sections.push_back(
            Section(".rodata", RcuSecType::RoData, RcuSecFlag::Alloc | RcuSecFlag::Read, 8, {1, 2, 3, 4}));
        object.sections.push_back(Section(".data", RcuSecType::Data,
                                          RcuSecFlag::Alloc | RcuSecFlag::Read | RcuSecFlag::Write, 8,
                                          {9, 10, 11, 12, 13, 14, 15, 16}));
        object.sections.push_back(Section(".bss", RcuSecType::Bss,
                                          RcuSecFlag::Alloc | RcuSecFlag::Read | RcuSecFlag::Write, 16,
                                          std::vector<std::uint8_t>(24)));
        object.symbols.push_back({"Zeroes", "int", 0, 24, RCU_BSS_IDX, RcuSymKind::Data, RcuSymVis::Global});

        const auto directory = std::filesystem::temp_directory_path();
        const ElfImage first = LinkImage({object}, directory / "rux-freebsd-aarch64-static-first");
        const ElfImage second = LinkImage({std::move(object)}, directory / "rux-freebsd-aarch64-static-second");
        CHECK(first.bytes == second.bytes);

        CHECK(first.OsAbi() == 9);     // ELFOSABI_FREEBSD
        CHECK(first.Type() == 2);      // ET_EXEC
        CHECK(first.Machine() == 183); // EM_AARCH64
        CHECK(first.Entry() % 4 == 0);
        CHECK_FALSE(first.SegmentOfType(3).has_value()); // no PT_INTERP
        CHECK_FALSE(first.SegmentOfType(2).has_value()); // no PT_DYNAMIC
        CHECK(first.Interpreter().empty());
        CHECK(first.NeededLibraries().empty());
        CHECK(first.DynamicTag(3) == 0);  // no DT_PLTGOT
        CHECK(first.DynamicTag(23) == 0); // no DT_JMPREL/PLT
        CHECK(std::string(first.bytes.begin(), first.bytes.end()).find("libc.so") == std::string::npos);

        const auto loads = LoadSegments(first);
        REQUIRE(loads.size() == 3);
        CHECK(loads[0].flags == 0x5); // read/execute
        CHECK(loads[1].flags == 0x4); // read-only
        CHECK(loads[2].flags == 0x6); // read/write
        for (std::size_t i = 0; i < loads.size(); ++i) {
            CHECK(loads[i].alignment == kPage);
            CHECK(loads[i].address % kPage == loads[i].offset % kPage);
            if (i != 0) {
                const auto previousPageEnd =
                    (loads[i - 1].address + loads[i - 1].memorySize + kPage - 1) & ~(kPage - 1);
                CHECK(previousPageEnd <= loads[i].address);
            }
        }
        CHECK(loads[2].fileSize == 8);
        CHECK(loads[2].memorySize == 40); // data, alignment padding, then 24 zero-initialized bytes
        CHECK(loads[2].offset + loads[2].fileSize == first.bytes.size());

        const std::uint64_t entry = first.Entry();
        CHECK(first.Word(entry) == 0x910003E9);     // mov x9, sp
        CHECK(first.Word(entry + 4) == 0x927CED29); // and x9, x9, #-16
        CHECK(first.Word(entry + 8) == 0x9100013F); // mov sp, x9
        CHECK(BranchTarget(first, entry + 12) == entry + 24);
        CHECK(first.Word(entry + 16) == (0xD2800008U | 1U << 5U)); // mov x8, #SYS_exit
        CHECK(first.Word(entry + 20) == 0xD4000001);               // svc #0
    }

    TEST_CASE("FreeBSD AArch64 freestanding executable resolves defined symbols across objects") {
        RcuFile caller;
        caller.arch = RcuArch::AArch64;
        auto text = Section(".text", RcuSecType::Text, RcuSecFlag::Alloc | RcuSecFlag::Read | RcuSecFlag::Exec, 4);
        for (const std::uint32_t word : {0x94000000U, 0x90000000U, 0x91000000U, 0xF9400001U, 0xD2800002U, 0xF2A00002U,
                                         0xF2C00002U, 0xF2E00002U, 0xD65F03C0U}) {
            AppendWord(text.data, word);
        }
        text.relocs = {{0, 1, RcuRelType::AArch64Call26, 0},       {4, 2, RcuRelType::AArch64AdrPrelPgHi21, 0},
                       {8, 2, RcuRelType::AArch64AddAbsLo12Nc, 0}, {12, 2, RcuRelType::AArch64LdstAbsLo12Nc, 0},
                       {16, 2, RcuRelType::AArch64MovwUabsG0, 0},  {20, 2, RcuRelType::AArch64MovwUabsG1, 0},
                       {24, 2, RcuRelType::AArch64MovwUabsG2, 0},  {28, 2, RcuRelType::AArch64MovwUabsG3, 0}};
        caller.sections.push_back(std::move(text));
        caller.sections.push_back(Section(".rodata", RcuSecType::RoData, RcuSecFlag::Alloc | RcuSecFlag::Read, 8));
        auto data = Section(".data", RcuSecType::Data, RcuSecFlag::Alloc | RcuSecFlag::Read | RcuSecFlag::Write, 8,
                            std::vector<std::uint8_t>(16));
        data.relocs = {{0, 2, RcuRelType::Abs64, 0}, {8, 1, RcuRelType::AArch64Prel64, 0}};
        caller.sections.push_back(std::move(data));
        caller.symbols = {{"Main", "int", 0, 36, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global},
                          {"Worker", "int", 0, 0, RCU_SEC_EXTERNAL, RcuSymKind::ExternFunc, RcuSymVis::Global},
                          {"State", "int", 0, 0, RCU_SEC_EXTERNAL, RcuSymKind::ExternData, RcuSymVis::Global}};

        RcuFile definitions;
        definitions.arch = RcuArch::AArch64;
        auto worker = Section(".text", RcuSecType::Text, RcuSecFlag::Alloc | RcuSecFlag::Read | RcuSecFlag::Exec, 4);
        AppendWord(worker.data, 0xD65F03C0); // ret
        definitions.sections.push_back(std::move(worker));
        definitions.sections.push_back(Section(".rodata", RcuSecType::RoData, RcuSecFlag::Alloc | RcuSecFlag::Read, 8));
        definitions.sections.push_back(
            Section(".data", RcuSecType::Data, RcuSecFlag::Alloc | RcuSecFlag::Read | RcuSecFlag::Write, 8));
        definitions.sections.push_back(Section(".bss", RcuSecType::Bss,
                                               RcuSecFlag::Alloc | RcuSecFlag::Read | RcuSecFlag::Write, 8,
                                               std::vector<std::uint8_t>(16)));
        definitions.symbols = {{"Worker", "int", 0, 4, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global},
                               {"State", "int", 0, 16, RCU_BSS_IDX, RcuSymKind::Data, RcuSymVis::Global}};

        const ElfImage image = LinkImage({std::move(caller), std::move(definitions)},
                                         std::filesystem::temp_directory_path() / "rux-freebsd-aarch64-relocations");
        const std::uint64_t main = image.Entry() + 24;
        const std::uint64_t workerAddress = BranchTarget(image, main);
        CHECK(image.Word(workerAddress) == 0xD65F03C0);

        const std::uint32_t adrp = image.Word(main + 4);
        const auto pages = static_cast<std::int32_t>(((adrp >> 5U & 0x7FFFFU) << 2U | (adrp >> 29U & 3U)) << 11U) >> 11;
        const std::uint64_t page = ((main + 4) & ~std::uint64_t{0xFFF}) + (static_cast<std::int64_t>(pages) << 12U);
        const std::uint64_t state = page + (image.Word(main + 8) >> 10U & 0xFFFU);
        CHECK((image.Word(main + 12) >> 10U & 0xFFFU) == (state & 0xFFFU) >> 3U);
        std::uint64_t movedAddress = 0;
        for (unsigned halfword = 0; halfword < 4; ++halfword) {
            movedAddress |= static_cast<std::uint64_t>(image.Word(main + 16 + halfword * 4) >> 5U & 0xFFFFU)
                         << (halfword * 16U);
        }
        CHECK(movedAddress == state);

        const std::uint64_t dataAddress = image.WritableSegmentAddress();
        CHECK(image.Giant(dataAddress) == state); // ABS64
        CHECK(static_cast<std::int64_t>(image.Giant(dataAddress + 8)) ==
              static_cast<std::int64_t>(workerAddress - (dataAddress + 8))); // PREL64
        const auto loads = LoadSegments(image);
        const auto writable = std::ranges::find(loads, 0x6U, &ElfImage::Segment::flags);
        REQUIRE(writable != loads.end());
        CHECK(state >= writable->address + writable->fileSize);
        CHECK(state + 16 <= writable->address + writable->memorySize);
    }

    TEST_CASE("FreeBSD AArch64 freestanding executable omits empty permission classes and honors high alignment") {
        RcuFile object = MainObject(32768);
        object.sections.push_back(Section(".rodata", RcuSecType::RoData, RcuSecFlag::Alloc | RcuSecFlag::Read, 64));
        object.sections.push_back(
            Section(".data", RcuSecType::Data, RcuSecFlag::Alloc | RcuSecFlag::Read | RcuSecFlag::Write, 64));
        const ElfImage image = LinkImage({std::move(object)},
                                         std::filesystem::temp_directory_path() / "rux-freebsd-aarch64-empty-sections");
        const auto loads = LoadSegments(image);
        REQUIRE(loads.size() == 1);
        CHECK(loads.front().flags == 0x5);
        CHECK(BranchTarget(image, image.Entry() + 12) % 32768 == 0);
    }

    TEST_CASE("FreeBSD AArch64 freestanding executable diagnoses missing and duplicate Main") {
        const auto output = std::filesystem::temp_directory_path() / "rux-freebsd-aarch64-bad-main";
        std::error_code ec;
        std::filesystem::remove(output, ec);

        RcuFile missing = MainObject();
        missing.symbols.front().name = "NotMain";
        Linker missingLinker({std::move(missing)}, "Missing", {}, ArtifactKind::Executable, Target::OS::FreeBSD,
                             Target::Arch::AArch64);
        CHECK_FALSE(missingLinker.Link(output));
        REQUIRE(missingLinker.Errors().size() == 1);
        CHECK(missingLinker.Errors().front().message == "undefined symbol 'Main' — no entry point found");
        CHECK_FALSE(std::filesystem::exists(output));

        RcuFile first = MainObject();
        RcuFile second = MainObject();
        Linker duplicateLinker({std::move(first), std::move(second)}, "Duplicate", {}, ArtifactKind::Executable,
                               Target::OS::FreeBSD, Target::Arch::AArch64);
        CHECK_FALSE(duplicateLinker.Link(output));
        REQUIRE(duplicateLinker.Errors().size() == 1);
        CHECK(duplicateLinker.Errors().front().message == "duplicate symbol 'Main'");
        CHECK_FALSE(std::filesystem::exists(output));
    }

    TEST_CASE("FreeBSD AArch64 freestanding executable diagnoses relocation range alignment and bounds") {
        const auto output = std::filesystem::temp_directory_path() / "rux-freebsd-aarch64-bad-relocation";
        const auto checkFailure = [&output](RcuFile object, const std::string &fragment) {
            std::error_code ec;
            std::filesystem::remove(output, ec);
            Linker linker({std::move(object)}, "Relocation", {}, ArtifactKind::Executable, Target::OS::FreeBSD,
                          Target::Arch::AArch64);
            CHECK_FALSE(linker.Link(output));
            REQUIRE(linker.Errors().size() == 1);
            CHECK(linker.Errors().front().message.find(fragment) != std::string::npos);
            CHECK_FALSE(std::filesystem::exists(output));
        };

        RcuFile branch = MainObject();
        branch.sections.front().data = {0x00, 0x00, 0x00, 0x94, 0xC0, 0x03, 0x5F, 0xD6};
        branch.sections.front().relocs.push_back(
            {0, 0, RcuRelType::AArch64Call26, std::numeric_limits<std::int32_t>::max()});
        checkFailure(std::move(branch), "a branch reaches 128 MB either way");

        RcuFile low12 = MainObject();
        low12.sections.front().data = {0x01, 0x00, 0x40, 0xF9, 0xC0, 0x03, 0x5F, 0xD6}; // ldr x1, [x0] / ret
        low12.sections.front().relocs.push_back({0, 1, RcuRelType::AArch64LdstAbsLo12Nc, 1});
        low12.sections.push_back(Section(".rodata", RcuSecType::RoData, RcuSecFlag::Alloc | RcuSecFlag::Read, 8));
        low12.sections.push_back(Section(".data", RcuSecType::Data,
                                         RcuSecFlag::Alloc | RcuSecFlag::Read | RcuSecFlag::Write, 8,
                                         std::vector<std::uint8_t>(8)));
        low12.symbols.push_back({"Value", "int", 0, 8, RCU_DATA_IDX, RcuSymKind::Data, RcuSymVis::Global});
        checkFailure(std::move(low12), "the symbol is not aligned to the access width");

        RcuFile bounds = MainObject();
        bounds.sections.front().relocs.push_back({8, 0, RcuRelType::AArch64Call26, 0});
        checkFailure(std::move(bounds), "exceeds section '.text'");
    }
} // TEST_SUITE
