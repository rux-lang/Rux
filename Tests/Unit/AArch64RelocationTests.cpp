#include "Linker/AArch64Relocation.h"
#include "Object/Rcu/Rcu.h"

#include <array>
#include <cstdint>
#include <doctest.h>
#include <string>

using namespace Rux;

namespace {
[[nodiscard]] uint32_t ReadWord(const Buf &buf) {
    return static_cast<uint32_t>(buf[0]) | static_cast<uint32_t>(buf[1]) << 8U | static_cast<uint32_t>(buf[2]) << 16U |
           static_cast<uint32_t>(buf[3]) << 24U;
}

[[nodiscard]] uint64_t ReadGiant(const Buf &buf) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(buf[i]) << (i * 8U);
    }
    return value;
}

[[nodiscard]] bool Apply(Buf &buf, const uint16_t type, const uint64_t targetVA, const int64_t addend,
                         const uint64_t siteVA, std::string &error) {
    return ApplyAArch64Relocation(buf, 0, type, targetVA, addend, siteVA, "Target", "test writer", error);
}
} // namespace

TEST_CASE("AArch64 relocation applier patches absolute and PC-relative fields") {
    std::string error;

    SUBCASE("ABS64 includes the inline addend") {
        Buf buf(8);
        REQUIRE(Apply(buf, RcuRelType::Abs64, 0x1122334455667000, 0x788, 0, error));
        CHECK(ReadGiant(buf) == 0x1122334455667788);
    }

    SUBCASE("ABS32 includes the inline addend") {
        Buf buf(4);
        REQUIRE(Apply(buf, RcuRelType::Abs32, 0x12345000, 0x678, 0, error));
        CHECK(ReadWord(buf) == 0x12345678);
    }

    SUBCASE("PREL64 writes S plus A minus P") {
        Buf buf(8);
        REQUIRE(Apply(buf, RcuRelType::AArch64Prel64, 0x2000, 8, 0x1000, error));
        CHECK(ReadGiant(buf) == 0x1008);
    }

    SUBCASE("PREL32 preserves a negative displacement") {
        Buf buf(4);
        REQUIRE(Apply(buf, RcuRelType::AArch64Prel32, 0x1000, -4, 0x1010, error));
        CHECK(ReadWord(buf) == 0xFFFFFFEC);
    }
}

TEST_CASE("AArch64 relocation applier patches every branch and address field") {
    std::string error;

    SUBCASE("CALL26 patches a forward branch") {
        Buf buf;
        WriteU32(buf, 0x94000000);
        REQUIRE(Apply(buf, RcuRelType::AArch64Call26, 0x1040, 0, 0x1000, error));
        CHECK(ReadWord(buf) == 0x94000010);
    }

    SUBCASE("JUMP26 patches a backward branch") {
        Buf buf;
        WriteU32(buf, 0x14000000);
        REQUIRE(Apply(buf, RcuRelType::AArch64Jump26, 0x1000, 0, 0x1040, error));
        CHECK(ReadWord(buf) == 0x17FFFFF0);
    }

    SUBCASE("CONDBR19 preserves the condition") {
        Buf buf;
        WriteU32(buf, 0x5400000D);
        REQUIRE(Apply(buf, RcuRelType::AArch64CondBr19, 0x100C, 0, 0x1000, error));
        CHECK(ReadWord(buf) == 0x5400006D);
    }

    SUBCASE("TSTBR14 patches a negative displacement") {
        Buf buf;
        WriteU32(buf, 0x36180005);
        REQUIRE(Apply(buf, RcuRelType::AArch64TstBr14, 0x0FFC, 0, 0x1000, error));
        CHECK(ReadWord(buf) == 0x361FFFE5);
    }

    SUBCASE("ADR_PREL_PG_HI21 splits its page offset") {
        Buf buf;
        WriteU32(buf, 0x90000007);
        REQUIRE(Apply(buf, RcuRelType::AArch64AdrPrelPgHi21, 0x4568, 0, 0x1234, error));
        CHECK(ReadWord(buf) == 0xF0000007);
    }

    SUBCASE("ADD_ABS_LO12_NC patches the low twelve bits") {
        Buf buf;
        WriteU32(buf, 0x91000021);
        REQUIRE(Apply(buf, RcuRelType::AArch64AddAbsLo12Nc, 0x1ABC, 0, 0, error));
        CHECK(ReadWord(buf) == 0x912AF021);
    }
}

TEST_CASE("AArch64 relocation applier scales every load-store low-twelve field") {
    struct Case {
        uint32_t instruction;
        unsigned scale;
    };

    constexpr std::array cases = {
        Case{0x39400041, 0}, // ldrb w1, [x2]
        Case{0x79400041, 1}, // ldrh w1, [x2]
        Case{0xB9400041, 2}, // ldr  w1, [x2]
        Case{0xF9400041, 3}, // ldr  x1, [x2]
        Case{0x3DC00041, 4}, // ldr  q1, [x2]
    };

    for (const auto &[instruction, scale] : cases) {
        CAPTURE(scale);
        Buf buf;
        WriteU32(buf, instruction);
        std::string error;
        REQUIRE(Apply(buf, RcuRelType::AArch64LdstAbsLo12Nc, 0x1AB0, 0, 0, error));
        const uint32_t patched = ReadWord(buf);
        CHECK((patched >> 10U & 0xFFFU) == (0xAB0U >> scale));
        CHECK((patched & ~(0xFFFU << 10U)) == instruction);
    }
}

TEST_CASE("AArch64 relocation applier patches every MOVW halfword") {
    constexpr uint64_t value = 0x1122334455667788;
    constexpr std::array<uint32_t, 4> instructions = {0xD2800002, 0xF2A00002, 0xF2C00002, 0xF2E00002};

    for (unsigned halfword = 0; halfword < instructions.size(); ++halfword) {
        CAPTURE(halfword);
        Buf buf;
        WriteU32(buf, instructions[halfword]);
        std::string error;
        REQUIRE(Apply(buf, RcuRelType::AArch64MovwUabsG0 + halfword, value, 0, 0, error));
        const uint32_t patched = ReadWord(buf);
        CHECK((patched >> 5U & 0xFFFFU) == (value >> (halfword * 16U) & 0xFFFFU));
        CHECK((patched & ~(0xFFFFU << 5U)) == instructions[halfword]);
    }
}

TEST_CASE("AArch64 relocation applier rejects every out-of-range field") {
    struct Case {
        uint16_t type;
        uint64_t targetVA;
        const char *description;
    };

    constexpr std::array cases = {
        Case{RcuRelType::AArch64Call26, uint64_t{1} << 27U, "a branch reaches 128 MB either way"},
        Case{RcuRelType::AArch64Jump26, uint64_t{1} << 27U, "a branch reaches 128 MB either way"},
        Case{RcuRelType::AArch64CondBr19, uint64_t{1} << 20U, "a conditional branch reaches 1 MB either way"},
        Case{RcuRelType::AArch64TstBr14, uint64_t{1} << 15U, "a test-and-branch reaches 32 KB either way"},
        Case{RcuRelType::AArch64AdrPrelPgHi21, uint64_t{1} << 32U, "an ADRP reaches 4 GB either way"},
        Case{RcuRelType::AArch64Prel32, uint64_t{1} << 31U, "the displacement does not fit in 32 bits"},
    };

    for (const auto &[type, targetVA, description] : cases) {
        CAPTURE(type);
        Buf buf(8);
        std::string error;
        CHECK_FALSE(Apply(buf, type, targetVA, 0, 0, error));
        CHECK(error ==
              std::string(RcuRelTypeName(type)) + " relocation against 'Target' is out of range: " + description);
    }
}

TEST_CASE("AArch64 relocation applier rejects every unaligned branch field") {
    struct Case {
        uint16_t type;
        const char *description;
    };

    constexpr std::array cases = {
        Case{RcuRelType::AArch64Call26, "a branch reaches 128 MB either way"},
        Case{RcuRelType::AArch64Jump26, "a branch reaches 128 MB either way"},
        Case{RcuRelType::AArch64CondBr19, "a conditional branch reaches 1 MB either way"},
        Case{RcuRelType::AArch64TstBr14, "a test-and-branch reaches 32 KB either way"},
    };

    for (const auto &[type, description] : cases) {
        CAPTURE(type);
        Buf buf(4);
        std::string error;
        CHECK_FALSE(Apply(buf, type, 2, 0, 0, error));
        CHECK(error ==
              std::string(RcuRelTypeName(type)) + " relocation against 'Target' is out of range: " + description);
    }
}

TEST_CASE("AArch64 relocation applier enforces every load-store access alignment") {
    constexpr std::array<uint32_t, 4> instructions = {
        0x79400041, // ldrh w1, [x2]
        0xB9400041, // ldr  w1, [x2]
        0xF9400041, // ldr  x1, [x2]
        0x3DC00041, // ldr  q1, [x2]
    };

    for (const uint32_t instruction : instructions) {
        CAPTURE(instruction);
        Buf buf;
        WriteU32(buf, instruction);
        std::string error;
        CHECK_FALSE(Apply(buf, RcuRelType::AArch64LdstAbsLo12Nc, 1, 0, 0, error));
        CHECK(error == "AARCH64_LDST_ABS_LO12_NC relocation against 'Target' is out of range: the symbol is not "
                       "aligned to the access width");
        CHECK(ReadWord(buf) == instruction);
    }
}

TEST_CASE("AArch64 relocation applier attributes unsupported forms to its image writer") {
    Buf buf(4);
    std::string error;
    CHECK_FALSE(Apply(buf, RcuRelType::Rel32, 0, 0, 0, error));
    CHECK(error == "relocation REL_32 against 'Target' is not supported by the test writer");
}
