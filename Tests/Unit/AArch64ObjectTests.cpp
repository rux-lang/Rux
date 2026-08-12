// The relocatable half of the AArch64 back end: the ELF `.o` that a
// StaticLibrary package's archive is built out of, and that another linker
// resolves rather than this one. BACKLOG.md Task 30.
//
// Everything here is read back out of the bytes the writer produced, through
// the section header table, the way a linker reads them -- not out of the
// layout the writer used to place them.

#include "Linker/NativeObjectWriter.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <doctest.h>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Rux;

namespace {
// ELF64 section header fields, by their offset inside the 64-byte header.
constexpr std::size_t kShType = 4;
constexpr std::size_t kShFlags = 8;
constexpr std::size_t kShOffset = 24;
constexpr std::size_t kShSize = 32;
constexpr std::size_t kShLink = 40;
constexpr std::size_t kShInfo = 44;
constexpr std::size_t kShAlign = 48;
constexpr std::size_t kShEntSize = 56;

class ElfObject {
public:
    explicit ElfObject(std::vector<std::uint8_t> inputBytes)
        : bytes(std::move(inputBytes)) {
    }

    [[nodiscard]] std::uint8_t Byte(const std::size_t offset) const {
        return bytes.at(offset);
    }

    [[nodiscard]] std::uint16_t Half(const std::size_t offset) const {
        return static_cast<std::uint16_t>(Byte(offset) | Byte(offset + 1) << 8U);
    }

    [[nodiscard]] std::uint32_t Word(const std::size_t offset) const {
        return static_cast<std::uint32_t>(Half(offset)) | static_cast<std::uint32_t>(Half(offset + 2)) << 16U;
    }

    [[nodiscard]] std::uint64_t Giant(const std::size_t offset) const {
        return static_cast<std::uint64_t>(Word(offset)) | static_cast<std::uint64_t>(Word(offset + 4)) << 32U;
    }

    [[nodiscard]] std::size_t SectionCount() const {
        return Half(60);
    }

    [[nodiscard]] std::size_t SectionHeader(const std::size_t index) const {
        return static_cast<std::size_t>(Giant(40)) + index * 64;
    }

    [[nodiscard]] std::string SectionName(const std::size_t index) const {
        const auto names = static_cast<std::size_t>(Giant(SectionHeader(Half(62)) + kShOffset));
        std::string name;
        for (auto at = names + Word(SectionHeader(index)); Byte(at) != 0; ++at) {
            name.push_back(static_cast<char>(Byte(at)));
        }
        return name;
    }

    [[nodiscard]] std::optional<std::size_t> FindSection(const std::string_view name) const {
        for (std::size_t i = 1; i < SectionCount(); ++i) {
            if (SectionName(i) == name) {
                return i;
            }
        }
        return std::nullopt;
    }

    // The `index`th RELA entry of section `header`, as {type, symbol, addend}.
    [[nodiscard]] std::array<std::int64_t, 3> Relocation(const std::size_t header, const std::size_t index) const {
        const auto at = static_cast<std::size_t>(Giant(header + kShOffset)) + index * 24;
        const std::uint64_t info = Giant(at + 8);
        return {static_cast<std::int64_t>(info & 0xFFFFFFFFU), static_cast<std::int64_t>(info >> 32U),
                static_cast<std::int64_t>(Giant(at + 16))};
    }

    [[nodiscard]] std::size_t RelocationCount(const std::size_t header) const {
        return static_cast<std::size_t>(Giant(header + kShSize) / 24);
    }

private:
    std::vector<std::uint8_t> bytes;
};

RcuSection TextSection(std::vector<std::uint8_t> data) {
    RcuSection text;
    text.name = ".text";
    text.type = RcuSecType::Text;
    text.flags = RcuSecFlag::Alloc | RcuSecFlag::Exec | RcuSecFlag::Read;
    text.alignment = 4;
    text.data = std::move(data);
    return text;
}

// A `.text` holding one instruction per relocation, so a relocation's site
// carries the word the writer has to decode to number it.
RcuFile ObjectOf(const std::vector<std::pair<std::uint32_t, std::uint16_t>> &instructions) {
    RcuFile file;
    file.arch = RcuArch::AArch64;
    file.sourcePath = "Library.rux";
    std::vector<std::uint8_t> code;
    RcuSection text = TextSection({});
    for (std::size_t i = 0; i < instructions.size(); ++i) {
        const auto [word, kind] = instructions[i];
        for (std::size_t byte = 0; byte < 4; ++byte) {
            code.push_back(static_cast<std::uint8_t>(word >> (byte * 8U)));
        }
        text.relocs.push_back({static_cast<std::uint32_t>(i * 4), 0, kind, 0});
    }
    text.data = std::move(code);
    file.sections.push_back(std::move(text));
    file.symbols.push_back({"Target", "", 0, 0, RCU_SEC_EXTERNAL, RcuSymKind::ExternFunc, RcuSymVis::Global});
    return file;
}

ElfObject WriteAArch64(const RcuFile &file) {
    NativeObject object;
    std::string error;
    REQUIRE_MESSAGE(WriteNativeObject(file, Target::OS::Linux, Target::Arch::AArch64, object, error), error);
    return ElfObject(std::move(object.bytes));
}
} // namespace

TEST_CASE("AArch64 object writer stamps an ET_REL header for EM_AARCH64") {
    RcuFile file;
    file.arch = RcuArch::AArch64;
    file.sourcePath = "Library.rux";
    file.sections.push_back(TextSection({0xC0, 0x03, 0x5F, 0xD6})); // ret
    file.symbols.push_back({"Answer", "int", 0, 4, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});

    NativeObject object;
    std::string error;
    REQUIRE(WriteNativeObject(file, Target::OS::Linux, Target::Arch::AArch64, object, error));
    CHECK(object.name == "Library.o");
    CHECK(object.publicSymbols == std::vector<std::string>{"Answer"});

    const ElfObject elf(std::move(object.bytes));
    CHECK(elf.Byte(0) == 0x7F);
    CHECK(elf.Byte(1) == 'E');
    CHECK(elf.Byte(4) == 2);    // ELFCLASS64
    CHECK(elf.Byte(5) == 1);    // ELFDATA2LSB, which AArch64 is linked as
    CHECK(elf.Byte(6) == 1);    // EV_CURRENT
    CHECK(elf.Half(16) == 1);   // ET_REL: this is an input to a link, not an image
    CHECK(elf.Half(18) == 183); // EM_AARCH64
    CHECK(elf.Word(48) == 0);   // e_flags: AArch64 defines none
    CHECK(elf.Half(52) == 64);  // e_ehsize
    CHECK(elf.Half(56) == 0);   // no program headers in a relocatable object
    CHECK(elf.Half(58) == 64);  // e_shentsize
    CHECK(elf.Giant(24) == 0);  // no entry point

    // Section index 0 is the reserved null entry: an index of 0 means "none",
    // so the header standing in for it describes nothing.
    const auto null = elf.SectionHeader(0);
    CHECK(elf.SectionName(0).empty());
    CHECK(elf.Word(null + kShType) == 0);
    CHECK(elf.Giant(null + kShSize) == 0);
    CHECK(elf.Giant(null + kShAlign) == 0);

    REQUIRE(elf.FindSection(".text").has_value());
    REQUIRE(elf.FindSection(".symtab").has_value());
    REQUIRE(elf.FindSection(".strtab").has_value());
}

TEST_CASE("AArch64 object writer numbers every relocation an instruction carries") {
    // Each pair is the instruction the relocation sits on and the RCU kind that
    // names it; the expectation below is the R_AARCH64_* number the ELF for the
    // Arm 64-bit Architecture document gives that kind.
    const auto file = ObjectOf({
        {0x94000000, RcuRelType::AArch64Call26},        // bl
        {0x14000000, RcuRelType::AArch64Jump26},        // b
        {0x54000000, RcuRelType::AArch64CondBr19},      // b.eq
        {0x36180000, RcuRelType::AArch64TstBr14},       // tbz
        {0x90000000, RcuRelType::AArch64AdrPrelPgHi21}, // adrp
        {0x91000000, RcuRelType::AArch64AddAbsLo12Nc},  // add
        {0xD2800000, RcuRelType::AArch64MovwUabsG0},    // movz
        {0xF2A00000, RcuRelType::AArch64MovwUabsG1},    // movk lsl #16
        {0xF2C00000, RcuRelType::AArch64MovwUabsG2},    // movk lsl #32
        {0xF2E00000, RcuRelType::AArch64MovwUabsG3},    // movk lsl #48
    });
    const auto elf = WriteAArch64(file);
    const auto rela = elf.FindSection(".rela.text");
    REQUIRE(rela.has_value());
    const auto header = elf.SectionHeader(*rela);
    REQUIRE(elf.RelocationCount(header) == 10);

    CHECK(elf.Relocation(header, 0)[0] == 283); // R_AARCH64_CALL26
    CHECK(elf.Relocation(header, 1)[0] == 282); // R_AARCH64_JUMP26
    CHECK(elf.Relocation(header, 2)[0] == 280); // R_AARCH64_CONDBR19
    CHECK(elf.Relocation(header, 3)[0] == 279); // R_AARCH64_TSTBR14
    CHECK(elf.Relocation(header, 4)[0] == 275); // R_AARCH64_ADR_PREL_PG_HI21
    CHECK(elf.Relocation(header, 5)[0] == 277); // R_AARCH64_ADD_ABS_LO12_NC
    // The four MOVW kinds together carry one 64-bit value, a halfword each, so
    // the three that have a bits-dropped spelling take it: a checking G0 would
    // reject every symbol above 65535.
    CHECK(elf.Relocation(header, 6)[0] == 264); // R_AARCH64_MOVW_UABS_G0_NC
    CHECK(elf.Relocation(header, 7)[0] == 266); // R_AARCH64_MOVW_UABS_G1_NC
    CHECK(elf.Relocation(header, 8)[0] == 268); // R_AARCH64_MOVW_UABS_G2_NC
    CHECK(elf.Relocation(header, 9)[0] == 269); // R_AARCH64_MOVW_UABS_G3
}

TEST_CASE("AArch64 object writer picks a load-store relocation by access width") {
    // RCU has one LDST kind and AAELF64 has five, one per access width, because
    // the linker scales the symbol's low 12 bits down by the width before
    // placing them. The width lives only in the instruction, so the writer
    // decodes the site rather than being told.
    const auto file = ObjectOf({
        {0x39400041, RcuRelType::AArch64LdstAbsLo12Nc}, // ldrb w1, [x2]
        {0x79400041, RcuRelType::AArch64LdstAbsLo12Nc}, // ldrh w1, [x2]
        {0xB9400041, RcuRelType::AArch64LdstAbsLo12Nc}, // ldr  w1, [x2]
        {0xF9400041, RcuRelType::AArch64LdstAbsLo12Nc}, // ldr  x1, [x2]
        {0x3DC00041, RcuRelType::AArch64LdstAbsLo12Nc}, // ldr  q1, [x2]
        {0x3D800041, RcuRelType::AArch64LdstAbsLo12Nc}, // str  q1, [x2]
    });
    const auto elf = WriteAArch64(file);
    const auto rela = elf.FindSection(".rela.text");
    REQUIRE(rela.has_value());
    const auto header = elf.SectionHeader(*rela);
    REQUIRE(elf.RelocationCount(header) == 6);

    CHECK(elf.Relocation(header, 0)[0] == 278); // R_AARCH64_LDST8_ABS_LO12_NC
    CHECK(elf.Relocation(header, 1)[0] == 284); // R_AARCH64_LDST16_ABS_LO12_NC
    CHECK(elf.Relocation(header, 2)[0] == 285); // R_AARCH64_LDST32_ABS_LO12_NC
    CHECK(elf.Relocation(header, 3)[0] == 286); // R_AARCH64_LDST64_ABS_LO12_NC
    // The 128-bit form is the one the two-bit size field cannot express on its
    // own, and it is read the same way out of a store as out of a load.
    CHECK(elf.Relocation(header, 4)[0] == 299); // R_AARCH64_LDST128_ABS_LO12_NC
    CHECK(elf.Relocation(header, 5)[0] == 299);
}

TEST_CASE("AArch64 relocation sections carry explicit addends and name their target") {
    RcuFile file;
    file.arch = RcuArch::AArch64;
    file.sourcePath = "Library.rux";
    RcuSection code = TextSection({0x00, 0x00, 0x00, 0x94}); // bl
    code.relocs.push_back({0, 1, RcuRelType::AArch64Call26, 12});
    file.sections.push_back(std::move(code));

    RcuSection pool;
    pool.name = ".rodata";
    pool.type = RcuSecType::RoData;
    pool.flags = RcuSecFlag::Alloc | RcuSecFlag::Read;
    pool.alignment = 8;
    pool.data.resize(32);
    pool.relocs.push_back({0, 1, RcuRelType::Abs64, 24});
    pool.relocs.push_back({8, 1, RcuRelType::Abs32, 0});
    pool.relocs.push_back({16, 1, RcuRelType::AArch64Prel64, 0});
    pool.relocs.push_back({24, 1, RcuRelType::AArch64Prel32, -8});
    file.sections.push_back(std::move(pool));

    // A local symbol sorts ahead of a global one, so the symbol the
    // relocations name is not the index RCU gave it.
    file.symbols.push_back({"Pool", "", 0, 32, 1, RcuSymKind::Const, RcuSymVis::Local});
    file.symbols.push_back({"Answer", "int", 0, 4, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});

    const auto elf = WriteAArch64(file);
    const auto text = elf.FindSection(".text");
    const auto rodata = elf.FindSection(".rodata");
    const auto symtab = elf.FindSection(".symtab");
    const auto relaText = elf.FindSection(".rela.text");
    const auto relaRodata = elf.FindSection(".rela.rodata");
    REQUIRE(text.has_value());
    REQUIRE(rodata.has_value());
    REQUIRE(symtab.has_value());
    REQUIRE(relaText.has_value());
    REQUIRE(relaRodata.has_value());

    // Both relocation sections say which symbols they name and which section
    // they patch. SHF_INFO_LINK is what makes sh_info a section index rather
    // than the count it is in a symbol table.
    for (const auto section : {*relaText, *relaRodata}) {
        const auto header = elf.SectionHeader(section);
        CHECK(elf.Word(header + kShType) == 4);      // SHT_RELA
        CHECK(elf.Giant(header + kShFlags) == 0x40); // SHF_INFO_LINK
        CHECK(elf.Word(header + kShLink) == *symtab);
        CHECK(elf.Giant(header + kShEntSize) == 24);
        CHECK(elf.Giant(header + kShAlign) == 8);
    }
    CHECK(elf.Word(elf.SectionHeader(*relaText) + kShInfo) == *text);
    CHECK(elf.Word(elf.SectionHeader(*relaRodata) + kShInfo) == *rodata);

    // RELA carries the addend, so the bytes at the site stay untouched and the
    // addend arrives exactly as RCU recorded it. `Answer` is symbol 2: the null
    // entry, then the local `Pool` the partition sorted ahead of it.
    const auto branch = elf.Relocation(elf.SectionHeader(*relaText), 0);
    CHECK(branch[0] == 283);
    CHECK(branch[1] == 2);
    CHECK(branch[2] == 12);

    const auto header = elf.SectionHeader(*relaRodata);
    REQUIRE(elf.RelocationCount(header) == 4);
    CHECK(elf.Relocation(header, 0) == std::array<std::int64_t, 3>{257, 2, 24}); // R_AARCH64_ABS64
    CHECK(elf.Relocation(header, 1) == std::array<std::int64_t, 3>{258, 2, 0});  // R_AARCH64_ABS32
    CHECK(elf.Relocation(header, 2) == std::array<std::int64_t, 3>{260, 2, 0});  // R_AARCH64_PREL64
    CHECK(elf.Relocation(header, 3) == std::array<std::int64_t, 3>{261, 2, -8}); // R_AARCH64_PREL32
}

TEST_CASE("an AArch64 relative relocation carries no x86-64 displacement bias") {
    // Rel32 is the architecture-neutral kind, and the two architectures measure
    // it from different places: R_X86_64_PC32 counts from the end of the four
    // bytes it patches and so reaches the symbol with a -4, while
    // R_AARCH64_PREL32 counts from the field itself.
    RcuFile file;
    file.sourcePath = "Library.rux";
    RcuSection data;
    data.name = ".data";
    data.type = RcuSecType::Data;
    data.flags = RcuSecFlag::Alloc | RcuSecFlag::Read | RcuSecFlag::Write;
    data.alignment = 8;
    data.data.resize(8);
    data.relocs.push_back({0, 0, RcuRelType::Rel32, 16});
    file.sections.push_back(std::move(data));
    file.symbols.push_back({"Answer", "", 0, 0, RCU_SEC_EXTERNAL, RcuSymKind::ExternFunc, RcuSymVis::Global});

    NativeObject object;
    std::string error;
    REQUIRE(WriteNativeObject(file, Target::OS::Linux, Target::Arch::X86_64, object, error));
    const ElfObject intel(std::move(object.bytes));
    const auto intelRela = intel.FindSection(".rela.data");
    REQUIRE(intelRela.has_value());
    CHECK(intel.Relocation(intel.SectionHeader(*intelRela), 0) == std::array<std::int64_t, 3>{2, 1, 12});

    file.arch = RcuArch::AArch64;
    const auto arm = WriteAArch64(file);
    const auto armRela = arm.FindSection(".rela.data");
    REQUIRE(armRela.has_value());
    CHECK(arm.Relocation(arm.SectionHeader(*armRela), 0) == std::array<std::int64_t, 3>{261, 1, 16});
}
