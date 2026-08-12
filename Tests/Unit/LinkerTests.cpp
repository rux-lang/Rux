#include "Linker/ArchiveWriter.h"
#include "Linker/Linker.h"

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

    // The ELF writer lays this object out for AArch64, but the PE writer does
    // not: the Windows path still assumes x86-64 code.
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

// An ELF64 image read back for inspection, with the accessors the AArch64
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

    [[nodiscard]] uint16_t Read16(const size_t offset) const {
        return static_cast<uint16_t>(bytes[offset] | bytes[offset + 1] << 8U);
    }

    [[nodiscard]] uint16_t Machine() const {
        return Read16(18);
    }

    [[nodiscard]] uint16_t Type() const {
        return Read16(16);
    }

    // One entry of the program header table, in the order the fields sit in.
    struct Segment {
        uint32_t type;
        uint32_t flags;
        uint64_t offset;
        uint64_t address;
        uint64_t fileSize;
        uint64_t memorySize;
        uint64_t alignment;
    };

    [[nodiscard]] std::vector<Segment> Segments() const {
        const auto table = static_cast<size_t>(Read64(32));
        std::vector<Segment> segments;
        for (size_t i = 0; i < Read16(56); ++i) {
            const size_t header = table + i * 56;
            segments.push_back({Read32(header), Read32(header + 4), Read64(header + 8), Read64(header + 16),
                                Read64(header + 32), Read64(header + 40), Read64(header + 48)});
        }
        return segments;
    }

    // The one segment of a kind the image carries, or nothing when it carries
    // none: PT_PHDR, PT_INTERP and PT_DYNAMIC are each written at most once.
    [[nodiscard]] std::optional<Segment> SegmentOfType(const uint32_t type) const {
        const auto segments = Segments();
        const auto found = std::ranges::find(segments, type, &Segment::type);
        return found == segments.end() ? std::nullopt : std::optional(*found);
    }

    [[nodiscard]] uint64_t Entry() const {
        return Read64(24);
    }

    // The file offset a virtual address maps to, found through the PT_LOAD
    // segment that covers it.
    [[nodiscard]] size_t OffsetOf(const uint64_t virtualAddress) const {
        for (const auto &segment : Segments()) {
            if (segment.type != 1) { // PT_LOAD
                continue;
            }
            if (virtualAddress >= segment.address && virtualAddress < segment.address + segment.memorySize) {
                return static_cast<size_t>(segment.offset + (virtualAddress - segment.address));
            }
        }
        return 0;
    }

    // The virtual address of the writable PT_LOAD, which is where .data lands.
    [[nodiscard]] uint64_t WritableSegmentAddress() const {
        for (const auto &segment : Segments()) {
            if (segment.type == 1 && (segment.flags & 0x2U) != 0) {
                return segment.address;
            }
        }
        return 0;
    }

    [[nodiscard]] uint32_t Word(const uint64_t virtualAddress) const {
        return Read32(OffsetOf(virtualAddress));
    }

    [[nodiscard]] uint64_t Giant(const uint64_t virtualAddress) const {
        return Read64(OffsetOf(virtualAddress));
    }

    // The p_align every PT_LOAD declares, or zero when they disagree.
    [[nodiscard]] uint64_t LoadAlignment() const {
        uint64_t alignment = 0;
        for (const auto &segment : Segments()) {
            if (segment.type != 1) { // PT_LOAD
                continue;
            }
            if (alignment != 0 && alignment != segment.alignment) {
                return 0;
            }
            alignment = segment.alignment;
        }
        return alignment;
    }

    // The value of a .dynamic tag, found through PT_DYNAMIC.
    [[nodiscard]] uint64_t DynamicTag(const uint64_t tag) const {
        const auto dynamic = SegmentOfType(2); // PT_DYNAMIC
        if (!dynamic) {
            return 0;
        }
        const auto offset = static_cast<size_t>(dynamic->offset);
        for (size_t at = offset; at + 16 <= offset + dynamic->fileSize; at += 16) {
            if (Read64(at) == tag) {
                return Read64(at + 8);
            }
        }
        return 0;
    }

    // The address an ADRP / LDR / ADD trio at `virtualAddress` reaches, read
    // back out of the three immediate fields. Both halves of the split must
    // agree, which is what makes this an assertion rather than a decode.
    [[nodiscard]] uint64_t GotSlotReachedBy(const uint64_t virtualAddress) const {
        const uint32_t adrp = Word(virtualAddress);
        const auto immediate = static_cast<int32_t>(((adrp >> 5U & 0x7FFFFU) << 2U | (adrp >> 29U & 3U)) << 11U) >> 11;
        const uint64_t page = (virtualAddress & ~uint64_t{0xFFF}) + (static_cast<int64_t>(immediate) << 12U);
        const uint64_t viaLoad = page + ((Word(virtualAddress + 4) >> 10U & 0xFFFU) << 3U);
        const uint64_t viaAdd = page + (Word(virtualAddress + 8) >> 10U & 0xFFFU);
        return viaLoad == viaAdd ? viaLoad : 0;
    }
};

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
    const auto begin = image.bytes.begin() + static_cast<std::ptrdiff_t>(interp->offset);
    CHECK(std::string(begin, begin + static_cast<std::ptrdiff_t>(interp->fileSize) - 1) == kAArch64Interpreter);
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
    CHECK(std::string(image.bytes.begin(), image.bytes.end()).find("libm.so.6") != std::string::npos);
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
        CHECK(image.Giant(jmprel + i * 24) == slot);
        CHECK(image.Giant(jmprel + i * 24 + 8) == (static_cast<uint64_t>(i + 1) << 32 | 1026));
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
    const uint64_t rela = image.DynamicTag(7); // DT_RELA
    REQUIRE(rela != 0);
    CHECK(image.DynamicTag(8) == 24); // DT_RELASZ
    CHECK(image.Giant(rela + 8) == 1027);
    CHECK(image.Giant(rela + 16) == image.Giant(image.Giant(rela)));
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

    // A relocation naming a field inside an instruction is an ELF-only form.
    // The AArch64 targets that use the other two containers still lower through
    // the platform toolchain, so neither has a number for one.
    file.sections[0].relocs.push_back({0, 0, RcuRelType::AArch64Call26, 0});
    REQUIRE(WriteNativeObject(file, Target::OS::Linux, Target::Arch::AArch64, object, error));
    CHECK_FALSE(WriteNativeObject(file, Target::OS::Windows, Target::Arch::AArch64, object, error));
    CHECK(error == "relocation AARCH64_CALL26 in section .text is not supported by the COFF object writer");
    CHECK_FALSE(WriteNativeObject(file, Target::OS::MacOS, Target::Arch::AArch64, object, error));
    CHECK(error == "relocation AARCH64_CALL26 in section .text is not supported by the Mach-O object writer");
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
