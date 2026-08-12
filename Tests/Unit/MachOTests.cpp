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
#include <limits>
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

std::vector<std::uint8_t> LinkBytes(std::vector<RcuFile> objects, const ArtifactKind kind,
                                    const std::filesystem::path &output,
                                    const Target::Arch targetArch = Target::Arch::X86_64) {
    std::error_code fileError;
    std::filesystem::remove(output, fileError);
    Linker linker(std::move(objects), "MachOTest", {}, kind, Target::OS::MacOS, targetArch);
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

std::vector<std::uint8_t> LinkBytes(RcuFile object, const ArtifactKind kind, const std::filesystem::path &output,
                                    const Target::Arch targetArch = Target::Arch::X86_64) {
    return LinkBytes(std::vector<RcuFile>{std::move(object)}, kind, output, targetArch);
}

MachOImage LinkAndRead(RcuFile object, const ArtifactKind kind, const std::filesystem::path &output,
                       const Target::Arch targetArch = Target::Arch::X86_64) {
    const std::vector<std::uint8_t> bytes = LinkBytes(std::move(object), kind, output, targetArch);
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

TEST_CASE("Mach-O links deterministic signed freestanding AArch64 executables") {
    RcuFile program;
    program.arch = RcuArch::AArch64;
    auto text = TextSection({
        0xA0, 0x04, 0x80, 0x52,                                                 // Main: mov w0, #37
        0xC0, 0x03, 0x5F, 0xD6,                                                 // ret
        0x00, 0x00, 0x00, 0x94,                                                 // bl Helper
        0x00, 0x00, 0x00, 0x90,                                                 // adrp x0, Value
        0x00, 0x00, 0x00, 0x91,                                                 // add x0, x0, Value@pageoff
        0x01, 0x00, 0x40, 0xF9,                                                 // ldr x1, [x0, Value@pageoff]
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0x03, 0x5F, 0xD6, // Helper: ret
    });
    text.relocs.push_back({8, 1, RcuRelType::AArch64Call26, 0});
    text.relocs.push_back({12, 2, RcuRelType::AArch64AdrPrelPgHi21, 0});
    text.relocs.push_back({16, 2, RcuRelType::AArch64AddAbsLo12Nc, 0});
    text.relocs.push_back({20, 2, RcuRelType::AArch64LdstAbsLo12Nc, 0});
    program.sections.push_back(std::move(text));

    RcuSection rodata;
    rodata.name = ".rodata";
    rodata.type = RcuSecType::RoData;
    rodata.flags = RcuSecFlag::Alloc | RcuSecFlag::Read;
    rodata.alignment = 8;
    rodata.data.resize(12);
    rodata.relocs.push_back({0, 0, RcuRelType::AArch64Prel32, 0});
    rodata.relocs.push_back({4, 0, RcuRelType::Abs64, 0});
    program.sections.push_back(std::move(rodata));

    RcuSection data;
    data.name = ".data";
    data.type = RcuSecType::Data;
    data.flags = RcuSecFlag::Alloc | RcuSecFlag::Read | RcuSecFlag::Write;
    data.alignment = 8;
    data.data.resize(16);
    program.sections.push_back(std::move(data));
    program.symbols.push_back({"Main", "int", 0, 8, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});
    program.symbols.push_back({"Helper", "", 32, 4, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Local});
    program.symbols.push_back({"Value", "", 8, 8, RCU_DATA_IDX, RcuSymKind::Data, RcuSymVis::Local});

    const auto temporary = std::filesystem::temp_directory_path();
    const std::vector<std::uint8_t> first =
        LinkBytes(program, ArtifactKind::Executable, temporary / "rux-macho-aarch64-first", Target::Arch::AArch64);
    const std::vector<std::uint8_t> second =
        LinkBytes(program, ArtifactKind::Executable, temporary / "rux-macho-aarch64-second", Target::Arch::AArch64);
    CHECK(first == second);

    MachOImage image;
    std::string error;
    REQUIRE_MESSAGE(ReadMachO64(first, image, error), error);
    CHECK(image.Architecture() == MachOArchitecture::AArch64);
    CHECK(image.cpuSubtype == 0);
    CHECK(image.fileType == 2);
    CHECK(image.HasCommand(0x05)); // LC_UNIXTHREAD
    CHECK(image.HasCommand(0x32)); // LC_BUILD_VERSION
    CHECK_FALSE(image.mainEntryOffset);
    REQUIRE(image.threadStateFlavor);
    REQUIRE(image.threadStateCount);
    CHECK(*image.threadStateFlavor == 6); // ARM_THREAD_STATE64
    CHECK(*image.threadStateCount == 68);
    REQUIRE(image.buildVersion);
    CHECK(image.buildVersion->platform == 1);
    CHECK(image.buildVersion->minimumOs == 0x001A'0000);
    CHECK(image.buildVersion->sdk == 0x001A'0000);
    CHECK(image.buildVersion->toolCount == 0);
    REQUIRE(image.threadEntryAddress);
    const MachOSection *imageText = image.Section("__TEXT", "__text");
    const MachOSection *imageRodata = image.Section("__TEXT", "__const");
    const MachOSection *imageData = image.Section("__DATA", "__data");
    REQUIRE(imageText != nullptr);
    REQUIRE(imageRodata != nullptr);
    REQUIRE(imageData != nullptr);
    CHECK(*image.threadEntryAddress == imageText->address);
    CHECK(image.Segment("__TEXT")->vmSize % 0x4000 == 0);
    CHECK(image.Segment("__DATA")->fileOffset % 0x4000 == 0);
    CHECK(image.Segment("__LINKEDIT")->fileOffset % 0x4000 == 0);

    CHECK(Detail::U32(first, imageText->offset) == 0x9400'0004);     // entry stub: bl Main
    CHECK(Detail::U32(first, imageText->offset + 4) == 0xD280'0030); // mov x16, #1
    CHECK(Detail::U32(first, imageText->offset + 8) == 0xD400'1001); // svc #0x80
    CHECK(Detail::U32(first, imageText->offset + 24) == 0x9400'0006);

    const std::uint32_t adrp = Detail::U32(first, imageText->offset + 28);
    std::int64_t pages = static_cast<std::int64_t>(((adrp >> 29U) & 3U) | ((adrp >> 5U) & 0x7FFFFU) << 2U);
    if ((pages & (1 << 20)) != 0) {
        pages |= ~((std::int64_t{1} << 21) - 1);
    }
    const std::uint64_t adrpSite = imageText->address + 28;
    const std::uint64_t dataAddress = imageData->address + 8;
    CHECK((adrpSite & ~0xFFFULL) + pages * 0x1000 == (dataAddress & ~0xFFFULL));
    CHECK((Detail::U32(first, imageText->offset + 32) >> 10U & 0xFFFU) == (dataAddress & 0xFFFU));
    CHECK((Detail::U32(first, imageText->offset + 36) >> 10U & 0xFFFU) == ((dataAddress & 0xFFFU) >> 3U));
    CHECK(static_cast<std::int32_t>(Detail::U32(first, imageRodata->offset)) ==
          static_cast<std::int64_t>(imageText->address + 16) - static_cast<std::int64_t>(imageRodata->address));
    CHECK(Detail::U64(first, imageRodata->offset + 4) == imageText->address + 16);

    REQUIRE(image.codeSignature);
    REQUIRE(image.codeDirectory);
    CHECK(image.codeSignature->offset + image.codeSignature->size == first.size());
    CHECK(image.codeDirectory->codeLimit == image.codeSignature->offset);
    const std::size_t signatureSlots =
        (image.codeSignature->offset + MachO::codeSignaturePageSize - 1) / MachO::codeSignaturePageSize;
    REQUIRE(image.codeDirectory->codeHashes.size() == signatureSlots);
    for (std::size_t slot = 0; slot < signatureSlots; ++slot) {
        const std::size_t offset = slot * MachO::codeSignaturePageSize;
        const std::size_t size =
            std::min<std::size_t>(MachO::codeSignaturePageSize, image.codeSignature->offset - offset);
        const Crypto::Sha256Digest expected = Crypto::Sha256(std::span(first).subspan(offset, size));
        CHECK(image.codeDirectory->codeHashes[slot] == std::vector<std::uint8_t>(expected.begin(), expected.end()));
    }
}

TEST_CASE("Mach-O preserves AArch64 section alignment between input objects") {
    RcuFile prefixObject;
    prefixObject.arch = RcuArch::AArch64;
    RcuSection prefixRodata;
    prefixRodata.name = ".rodata";
    prefixRodata.type = RcuSecType::RoData;
    prefixRodata.flags = RcuSecFlag::Alloc | RcuSecFlag::Read;
    prefixRodata.alignment = 1;
    prefixRodata.data.push_back(0xA5);
    prefixObject.sections.push_back(std::move(prefixRodata));

    RcuFile program;
    program.arch = RcuArch::AArch64;
    auto text = TextSection({
        0x10,
        0x00,
        0x00,
        0x90, // adrp x16, __f64_0
        0x08,
        0x02,
        0x40,
        0xFD, // ldr d8, [x16, :lo12:__f64_0]
        0x00,
        0x00,
        0x80,
        0x52, // mov w0, #0
        0xC0,
        0x03,
        0x5F,
        0xD6, // ret
    });
    text.relocs.push_back({0, 1, RcuRelType::AArch64AdrPrelPgHi21, 0});
    text.relocs.push_back({4, 1, RcuRelType::AArch64LdstAbsLo12Nc, 0});
    program.sections.push_back(std::move(text));

    RcuSection constants;
    constants.name = ".rodata";
    constants.type = RcuSecType::RoData;
    constants.flags = RcuSecFlag::Alloc | RcuSecFlag::Read;
    constants.alignment = 8;
    constants.data.resize(8);
    program.sections.push_back(std::move(constants));
    program.symbols.push_back({"Main", "int", 0, 16, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});
    program.symbols.push_back({"__f64_0", "", 0, 8, RCU_RODATA_IDX, RcuSymKind::Const, RcuSymVis::Local});

    const auto output = std::filesystem::temp_directory_path() / "rux-macho-aarch64-object-alignment";
    const std::vector<std::uint8_t> bytes = LinkBytes({std::move(prefixObject), std::move(program)},
                                                      ArtifactKind::Executable, output, Target::Arch::AArch64);

    MachOImage image;
    std::string error;
    REQUIRE_MESSAGE(ReadMachO64(bytes, image, error), error);
    const MachOSection *rodata = image.Section("__TEXT", "__const");
    REQUIRE(rodata != nullptr);
    CHECK(rodata->size == 16);
    CHECK(bytes[rodata->offset] == 0xA5);
    CHECK((rodata->address + 8) % 8 == 0);
}

TEST_CASE("Mach-O links imported AArch64 executables with eager Apple stubs") {
    RcuFile program;
    program.arch = RcuArch::AArch64;
    auto text = TextSection({
        0x00, 0x00, 0x00, 0x94, // Main: bl puts
        0x00, 0x00, 0x00, 0x94, // bl strlen
        0x00, 0x00, 0x00, 0x94, // bl puts (deduplicated import)
        0x00, 0x00, 0x80, 0x52, // mov w0, #0
        0xC0, 0x03, 0x5F, 0xD6, // ret
    });
    text.relocs.push_back({0, 1, RcuRelType::AArch64Call26, 0});
    text.relocs.push_back({4, 2, RcuRelType::AArch64Call26, 0});
    text.relocs.push_back({8, 1, RcuRelType::AArch64Call26, 0});
    program.sections.push_back(std::move(text));
    program.symbols.push_back({"Main", "int", 0, 20, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});
    program.symbols.push_back(
        {"puts", "libSystem.B.dylib", 0, 0, RCU_SEC_EXTERNAL, RcuSymKind::ExternFunc, RcuSymVis::Global});
    program.symbols.push_back(
        {"strlen", "/opt/Rux/libExample.dylib", 0, 0, RCU_SEC_EXTERNAL, RcuSymKind::ExternFunc, RcuSymVis::Global});

    const auto temporary = std::filesystem::temp_directory_path();
    const std::vector<std::uint8_t> first = LinkBytes(
        program, ArtifactKind::Executable, temporary / "rux-macho-aarch64-import-first", Target::Arch::AArch64);
    const std::vector<std::uint8_t> second = LinkBytes(
        program, ArtifactKind::Executable, temporary / "rux-macho-aarch64-import-second", Target::Arch::AArch64);
    CHECK(first == second);

    MachOImage image;
    std::string error;
    REQUIRE_MESSAGE(ReadMachO64(first, image, error), error);
    CHECK(image.Architecture() == MachOArchitecture::AArch64);
    CHECK(image.fileType == 2);
    CHECK(image.HasCommand(0x0E));        // LC_LOAD_DYLINKER
    CHECK(image.HasCommand(0x0C));        // LC_LOAD_DYLIB
    CHECK(image.HasCommand(0x8000'0022)); // LC_DYLD_INFO_ONLY
    CHECK(image.HasCommand(0x02));        // LC_SYMTAB
    CHECK(image.HasCommand(0x0B));        // LC_DYSYMTAB
    CHECK(image.HasCommand(0x8000'0028)); // LC_MAIN
    CHECK_FALSE(image.HasCommand(0x05));  // LC_UNIXTHREAD
    REQUIRE(image.mainEntryOffset);

    std::vector<std::string> dylibCommands;
    for (const auto &command : image.commands) {
        if (command.command == 0x0C) {
            dylibCommands.push_back(command.value);
        }
        if (command.command == 0x0E) {
            CHECK(command.value == "/usr/lib/dyld");
        }
    }
    CHECK((dylibCommands == std::vector<std::string>{"/opt/Rux/libExample.dylib", "/usr/lib/libSystem.B.dylib"}));

    const MachOSection *imageText = image.Section("__TEXT", "__text");
    const MachOSection *stubs = image.Section("__TEXT", "__stubs");
    const MachOSection *pointers = image.Section("__DATA", "__nl_symbol_ptr");
    REQUIRE(imageText != nullptr);
    REQUIRE(stubs != nullptr);
    REQUIRE(pointers != nullptr);
    CHECK(*image.mainEntryOffset == imageText->offset);
    CHECK(stubs->size == 24);
    CHECK(stubs->alignmentPower == 2);
    CHECK(stubs->reserved1 == 0);
    CHECK(stubs->reserved2 == 12);
    CHECK(pointers->size == 16);
    CHECK(pointers->alignmentPower == 3);
    CHECK(pointers->reserved1 == 2);
    CHECK((pointers->flags & 0xFF) == 0x06); // S_NON_LAZY_SYMBOL_POINTERS

    CHECK(Detail::U32(first, imageText->offset) == 0xA9BF'7BFD);     // stp x29, x30, [sp, #-16]!
    CHECK(Detail::U32(first, imageText->offset + 4) == 0x9100'03FD); // mov x29, sp
    CHECK(Detail::U32(first, imageText->offset + 12) == 0xA8C1'7BFD);
    CHECK(Detail::U32(first, imageText->offset + 16) == 0xD65F'03C0);

    const auto branchTarget = [&](const std::uint64_t address, const std::uint32_t instruction) {
        std::int64_t immediate = instruction & 0x03FF'FFFFU;
        if ((immediate & (std::int64_t{1} << 25)) != 0) {
            immediate |= ~((std::int64_t{1} << 26) - 1);
        }
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(address) + immediate * 4);
    };
    CHECK(branchTarget(imageText->address + 8, Detail::U32(first, imageText->offset + 8)) ==
          imageText->address + 32); // dynamic entry calls aligned Main
    CHECK(branchTarget(imageText->address + 32, Detail::U32(first, imageText->offset + 32)) == stubs->address);
    CHECK(branchTarget(imageText->address + 36, Detail::U32(first, imageText->offset + 36)) == stubs->address + 12);
    CHECK(branchTarget(imageText->address + 40, Detail::U32(first, imageText->offset + 40)) == stubs->address);

    for (std::size_t index = 0; index < 2; ++index) {
        const std::size_t fileOffset = stubs->offset + index * 12;
        const std::uint64_t stubAddress = stubs->address + index * 12;
        const std::uint32_t adrp = Detail::U32(first, fileOffset);
        std::int64_t pages = static_cast<std::int64_t>(((adrp >> 29U) & 3U) | ((adrp >> 5U) & 0x7FFFFU) << 2U);
        if ((pages & (std::int64_t{1} << 20)) != 0) {
            pages |= ~((std::int64_t{1} << 21) - 1);
        }
        const std::uint32_t load = Detail::U32(first, fileOffset + 4);
        const std::uint64_t pointerAddress =
            (stubAddress & ~0xFFFULL) + pages * 0x1000 + static_cast<std::uint64_t>((load >> 10U) & 0xFFFU) * 8;
        CHECK((adrp & 0x9F00'001FU) == 0x9000'0010);              // ADRP X16
        CHECK((load & 0xFFC0'03FFU) == 0xF940'0210);              // LDR X16, [X16, #imm]
        CHECK(Detail::U32(first, fileOffset + 8) == 0xD61F'0200); // BR X16
        CHECK(pointerAddress == pointers->address + index * 8);
    }

    REQUIRE(image.symbols.size() == 2);
    CHECK(image.symbols[0].name == "_puts");
    const std::uint16_t putsOrdinal = image.symbols[0].description >> 8;
    CHECK(putsOrdinal == 2); // sorted libSystem ordinal
    CHECK(image.symbols[1].name == "_strlen");
    const std::uint16_t strlenOrdinal = image.symbols[1].description >> 8;
    CHECK(strlenOrdinal == 1);
    CHECK((image.indirectSymbols == std::vector<std::uint32_t>{0, 1, 0, 1}));
    REQUIRE(image.binds.size() == 2);
    CHECK(image.binds[0].symbol == "_puts");
    CHECK(image.binds[0].libraryOrdinal == 2);
    CHECK(image.binds[0].segmentIndex == 2);
    CHECK(image.binds[0].segmentOffset == 0);
    CHECK(image.binds[1].symbol == "_strlen");
    CHECK(image.binds[1].libraryOrdinal == 1);
    CHECK(image.binds[1].segmentIndex == 2);
    CHECK(image.binds[1].segmentOffset == 8);
    REQUIRE(image.dyldInfo);
    CHECK(image.dyldInfo->bindSize > 0);
    CHECK(image.dyldInfo->lazyBindSize == 0);
    CHECK(image.dyldInfo->weakBindSize == 0);
    REQUIRE(image.codeSignature);
    CHECK(image.codeSignature->offset + image.codeSignature->size == first.size());
}

TEST_CASE("Mach-O links signed AArch64 dylibs with exports imports and rebases") {
    RcuFile publicObject;
    publicObject.arch = RcuArch::AArch64;
    auto publicText = TextSection({
        0x00,
        0x00,
        0x00,
        0x94, // Answer: bl Helper
        0x00,
        0x00,
        0x00,
        0x94, // bl puts
        0xC0,
        0x03,
        0x5F,
        0xD6, // ret
    });
    publicText.relocs.push_back({0, 1, RcuRelType::AArch64Call26, 0});
    publicText.relocs.push_back({4, 2, RcuRelType::AArch64Call26, 0});
    publicObject.sections.push_back(std::move(publicText));
    RcuSection data;
    data.name = ".data";
    data.type = RcuSecType::Data;
    data.flags = RcuSecFlag::Alloc | RcuSecFlag::Read | RcuSecFlag::Write;
    data.alignment = 8;
    data.data.resize(16);
    data.data[8] = 42;
    data.relocs.push_back({0, 0, RcuRelType::Abs64, 0});
    publicObject.sections.push_back(std::move(data));
    publicObject.symbols.push_back({"Answer", "int", 0, 12, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});
    publicObject.symbols.push_back({"Helper", "", 0, 0, RCU_SEC_EXTERNAL, RcuSymKind::ExternFunc, RcuSymVis::Global});
    publicObject.symbols.push_back(
        {"puts", "libSystem.B.dylib", 0, 0, RCU_SEC_EXTERNAL, RcuSymKind::ExternFunc, RcuSymVis::Global});
    publicObject.symbols.push_back({"AnswerPointer", "", 0, 8, RCU_DATA_IDX, RcuSymKind::Data, RcuSymVis::Global});
    publicObject.symbols.push_back({"Value", "int", 8, 8, RCU_DATA_IDX, RcuSymKind::Data, RcuSymVis::Global});

    RcuFile helperObject;
    helperObject.arch = RcuArch::AArch64;
    helperObject.sections.push_back(TextSection({0xC0, 0x03, 0x5F, 0xD6}));
    helperObject.symbols.push_back({"Helper", "int", 0, 4, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});

    const std::vector<RcuFile> objects{publicObject, helperObject};
    const auto temporary = std::filesystem::temp_directory_path();
    const std::vector<std::uint8_t> first =
        LinkBytes(objects, ArtifactKind::SharedLibrary, temporary / "rux-macho-dylib-first" / "libAnswers.dylib",
                  Target::Arch::AArch64);
    const std::vector<std::uint8_t> second =
        LinkBytes(objects, ArtifactKind::SharedLibrary, temporary / "rux-macho-dylib-second" / "libAnswers.dylib",
                  Target::Arch::AArch64);
    CHECK(first == second);

    MachOImage image;
    std::string error;
    REQUIRE_MESSAGE(ReadMachO64(first, image, error), error);
    CHECK(image.Architecture() == MachOArchitecture::AArch64);
    CHECK(image.fileType == 6);        // MH_DYLIB
    CHECK(image.flags == 0x0000'0005); // MH_NOUNDEFS | MH_DYLDLINK
    CHECK(image.Segment("__PAGEZERO") == nullptr);
    CHECK_FALSE(image.HasCommand(0x05));        // LC_UNIXTHREAD
    CHECK_FALSE(image.HasCommand(0x8000'0028)); // LC_MAIN
    CHECK_FALSE(image.HasCommand(0x0E));        // LC_LOAD_DYLINKER
    CHECK_FALSE(image.mainEntryOffset);
    CHECK_FALSE(image.threadEntryAddress);
    REQUIRE(image.buildVersion);
    CHECK(image.buildVersion->minimumOs == 0x001A'0000);

    std::vector<std::string> identities;
    std::vector<std::string> dependencies;
    for (const auto &command : image.commands) {
        if (command.command == 0x0D) {
            identities.push_back(command.value);
        }
        else if (command.command == 0x0C) {
            dependencies.push_back(command.value);
        }
    }
    CHECK((identities == std::vector<std::string>{"@rpath/libAnswers.dylib"}));
    CHECK((dependencies == std::vector<std::string>{"/usr/lib/libSystem.B.dylib"}));

    const MachOSegment *textSegment = image.Segment("__TEXT");
    const MachOSegment *dataSegment = image.Segment("__DATA");
    const MachOSection *imageText = image.Section("__TEXT", "__text");
    const MachOSection *stubs = image.Section("__TEXT", "__stubs");
    const MachOSection *pointers = image.Section("__DATA", "__nl_symbol_ptr");
    const MachOSection *imageData = image.Section("__DATA", "__data");
    REQUIRE(textSegment != nullptr);
    REQUIRE(dataSegment != nullptr);
    REQUIRE(imageText != nullptr);
    REQUIRE(stubs != nullptr);
    REQUIRE(pointers != nullptr);
    REQUIRE(imageData != nullptr);
    CHECK(imageText->size == 20); // no entry stub; Helper starts at its requested alignment
    CHECK(stubs->size == 12);
    CHECK(stubs->reserved2 == 12);
    CHECK(pointers->size == 8);
    CHECK(pointers->reserved1 == 1);

    const auto branchTarget = [&](const std::uint64_t address, const std::uint32_t instruction) {
        std::int64_t immediate = instruction & 0x03FF'FFFFU;
        if ((immediate & (std::int64_t{1} << 25)) != 0) {
            immediate |= ~((std::int64_t{1} << 26) - 1);
        }
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(address) + immediate * 4);
    };
    CHECK(branchTarget(imageText->address, Detail::U32(first, imageText->offset)) == imageText->address + 16);
    CHECK(branchTarget(imageText->address + 4, Detail::U32(first, imageText->offset + 4)) == stubs->address);
    CHECK(Detail::U64(first, imageData->offset) == imageText->address);

    REQUIRE(image.binds.size() == 1);
    CHECK(image.binds[0].symbol == "_puts");
    CHECK(image.binds[0].libraryOrdinal == 1);
    CHECK(image.binds[0].segmentIndex == 1); // __DATA in a dylib without __PAGEZERO
    CHECK(image.binds[0].segmentOffset == 0);
    REQUIRE(image.rebases.size() == 1);
    CHECK(image.rebases[0].segmentIndex == 1);
    CHECK(image.rebases[0].segmentOffset == imageData->address - dataSegment->vmAddress);

    REQUIRE(image.symbols.size() == 5);
    CHECK(image.symbols[0].name == "_Answer");
    CHECK(image.symbols[1].name == "_AnswerPointer");
    CHECK(image.symbols[2].name == "_Helper");
    CHECK(image.symbols[3].name == "_Value");
    CHECK(image.symbols[4].name == "_puts");
    CHECK(std::ranges::all_of(image.symbols.begin(), image.symbols.begin() + 4,
                              [](const MachOSymbol &symbol) { return (symbol.type & 0x0E) == 0x0E; }));
    CHECK((image.symbols[4].type & 0x0E) == 0);
    REQUIRE(image.dyldInfo);
    CHECK(image.dyldInfo->rebaseSize > 0);
    CHECK(image.dyldInfo->bindSize > 0);
    CHECK(image.dyldInfo->exportSize > 0);

    REQUIRE(image.codeSignature);
    REQUIRE(image.codeDirectory);
    CHECK(image.codeDirectory->executableSegmentFlags == 0);
    CHECK(image.codeSignature->offset + image.codeSignature->size == first.size());
    const std::size_t signatureSlots =
        (image.codeSignature->offset + MachO::codeSignaturePageSize - 1) / MachO::codeSignaturePageSize;
    REQUIRE(image.codeDirectory->codeHashes.size() == signatureSlots);
    for (std::size_t slot = 0; slot < signatureSlots; ++slot) {
        const std::size_t offset = slot * MachO::codeSignaturePageSize;
        const std::size_t size =
            std::min<std::size_t>(MachO::codeSignaturePageSize, image.codeSignature->offset - offset);
        const Crypto::Sha256Digest expected = Crypto::Sha256(std::span(first).subspan(offset, size));
        CHECK(image.codeDirectory->codeHashes[slot] == std::vector<std::uint8_t>(expected.begin(), expected.end()));
    }
}

TEST_CASE("Mach-O AArch64 dylib diagnostics reject duplicate and unresolved definitions") {
    const auto linkError = [](std::vector<RcuFile> objects, const std::string_view suffix) {
        Linker linker(std::move(objects), "MachOTest", {}, ArtifactKind::SharedLibrary, Target::OS::MacOS,
                      Target::Arch::AArch64);
        CHECK_FALSE(
            linker.Link(std::filesystem::temp_directory_path() / ("rux-macho-aarch64-dylib-" + std::string(suffix))));
        REQUIRE_FALSE(linker.Errors().empty());
        return linker.Errors().front().message;
    };

    SUBCASE("duplicate definition") {
        RcuFile first;
        first.arch = RcuArch::AArch64;
        first.sections.push_back(TextSection({0xC0, 0x03, 0x5F, 0xD6}));
        first.symbols.push_back({"Answer", "int", 0, 4, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});
        RcuFile second = first;
        CHECK(linkError({std::move(first), std::move(second)}, "duplicate") ==
              "duplicate definition of symbol 'Answer'");
    }

    SUBCASE("unresolved definition") {
        RcuFile library;
        library.arch = RcuArch::AArch64;
        auto text = TextSection({0x00, 0x00, 0x00, 0x94});
        text.relocs.push_back({0, 0, RcuRelType::AArch64Call26, 0});
        library.sections.push_back(std::move(text));
        library.symbols.push_back({"Missing", "", 0, 0, RCU_SEC_EXTERNAL, RcuSymKind::Func, RcuSymVis::Global});
        CHECK(linkError({std::move(library)}, "unresolved") == "undefined symbol 'Missing'");
    }
}

TEST_CASE("Mach-O AArch64 executable diagnostics reject unsupported or invalid inputs") {
    const auto linkError = [](RcuFile program, const std::string_view suffix) {
        Linker linker({std::move(program)}, "MachOTest", {}, ArtifactKind::Executable, Target::OS::MacOS,
                      Target::Arch::AArch64);
        CHECK_FALSE(
            linker.Link(std::filesystem::temp_directory_path() / ("rux-macho-aarch64-error-" + std::string(suffix))));
        REQUIRE_FALSE(linker.Errors().empty());
        return linker.Errors().front().message;
    };

    SUBCASE("missing entry point") {
        RcuFile program;
        program.arch = RcuArch::AArch64;
        program.sections.push_back(TextSection({0xC0, 0x03, 0x5F, 0xD6}));
        CHECK(linkError(std::move(program), "entry").contains("no entry point found"));
    }
    SUBCASE("imported data") {
        RcuFile program;
        program.arch = RcuArch::AArch64;
        auto text = TextSection({0, 0, 0, 0});
        text.relocs.push_back({0, 1, RcuRelType::Abs64, 0});
        program.sections.push_back(std::move(text));
        program.symbols.push_back({"Main", "int", 0, 4, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});
        program.symbols.push_back(
            {"errno", "libSystem.B.dylib", 0, 0, RCU_SEC_EXTERNAL, RcuSymKind::ExternData, RcuSymVis::Global});
        CHECK(linkError(std::move(program), "data-import") ==
              "external data symbol 'errno' cannot be imported by the Mach-O AArch64 linker because GOT-aware "
              "lowering is not implemented");
    }
    SUBCASE("out-of-range branch") {
        RcuFile program;
        program.arch = RcuArch::AArch64;
        auto text = TextSection({0, 0, 0, 0});
        text.relocs.push_back({0, 0, RcuRelType::AArch64Call26, std::numeric_limits<std::int32_t>::max()});
        program.sections.push_back(std::move(text));
        program.symbols.push_back({"Main", "int", 0, 4, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});
        CHECK(linkError(std::move(program), "branch-range")
                  .contains("AARCH64_CALL26 relocation against 'Main' is out of range"));
    }
    SUBCASE("absolute 32-bit overflow") {
        RcuFile program;
        program.arch = RcuArch::AArch64;
        auto text = TextSection({0, 0, 0, 0});
        text.relocs.push_back({0, 0, RcuRelType::Abs32, 0});
        program.sections.push_back(std::move(text));
        program.symbols.push_back({"Main", "int", 0, 4, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});
        CHECK(linkError(std::move(program), "abs32-overflow") ==
              "ABS_32 relocation against 'Main' does not fit in 32 bits");
    }
    SUBCASE("misaligned low12 access") {
        RcuFile program;
        program.arch = RcuArch::AArch64;
        auto text = TextSection({0x00, 0x00, 0x40, 0xF9});
        text.relocs.push_back({0, 1, RcuRelType::AArch64LdstAbsLo12Nc, 0});
        program.sections.push_back(std::move(text));
        RcuSection data;
        data.type = RcuSecType::Data;
        data.data.resize(8);
        program.sections.push_back(std::move(data));
        program.symbols.push_back({"Main", "int", 0, 4, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});
        program.symbols.push_back({"Value", "", 1, 1, RCU_DATA_IDX, RcuSymKind::Data, RcuSymVis::Local});
        CHECK(linkError(std::move(program), "low12-alignment").contains("symbol is not aligned to the access width"));
    }
}
