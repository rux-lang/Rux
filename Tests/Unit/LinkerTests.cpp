#include "ElfReader.h"
#include "Linker/ArchiveWriter.h"
#include "Linker/Linker.h"
#include "Target/ElfProfile.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <vector>

using namespace Rux;
using Rux::Testing::ElfImage;

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
    const ElfImage image{{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()}};
    CHECK(std::ranges::contains(image.NeededLibraries(), "libm.so.6"));

    stream.close();
    std::filesystem::remove(output, ec);
}

TEST_CASE("ELF linker resolves local functions defined in another object") {
    const auto link = [](const std::uint8_t objectArch, const Target::Arch targetArch,
                         std::vector<std::uint8_t> mainCode, std::vector<std::uint8_t> helperCode,
                         const std::uint32_t relocationOffset, const std::uint16_t relocationType,
                         const std::string_view suffix) {
        RcuFile caller;
        caller.arch = objectArch;
        RcuSection callerText;
        callerText.name = ".text";
        callerText.type = RcuSecType::Text;
        callerText.flags = RcuSecFlag::Alloc | RcuSecFlag::Exec | RcuSecFlag::Read;
        callerText.alignment = targetArch == Target::Arch::AArch64 ? 4 : 16;
        callerText.data = std::move(mainCode);
        callerText.relocs.push_back({relocationOffset, 1, relocationType, 0});
        const auto mainSize = static_cast<std::uint32_t>(callerText.data.size());
        caller.sections.push_back(std::move(callerText));
        caller.symbols.push_back({"Main", "int", 0, mainSize, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});
        caller.symbols.push_back(
            {"PrivateHelper", "", 0, 0, RCU_SEC_EXTERNAL, RcuSymKind::ExternFunc, RcuSymVis::Global});

        RcuFile definitions;
        definitions.arch = objectArch;
        RcuSection helperText;
        helperText.name = ".text";
        helperText.type = RcuSecType::Text;
        helperText.flags = RcuSecFlag::Alloc | RcuSecFlag::Exec | RcuSecFlag::Read;
        helperText.alignment = targetArch == Target::Arch::AArch64 ? 4 : 16;
        helperText.data = std::move(helperCode);
        const auto helperSize = static_cast<std::uint32_t>(helperText.data.size());
        definitions.sections.push_back(std::move(helperText));
        definitions.symbols.push_back(
            {"PrivateHelper", "int", 0, helperSize, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Local});

        const auto output =
            std::filesystem::temp_directory_path() / ("rux-elf-cross-object-local-function-" + std::string(suffix));
        std::error_code ec;
        std::filesystem::remove(output, ec);
        Linker linker({std::move(caller), std::move(definitions)}, "LinkerTest", {}, ArtifactKind::Executable,
                      Target::OS::Linux, targetArch);
        const bool linked = linker.Link(output);
        CAPTURE(linker.Errors().empty() ? std::string{} : linker.Errors().front().message);
        REQUIRE(linked);

        std::ifstream stream(output, std::ios::binary);
        REQUIRE(stream.is_open());
        ElfImage image{{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()}};
        stream.close();
        std::filesystem::remove(output, ec);
        return image;
    };

    const ElfImage x86 = link(RcuArch::X86_64, Target::Arch::X86_64,
                              {0xE8, 0, 0, 0, 0, 0x31, 0xC0, 0xC3}, // call PrivateHelper; xor eax, eax; ret
                              {0xC3}, 1, RcuRelType::Rel32, "x86-64");
    CHECK(x86.Machine() == 0x3E);
    CHECK_FALSE(x86.SegmentOfType(2).has_value()); // no PT_DYNAMIC
    CHECK_FALSE(x86.SegmentOfType(3).has_value()); // no PT_INTERP
    CHECK(x86.NeededLibraries().empty());

    const ElfImage aarch64 = link(RcuArch::AArch64, Target::Arch::AArch64,
                                  {0x00, 0x00, 0x00, 0x94, 0xC0, 0x03, 0x5F, 0xD6}, // bl PrivateHelper; ret
                                  {0xC0, 0x03, 0x5F, 0xD6}, 0, RcuRelType::AArch64Call26, "aarch64");
    CHECK(aarch64.Machine() == 0xB7);
    CHECK_FALSE(aarch64.SegmentOfType(2).has_value()); // no PT_DYNAMIC
    CHECK_FALSE(aarch64.SegmentOfType(3).has_value()); // no PT_INTERP
    CHECK(aarch64.NeededLibraries().empty());
}

TEST_CASE("ELF linker names the loader and C library of the target, not of the host") {
    // A dynamic image whose one import names no library of its own, so the
    // writer supplies the target's C library and the loader that binds it.
    // Every answer below follows the target alone: the same host links both.
    const auto link = [](const Target::OS os) {
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

        const auto output = std::filesystem::temp_directory_path() / "rux-elf-target-loader-test";
        std::error_code ec;
        std::filesystem::remove(output, ec);
        Linker linker({std::move(caller)}, "LinkerTest", {}, ArtifactKind::Executable, os, Target::Arch::X86_64);
        REQUIRE(linker.Link(output));

        std::ifstream stream(output, std::ios::binary);
        REQUIRE(stream.is_open());
        ElfImage image{{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()}};
        stream.close();
        std::filesystem::remove(output, ec);
        return image;
    };

    const ElfImage linux = link(Target::OS::Linux);
    CHECK(linux.OsAbi() == 0); // ELFOSABI_SYSV
    CHECK(linux.Interpreter() == "/lib64/ld-linux-x86-64.so.2");
    CHECK(std::ranges::contains(linux.NeededLibraries(), "libc.so.6"));

    const ElfImage freebsd = link(Target::OS::FreeBSD);
    CHECK(freebsd.OsAbi() == 9); // ELFOSABI_FREEBSD
    CHECK(freebsd.Interpreter() == "/libexec/ld-elf.so.1");
    CHECK(std::ranges::contains(freebsd.NeededLibraries(), "libc.so.7"));
    // BSD libc expects the process globals crt1 would define; an image that
    // starts at Main defines them itself.
    const auto symbols = freebsd.DynamicSymbols();
    CHECK(std::ranges::contains(symbols, "__progname", &ElfImage::DynamicSymbol::name));
}

TEST_CASE("ELF target profiles own OS and architecture policy") {
    const auto linuxX86 = Target::Elf64ProfileFor(Target::OS::Linux, Target::Arch::X86_64);
    REQUIRE(linuxX86.has_value());
    CHECK(linuxX86->osAbi == 0);
    CHECK(linuxX86->machine == 0x3E);
    CHECK(linuxX86->maximumLoadAlignment == 0x1000);
    CHECK(linuxX86->freestandingExitSyscall == 60);
    CHECK(linuxX86->dynamicRelocations.jumpSlot == 7);
    CHECK(linuxX86->pltInstructionShape == Target::ElfPltInstructionShape::X86_64);

    const auto linuxAarch64 = Target::Elf64ProfileFor(Target::OS::Linux, Target::Arch::AArch64);
    REQUIRE(linuxAarch64.has_value());
    CHECK(linuxAarch64->machine == 0xB7);
    CHECK(linuxAarch64->maximumLoadAlignment == 0x10000);
    CHECK(linuxAarch64->freestandingExitSyscall == 93);
    CHECK(linuxAarch64->dynamicRelocations.jumpSlot == 1026);
    CHECK_FALSE(linuxAarch64->definesElfAuxVector);
    CHECK(linuxAarch64->pltInstructionShape == Target::ElfPltInstructionShape::AArch64);

    const auto freebsdAarch64 = Target::Elf64ProfileFor(Target::OS::FreeBSD, Target::Arch::AArch64);
    REQUIRE(freebsdAarch64.has_value());
    CHECK(freebsdAarch64->targetName == "freebsd-aarch64");
    CHECK(freebsdAarch64->osAbi == 9);
    CHECK(freebsdAarch64->machine == 0xB7);
    CHECK(freebsdAarch64->interpreter == "/libexec/ld-elf.so.1");
    CHECK(freebsdAarch64->defaultLibc == "libc.so.7");
    CHECK(freebsdAarch64->freestandingExitSyscall == 1);
    CHECK(freebsdAarch64->dynamicRelocations.jumpSlot == 1026);
    CHECK(freebsdAarch64->dynamicRelocations.globDat == 1025);
    CHECK(freebsdAarch64->dynamicRelocations.relative == 1027);
    CHECK(freebsdAarch64->definesBsdProcessGlobals);
    CHECK(freebsdAarch64->definesElfAuxVector);

    // Only the two ELF operating systems have a profile; Windows and macOS
    // reach their own writers instead.
    CHECK_FALSE(Target::Elf64ProfileFor(Target::OS::Unknown, Target::Arch::AArch64).has_value());
    CHECK_FALSE(Target::Elf64ProfileFor(Target::OS::Windows, Target::Arch::X86_64).has_value());
}

TEST_CASE("ELF linker rejects a target without an explicit profile") {
    RcuFile object;
    object.arch = RcuArch::AArch64;
    const auto output = std::filesystem::temp_directory_path() / "rux-elf-unsupported-profile-test";
    std::error_code ec;
    std::filesystem::remove(output, ec);

    Linker linker({std::move(object)}, "LinkerTest", {}, ArtifactKind::Executable, Target::OS::Unknown,
                  Target::Arch::AArch64);
    CHECK_FALSE(linker.Link(output));
    REQUIRE(linker.Errors().size() == 1);
    CHECK(linker.Errors().front().message ==
          "cannot link ELF executable 'LinkerTest': ELF writer does not implement the complete target "
          "'unknown-aarch64'");
    CHECK_FALSE(std::filesystem::exists(output));
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
    const ElfImage image{{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()}};
    CHECK(image.Type() == 3); // ET_DYN
    CHECK(image.Entry() == 0);
    const auto symbols = image.DynamicSymbols();
    CHECK(std::ranges::contains(symbols, "Answer", &ElfImage::DynamicSymbol::name));
    CHECK(image.DynamicRelocations().size() == 1);

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
    CHECK(linker.Errors().front().message ==
          "cannot link ELF executable 'LinkerTest': RCU object 'Main.rux' was compiled for AArch64, but the link "
          "target is x86-64");
    CHECK_FALSE(std::filesystem::exists(output));

    // Task 9 enables the lower-level PE executable writer while the compiler
    // driver remains gated until all Windows AArch64 artifact kinds exist.
    Linker windows({std::move(foreign)}, "LinkerTest", {}, ArtifactKind::Executable, Target::OS::Windows,
                   Target::Arch::AArch64);
    CHECK(windows.Link(output));
    CHECK(windows.Errors().empty());
    CHECK(std::filesystem::exists(output));

    std::filesystem::remove(output, ec);
}

TEST_CASE("link diagnostics name every image format artifact and corrupt RCU owner") {
    struct Case {
        Target::OS os;
        ArtifactKind artifact;
        std::string_view prefix;
    };

    constexpr std::array cases = {
        Case{Target::OS::Linux, ArtifactKind::Executable, "cannot link ELF executable 'Broken': "},
        Case{Target::OS::Windows, ArtifactKind::SharedLibrary, "cannot link PE/COFF shared library 'Broken': "},
        Case{Target::OS::MacOS, ArtifactKind::StaticLibrary, "cannot link Mach-O static library 'Broken': "},
    };

    for (const auto &[os, artifact, prefix] : cases) {
        CAPTURE(prefix);
        RcuFile object;
        object.sourcePath = "Broken.rcu";
        RcuSection text;
        text.name = ".text";
        text.type = RcuSecType::Text;
        text.alignment = 0;
        object.sections.push_back(std::move(text));

        Linker linker({std::move(object)}, "Broken", {}, artifact, os, Target::Arch::X86_64);
        CHECK_FALSE(linker.Link(std::filesystem::temp_directory_path() / "rux-broken-link-diagnostic"));
        REQUIRE(linker.Errors().size() == 1);
        CHECK(linker.Errors().front().message ==
              std::string(prefix) + "RCU object 'Broken.rcu' has invalid alignment for section '.text'");
        CHECK(linker.Errors().front().notes ==
              std::vector<std::string>{"alignment: 0 bytes", "section alignment must be a non-zero power of two"});
        CHECK_FALSE(linker.Errors().front().message.contains("external linker"));
        CHECK_FALSE(linker.Errors().front().message.contains("toolchain"));
    }
}

namespace {
// Appends one little-endian AArch64 instruction word to a section.
void AppendWord(std::vector<uint8_t> &data, const uint32_t word) {
    for (unsigned i = 0; i < 4; ++i) {
        data.push_back(static_cast<uint8_t>(word >> (i * 8U)));
    }
}

ElfImage LinkAArch64Image(std::vector<RcuFile> objects, const std::filesystem::path &output, const ArtifactKind kind) {
    std::error_code ec;
    std::filesystem::remove(output, ec);
    Linker linker(std::move(objects), "LinkerTest", {}, kind, Target::OS::Linux, Target::Arch::AArch64);
    REQUIRE(linker.Link(output));
    std::ifstream stream(output, std::ios::binary);
    REQUIRE(stream.is_open());
    ElfImage image{{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()}};
    stream.close();
    std::filesystem::remove(output, ec);
    return image;
}

ElfImage LinkAArch64Executable(std::vector<RcuFile> objects, const std::filesystem::path &output) {
    return LinkAArch64Image(std::move(objects), output, ArtifactKind::Executable);
}

ElfImage LinkAArch64Executable(RcuFile object, const std::filesystem::path &output) {
    std::vector<RcuFile> objects;
    objects.push_back(std::move(object));
    return LinkAArch64Executable(std::move(objects), output);
}

// An AArch64 section carrying `data`, ready to be pushed onto an object.
RcuSection MakeSection(const std::string &name, const uint32_t type, const uint32_t flags, const uint16_t alignment,
                       std::vector<uint8_t> data) {
    RcuSection section;
    section.name = name;
    section.type = type;
    section.flags = flags;
    section.alignment = alignment;
    section.data = std::move(data);
    return section;
}

// A freestanding AArch64 object with one section of each kind: a Main that
// returns 42, eight bytes of read-only data and eight bytes of writable data,
// which is one loadable segment each.
RcuFile MakeAArch64Object() {
    RcuFile object;
    object.arch = RcuArch::AArch64;
    std::vector<uint8_t> code;
    AppendWord(code, 0xD2800540); // mov x0, #42
    AppendWord(code, 0xD65F03C0); // ret
    object.sections.push_back(MakeSection(".text", RcuSecType::Text,
                                          RcuSecFlag::Alloc | RcuSecFlag::Exec | RcuSecFlag::Read, 4, std::move(code)));
    object.sections.push_back(
        MakeSection(".rodata", RcuSecType::RoData, RcuSecFlag::Alloc | RcuSecFlag::Read, 8, {1, 2, 3, 4, 5, 6, 7, 8}));
    object.sections.push_back(MakeSection(".data", RcuSecType::Data,
                                          RcuSecFlag::Alloc | RcuSecFlag::Read | RcuSecFlag::Write, 8,
                                          {9, 10, 11, 12, 13, 14, 15, 16}));
    object.symbols.push_back({"Main", "int", 0, 8, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});
    object.symbols.push_back({"Table", "int", 0, 8, RCU_RODATA_IDX, RcuSymKind::Data, RcuSymVis::Global});
    object.symbols.push_back({"Counter", "int", 0, 8, RCU_DATA_IDX, RcuSymKind::Data, RcuSymVis::Global});
    return object;
}

// The AArch64 loader an image asks for by name, with the terminator it is
// written with.
constexpr std::string_view kAArch64Interpreter = "/lib/ld-linux-aarch64.so.1";
} // namespace

TEST_CASE("ELF linker fills every field of an AArch64 executable's header") {
    const ElfImage image = LinkAArch64Executable(MakeAArch64Object(), std::filesystem::temp_directory_path() /
                                                                          "rux-elf-aarch64-header-test");

    // e_ident: the magic, ELFCLASS64, ELFDATA2LSB and EV_CURRENT. The byte
    // after them is the ABI, which follows the host rather than the target —
    // it is the kernel that reads it — and the seven after that are reserved.
    REQUIRE(image.bytes.size() > 64);
    CHECK(std::string(image.bytes.begin(), image.bytes.begin() + 4) == std::string("\x7F"
                                                                                   "ELF",
                                                                                   4));
    CHECK(image.bytes[4] == 2);
    CHECK(image.bytes[5] == 1);
    CHECK(image.bytes[6] == 1);
    CHECK(std::ranges::all_of(image.bytes | std::views::drop(9) | std::views::take(7),
                              [](const uint8_t byte) { return byte == 0; }));

    CHECK(image.Type() == 2);       // ET_EXEC
    CHECK(image.Machine() == 0xB7); // EM_AARCH64
    CHECK(image.Read32(20) == 1);   // e_version
    CHECK(image.Read64(32) == 64);  // e_phoff: the table follows the header
    CHECK(image.Read32(48) == 0);   // e_flags: AArch64 defines none for an ELF64 image
    CHECK(image.Read16(52) == 64);  // e_ehsize
    CHECK(image.Read16(54) == 56);  // e_phentsize
    CHECK(image.Read16(56) == image.Segments().size());

    // This writer emits no section header table at all: an executable is read
    // by the loader, which reads program headers, and nothing else has to
    // parse it. Every field describing that table is therefore zero, and a
    // reader must not be told there is a table of zero-sized entries.
    CHECK(image.Read64(40) == 0); // e_shoff
    CHECK(image.Read16(58) == 0); // e_shentsize
    CHECK(image.Read16(60) == 0); // e_shnum
    CHECK(image.Read16(62) == 0); // e_shstrndx

    // The entry is an instruction address in the executable segment, so it is
    // four-byte aligned and inside the segment's memory range.
    const uint64_t entry = image.Entry();
    CHECK(entry % 4 == 0);
    const auto segments = image.Segments();
    const auto text =
        std::ranges::find_if(segments, [](const ElfImage::Segment &s) { return s.type == 1 && (s.flags & 0x1U) != 0; });
    REQUIRE(text != segments.end());
    CHECK(entry >= text->address);
    CHECK(entry < text->address + text->memorySize);
}

TEST_CASE("ELF linker gives an AArch64 executable one loadable segment per permission") {
    const ElfImage image = LinkAArch64Executable(MakeAArch64Object(),
                                                 std::filesystem::temp_directory_path() / "rux-elf-aarch64-phdr-test");

    const auto segments = image.Segments();
    std::vector<ElfImage::Segment> loads;
    std::ranges::copy_if(segments, std::back_inserter(loads), [](const ElfImage::Segment &s) { return s.type == 1; });

    // Three sections with three sets of permissions are three segments: the
    // code, the read-only data and the writable data, in that order.
    REQUIRE(loads.size() == 3);
    CHECK(loads[0].flags == 0x5); // PF_R | PF_X
    CHECK(loads[1].flags == 0x4); // PF_R
    CHECK(loads[2].flags == 0x6); // PF_R | PF_W

    // The first segment holds the entry stub and the object's code, one page
    // above the image base, and each of the others starts a page later.
    CHECK(loads[0].offset == 0x10000);
    CHECK(loads[0].address == 0x400000 + 0x10000);
    for (size_t i = 0; i + 1 < loads.size(); ++i) {
        CHECK(loads[i + 1].address > loads[i].address + loads[i].memorySize);
        CHECK(loads[i + 1].offset > loads[i].offset);
    }

    for (const auto &load : loads) {
        // A segment is mapped from the file whole, so what it occupies in
        // memory is what it occupies in the file, and the file has to hold it.
        CHECK(load.fileSize == load.memorySize);
        CHECK(load.offset + load.fileSize <= image.bytes.size());

        // mmap maps whole pages, so a segment's address and its file offset
        // must agree modulo the page it declares — the largest an AArch64
        // kernel may be configured for.
        CHECK(load.alignment == 0x10000);
        CHECK(load.address % load.alignment == load.offset % load.alignment);
    }

    // Both data sections are readable at the addresses their segments name,
    // which is what makes the two segments above more than a pair of numbers.
    CHECK(image.Giant(image.WritableSegmentAddress()) == 0x100F0E0D0C0B0A09);
    CHECK(image.Giant(loads[1].address) == 0x0807060504030201);
}

TEST_CASE("ELF linker points an AArch64 dynamic executable at its interpreter") {
    RcuFile object = MakeAArch64Object();
    object.sections[RCU_TEXT_IDX].data.clear();
    AppendWord(object.sections[RCU_TEXT_IDX].data, 0x94000000); // bl sqrt
    AppendWord(object.sections[RCU_TEXT_IDX].data, 0xD65F03C0); // ret
    object.sections[RCU_TEXT_IDX].relocs.push_back({0, 3, RcuRelType::AArch64Call26, 0});
    object.symbols.push_back({"sqrt", "libm.so.6", 0, 0, RCU_SEC_EXTERNAL, RcuSymKind::ExternFunc, RcuSymVis::Global});

    const ElfImage image = LinkAArch64Executable(std::move(object), std::filesystem::temp_directory_path() /
                                                                        "rux-elf-aarch64-interp-test");

    // An image with imports is laid out differently: two segments rather than
    // three, since .rodata is mapped with the code, and four headers describing
    // the image to the loader before them.
    const auto phdr = image.SegmentOfType(6);    // PT_PHDR
    const auto interp = image.SegmentOfType(3);  // PT_INTERP
    const auto dynamic = image.SegmentOfType(2); // PT_DYNAMIC
    REQUIRE(phdr.has_value());
    REQUIRE(interp.has_value());
    REQUIRE(dynamic.has_value());

    // PT_PHDR describes the table it is itself an entry of.
    CHECK(phdr->offset == image.Read64(32));
    CHECK(phdr->fileSize == static_cast<uint64_t>(image.Read16(56)) * 56);
    CHECK(phdr->fileSize == phdr->memorySize);

    // PT_INTERP names the AArch64 loader, with the terminator the kernel reads
    // the name up to.
    REQUIRE(interp->fileSize == kAArch64Interpreter.size() + 1);
    CHECK(image.Interpreter() == kAArch64Interpreter);
    CHECK(image.bytes[static_cast<size_t>(interp->offset + interp->fileSize) - 1] == 0);
    CHECK(interp->alignment == 1);

    // PT_DYNAMIC sits inside the writable segment — the loader writes the GOT
    // slots it describes — and ends with the DT_NULL that terminates it.
    const auto segments = image.Segments();
    const auto writable =
        std::ranges::find_if(segments, [](const ElfImage::Segment &s) { return s.type == 1 && (s.flags & 0x2U) != 0; });
    REQUIRE(writable != segments.end());
    CHECK(dynamic->address >= writable->address);
    CHECK(dynamic->address + dynamic->memorySize <= writable->address + writable->memorySize);
    CHECK(dynamic->alignment == 8);
    CHECK(dynamic->fileSize % 16 == 0);
    CHECK(image.Read64(static_cast<size_t>(dynamic->offset + dynamic->fileSize) - 16) == 0); // DT_NULL
    CHECK(image.DynamicTag(5) != 0);                                                         // DT_STRTAB
    CHECK(std::ranges::contains(image.NeededLibraries(), "libm.so.6"));
}

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

TEST_CASE("ELF linker binds an AArch64 import through a PLT stub and its GOT slot") {
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

    const ElfImage image =
        LinkAArch64Executable(std::move(object), std::filesystem::temp_directory_path() / "rux-elf-aarch64-plt-test");

    CHECK(image.Machine() == 0xB7); // EM_AARCH64
    // Two segments may not share a page and carry different permissions, so an
    // AArch64 image separates them by the largest page it may be mapped with.
    CHECK(image.LoadAlignment() == 0x10000);
    CHECK(std::string(image.bytes.begin(), image.bytes.end()).find("/lib/ld-linux-aarch64.so.1") != std::string::npos);

    // A dynamic program leaves through libc exit() rather than through a raw
    // syscall, so buffered stdio is flushed; Main's result is already in X0.
    const uint64_t entry = image.Entry();
    const uint32_t callExit = image.Word(entry + 16);
    CHECK((callExit & 0xFC000000U) == 0x94000000U);
    CHECK(image.Word(entry + 20) == 0xD4200000); // brk #0, unreachable
    const auto exitOffset = static_cast<int32_t>((callExit & 0x03FFFFFFU) << 6U) >> 6;
    const uint64_t exitStub = entry + 16 + 4 * static_cast<int64_t>(exitOffset);

    // exit and sqrt are the only imports and their stubs follow the header in
    // sorted order, so exit's stub is the first and names the PLT itself.
    const uint64_t pltVA = exitStub - 32;
    CHECK(pltVA % 16 == 0);
    const uint64_t got = image.DynamicTag(3);     // DT_PLTGOT
    const uint64_t jmprel = image.DynamicTag(23); // DT_JMPREL
    REQUIRE(got != 0);
    REQUIRE(jmprel != 0);
    CHECK(image.DynamicTag(20) == 7);     // DT_PLTREL = DT_RELA
    CHECK(image.DynamicTag(2) == 2 * 24); // DT_PLTRELSZ: one entry per import
    CHECK(image.DynamicTag(4) != 0);      // DT_HASH
    const auto pltRelocations = image.PltRelocations();
    REQUIRE(pltRelocations.size() == 2);

    // The trio every stub reaches its GOT slot with, whatever address the two
    // immediates carry: the page in X16, the slot's contents in X17 and the
    // slot's own address back in X16, which is what the resolver expects to
    // find there.
    const auto checkGotTrio = [&image](const uint64_t at) {
        CHECK((image.Word(at) & 0x9F00001FU) == 0x90000010U);     // adrp x16, <page>
        CHECK((image.Word(at + 4) & 0xFFC003FFU) == 0xF9400211U); // ldr  x17, [x16, #<lo12>]
        CHECK((image.Word(at + 8) & 0xFFC003FFU) == 0x91000210U); // add  x16, x16, #<lo12>
    };

    // PLT[0] pushes the stub's GOT pointer and the return address where the
    // resolver reads them, then enters the resolver through .got.plt[2]. The
    // three NOPs pad the header out to the sixteen bytes a stub occupies, so
    // that a stub's index is its offset.
    CHECK(image.Word(pltVA) == 0xA9BF7BF0); // stp x16, x30, [sp, #-16]!
    checkGotTrio(pltVA + 4);
    CHECK(image.GotSlotReachedBy(pltVA + 4) == got + 16);
    CHECK(image.Word(pltVA + 16) == 0xD61F0220); // br x17
    CHECK(image.Word(pltVA + 20) == 0xD503201F); // nop
    CHECK(image.Word(pltVA + 24) == 0xD503201F);
    CHECK(image.Word(pltVA + 28) == 0xD503201F);

    // Each stub reaches its own GOT slot with the same ADRP / LDR / ADD trio and
    // branches through it, and the slot an unbound call reads points back at
    // PLT[0], which is where an AArch64 stub resumes.
    for (size_t i = 0; i < 2; ++i) {
        const uint64_t stub = pltVA + 32 + i * 16;
        const uint64_t slot = got + (3 + i) * 8;
        checkGotTrio(stub);
        CHECK(image.GotSlotReachedBy(stub) == slot);
        CHECK(image.Word(stub + 12) == 0xD61F0220); // br x17
        CHECK(image.Giant(slot) == pltVA);

        // .rela.plt names that slot with R_AARCH64_JUMP_SLOT and the import's
        // own .dynsym index, in the order the resolver counts them in.
        CHECK(pltRelocations[i].offset == slot);
        CHECK(pltRelocations[i].symbolIndex == i + 1);
        CHECK(pltRelocations[i].type == 1026); // R_AARCH64_JUMP_SLOT
    }

    // The call site is bound to sqrt's stub, the second of the two.
    const uint64_t main = entry + 24;
    const uint32_t call = image.Word(main);
    CHECK((call & 0xFC000000U) == 0x94000000U);
    const auto callOffset = static_cast<int32_t>((call & 0x03FFFFFFU) << 6U) >> 6;
    CHECK(main + 4 * static_cast<int64_t>(callOffset) == pltVA + 48);
}

TEST_CASE("ELF linker writes an AArch64 shared library whose pointers rebase themselves") {
    RcuFile library;
    library.arch = RcuArch::AArch64;
    RcuSection text;
    text.name = ".text";
    text.type = RcuSecType::Text;
    text.flags = RcuSecFlag::Alloc | RcuSecFlag::Exec | RcuSecFlag::Read;
    text.alignment = 4;
    AppendWord(text.data, 0xD2800540); // mov x0, #42
    AppendWord(text.data, 0xD65F03C0); // ret
    library.sections.push_back(std::move(text));
    RcuSection data;
    data.name = ".data";
    data.type = RcuSecType::Data;
    data.flags = RcuSecFlag::Alloc | RcuSecFlag::Read | RcuSecFlag::Write;
    data.alignment = 8;
    data.data.resize(8);
    data.relocs.push_back({0, 0, RcuRelType::Abs64, 0}); // a pointer to Answer
    library.sections.push_back(std::move(data));
    library.symbols.push_back({"Answer", "int", 0, 8, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});

    const ElfImage image =
        LinkAArch64Image({std::move(library)}, std::filesystem::temp_directory_path() / "librux-linker-aarch64-test.so",
                         ArtifactKind::SharedLibrary);

    CHECK(image.Machine() == 0xB7);                        // EM_AARCH64
    CHECK((image.bytes[16] | image.bytes[17] << 8U) == 3); // ET_DYN
    CHECK(image.Entry() == 0);
    CHECK(image.LoadAlignment() == 0x10000);
    CHECK(image.DynamicTag(9) == 24); // DT_RELAENT

    // The one absolute pointer in .data is left to the loader as an
    // R_AARCH64_RELATIVE carrying its link-time value as the addend.
    const auto relocations = image.DynamicRelocations();
    REQUIRE(relocations.size() == 1);
    CHECK(image.DynamicTag(8) == 24); // DT_RELASZ
    CHECK(relocations[0].type == 1027);
    CHECK(static_cast<uint64_t>(relocations[0].addend) == image.Giant(relocations[0].offset));
}

TEST_CASE("ELF linker leaves a pointer to an import in an AArch64 shared library to the loader") {
    RcuFile library;
    library.arch = RcuArch::AArch64;
    std::vector<uint8_t> code;
    AppendWord(code, 0xD65F03C0); // ret
    library.sections.push_back(MakeSection(
        ".text", RcuSecType::Text, RcuSecFlag::Alloc | RcuSecFlag::Exec | RcuSecFlag::Read, 4, std::move(code)));
    RcuSection data =
        MakeSection(".data", RcuSecType::Data, RcuSecFlag::Alloc | RcuSecFlag::Read | RcuSecFlag::Write, 8, {});
    data.data.resize(8);
    data.relocs.push_back({0, 2, RcuRelType::Abs64, 0}); // a pointer to sqrt
    library.sections.push_back(std::move(data));
    library.symbols.push_back({"Answer", "int", 0, 4, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});
    library.symbols.push_back({"Slot", "int", 0, 8, RCU_DATA_IDX, RcuSymKind::Data, RcuSymVis::Global});
    library.symbols.push_back({"sqrt", "libm.so.6", 0, 0, RCU_SEC_EXTERNAL, RcuSymKind::ExternFunc, RcuSymVis::Global});

    const ElfImage image = LinkAArch64Image(
        {std::move(library)}, std::filesystem::temp_directory_path() / "librux-linker-aarch64-globdat-test.so",
        ArtifactKind::SharedLibrary);

    // A pointer to an import cannot be resolved to this image's own PLT stub —
    // the address the program is meant to see is the one the loader binds — so
    // the slot is named by an R_AARCH64_GLOB_DAT against the import's own
    // .dynsym entry, with nothing added to it.
    const uint64_t rela = image.DynamicTag(7); // DT_RELA
    REQUIRE(rela != 0);
    REQUIRE(image.DynamicTag(8) == 24); // DT_RELASZ: this one relocation
    // The slot is in the writable segment, past the .dynamic and GOT that open
    // it, and the loader writes a whole address into it.
    CHECK(image.Giant(rela) > image.WritableSegmentAddress());
    CHECK(image.Giant(rela) % 8 == 0);
    CHECK((image.Giant(rela + 8) & 0xFFFFFFFFU) == 1025); // R_AARCH64_GLOB_DAT
    CHECK((image.Giant(rela + 8) >> 32) != 0);            // the .dynsym index of sqrt
    CHECK(image.Giant(rela + 16) == 0);
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

    // Every AArch64 container carries the branch relocation using its native
    // spelling, including ARM64_RELOC_BRANCH26 in Mach-O.
    file.sections[0].data = {0x00, 0x00, 0x00, 0x94}; // bl
    file.sections[0].relocs.push_back({0, 0, RcuRelType::AArch64Call26, 0});
    REQUIRE(WriteNativeObject(file, Target::OS::Linux, Target::Arch::AArch64, object, error));
    REQUIRE(WriteNativeObject(file, Target::OS::Windows, Target::Arch::AArch64, object, error));
    REQUIRE(WriteNativeObject(file, Target::OS::MacOS, Target::Arch::AArch64, object, error));
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

TEST_CASE("ELF linker starts each object's sections on the alignment that object asked for") {
    // A pooled floating-point constant is reached by a scaled LDR whose twelve
    // bits count doublewords, so an address that does not divide by eight has
    // no encoding at all. The first object's read-only data ends on an odd
    // byte, which leaves the second object's constant aligned only if the
    // linker pads between the two.
    RcuFile first;
    first.arch = RcuArch::AArch64;
    std::vector<uint8_t> entry;
    AppendWord(entry, 0xD65F03C0); // ret
    first.sections.push_back(
        MakeSection(".text", RcuSecType::Text, RcuSecFlag::Alloc | RcuSecFlag::Exec | RcuSecFlag::Read, 4, entry));
    first.sections.push_back(
        MakeSection(".rodata", RcuSecType::RoData, RcuSecFlag::Alloc | RcuSecFlag::Read, 8, {'h', 'i', 0}));
    first.symbols.push_back({"Main", "int", 0, 4, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});

    RcuFile second;
    second.arch = RcuArch::AArch64;
    std::vector<uint8_t> loader;
    AppendWord(loader, 0x90000010); // adrp x16, Pooled
    AppendWord(loader, 0xFD400210); // ldr  d16, [x16, :lo12:Pooled]
    RcuSection text =
        MakeSection(".text", RcuSecType::Text, RcuSecFlag::Alloc | RcuSecFlag::Exec | RcuSecFlag::Read, 4, loader);
    text.relocs.push_back({0, 0, RcuRelType::AArch64AdrPrelPgHi21, 0});
    text.relocs.push_back({4, 0, RcuRelType::AArch64LdstAbsLo12Nc, 0});
    second.sections.push_back(std::move(text));

    constexpr uint64_t payload = 0x0123456789ABCDEFULL;
    std::vector<uint8_t> pooled;
    for (unsigned i = 0; i < 8; ++i) {
        pooled.push_back(static_cast<uint8_t>(payload >> (i * 8U)));
    }
    second.sections.push_back(
        MakeSection(".rodata", RcuSecType::RoData, RcuSecFlag::Alloc | RcuSecFlag::Read, 8, std::move(pooled)));
    second.symbols.push_back({"Pooled", "float64", 0, 8, RCU_RODATA_IDX, RcuSymKind::Const, RcuSymVis::Local});

    std::vector<RcuFile> objects;
    objects.push_back(std::move(first));
    objects.push_back(std::move(second));
    // Linking at all is half the assertion: an unaligned constant is reported
    // rather than truncated, so this fails before it reads anything back.
    const ElfImage image = LinkAArch64Executable(std::move(objects),
                                                 std::filesystem::temp_directory_path() / "rux-elf-aarch64-align-test");

    // The second object's text follows the entry stub and the first object's
    // single instruction.
    const uint64_t site = image.Entry() + 24 + 4;
    const uint32_t adrp = image.Word(site);
    const auto pageOffset = static_cast<int32_t>(((adrp >> 5U & 0x7FFFFU) << 2U | (adrp >> 29U & 3U)) << 11U) >> 11;
    const uint64_t page = (site & ~uint64_t{0xFFF}) + (static_cast<int64_t>(pageOffset) << 12U);
    const uint64_t address = page + ((image.Word(site + 4) >> 10U & 0xFFFU) << 3U);

    CHECK(address % 8 == 0);
    CHECK(image.Read64(image.OffsetOf(address)) == payload);
}
