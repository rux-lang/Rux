#include "Linker/Linker.h"
#include "MachOReader.h"

#include <algorithm>
#include <cstdint>
#include <doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace Rux;
using namespace Rux::Testing;

namespace {
RcuSection TextSection(std::vector<std::uint8_t> bytes) {
    RcuSection text;
    text.name = ".text";
    text.type = RcuSecType::Text;
    text.flags = RcuSecFlag::Alloc | RcuSecFlag::Exec | RcuSecFlag::Read;
    text.alignment = 16;
    text.data = std::move(bytes);
    return text;
}

MachOImage LinkAndRead(RcuFile object, const ArtifactKind kind, const std::filesystem::path &output) {
    std::error_code fileError;
    std::filesystem::remove(output, fileError);
    Linker linker({std::move(object)}, "MachOTest", {}, kind, Target::OS::MacOS, Target::Arch::X86_64);
    const bool linked = linker.Link(output);
    if (!linked) {
        // Task 3 replaces this final host-tool signing step. Until then, the
        // complete unsigned image written immediately before it remains the
        // portable structure under test on non-macOS hosts.
        REQUIRE(linker.Errors().size() == 1);
        REQUIRE(linker.Errors().front().message.contains("codesign"));
    }

    std::ifstream stream(output, std::ios::binary);
    REQUIRE(stream.is_open());
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    stream.close();
    std::filesystem::remove(output, fileError);

    MachOImage image;
    std::string error;
    const bool parsed = ReadMachO64(bytes, image, error);
    CAPTURE(error);
    REQUIRE(parsed);
    return image;
}
} // namespace

TEST_CASE("Mach-O reader reports the freestanding x86-64 thread entry") {
    RcuFile program;
    program.arch = RcuArch::X86_64;
    program.sections.push_back(TextSection({0x31, 0xC0, 0xC3}));
    program.symbols.push_back({"Main", "int", 0, 3, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});

    const MachOImage image =
        LinkAndRead(std::move(program), ArtifactKind::Executable,
                    std::filesystem::temp_directory_path() / "rux-macho-reader-static-executable-test");
    CHECK(image.Architecture() == MachOArchitecture::X86_64);
    CHECK(image.fileType == 2); // MH_EXECUTE
    CHECK(image.HasCommand(0x05));
    CHECK_FALSE(image.mainEntryOffset);
    REQUIRE(image.threadEntryAddress);
    REQUIRE(image.Section("__TEXT", "__text") != nullptr);
    CHECK(*image.threadEntryAddress == image.Section("__TEXT", "__text")->address);
    CHECK_FALSE(image.dyldInfo);
}

TEST_CASE("Mach-O reader reports the x86-64 executable structure") {
    RcuFile program;
    program.arch = RcuArch::X86_64;
    auto text = TextSection({0xE8, 0, 0, 0, 0, 0x31, 0xC0, 0xC3});
    text.relocs.push_back({1, 1, RcuRelType::Rel32, 0});
    program.sections.push_back(std::move(text));
    program.symbols.push_back({"Main", "int", 5, 3, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});
    program.symbols.push_back(
        {"getpid", "libSystem.B.dylib", 0, 0, RCU_SEC_EXTERNAL, RcuSymKind::ExternFunc, RcuSymVis::Global});

    const MachOImage image = LinkAndRead(std::move(program), ArtifactKind::Executable,
                                         std::filesystem::temp_directory_path() / "rux-macho-reader-executable-test");
    CHECK(image.Architecture() == MachOArchitecture::X86_64);
    CHECK(image.cpuSubtype == 3);
    CHECK(image.fileType == 2); // MH_EXECUTE
    CHECK(image.commands.size() == image.declaredCommandCount);
    CHECK(image.HasCommand(0x8000'0028)); // LC_MAIN
    CHECK_FALSE(image.HasCommand(0x05));  // LC_UNIXTHREAD
    REQUIRE(image.mainEntryOffset);
    CHECK(*image.mainEntryOffset == image.Section("__TEXT", "__text")->offset);
    REQUIRE(image.dyldInfo);
    CHECK(image.dyldInfo->bindSize > 0);
    CHECK(image.Segment("__PAGEZERO") != nullptr);
    CHECK(image.Segment("__LINKEDIT") != nullptr);
    REQUIRE(image.Section("__TEXT", "__stubs") != nullptr);
    CHECK(image.Section("__TEXT", "__stubs")->alignmentPower == 1);
    CHECK(image.Section("__TEXT", "__stubs")->reserved2 == 6);
    REQUIRE(image.Section("__DATA", "__nl_symbol_ptr") != nullptr);
    REQUIRE(image.symbols.size() == 1);
    CHECK(image.symbols.front().name == "_getpid");
    CHECK((image.symbols.front().type & 0x0E) == 0); // N_UNDF
    if (image.codeSignature) {
        CHECK(image.codeSignature->size > 0);
    }
}

TEST_CASE("Mach-O reader reports the x86-64 dylib structure") {
    RcuFile library;
    library.arch = RcuArch::X86_64;
    library.sections.push_back(TextSection({0xB8, 0x2A, 0, 0, 0, 0xC3}));
    RcuSection data;
    data.name = ".data";
    data.type = RcuSecType::Data;
    data.flags = RcuSecFlag::Alloc | RcuSecFlag::Read | RcuSecFlag::Write;
    data.alignment = 8;
    data.data = {0x2A, 0, 0, 0};
    library.sections.push_back(std::move(data));
    library.symbols.push_back({"Answer", "int", 0, 6, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});
    library.symbols.push_back({"Value", "int", 0, 4, RCU_DATA_IDX, RcuSymKind::Data, RcuSymVis::Global});

    const MachOImage image = LinkAndRead(std::move(library), ArtifactKind::SharedLibrary,
                                         std::filesystem::temp_directory_path() / "librux-macho-reader-test.dylib");
    CHECK(image.Architecture() == MachOArchitecture::X86_64);
    CHECK(image.fileType == 6); // MH_DYLIB
    CHECK(image.Segment("__PAGEZERO") == nullptr);
    CHECK(image.Section("__TEXT", "__text") != nullptr);
    CHECK(image.Section("__TEXT", "__const") != nullptr);
    CHECK(image.Section("__DATA", "__data") != nullptr);
    CHECK_FALSE(image.mainEntryOffset);
    CHECK_FALSE(image.threadEntryAddress);
    CHECK(image.HasCommand(0x0D)); // LC_ID_DYLIB
    CHECK(image.HasCommand(0x02)); // LC_SYMTAB
    CHECK(image.HasCommand(0x0B)); // LC_DYSYMTAB
    REQUIRE(image.dyldInfo);
    CHECK(image.dyldInfo->exportSize > 0);
    REQUIRE(image.symbols.size() == 2);
    CHECK(image.symbols[0].name == "_Answer");
    CHECK(image.symbols[1].name == "_Value");
    CHECK(std::ranges::all_of(image.symbols, [](const MachOSymbol &symbol) { return (symbol.type & 0x0E) == 0x0E; }));
}

TEST_CASE("Mach-O architecture profile diagnoses invalid relocation patches") {
    const auto link = [](const std::uint32_t offset, const std::uint16_t type) {
        RcuFile program;
        program.arch = RcuArch::X86_64;
        auto text = TextSection({0, 0, 0, 0});
        text.relocs.push_back({offset, 0, type, 0});
        program.sections.push_back(std::move(text));
        program.symbols.push_back({"Main", "int", 0, 4, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});
        Linker linker({std::move(program)}, "MachOTest", {}, ArtifactKind::Executable, Target::OS::MacOS,
                      Target::Arch::X86_64);
        CHECK_FALSE(linker.Link(std::filesystem::temp_directory_path() / "rux-macho-invalid-relocation-test"));
        REQUIRE(linker.Errors().size() == 1);
        return linker.Errors().front().message;
    };

    CHECK(link(4, RcuRelType::Rel32) == "REL_32 relocation against 'Main' is outside its Mach-O section");
    CHECK(link(0, RcuRelType::AArch64Call26) ==
          "relocation AARCH64_CALL26 against 'Main' is not supported by the Mach-O x86-64 profile");
    CHECK(link(0, RcuRelType::Abs32) == "ABS_32 relocation against 'Main' does not fit in 32 bits");
}

TEST_CASE("Mach-O reader identifies AArch64 and validates the code-signature range") {
    std::vector<std::uint8_t> bytes(64);
    const auto write32 = [&](const std::size_t offset, const std::uint32_t value) {
        for (unsigned byte = 0; byte < 4; ++byte) {
            bytes[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8));
        }
    };
    write32(0, 0xFEED'FACF); // MH_MAGIC_64
    write32(4, 0x0100'000C); // CPU_TYPE_ARM64
    write32(8, 0);           // CPU_SUBTYPE_ARM64_ALL
    write32(12, 2);          // MH_EXECUTE
    write32(16, 1);
    write32(20, 16);
    write32(32, 0x1D); // LC_CODE_SIGNATURE
    write32(36, 16);
    write32(40, 48);
    write32(44, 16);

    MachOImage image;
    std::string error;
    const bool parsed = ReadMachO64(bytes, image, error);
    CAPTURE(error);
    REQUIRE(parsed);
    CHECK(image.Architecture() == MachOArchitecture::AArch64);
    REQUIRE(image.codeSignature);
    CHECK(image.codeSignature->offset == 48);
    CHECK(image.codeSignature->size == 16);

    write32(44, 17);
    CHECK_FALSE(ReadMachO64(bytes, image, error));
    CHECK(error == "Mach-O code signature extends beyond the image");
}
