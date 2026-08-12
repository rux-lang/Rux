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
#include <utility>
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

RcuFile ImportingMain(const std::vector<std::pair<std::string, std::string>> &imports) {
    RcuFile object;
    object.arch = RcuArch::AArch64;
    auto text = Section(".text", RcuSecType::Text, RcuSecFlag::Alloc | RcuSecFlag::Read | RcuSecFlag::Exec, 4);
    for (std::size_t index = 0; index < imports.size(); ++index) {
        AppendWord(text.data, 0x94000000); // bl import
        text.relocs.push_back({static_cast<std::uint32_t>(index * 4), static_cast<std::uint32_t>(index + 1),
                               RcuRelType::AArch64Call26, 0});
    }
    AppendWord(text.data, 0xD65F03C0); // ret
    object.sections.push_back(std::move(text));
    object.symbols.push_back({"Main", "int", 0, static_cast<std::uint32_t>((imports.size() + 1) * 4), RCU_TEXT_IDX,
                              RcuSymKind::Func, RcuSymVis::Global});
    for (const auto &[name, library] : imports) {
        object.symbols.push_back({name, library, 0, 0, RCU_SEC_EXTERNAL, RcuSymKind::ExternFunc, RcuSymVis::Global});
    }
    return object;
}

RcuFile ImportDeclarations(const std::vector<std::pair<std::string, std::string>> &imports) {
    RcuFile object;
    object.arch = RcuArch::AArch64;
    for (const auto &[name, library] : imports) {
        object.symbols.push_back({name, library, 0, 0, RCU_SEC_EXTERNAL, RcuSymKind::ExternFunc, RcuSymVis::Global});
    }
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

TEST_SUITE("FreeBSD AArch64 dynamic ELF") {
    TEST_CASE("FreeBSD AArch64 dynamic executable owns loader metadata and process globals") {
        // puts is a fixed-argument C call and printf is C-variadic. At image
        // level both are ordinary AAPCS64 function imports; Task 6 separately
        // hardens the call classifier that prepares their argument registers.
        RcuFile main = ImportingMain({{"puts", ""}, {"printf", ""}, {"cos", "libm.so.5"}, {"sin", ""}});
        RcuFile declarations = ImportDeclarations({{"sin", "libm.so.5"}, {"puts", ""}});
        const auto directory = std::filesystem::temp_directory_path();
        const ElfImage image =
            LinkImage({std::move(main), std::move(declarations)}, directory / "rux-freebsd-aarch64-dynamic-metadata");

        CHECK(image.OsAbi() == 9);     // ELFOSABI_FREEBSD
        CHECK(image.Type() == 2);      // ET_EXEC
        CHECK(image.Machine() == 183); // EM_AARCH64
        CHECK(image.Interpreter() == "/libexec/ld-elf.so.1");
        CHECK(image.LoadAlignment() == kPage);
        const std::vector<std::string> expectedLibraries{"libc.so.7", "libm.so.5"};
        CHECK(image.NeededLibraries() == expectedLibraries);
        const std::string fileStrings(image.bytes.begin(), image.bytes.end());
        CHECK(fileStrings.find("ld-linux") == std::string::npos);
        CHECK(fileStrings.find("libc.so.6") == std::string::npos);

        const auto interp = image.SegmentOfType(3);  // PT_INTERP
        const auto dynamic = image.SegmentOfType(2); // PT_DYNAMIC
        const auto phdr = image.SegmentOfType(6);    // PT_PHDR
        REQUIRE(interp.has_value());
        REQUIRE(dynamic.has_value());
        REQUIRE(phdr.has_value());
        CHECK(interp->fileSize == std::string_view("/libexec/ld-elf.so.1").size() + 1);
        CHECK(dynamic->flags == 0x6); // PF_R | PF_W
        CHECK(dynamic->alignment == 8);
        CHECK(phdr->address == 0x400000 + image.Read64(32));
        CHECK(phdr->fileSize == static_cast<std::uint64_t>(image.Read16(56)) * image.Read16(54));

        const auto loads = LoadSegments(image);
        REQUIRE(loads.size() == 2);
        CHECK(loads[0].flags == 0x5); // PF_R | PF_X
        CHECK(loads[1].flags == 0x6); // PF_R | PF_W
        CHECK(loads[0].address + loads[0].memorySize <= loads[1].address);
        for (const auto &load : loads) {
            CHECK(load.alignment == kPage);
            CHECK(load.address % kPage == load.offset % kPage);
            CHECK(load.offset + load.fileSize <= image.bytes.size());
        }
        CHECK(dynamic->address >= loads[1].address);
        CHECK(dynamic->address + dynamic->memorySize <= loads[1].address + loads[1].memorySize);

        // The loader-consumed tables are all mapped, coherently sized, and
        // describe RELA-format lazy binding records.
        for (const std::uint64_t tag : {4U, 5U, 6U, 3U, 23U}) { // HASH, STRTAB, SYMTAB, PLTGOT, JMPREL
            const std::uint64_t address = image.DynamicTag(tag);
            REQUIRE(address != 0);
            CHECK_FALSE(image.MappedBytes(address, 1).empty());
        }
        CHECK(image.DynamicTag(11) == 24);         // DT_SYMENT
        CHECK(image.DynamicTag(9) == 24);          // DT_RELAENT
        CHECK(image.DynamicTag(20) == 7);          // DT_PLTREL = DT_RELA
        CHECK(image.DynamicTag(2) == 5 * 24);      // DT_PLTRELSZ: four calls plus implicit exit
        CHECK(image.DynamicRelocations().empty()); // ET_EXEC addresses need no load-bias relocation

        const auto symbols = image.DynamicSymbols();
        const std::vector<std::string> importedNames{"cos", "exit", "printf", "puts", "sin"};
        for (std::size_t index = 0; index < importedNames.size(); ++index) {
            REQUIRE(index + 1 < symbols.size());
            CHECK(symbols[index + 1].name == importedNames[index]);
            CHECK(symbols[index + 1].info == 0x12);      // STB_GLOBAL | STT_FUNC
            CHECK(symbols[index + 1].sectionIndex == 0); // SHN_UNDEF
            CHECK(symbols[index + 1].value == 0);
            CHECK(symbols[index + 1].size == 0);
            CHECK(image.HashedDynamicSymbolIndex(importedNames[index]) == index + 1);
        }

        // FreeBSD rtld's set_program_var writes these definitions before it
        // enters the program. Each symbol is a pointer-sized global object in
        // file-backed writable storage, initialized to null in the image.
        const auto segments = image.Segments();
        const auto writable = std::ranges::find_if(
            segments, [](const ElfImage::Segment &segment) { return segment.type == 1 && segment.flags == 0x6; });
        REQUIRE(writable != segments.end());
        for (const std::string &name : {"__progname", "environ", "__elf_aux_vector"}) {
            const auto found = std::ranges::find(symbols, name, &ElfImage::DynamicSymbol::name);
            REQUIRE(found != symbols.end());
            CHECK(found->info == 0x11);           // STB_GLOBAL | STT_OBJECT
            CHECK(found->sectionIndex == 0xFFF1); // SHN_ABS: no section table
            CHECK(found->size == 8);
            CHECK(found->value >= writable->address);
            CHECK(found->value + found->size <= writable->address + writable->fileSize);
            const auto initialValue = image.MappedBytes(found->value, found->size);
            REQUIRE(initialValue.size() == 8);
            CHECK(std::ranges::all_of(initialValue, [](const std::uint8_t byte) { return byte == 0; }));
            CHECK(image.HashedDynamicSymbolIndex(name).has_value());
        }
        CHECK_FALSE(image.HashedDynamicSymbolIndex("missing").has_value());
    }

    TEST_CASE("FreeBSD AArch64 dynamic PLT obeys the rtld resolver contract") {
        const ElfImage image = LinkImage({ImportingMain({{"printf", ""}, {"puts", ""}})},
                                         std::filesystem::temp_directory_path() / "rux-freebsd-aarch64-dynamic-plt");
        const std::uint64_t entry = image.Entry();

        // rtld_start.S passes ps_strings in X0. Stack normalization uses only
        // X9 and SP, leaving X0 intact until the call to Main intentionally
        // hands process entry state to the Rux entry policy.
        CHECK(image.Word(entry) == 0x910003E9);     // mov x9, sp
        CHECK(image.Word(entry + 4) == 0x927CED29); // and x9, x9, #-16
        CHECK(image.Word(entry + 8) == 0x9100013F); // mov sp, x9
        CHECK(BranchTarget(image, entry + 12) == entry + 24);

        // Main's result is already in X0 for exit. The dynamic entry stub calls
        // libc rather than issuing SYS_exit, preserving libc's flush behavior.
        const std::uint64_t exitStub = BranchTarget(image, entry + 16);
        CHECK(image.Word(entry + 20) == 0xD4200000); // brk #0, unreachable
        const std::uint64_t plt = exitStub - 32;     // exit sorts first
        const std::uint64_t got = image.DynamicTag(3);
        const auto dynamic = image.SegmentOfType(2);
        REQUIRE(dynamic.has_value());
        REQUIRE(got != 0);
        CHECK(image.Giant(got) == dynamic->address); // GOT[0] = &_DYNAMIC
        CHECK(image.Giant(got + 8) == 0);            // loader object, filled by rtld
        CHECK(image.Giant(got + 16) == 0);           // resolver, filled by rtld

        const auto checkGotAddressing = [&image](const std::uint64_t at, const std::uint64_t slot) {
            CHECK((image.Word(at) & 0x9F00001FU) == 0x90000010U);     // adrp x16, page(slot)
            CHECK((image.Word(at + 4) & 0xFFC003FFU) == 0xF9400211U); // ldr x17, [x16, lo12]
            CHECK((image.Word(at + 8) & 0xFFC003FFU) == 0x91000210U); // add x16, x16, lo12
            CHECK(image.Word(at + 12) == 0xD61F0220);                 // br x17
            CHECK(image.GotSlotReachedBy(at) == slot);
        };

        // PLT0 saves &GOT[index] and X30 in exactly the stack pair consumed by
        // FreeBSD _rtld_bind_start, then loads the resolver through GOT[2].
        CHECK(image.Word(plt) == 0xA9BF7BF0); // stp x16, x30, [sp, #-16]!
        checkGotAddressing(plt + 4, got + 16);
        CHECK(image.Word(plt + 20) == 0xD503201F);
        CHECK(image.Word(plt + 24) == 0xD503201F);
        CHECK(image.Word(plt + 28) == 0xD503201F);

        const auto relocations = image.PltRelocations();
        const auto symbols = image.DynamicSymbols();
        const std::vector<std::string> expectedImports{"exit", "printf", "puts"};
        REQUIRE(relocations.size() == 3); // exit, printf, puts
        for (std::size_t index = 0; index < relocations.size(); ++index) {
            const std::uint64_t stub = plt + 32 + index * 16;
            const std::uint64_t slot = got + (3 + index) * 8;
            checkGotAddressing(stub, slot);
            CHECK(image.Giant(slot) == plt); // lazy initial value enters PLT0
            CHECK(relocations[index].offset == slot);
            CHECK(relocations[index].symbolIndex == index + 1);
            CHECK(relocations[index].type == 1026); // R_AARCH64_JUMP_SLOT
            CHECK(relocations[index].addend == 0);
            REQUIRE(relocations[index].symbolIndex < symbols.size());
            CHECK(symbols[relocations[index].symbolIndex].name == expectedImports[index]);

            // rtld derives the relocation ordinal from &GOT[index]. That
            // ordinal selects this exact 24-byte record in DT_JMPREL.
            CHECK((slot - (got + 24)) / 8 == index);
            CHECK_FALSE(image.MappedBytes(image.DynamicTag(23) + index * 24, 24).empty());
        }

        // The two source calls reach their alphabetically assigned stubs.
        const std::uint64_t main = entry + 24;
        CHECK(BranchTarget(image, main) == plt + 48);     // printf
        CHECK(BranchTarget(image, main + 4) == plt + 64); // puts
    }

    TEST_CASE("FreeBSD AArch64 dynamic imports and libraries are deduplicated deterministically") {
        const RcuFile object = ImportingMain({{"puts", ""}, {"puts", ""}, {"puts", ""}});
        const auto directory = std::filesystem::temp_directory_path();
        const ElfImage first = LinkImage({object}, directory / "rux-freebsd-aarch64-dynamic-repeat-first");
        const ElfImage second = LinkImage({object}, directory / "rux-freebsd-aarch64-dynamic-repeat-second");
        CHECK(first.bytes == second.bytes);
        const std::vector<std::string> expectedLibraries{"libc.so.7"};
        CHECK(first.NeededLibraries() == expectedLibraries);

        const auto symbols = first.DynamicSymbols();
        REQUIRE(symbols.size() == 6); // null, exit, puts, and three process globals
        CHECK(symbols[1].name == "exit");
        CHECK(symbols[2].name == "puts");
        CHECK(first.PltRelocations().size() == 2);
        CHECK(first.HashedDynamicSymbolIndex("exit") == 1);
        CHECK(first.HashedDynamicSymbolIndex("puts") == 2);

        const std::uint64_t main = first.Entry() + 24;
        const std::uint64_t putsStub = BranchTarget(first, main);
        CHECK(BranchTarget(first, main + 4) == putsStub);
        CHECK(BranchTarget(first, main + 8) == putsStub);
    }

    TEST_CASE("FreeBSD AArch64 dynamic dependencies ignore declaration object order") {
        const RcuFile main = ImportingMain({{"alpha", ""}, {"beta", ""}, {"gamma", ""}});
        const RcuFile alpha = ImportDeclarations({{"alpha", "libzeta.so.1"}});
        const RcuFile beta = ImportDeclarations({{"beta", "libalpha.so.2"}});
        const RcuFile gamma = ImportDeclarations({{"gamma", "libzeta.so.1"}});
        const auto directory = std::filesystem::temp_directory_path();

        const ElfImage forward =
            LinkImage({main, alpha, beta, gamma}, directory / "rux-freebsd-aarch64-dynamic-libraries-forward");
        const ElfImage reverse =
            LinkImage({main, gamma, beta, alpha}, directory / "rux-freebsd-aarch64-dynamic-libraries-reverse");
        CHECK(forward.bytes == reverse.bytes);
        const std::vector<std::string> expectedLibraries{"libalpha.so.2", "libc.so.7", "libzeta.so.1"};
        CHECK(forward.NeededLibraries() == expectedLibraries);

        // Declaration-only objects assign libraries without adding imports of
        // their own. The call names and relocation order therefore remain the
        // sorted function set plus the executable's implicit exit dependency.
        const auto symbols = forward.DynamicSymbols();
        const std::vector<std::string> expectedImports{"alpha", "beta", "exit", "gamma"};
        const auto relocations = forward.PltRelocations();
        REQUIRE(relocations.size() == expectedImports.size());
        for (std::size_t index = 0; index < expectedImports.size(); ++index) {
            REQUIRE(relocations[index].symbolIndex < symbols.size());
            CHECK(symbols[relocations[index].symbolIndex].name == expectedImports[index]);
            CHECK(forward.HashedDynamicSymbolIndex(expectedImports[index]) == relocations[index].symbolIndex);
        }
    }

    TEST_CASE("FreeBSD AArch64 dynamic SysV hash chains cover every symbol exactly once") {
        const ElfImage image = LinkImage({ImportingMain({{"alpha", ""},
                                                         {"bravo", ""},
                                                         {"charlie", ""},
                                                         {"delta", ""},
                                                         {"echo", ""},
                                                         {"foxtrot", ""},
                                                         {"golf", ""},
                                                         {"hotel", ""}})},
                                         std::filesystem::temp_directory_path() / "rux-freebsd-aarch64-dynamic-hash");
        const auto symbols = image.DynamicSymbols();
        const std::size_t hash = image.OffsetOf(image.DynamicTag(4));
        const std::uint32_t bucketCount = image.Read32(hash);
        const std::uint32_t chainCount = image.Read32(hash + 4);
        REQUIRE(bucketCount != 0);
        REQUIRE(chainCount == symbols.size());

        std::vector<bool> visited(chainCount);
        visited[0] = true; // STN_UNDEF is intentionally in no bucket.
        for (std::uint32_t bucket = 0; bucket < bucketCount; ++bucket) {
            std::uint32_t symbol = image.Read32(hash + 8 + static_cast<std::size_t>(bucket) * 4);
            while (symbol != 0) {
                REQUIRE(symbol < chainCount);
                CHECK_FALSE(visited[symbol]);
                visited[symbol] = true;
                symbol = image.Read32(hash + 8 + static_cast<std::size_t>(bucketCount + symbol) * 4);
            }
        }
        CHECK(std::ranges::all_of(visited, [](const bool entry) { return entry; }));
        for (std::uint32_t index = 1; index < chainCount; ++index) {
            CHECK(image.HashedDynamicSymbolIndex(symbols[index].name) == index);
        }
    }

    TEST_CASE("FreeBSD AArch64 dynamic explicit exit library overrides the default libc") {
        const ElfImage image =
            LinkImage({ImportingMain({{"exit", "libtermination.so.1"}})},
                      std::filesystem::temp_directory_path() / "rux-freebsd-aarch64-dynamic-explicit-exit");
        const std::vector<std::string> expectedLibraries{"libtermination.so.1"};
        CHECK(image.NeededLibraries() == expectedLibraries);
        CHECK(std::string(image.bytes.begin(), image.bytes.end()).find("libc.so.7") == std::string::npos);

        const auto symbols = image.DynamicSymbols();
        REQUIRE(symbols.size() == 5); // null, exit, and three process globals
        CHECK(symbols[1].name == "exit");
        const auto relocations = image.PltRelocations();
        REQUIRE(relocations.size() == 1);
        CHECK(relocations.front().symbolIndex == 1);
        const std::uint64_t stubFromEntry = BranchTarget(image, image.Entry() + 16);
        const std::uint64_t stubFromMain = BranchTarget(image, image.Entry() + 24);
        CHECK(stubFromEntry == stubFromMain);
    }

    TEST_CASE("FreeBSD AArch64 dynamic executable diagnoses invalid imports and missing Main") {
        const auto output = std::filesystem::temp_directory_path() / "rux-freebsd-aarch64-dynamic-error";
        const auto checkFailure = [&output](std::vector<RcuFile> objects, const std::string &message) {
            std::error_code ec;
            std::filesystem::remove(output, ec);
            Linker linker(std::move(objects), "DynamicError", {}, ArtifactKind::Executable, Target::OS::FreeBSD,
                          Target::Arch::AArch64);
            CHECK_FALSE(linker.Link(output));
            REQUIRE(linker.Errors().size() == 1);
            CHECK(linker.Errors().front().message == message);
            CHECK_FALSE(std::filesystem::exists(output));
        };

        RcuFile importedData = MainObject();
        importedData.sections.front().data = {0x00, 0x00, 0x00, 0x90, 0xC0, 0x03, 0x5F, 0xD6}; // adrp / ret
        importedData.sections.front().relocs.push_back({0, 1, RcuRelType::AArch64AdrPrelPgHi21, 0});
        importedData.symbols.push_back(
            {"errno", "libc.so.7", 0, 0, RCU_SEC_EXTERNAL, RcuSymKind::ExternData, RcuSymVis::Global});
        checkFailure(
            {std::move(importedData)},
            "external data symbol 'errno' cannot be imported for target 'freebsd-aarch64' because GOT-aware data "
            "lowering and symbol-size metadata are not implemented");

        RcuFile conflicting = ImportingMain({{"puts", "libfirst.so"}});
        RcuFile conflictDeclaration = ImportDeclarations({{"puts", "libsecond.so"}});
        checkFailure({std::move(conflicting), std::move(conflictDeclaration)},
                     "external symbol 'puts' is assigned to both 'libfirst.so' and 'libsecond.so'");

        RcuFile missingMain = ImportingMain({{"puts", ""}});
        missingMain.symbols.front().name = "NotMain";
        checkFailure({std::move(missingMain)}, "undefined symbol 'Main' — no entry point found");
    }
} // TEST_SUITE
