#include "Object/Rcu/Rcu.h"
#include "Object/Rcu/RcuSerialization.h"

#include <array>
#include <cstdint>
#include <doctest.h>
#include <string>
#include <vector>

using namespace Rux;

namespace {
// Every relocation kind an AArch64 object can carry, in declaration order.
constexpr std::array AArch64RelTypes = {
    RcuRelType::AArch64Call26,        RcuRelType::AArch64Jump26,        RcuRelType::AArch64CondBr19,
    RcuRelType::AArch64TstBr14,       RcuRelType::AArch64AdrPrelPgHi21, RcuRelType::AArch64AddAbsLo12Nc,
    RcuRelType::AArch64LdstAbsLo12Nc, RcuRelType::AArch64MovwUabsG0,    RcuRelType::AArch64MovwUabsG1,
    RcuRelType::AArch64MovwUabsG2,    RcuRelType::AArch64MovwUabsG3,    RcuRelType::AArch64Prel32,
    RcuRelType::AArch64Prel64,
};

std::uint16_t ReadU16(const std::vector<std::uint8_t> &bytes, const std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset] | (static_cast<std::uint16_t>(bytes[offset + 1]) << 8U));
}

std::uint32_t ReadU32(const std::vector<std::uint8_t> &bytes, const std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(bytes[offset + i]) << (i * 8U);
    }
    return value;
}

// One .text section holding a relocation of every AArch64 kind.
RcuFile AArch64Object() {
    RcuFile file;
    file.arch = RcuArch::AArch64;
    file.sourcePath = "Main.rux";
    file.packageName = "RcuTest";
    RcuSection text;
    text.name = ".text";
    text.type = RcuSecType::Text;
    text.flags = RcuSecFlag::Alloc | RcuSecFlag::Exec | RcuSecFlag::Read;
    text.alignment = 4;
    text.data.assign(AArch64RelTypes.size() * 4, 0);
    for (std::size_t i = 0; i < AArch64RelTypes.size(); ++i) {
        text.relocs.push_back({static_cast<std::uint32_t>(i * 4), 0, AArch64RelTypes[i], static_cast<std::int32_t>(i)});
    }
    file.sections.push_back(std::move(text));
    file.symbols.push_back({"Main", "int", 0, 4, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});
    return file;
}
} // namespace

TEST_CASE("RCU architecture constants name the two supported machines") {
    CHECK(RcuArch::X86_64 == 0x01);
    CHECK(RcuArch::AArch64 == 0x02);
    CHECK(RcuFile{}.arch == RcuArch::X86_64);
    CHECK(RcuArchFor(Target::Arch::X86_64) == RcuArch::X86_64);
    CHECK(RcuArchFor(Target::Arch::AArch64) == RcuArch::AArch64);
    CHECK(RcuArchFor(Target::Arch::RISCV64) == RcuArch::Unknown);
    CHECK(RcuArchName(RcuArch::X86_64) == "x86-64");
    CHECK(RcuArchName(RcuArch::AArch64) == "AArch64");
    CHECK(RcuArchName(RcuArch::Unknown) == "unknown");
}

TEST_CASE("RCU serialization round-trips the architecture byte and every relocation kind") {
    const auto file = AArch64Object();
    const auto bytes = SerializeRcuFile(file);

    // File header: magic, version, architecture, section and symbol counts.
    REQUIRE(bytes.size() > 32);
    CHECK(bytes[0] == 0x52);
    CHECK(bytes[1] == 0x43);
    CHECK(bytes[2] == 0x55);
    CHECK(bytes[6] == RcuArch::AArch64);
    REQUIRE(ReadU16(bytes, 8) == 1);
    REQUIRE(ReadU32(bytes, 12) == 1);

    // Section table entry: 40 bytes, with the relocation count at 30 and the
    // relocation block offset at 32.
    constexpr std::size_t sectionEntry = 32;
    REQUIRE(ReadU16(bytes, sectionEntry + 30) == AArch64RelTypes.size());
    const std::size_t relocOffset = ReadU32(bytes, sectionEntry + 32);
    REQUIRE(relocOffset + AArch64RelTypes.size() * 16 <= bytes.size());
    for (std::size_t i = 0; i < AArch64RelTypes.size(); ++i) {
        const std::size_t entry = relocOffset + i * 16;
        CHECK(ReadU32(bytes, entry) == i * 4);
        CHECK(ReadU32(bytes, entry + 4) == 0);
        CHECK(ReadU16(bytes, entry + 8) == AArch64RelTypes[i]);
        CHECK(ReadU32(bytes, entry + 12) == i);
    }

    // Every kind has a distinct value, so no two relocations can be confused.
    for (std::size_t i = 0; i < AArch64RelTypes.size(); ++i) {
        for (std::size_t j = i + 1; j < AArch64RelTypes.size(); ++j) {
            CHECK(AArch64RelTypes[i] != AArch64RelTypes[j]);
        }
    }
}

TEST_CASE("RCU dump names the architecture and every relocation kind") {
    const auto text = DumpRcuFileText(AArch64Object());
    CHECK(text.find("; Architecture: AArch64\n") != std::string::npos);
    for (const auto type : AArch64RelTypes) {
        const std::string name(RcuRelTypeName(type));
        CHECK(name != "?");
        CHECK(text.find(name) != std::string::npos);
    }
    CHECK(text.find("AARCH64_ADR_PREL_PG_HI21") != std::string::npos);
    CHECK(text.find("AARCH64_LDST_ABS_LO12_NC") != std::string::npos);

    RcuFile hostFile;
    RcuSection data;
    data.name = ".data";
    data.type = RcuSecType::Data;
    data.alignment = 8;
    data.data.resize(8);
    data.relocs.push_back({0, 0, RcuRelType::Abs64, 0});
    hostFile.sections.push_back(std::move(data));
    hostFile.symbols.push_back({"Value", "int", 0, 8, 0, RcuSymKind::Data, RcuSymVis::Global});
    const auto hostText = DumpRcuFileText(hostFile);
    CHECK(hostText.find("; Architecture: x86-64\n") != std::string::npos);
    CHECK(hostText.find("ABS_64") != std::string::npos);
}
