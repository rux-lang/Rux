#include "Linker/ArchiveWriter.h"
#include "Linker/Linker.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <ranges>
#include <string>
#include <vector>

using namespace Rux;

TEST_CASE("ELF linker preserves an extern library declared in another object") {
    RcuFile caller;
    RcuSection text;
    text.name = ".text";
    text.type = RcuSecType::Text;
    text.flags = RcuSecFlag::Alloc | RcuSecFlag::Exec | RcuSecFlag::Read;
    text.alignment = 16;
    text.data = {0xE8, 0x00, 0x00, 0x00, 0x00, // call sqrt
                 0x31, 0xC0,                   // xor eax, eax
                 0xC3};                        // ret
    text.relocs.push_back({1, 1, RcuRelType::Rel32, 0});
    caller.sections.push_back(std::move(text));
    caller.symbols.push_back({"Main", "int", 0, 8, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});
    caller.symbols.push_back({"sqrt", "", 0, 0, RCU_SEC_EXTERNAL, RcuSymKind::ExternFunc, RcuSymVis::Global});

    RcuFile declarations;
    declarations.symbols.push_back(
        {"sqrt", "libm.so.6", 0, 0, RCU_SEC_EXTERNAL, RcuSymKind::ExternFunc, RcuSymVis::Global});

    const auto output = std::filesystem::temp_directory_path() / "rux-elf-cross-object-import-test";
    std::error_code ec;
    std::filesystem::remove(output, ec);

    Linker linker({std::move(caller), std::move(declarations)}, "LinkerTest", {}, ArtifactKind::Executable,
                  Target::OS::Linux, Target::Arch::X86_64);
    REQUIRE(linker.Link(output));

    std::ifstream stream(output, std::ios::binary);
    REQUIRE(stream.is_open());
    const std::string executable((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    CHECK(executable.find("libm.so.6") != std::string::npos);

    stream.close();
    std::filesystem::remove(output, ec);
}

TEST_CASE("ELF shared linker emits ET_DYN exports without an executable entry") {
    RcuFile library;
    RcuSection text;
    text.name = ".text";
    text.type = RcuSecType::Text;
    text.flags = RcuSecFlag::Alloc | RcuSecFlag::Exec | RcuSecFlag::Read;
    text.alignment = 16;
    text.data = {0xB8, 0x2A, 0x00, 0x00, 0x00, // mov eax, 42
                 0xC3};                        // ret
    library.sections.push_back(std::move(text));
    RcuSection data;
    data.name = ".data";
    data.type = RcuSecType::Data;
    data.flags = RcuSecFlag::Alloc | RcuSecFlag::Read | RcuSecFlag::Write;
    data.alignment = 8;
    data.data.resize(8);
    data.relocs.push_back({0, 0, RcuRelType::Abs64, 0});
    library.sections.push_back(std::move(data));
    library.symbols.push_back({"Answer", "int", 0, 6, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});

    const auto output = std::filesystem::temp_directory_path() / "librux-linker-test.so";
    std::error_code ec;
    std::filesystem::remove(output, ec);

    Linker linker({std::move(library)}, "LinkerTest", {}, ArtifactKind::SharedLibrary, Target::OS::Linux,
                  Target::Arch::X86_64);
    REQUIRE(linker.Link(output));

    std::ifstream stream(output, std::ios::binary);
    REQUIRE(stream.is_open());
    const std::vector<uint8_t> image((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    REQUIRE(image.size() >= 32);
    CHECK(image[16] == 3); // ET_DYN
    CHECK(image[17] == 0);
    CHECK(std::ranges::all_of(image | std::views::drop(24) | std::views::take(8),
                              [](const uint8_t byte) { return byte == 0; }));
    const std::string contents(image.begin(), image.end());
    CHECK(contents.find("Answer") != std::string::npos);
    CHECK(contents.find("librux-linker-test.so") != std::string::npos);

    const auto read64 = [&image](const size_t offset) {
        uint64_t value = 0;
        for (size_t i = 0; i < 8; ++i) {
            value |= static_cast<uint64_t>(image[offset + i]) << (i * 8U);
        }
        return value;
    };
    const auto read32 = [&image](const size_t offset) {
        uint32_t value = 0;
        for (size_t i = 0; i < 4; ++i) {
            value |= static_cast<uint32_t>(image[offset + i]) << (i * 8U);
        }
        return value;
    };
    const size_t programHeaderOffset = static_cast<size_t>(read64(32));
    const size_t programHeaderCount = image[56] | (static_cast<size_t>(image[57]) << 8U);
    size_t dynamicOffset = 0;
    size_t dynamicSize = 0;
    for (size_t i = 0; i < programHeaderCount; ++i) {
        const size_t header = programHeaderOffset + i * 56;
        if (read32(header) == 2) { // PT_DYNAMIC
            dynamicOffset = static_cast<size_t>(read64(header + 8));
            dynamicSize = static_cast<size_t>(read64(header + 32));
        }
    }
    REQUIRE(dynamicSize != 0);
    uint64_t dynamicRelocationSize = 0;
    for (size_t offset = dynamicOffset; offset < dynamicOffset + dynamicSize; offset += 16) {
        if (read64(offset) == 8) { // DT_RELASZ
            dynamicRelocationSize = read64(offset + 8);
        }
    }
    CHECK(dynamicRelocationSize == 24);

    stream.close();
    std::filesystem::remove(output, ec);
}

TEST_CASE("Linker rejects an object built for another architecture") {
    RcuFile foreign;
    foreign.arch = RcuArch::AArch64;
    foreign.sourcePath = "Main.rux";
    RcuSection text;
    text.name = ".text";
    text.type = RcuSecType::Text;
    text.flags = RcuSecFlag::Alloc | RcuSecFlag::Exec | RcuSecFlag::Read;
    text.alignment = 4;
    text.data = {0x40, 0x05, 0x80, 0xD2,  // mov x0, #42
                 0xC0, 0x03, 0x5F, 0xD6}; // ret
    foreign.sections.push_back(std::move(text));
    foreign.symbols.push_back({"Main", "int", 0, 8, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});

    const auto output = std::filesystem::temp_directory_path() / "rux-linker-arch-mismatch-test";
    std::error_code ec;
    std::filesystem::remove(output, ec);

    Linker linker({foreign}, "LinkerTest", {}, ArtifactKind::Executable, Target::OS::Linux, Target::Arch::X86_64);
    CHECK_FALSE(linker.Link(output));
    REQUIRE(linker.Errors().size() == 1);
    CHECK(linker.Errors().front().message == "object Main.rux was compiled for AArch64, but the link target is x86-64");
    CHECK_FALSE(std::filesystem::exists(output));

    // The ELF writer lays this object out as an executable, but not as a
    // shared library: that needs the PLT and GOT Task 29 brings.
    Linker shared({foreign}, "LinkerTest", {}, ArtifactKind::SharedLibrary, Target::OS::Linux, Target::Arch::AArch64);
    CHECK_FALSE(shared.Link(output));
    REQUIRE(shared.Errors().size() == 1);
    CHECK(shared.Errors().front().message == "linking a shared library for AArch64 is not implemented yet");

    // Nor as a PE image: the Windows writer still assumes x86-64 code.
    Linker windows({std::move(foreign)}, "LinkerTest", {}, ArtifactKind::Executable, Target::OS::Windows,
                   Target::Arch::AArch64);
    CHECK_FALSE(windows.Link(output));
    REQUIRE(windows.Errors().size() == 1);
    CHECK(windows.Errors().front().message == "linking an executable for AArch64 is not implemented yet");

    std::filesystem::remove(output, ec);
}

namespace {
// Appends one little-endian AArch64 instruction word to a section.
void AppendWord(std::vector<uint8_t> &data, const uint32_t word) {
    for (unsigned i = 0; i < 4; ++i) {
        data.push_back(static_cast<uint8_t>(word >> (i * 8U)));
    }
}

// An ELF64 image read back for inspection, with the accessors the two AArch64
// cases below share.
struct ElfImage {
    std::vector<uint8_t> bytes;

    [[nodiscard]] uint32_t Read32(const size_t offset) const {
        uint32_t value = 0;
        for (size_t i = 0; i < 4; ++i) {
            value |= static_cast<uint32_t>(bytes[offset + i]) << (i * 8U);
        }
        return value;
    }

    [[nodiscard]] uint64_t Read64(const size_t offset) const {
        uint64_t value = 0;
        for (size_t i = 0; i < 8; ++i) {
            value |= static_cast<uint64_t>(bytes[offset + i]) << (i * 8U);
        }
        return value;
    }

    [[nodiscard]] uint16_t Machine() const {
        return static_cast<uint16_t>(bytes[18] | bytes[19] << 8U);
    }

    [[nodiscard]] uint64_t Entry() const {
        return Read64(24);
    }

    // The file offset a virtual address maps to, found through the PT_LOAD
    // segment that covers it.
    [[nodiscard]] size_t OffsetOf(const uint64_t virtualAddress) const {
        const auto programHeaders = static_cast<size_t>(Read64(32));
        const size_t count = bytes[56] | static_cast<size_t>(bytes[57]) << 8U;
        for (size_t i = 0; i < count; ++i) {
            const size_t header = programHeaders + i * 56;
            if (Read32(header) != 1) { // PT_LOAD
                continue;
            }
            const uint64_t base = Read64(header + 16);
            if (virtualAddress >= base && virtualAddress < base + Read64(header + 40)) {
                return static_cast<size_t>(Read64(header + 8) + (virtualAddress - base));
            }
        }
        return 0;
    }

    // The virtual address of the writable PT_LOAD, which is where .data lands.
    [[nodiscard]] uint64_t WritableSegmentAddress() const {
        const auto programHeaders = static_cast<size_t>(Read64(32));
        const size_t count = bytes[56] | static_cast<size_t>(bytes[57]) << 8U;
        for (size_t i = 0; i < count; ++i) {
            const size_t header = programHeaders + i * 56;
            if (Read32(header) == 1 && (Read32(header + 4) & 0x2U) != 0) {
                return Read64(header + 16);
            }
        }
        return 0;
    }

    [[nodiscard]] uint32_t Word(const uint64_t virtualAddress) const {
        return Read32(OffsetOf(virtualAddress));
    }
};

ElfImage LinkAArch64Executable(RcuFile object, const std::filesystem::path &output) {
    std::error_code ec;
    std::filesystem::remove(output, ec);
    Linker linker({std::move(object)}, "LinkerTest", {}, ArtifactKind::Executable, Target::OS::Linux,
                  Target::Arch::AArch64);
    REQUIRE(linker.Link(output));
    std::ifstream stream(output, std::ios::binary);
    REQUIRE(stream.is_open());
    ElfImage image{{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()}};
    stream.close();
    std::filesystem::remove(output, ec);
    return image;
}
} // namespace

TEST_CASE("ELF linker writes an AArch64 executable that exits through its entry stub") {
    RcuFile object;
    object.arch = RcuArch::AArch64;
    RcuSection text;
    text.name = ".text";
    text.type = RcuSecType::Text;
    text.flags = RcuSecFlag::Alloc | RcuSecFlag::Exec | RcuSecFlag::Read;
    text.alignment = 4;
    AppendWord(text.data, 0xD2800540); // mov x0, #42
    AppendWord(text.data, 0xD65F03C0); // ret
    object.sections.push_back(std::move(text));
    object.symbols.push_back({"Main", "int", 0, 8, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});

    const ElfImage image =
        LinkAArch64Executable(std::move(object), std::filesystem::temp_directory_path() / "rux-elf-aarch64-entry-test");

    CHECK(image.Machine() == 0xB7); // EM_AARCH64
    const uint64_t entry = image.Entry();

    // mov x9, sp / and x9, x9, #-16 / mov sp, x9: the stack is 16-byte aligned
    // before Main is entered.
    CHECK(image.Word(entry) == 0x910003E9);
    CHECK(image.Word(entry + 4) == 0x927CED29);
    CHECK(image.Word(entry + 8) == 0x9100013F);

    // bl Main, whose imm26 counts instructions from the branch itself. Main
    // follows the six-instruction stub.
    const uint32_t branch = image.Word(entry + 12);
    CHECK((branch & 0xFC000000U) == 0x94000000U);
    CHECK(entry + 12 + 4 * (branch & 0x03FFFFFFU) == entry + 24);

    // mov x8, #93 (Linux exit) / svc #0, with Main's result already in X0.
    CHECK(image.Word(entry + 16) == (0xD2800008U | 93U << 5U));
    CHECK(image.Word(entry + 20) == 0xD4000001);

    CHECK(image.Word(entry + 24) == 0xD2800540);
    CHECK(image.Word(entry + 28) == 0xD65F03C0);
}

TEST_CASE("ELF linker patches AArch64 relocations into instruction fields") {
    RcuFile object;
    object.arch = RcuArch::AArch64;

    RcuSection text;
    text.name = ".text";
    text.type = RcuSecType::Text;
    text.flags = RcuSecFlag::Alloc | RcuSecFlag::Exec | RcuSecFlag::Read;
    text.alignment = 4;
    AppendWord(text.data, 0x94000000); // bl Callee
    AppendWord(text.data, 0x90000000); // adrp x0, Answer
    AppendWord(text.data, 0x91000000); // add x0, x0, :lo12:Answer
    AppendWord(text.data, 0xF9400001); // ldr x1, [x0, :lo12:Answer]
    AppendWord(text.data, 0xD2800002); // movz x2, #0
    AppendWord(text.data, 0xF2A00002); // movk x2, #0, lsl #16
    AppendWord(text.data, 0xF2C00002); // movk x2, #0, lsl #32
    AppendWord(text.data, 0xF2E00002); // movk x2, #0, lsl #48
    AppendWord(text.data, 0xD65F03C0); // ret
    AppendWord(text.data, 0xD65F03C0); // Callee: ret
    text.relocs.push_back({0, 1, RcuRelType::AArch64Call26, 0});
    text.relocs.push_back({4, 2, RcuRelType::AArch64AdrPrelPgHi21, 0});
    text.relocs.push_back({8, 2, RcuRelType::AArch64AddAbsLo12Nc, 0});
    text.relocs.push_back({12, 2, RcuRelType::AArch64LdstAbsLo12Nc, 0});
    text.relocs.push_back({16, 2, RcuRelType::AArch64MovwUabsG0, 0});
    text.relocs.push_back({20, 2, RcuRelType::AArch64MovwUabsG1, 0});
    text.relocs.push_back({24, 2, RcuRelType::AArch64MovwUabsG2, 0});
    text.relocs.push_back({28, 2, RcuRelType::AArch64MovwUabsG3, 0});
    object.sections.push_back(std::move(text));

    RcuSection data;
    data.name = ".data";
    data.type = RcuSecType::Data;
    data.flags = RcuSecFlag::Alloc | RcuSecFlag::Read | RcuSecFlag::Write;
    data.alignment = 8;
    data.data.resize(8);
    object.sections.push_back(std::move(data));

    object.symbols.push_back({"Main", "int", 0, 40, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});
    object.symbols.push_back({"Callee", "int", 36, 4, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});
    object.symbols.push_back({"Answer", "int", 0, 8, RCU_DATA_IDX, RcuSymKind::Data, RcuSymVis::Global});

    const ElfImage image =
        LinkAArch64Executable(std::move(object), std::filesystem::temp_directory_path() / "rux-elf-aarch64-reloc-test");

    const uint64_t main = image.Entry() + 24;
    const uint64_t answer = image.WritableSegmentAddress();
    REQUIRE(answer != 0);

    // CALL26 counts instructions from the branch and reaches Callee, which the
    // nine instructions before it put 36 bytes further on.
    const uint32_t branch = image.Word(main);
    CHECK((branch & 0xFC000000U) == 0x94000000U);
    CHECK(main + 4 * (branch & 0x03FFFFFFU) == main + 36);

    // ADR_PREL_PG_HI21 names the page the symbol sits on, relative to the page
    // the ADRP sits on, with the immediate split across two fields.
    const uint32_t adrp = image.Word(main + 4);
    const auto pageOffset = static_cast<int32_t>(((adrp >> 5U & 0x7FFFFU) << 2U | (adrp >> 29U & 3U)) << 11U) >> 11;
    const uint64_t page = ((main + 4) & ~uint64_t{0xFFF}) + (static_cast<int64_t>(pageOffset) << 12U);

    // ADD_ABS_LO12_NC supplies the rest of the address; the two together must
    // form the symbol's own.
    const uint64_t add = image.Word(main + 8) >> 10U & 0xFFFU;
    CHECK(page + add == answer);

    // LDST_ABS_LO12_NC carries the same twelve bits scaled by the access
    // width, which it reads out of the instruction it patches — three here,
    // for a 64-bit LDR.
    CHECK((image.Word(main + 12) >> 10U & 0xFFFU) == (answer & 0xFFFU) >> 3U);

    // MOVW_UABS_G0 through G3 take one halfword each of the whole address.
    uint64_t assembled = 0;
    for (unsigned halfword = 0; halfword < 4; ++halfword) {
        assembled |= static_cast<uint64_t>(image.Word(main + 16 + 4 * halfword) >> 5U & 0xFFFFU) << (16U * halfword);
    }
    CHECK(assembled == answer);
}

TEST_CASE("ELF linker refuses an AArch64 program that imports from a shared library") {
    RcuFile object;
    object.arch = RcuArch::AArch64;
    RcuSection text;
    text.name = ".text";
    text.type = RcuSecType::Text;
    text.flags = RcuSecFlag::Alloc | RcuSecFlag::Exec | RcuSecFlag::Read;
    text.alignment = 4;
    AppendWord(text.data, 0x94000000); // bl sqrt
    AppendWord(text.data, 0xD65F03C0); // ret
    text.relocs.push_back({0, 1, RcuRelType::AArch64Call26, 0});
    object.sections.push_back(std::move(text));
    object.symbols.push_back({"Main", "int", 0, 8, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});
    object.symbols.push_back({"sqrt", "libm.so.6", 0, 0, RCU_SEC_EXTERNAL, RcuSymKind::ExternFunc, RcuSymVis::Global});

    const auto output = std::filesystem::temp_directory_path() / "rux-elf-aarch64-dynamic-test";
    std::error_code ec;
    std::filesystem::remove(output, ec);

    Linker linker({std::move(object)}, "LinkerTest", {}, ArtifactKind::Executable, Target::OS::Linux,
                  Target::Arch::AArch64);
    CHECK_FALSE(linker.Link(output));
    REQUIRE(linker.Errors().size() == 1);
    CHECK(linker.Errors().front().message ==
          "dynamic linking for AArch64 is not implemented yet: 'sqrt' is imported from 'libm.so.6'");
    CHECK_FALSE(std::filesystem::exists(output));
}

TEST_CASE("native object writer stamps the target architecture into every format") {
    RcuFile file;
    file.arch = RcuArch::AArch64;
    file.sourcePath = "Library.rux";
    RcuSection text;
    text.name = ".text";
    text.type = RcuSecType::Text;
    text.flags = RcuSecFlag::Alloc | RcuSecFlag::Exec | RcuSecFlag::Read;
    text.alignment = 4;
    text.data = {0xC0, 0x03, 0x5F, 0xD6}; // ret
    file.sections.push_back(std::move(text));
    file.symbols.push_back({"Answer", "int", 0, 4, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});

    NativeObject object;
    std::string error;
    REQUIRE(WriteNativeObject(file, Target::OS::Linux, Target::Arch::AArch64, object, error));
    CHECK(object.bytes[18] == 183); // EM_AARCH64
    CHECK(object.bytes[19] == 0);

    REQUIRE(WriteNativeObject(file, Target::OS::Windows, Target::Arch::AArch64, object, error));
    CHECK(object.bytes[0] == 0x64); // IMAGE_FILE_MACHINE_ARM64
    CHECK(object.bytes[1] == 0xAA);

    REQUIRE(WriteNativeObject(file, Target::OS::MacOS, Target::Arch::AArch64, object, error));
    CHECK(object.bytes[4] == 0x0C); // CPU_TYPE_ARM64
    CHECK(object.bytes[7] == 0x01);

    CHECK_FALSE(WriteNativeObject(file, Target::OS::Linux, Target::Arch::X86_64, object, error));
    CHECK(error == "object was compiled for AArch64, but the target is x86-64");

    file.sections[0].relocs.push_back({0, 0, RcuRelType::AArch64Call26, 0});
    CHECK_FALSE(WriteNativeObject(file, Target::OS::Linux, Target::Arch::AArch64, object, error));
    CHECK(error == "relocation AARCH64_CALL26 in section .text is not supported by the object writer yet");
}

TEST_CASE("native archive writers emit deterministic target-specific indexes") {
    NativeObject object;
    object.name = "Library.o";
    object.bytes = {0x01, 0x02, 0x03, 0x04};
    object.publicSymbols = {"Answer"};

    const auto directory = std::filesystem::temp_directory_path();
    const auto gnuFirst = directory / "rux-archive-gnu-first.a";
    const auto gnuSecond = directory / "rux-archive-gnu-second.a";
    const auto bsd = directory / "rux-archive-bsd.a";
    const auto coff = directory / "rux-archive-coff.lib";
    const auto importLibrary = directory / "rux-import-library.lib";
    std::error_code ec;
    for (const auto &path : {gnuFirst, gnuSecond, bsd, coff, importLibrary}) {
        std::filesystem::remove(path, ec);
    }

    std::string error;
    REQUIRE(WriteNativeArchive({&object, 1}, Target::OS::Linux, Target::Arch::X86_64, gnuFirst, error));
    REQUIRE(WriteNativeArchive({&object, 1}, Target::OS::Linux, Target::Arch::X86_64, gnuSecond, error));
    REQUIRE(WriteNativeArchive({&object, 1}, Target::OS::MacOS, Target::Arch::X86_64, bsd, error));
    REQUIRE(WriteNativeArchive({&object, 1}, Target::OS::Windows, Target::Arch::X86_64, coff, error));
    const std::array<std::string, 1> exports = {"Answer"};
    REQUIRE(WriteWindowsImportLibrary("Native.dll", exports, Target::Arch::X86_64, importLibrary, error));

    const auto read = [](const std::filesystem::path &path) {
        std::ifstream stream(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    };
    const auto gnuBytes = read(gnuFirst);
    CHECK(gnuBytes == read(gnuSecond));
    CHECK(gnuBytes.starts_with("!<arch>\n/"));
    CHECK(read(bsd).starts_with("!<arch>\n__.SYMDEF SORTED"));
    const auto coffBytes = read(coff);
    CHECK(coffBytes.starts_with("!<arch>\n/"));
    CHECK(read(importLibrary).find("__imp_Answer") != std::string::npos);

    for (const auto &path : {gnuFirst, gnuSecond, bsd, coff, importLibrary}) {
        std::filesystem::remove(path, ec);
    }
}
