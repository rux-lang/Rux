// Thread-local variables: both emitters place them in the thread-local template section and reach them through the
// target's thread-relative relocations, and every image writer turns the template into what its loader builds each
// thread's block from: PT_TLS on ELF, a TLS directory on PE and TLV descriptors on Mach-O.

#include "AArch64RcuEmitterTestSupport.h"
#include "CodeGen/AArch64/RcuEmitter.h"
#include "CodeGen/X86_64/RcuEmitter.h"
#include "ElfReader.h"
#include "Linker/Linker.h"
#include "MachOReader.h"
#include "TemporaryDirectory.h"

#include <algorithm>
#include <cstdint>
#include <doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using namespace Rux;
using Rux::Testing::ElfImage;
using Rux::Testing::MachOBind;
using Rux::Testing::MachOImage;

namespace {
constexpr const char *Source = R"(
    thread var Depth: int64 = 0;
    pub thread var Seed: uint32 = 0xDEADBEEF;
    func Main() -> int {
        Depth = Depth + 1;
        return Depth as int + Seed as int;
    }
)";

[[nodiscard]] bool HasRelocation(const RcuFile &object, const std::string_view symbol, const std::uint16_t type) {
    const auto relocations = Rux::Testing::RelocsFor(object, RCU_TEXT_IDX, symbol);
    return std::ranges::any_of(relocations, [&](const RcuReloc &relocation) { return relocation.type == type; });
}

void AppendBytes(std::vector<std::uint8_t> &data, const std::initializer_list<std::uint8_t> bytes) {
    data.insert(data.end(), bytes);
}

void AppendWord(std::vector<std::uint8_t> &data, const std::uint32_t word) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        data.push_back(static_cast<std::uint8_t>(word >> shift));
    }
}

[[nodiscard]] std::uint32_t Read32(const std::vector<std::uint8_t> &bytes, const std::size_t offset) {
    std::uint32_t value = 0;
    for (unsigned index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(bytes[offset + index]) << (8U * index);
    }
    return value;
}

[[nodiscard]] std::uint64_t Read64(const std::vector<std::uint8_t> &bytes, const std::size_t offset) {
    return Read32(bytes, offset) | static_cast<std::uint64_t>(Read32(bytes, offset + 4)) << 32U;
}

/// One object whose `Main` addresses `Second`, the middle of three thread-local variables, the way the emitter for
/// `arch` and `os` does, with a 24-byte template whose middle word is 42.
[[nodiscard]] RcuFile ThreadLocalObject(const std::uint8_t arch, const Target::OS os) {
    RcuFile object;
    object.arch = arch;
    RcuSection text;
    text.name = ".text";
    text.type = RcuSecType::Text;
    text.flags = RcuSecFlag::Alloc | RcuSecFlag::Exec | RcuSecFlag::Read;
    text.alignment = 16;
    std::vector<RcuReloc> relocations;
    if (arch == RcuArch::X86_64) {
        if (os == Target::OS::MacOS) {
            AppendBytes(text.data, {0x48, 0x8D, 0x3D}); // lea rdi, [rip + descriptor]
            relocations.push_back({static_cast<std::uint32_t>(text.data.size()), 1, RcuRelType::TlvDescriptorRel32, 0});
            AppendWord(text.data, 0);
            AppendBytes(text.data, {0xFF, 0x17}); // call [rdi]
        }
        else if (os == Target::OS::Windows) {
            // mov r11, gs:[0x58]; mov r10d, [rip + _tls_index]; mov r11, [r11 + r10 * 8]; lea rax, [r11 + offset]
            AppendBytes(text.data, {0x65, 0x4C, 0x8B, 0x1C, 0x25, 0x58, 0, 0, 0, 0x44, 0x8B, 0x15});
            relocations.push_back({static_cast<std::uint32_t>(text.data.size()), 4, RcuRelType::Rel32, 0});
            AppendWord(text.data, 0);
            AppendBytes(text.data, {0x4F, 0x8B, 0x1C, 0xD3, 0x49, 0x8D, 0x83});
            relocations.push_back({static_cast<std::uint32_t>(text.data.size()), 1, RcuRelType::TlsOffset32, 0});
            AppendWord(text.data, 0);
        }
        else {
            // mov rax, fs:[0]; lea rax, [rax + offset]
            AppendBytes(text.data, {0x64, 0x48, 0x8B, 0x04, 0x25, 0, 0, 0, 0, 0x48, 0x8D, 0x80});
            relocations.push_back({static_cast<std::uint32_t>(text.data.size()), 1, RcuRelType::TlsOffset32, 0});
            AppendWord(text.data, 0);
        }
        AppendBytes(text.data, {0x8B, 0x00, 0xC3}); // mov eax, [rax]; ret
    }
    else {
        AppendWord(text.data, 0xD53BD049); // mrs x9, tpidr_el0
        relocations.push_back({static_cast<std::uint32_t>(text.data.size()), 1, RcuRelType::AArch64TlsHi12, 0});
        AppendWord(text.data, 0x91400129); // add x9, x9, #hi, lsl #12
        relocations.push_back({static_cast<std::uint32_t>(text.data.size()), 1, RcuRelType::AArch64TlsLo12Nc, 0});
        AppendWord(text.data, 0x91000129); // add x9, x9, #lo
        AppendWord(text.data, 0xB9400120); // ldr w0, [x9]
        AppendWord(text.data, 0xD65F03C0); // ret
    }
    text.relocs = std::move(relocations);
    object.sections.push_back(std::move(text));

    for (const auto &[name, type] : {std::pair{".rodata", RcuSecType::RoData}, std::pair{".data", RcuSecType::Data},
                                     std::pair{".bss", RcuSecType::Bss}}) {
        RcuSection section;
        section.name = name;
        section.type = type;
        section.flags = RcuSecFlag::Alloc | RcuSecFlag::Read;
        section.alignment = 8;
        object.sections.push_back(std::move(section));
    }
    RcuSection tls;
    tls.name = ".tls";
    tls.type = RcuSecType::Tls;
    tls.flags = RcuSecFlag::Alloc | RcuSecFlag::Read | RcuSecFlag::Write | RcuSecFlag::Tls;
    tls.alignment = 8;
    tls.data.assign(24, 0);
    tls.data[8] = 42;
    object.sections.push_back(std::move(tls));

    object.symbols.push_back({"Main", "int", 0, 0, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});
    object.symbols.push_back({"Second", "int64", 8, 8, RCU_TLS_IDX, RcuSymKind::Data, RcuSymVis::Global});
    object.symbols.push_back({"First", "int64", 0, 8, RCU_TLS_IDX, RcuSymKind::Data, RcuSymVis::Global});
    object.symbols.push_back({"Third", "int64", 16, 8, RCU_TLS_IDX, RcuSymKind::Data, RcuSymVis::Global});
    if (os == Target::OS::Windows) {
        object.symbols.push_back(
            {std::string(kTlsIndexSymbol), "", 0, 0, RCU_SEC_EXTERNAL, RcuSymKind::ExternData, RcuSymVis::Global});
    }
    return object;
}

/// Links `object` and returns the image's bytes, or nothing with the first error when the link fails.
[[nodiscard]] std::vector<std::uint8_t> Link(RcuFile object, const Target::OS os, const Target::Arch arch,
                                             const std::string_view name, std::string &error,
                                             const ArtifactKind kind = ArtifactKind::Executable) {
    const auto output = Rux::Testing::TestTemporaryDirectory() / name;
    std::error_code fileError;
    std::filesystem::remove(output, fileError);
    Linker linker({std::move(object)}, "ThreadLocalTest", {}, kind, os, arch);
    if (!linker.Link(output)) {
        error = linker.Errors().empty() ? std::string{} : linker.Errors().front().message;
        return {};
    }
    std::ifstream stream(output, std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    stream.close();
    std::filesystem::remove(output, fileError);
    return bytes;
}
} // namespace

TEST_CASE("both emitters place thread-local variables in the thread-local template") {
    const auto package = Rux::Testing::CompileToAArch64Lir(Source, "linux-x86_64");
    const auto x86Objects = RcuEmitter(package, "test", Target::OS::Linux).Generate();
    const auto armObjects = AArch64RcuEmitter(package, "test", Target::OS::Linux).Generate();
    REQUIRE_FALSE(x86Objects.empty());
    REQUIRE_FALSE(armObjects.empty());
    for (const auto *object : {&x86Objects.front(), &armObjects.front()}) {
        const auto *depth = Rux::Testing::FindSymbol(*object, "test::Depth");
        const auto *seed = Rux::Testing::FindSymbol(*object, "test::Seed");
        REQUIRE(depth != nullptr);
        REQUIRE(seed != nullptr);
        CHECK(depth->sectionIdx == RCU_TLS_IDX);
        CHECK(seed->sectionIdx == RCU_TLS_IDX);
        CHECK(depth->value % 8 == 0);
        // Zero and non-zero initializers alike are materialized, because the template is copied whole.
        REQUIRE(object->sections.size() > RCU_TLS_IDX);
        const RcuSection &tls = object->sections[RCU_TLS_IDX];
        CHECK(tls.name == ".tls");
        CHECK(tls.type == RcuSecType::Tls);
        CHECK((tls.flags & RcuSecFlag::Tls) != 0);
        CHECK(tls.data.size() >= seed->value + 4);
        CHECK(Read32(tls.data, seed->value) == 0xDEADBEEF);
    }
    CHECK(HasRelocation(x86Objects.front(), "test::Depth", RcuRelType::TlsOffset32));
    CHECK(HasRelocation(armObjects.front(), "test::Depth", RcuRelType::AArch64TlsHi12));
    CHECK(HasRelocation(armObjects.front(), "test::Depth", RcuRelType::AArch64TlsLo12Nc));
}

TEST_CASE("each system reaches a thread-local variable its own way") {
    const auto package = Rux::Testing::CompileToAArch64Lir(Source, "linux-x86_64");
    const auto windows = RcuEmitter(package, "test", Target::OS::Windows).Generate();
    const auto mac = RcuEmitter(package, "test", Target::OS::MacOS).Generate();
    const auto armWindows = AArch64RcuEmitter(package, "test", Target::OS::Windows).Generate();
    const auto armMac = AArch64RcuEmitter(package, "test", Target::OS::MacOS).Generate();
    REQUIRE_FALSE(windows.empty());
    REQUIRE_FALSE(mac.empty());
    REQUIRE_FALSE(armWindows.empty());
    REQUIRE_FALSE(armMac.empty());
    // Windows finds the thread's block through the image's TLS index, then adds the variable's offset in it.
    CHECK_FALSE(Rux::Testing::RelocsFor(windows.front(), RCU_TEXT_IDX, "_tls_index").empty());
    CHECK(HasRelocation(windows.front(), "test::Depth", RcuRelType::TlsOffset32));
    CHECK_FALSE(Rux::Testing::RelocsFor(armWindows.front(), RCU_TEXT_IDX, "_tls_index").empty());
    CHECK(HasRelocation(armWindows.front(), "test::Depth", RcuRelType::AArch64TlsLo12Nc));
    // macOS calls through the variable's descriptor.
    CHECK(HasRelocation(mac.front(), "test::Depth", RcuRelType::TlvDescriptorRel32));
    CHECK(HasRelocation(armMac.front(), "test::Depth", RcuRelType::AArch64TlvDescriptorPage21));
    CHECK(HasRelocation(armMac.front(), "test::Depth", RcuRelType::AArch64TlvDescriptorPageOff12));
}

TEST_CASE("an x86-64 ELF executable describes its template in PT_TLS and reaches it below the thread pointer") {
    std::string error;
    const auto bytes = Link(ThreadLocalObject(RcuArch::X86_64, Target::OS::Linux), Target::OS::Linux,
                            Target::Arch::X86_64, "rux-tls-elf-x86", error);
    CAPTURE(error);
    REQUIRE_FALSE(bytes.empty());
    const ElfImage image{bytes};
    // The loader only builds thread blocks for a dynamically linked program.
    REQUIRE(image.SegmentOfType(3).has_value());
    const auto tls = image.SegmentOfType(7);
    REQUIRE(tls.has_value());
    CHECK(tls->fileSize == 24);
    CHECK(tls->memorySize == 24);
    CHECK(tls->alignment == 8);
    CHECK(tls->address % 8 == 0);
    CHECK(image.bytes[tls->offset + 8] == 42);

    // `Second` sits sixteen bytes below the end of the block, and the block ends at the thread pointer.
    const auto main = std::ranges::search(image.bytes, std::vector<std::uint8_t>{0x64, 0x48, 0x8B, 0x04, 0x25});
    REQUIRE_FALSE(main.empty());
    const auto at = static_cast<std::size_t>(main.begin() - image.bytes.begin());
    CHECK(static_cast<std::int32_t>(image.Read32(at + 12)) == -16);
}

TEST_CASE("an AArch64 ELF executable reaches its template after the thread control block") {
    std::string error;
    const auto bytes = Link(ThreadLocalObject(RcuArch::AArch64, Target::OS::Linux), Target::OS::Linux,
                            Target::Arch::AArch64, "rux-tls-elf-aarch64", error);
    CAPTURE(error);
    REQUIRE_FALSE(bytes.empty());
    const ElfImage image{bytes};
    REQUIRE(image.SegmentOfType(7).has_value());
    const auto mrs = std::ranges::search(image.bytes, std::vector<std::uint8_t>{0x49, 0xD0, 0x3B, 0xD5});
    REQUIRE_FALSE(mrs.empty());
    const auto at = static_cast<std::size_t>(mrs.begin() - image.bytes.begin());
    // Sixteen bytes of control block, then `Second` at offset 8.
    CHECK(image.Read32(at + 4) == 0x91400129);
    CHECK(image.Read32(at + 8) == (0x91000129U | 24U << 10U));
}

TEST_CASE("an ELF shared library with thread-local variables is rejected") {
    std::string error;
    const auto bytes = Link(ThreadLocalObject(RcuArch::X86_64, Target::OS::Linux), Target::OS::Linux,
                            Target::Arch::X86_64, "rux-tls-elf-shared", error, ArtifactKind::SharedLibrary);
    CHECK(bytes.empty());
    CHECK(error.find("thread-local variables are not supported in an ELF shared library yet") != std::string::npos);
}

TEST_CASE("a static library with thread-local variables is rejected") {
    std::string error;
    const auto bytes = Link(ThreadLocalObject(RcuArch::X86_64, Target::OS::Linux), Target::OS::Linux,
                            Target::Arch::X86_64, "rux-tls-static.a", error, ArtifactKind::StaticLibrary);
    CHECK(bytes.empty());
    CHECK(error.find("thread-local variables are not supported in a static library yet") != std::string::npos);
}

TEST_CASE("a PE image names its template in a TLS directory and supplies the TLS index") {
    std::string error;
    const auto bytes = Link(ThreadLocalObject(RcuArch::X86_64, Target::OS::Windows), Target::OS::Windows,
                            Target::Arch::X86_64, "rux-tls-pe.exe", error);
    CAPTURE(error);
    REQUIRE_FALSE(bytes.empty());

    const std::uint32_t peHeader = Read32(bytes, 0x3C);
    const std::size_t optionalHeader = peHeader + 24;
    const std::uint64_t imageBase = Read64(bytes, optionalHeader + 24);
    // Data directory 9 is the TLS directory, an IMAGE_TLS_DIRECTORY64 of 40 bytes.
    const std::uint32_t directoryRva = Read32(bytes, optionalHeader + 112 + 9 * 8);
    CHECK(Read32(bytes, optionalHeader + 112 + 9 * 8 + 4) == 40);
    REQUIRE(directoryRva != 0);

    const auto sectionCount = static_cast<std::uint16_t>(Read32(bytes, peHeader + 6) & 0xFFFFU);
    const std::size_t sectionTable = optionalHeader + (Read32(bytes, peHeader + 20) & 0xFFFFU);
    const auto offsetOf = [&](const std::uint64_t rva) -> std::size_t {
        for (std::uint16_t index = 0; index < sectionCount; ++index) {
            const std::size_t header = sectionTable + index * 40;
            const std::uint32_t virtualAddress = Read32(bytes, header + 12);
            if (rva >= virtualAddress && rva < virtualAddress + Read32(bytes, header + 8)) {
                return Read32(bytes, header + 20) + (rva - virtualAddress);
            }
        }
        return 0;
    };
    const std::size_t directory = offsetOf(directoryRva);
    REQUIRE(directory != 0);
    const std::uint64_t start = Read64(bytes, directory);
    const std::uint64_t end = Read64(bytes, directory + 8);
    CHECK(end - start == 24);
    CHECK(bytes[offsetOf(start - imageBase) + 8] == 42);
    CHECK(Read64(bytes, directory + 16) != 0);                                      // AddressOfIndex
    CHECK(Read64(bytes, offsetOf(Read64(bytes, directory + 24) - imageBase)) == 0); // no callbacks
    CHECK(Read32(bytes, directory + 36) == 0x0040'0000);                            // IMAGE_SCN_ALIGN_8BYTES

    // The code reads the index the directory names, and `Second` is eight bytes into the thread's block.
    const auto load = std::ranges::search(bytes, std::vector<std::uint8_t>{0x44, 0x8B, 0x15});
    REQUIRE_FALSE(load.empty());
    const auto site = static_cast<std::size_t>(load.begin() - bytes.begin());
    CHECK(Read32(bytes, site + 14) == 8);
    std::uint64_t siteRva = 0;
    for (std::uint16_t index = 0; index < sectionCount; ++index) {
        const std::size_t header = sectionTable + index * 40;
        const std::uint32_t fileOffset = Read32(bytes, header + 20);
        if (site >= fileOffset && site < fileOffset + Read32(bytes, header + 16)) {
            siteRva = Read32(bytes, header + 12) + (site - fileOffset);
        }
    }
    const auto displacement = static_cast<std::int32_t>(Read32(bytes, site + 3));
    CHECK(imageBase + siteRva + 7 + static_cast<std::uint64_t>(static_cast<std::int64_t>(displacement)) ==
          Read64(bytes, directory + 16));
}

TEST_CASE("a Mach-O image gives each thread-local variable a descriptor bound to the bootstrap") {
    std::string error;
    const auto bytes = Link(ThreadLocalObject(RcuArch::X86_64, Target::OS::MacOS), Target::OS::MacOS,
                            Target::Arch::X86_64, "rux-tls-macho", error);
    CAPTURE(error);
    REQUIRE_FALSE(bytes.empty());
    MachOImage image;
    std::string readError;
    REQUIRE(ReadMachO64(bytes, image, readError));
    CHECK((image.flags & 0x0080'0000U) != 0); // MH_HAS_TLV_DESCRIPTORS

    const auto *variables = image.Section("__DATA", "__thread_vars");
    const auto *data = image.Section("__DATA", "__thread_data");
    REQUIRE(variables != nullptr);
    REQUIRE(data != nullptr);
    CHECK(variables->flags == 0x13);
    CHECK(data->flags == 0x11);
    CHECK(variables->size == 3 * 24);
    CHECK(data->size == 24);
    CHECK(bytes[data->offset + 8] == 42);

    // Every descriptor's first word is bound to the bootstrap; its third is the variable's offset in the template.
    std::vector<std::uint64_t> offsets;
    for (std::uint64_t descriptor = 0; descriptor < 3; ++descriptor) {
        offsets.push_back(Read64(bytes, variables->offset + descriptor * 24 + 16));
        const std::uint64_t slot = variables->address + descriptor * 24;
        CHECK(std::ranges::any_of(image.binds, [&](const MachOBind &bind) {
            return bind.symbol == "__tlv_bootstrap" &&
                   image.segments[bind.segmentIndex].vmAddress + bind.segmentOffset == slot;
        }));
    }
    std::ranges::sort(offsets);
    CHECK(offsets == std::vector<std::uint64_t>{0, 8, 16});

    // The code addresses `Second`'s descriptor.
    const auto lea = std::ranges::search(bytes, std::vector<std::uint8_t>{0x48, 0x8D, 0x3D});
    REQUIRE_FALSE(lea.empty());
    const auto site = static_cast<std::size_t>(lea.begin() - bytes.begin());
    const auto *text = image.Section("__TEXT", "__text");
    REQUIRE(text != nullptr);
    const std::uint64_t next = text->address + (site - text->offset) + 7;
    const std::uint64_t descriptor =
        next + static_cast<std::uint64_t>(static_cast<std::int32_t>(Read32(bytes, site + 3)));
    CHECK(Read64(bytes, variables->offset + (descriptor - variables->address) + 16) == 8);
}
