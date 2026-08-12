#include "Crypto/Sha256.h"
#include "Linker/Linker.h"
#include "Linker/MachO/CodeSignature.h"
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

std::vector<std::uint8_t> LinkBytes(RcuFile object, const ArtifactKind kind, const std::filesystem::path &output) {
    std::error_code fileError;
    std::filesystem::remove(output, fileError);
    Linker linker({std::move(object)}, "MachOTest", {}, kind, Target::OS::MacOS, Target::Arch::X86_64);
    const bool linked = linker.Link(output);
    CAPTURE(linker.Errors().empty() ? std::string{} : linker.Errors().front().message);
    REQUIRE(linked);

    std::ifstream stream(output, std::ios::binary);
    REQUIRE(stream.is_open());
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    stream.close();
    std::filesystem::remove(output, fileError);
    return bytes;
}

MachOImage LinkAndRead(RcuFile object, const ArtifactKind kind, const std::filesystem::path &output) {
    const std::vector<std::uint8_t> bytes = LinkBytes(std::move(object), kind, output);
    MachOImage image;
    std::string error;
    const bool parsed = ReadMachO64(bytes, image, error);
    CAPTURE(error);
    REQUIRE(parsed);

    REQUIRE(image.codeSignature);
    REQUIRE(image.codeDirectory);
    CHECK(image.codeSignature->offset == image.codeDirectory->codeLimit);
    CHECK(image.codeSignature->offset + image.codeSignature->size == bytes.size());
    CHECK(image.codeDirectory->version == 0x0002'0400);
    CHECK(image.codeDirectory->flags == 0x0002'0002);
    CHECK(image.codeDirectory->identifier == "MachOTest");
    CHECK(image.codeDirectory->hashSize == Crypto::sha256DigestLength);
    CHECK(image.codeDirectory->hashType == 2);
    CHECK(image.codeDirectory->pageSizePower == 12);
    REQUIRE(image.Segment("__TEXT") != nullptr);
    CHECK(image.codeDirectory->executableSegmentBase == image.Segment("__TEXT")->fileOffset);
    CHECK(image.codeDirectory->executableSegmentLimit == image.Segment("__TEXT")->fileSize);
    CHECK(image.codeDirectory->executableSegmentFlags == (kind == ArtifactKind::Executable ? 1 : 0));
    const std::size_t expectedSlots =
        (image.codeSignature->offset + MachO::codeSignaturePageSize - 1) / MachO::codeSignaturePageSize;
    REQUIRE(image.codeDirectory->codeHashes.size() == expectedSlots);
    for (std::size_t slot = 0; slot < expectedSlots; ++slot) {
        const std::size_t offset = slot * MachO::codeSignaturePageSize;
        const std::size_t size =
            std::min<std::size_t>(MachO::codeSignaturePageSize, image.codeSignature->offset - offset);
        const Crypto::Sha256Digest expected = Crypto::Sha256(std::span(bytes).subspan(offset, size));
        CHECK(image.codeDirectory->codeHashes[slot] == std::vector<std::uint8_t>(expected.begin(), expected.end()));
    }
    return image;
}
} // namespace

TEST_CASE("Mach-O ad-hoc signature builder covers empty and partial pages") {
    std::vector<std::uint8_t> signature;
    std::string error;
    REQUIRE(MachO::BuildAdHocCodeSignature({}, "Empty", 0, 0, 0, signature, error));
    CHECK(signature.size() == MachO::AdHocCodeSignatureSize(0, "Empty", error));
    CHECK(Detail::BigU32(signature, 0) == 0xFADE'0CC0);
    CHECK(Detail::BigU32(signature, 12) == 0); // CSSLOT_CODEDIRECTORY
    const std::size_t emptyDirectory = Detail::BigU32(signature, 16);
    CHECK(Detail::BigU32(signature, emptyDirectory + 28) == 0);

    const std::vector<std::uint8_t> partial(MachO::codeSignaturePageSize + 1, 0xA5);
    REQUIRE(MachO::BuildAdHocCodeSignature(partial, "Partial", 0, partial.size(), 1, signature, error));
    const std::size_t directory = Detail::BigU32(signature, 16);
    CHECK(Detail::BigU32(signature, directory + 28) == 2);
    CHECK(Detail::BigU32(signature, directory + 32) == partial.size());
    const std::size_t identifierOffset = Detail::BigU32(signature, directory + 20);
    CHECK(std::string(reinterpret_cast<const char *>(signature.data() + directory + identifierOffset)) == "Partial");
}

TEST_CASE("Mach-O ad-hoc signature builder diagnoses malformed inputs") {
    std::vector<std::uint8_t> signature;
    std::string error;
    CHECK_FALSE(MachO::BuildAdHocCodeSignature({}, "", 0, 0, 0, signature, error));
    CHECK(error == "Mach-O code-signature identifier cannot be empty");

    const std::string embeddedNull("bad\0identifier", 14);
    CHECK_FALSE(MachO::BuildAdHocCodeSignature({}, embeddedNull, 0, 0, 0, signature, error));
    CHECK(error == "Mach-O code-signature identifier cannot contain a null byte");
    CHECK(MachO::AdHocCodeSignatureSize(std::uint64_t{1} << 32U, "TooLarge", error) == 0);
    CHECK(error == "Mach-O signed prefix does not fit in the CodeDirectory code-limit field");
}

TEST_CASE("Mach-O signed output is deterministic") {
    RcuFile program;
    program.arch = RcuArch::X86_64;
    program.sections.push_back(TextSection({0x31, 0xC0, 0xC3}));
    program.symbols.push_back({"Main", "int", 0, 3, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});

    const auto temporary = std::filesystem::temp_directory_path();
    const std::vector<std::uint8_t> first =
        LinkBytes(program, ArtifactKind::Executable, temporary / "rux-macho-deterministic-first");
    const std::vector<std::uint8_t> second =
        LinkBytes(std::move(program), ArtifactKind::Executable, temporary / "rux-macho-deterministic-second");
    CHECK(first == second);
}

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
