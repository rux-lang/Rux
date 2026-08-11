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
                  Target::OS::Linux);
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

    Linker linker({std::move(library)}, "LinkerTest", {}, ArtifactKind::SharedLibrary, Target::OS::Linux);
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
    REQUIRE(WriteNativeArchive({&object, 1}, Target::OS::Linux, gnuFirst, error));
    REQUIRE(WriteNativeArchive({&object, 1}, Target::OS::Linux, gnuSecond, error));
    REQUIRE(WriteNativeArchive({&object, 1}, Target::OS::MacOS, bsd, error));
    REQUIRE(WriteNativeArchive({&object, 1}, Target::OS::Windows, coff, error));
    const std::array<std::string, 1> exports = {"Answer"};
    REQUIRE(WriteWindowsImportLibrary("Native.dll", exports, importLibrary, error));

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
