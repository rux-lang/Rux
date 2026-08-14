#include "Linker/RcuObjectLayout.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <doctest.h>
#include <limits>
#include <span>
#include <utility>
#include <vector>

using namespace Rux;

namespace {
RcuSection Section(const std::uint32_t type, const std::uint16_t alignment, std::vector<std::uint8_t> data,
                   std::vector<RcuReloc> relocations = {}) {
    return {.name = {},
            .type = type,
            .flags = 0,
            .alignment = alignment,
            .data = std::move(data),
            .relocs = std::move(relocations)};
}

std::array<RcuFile, 2> OddSizedObjects() {
    RcuFile first;
    first.sections = {Section(RcuSecType::Text, 4, {0x11, 0x12, 0x13}),
                      Section(RcuSecType::RoData, 1, {0x21, 0x22, 0x23}), Section(RcuSecType::Data, 2, {0x31}),
                      Section(RcuSecType::Bss, 4, {0, 0, 0})};

    RcuFile second;
    second.sections = {Section(RcuSecType::Text, 16, {0x41, 0x42, 0x43, 0x44}), Section(RcuSecType::RoData, 8, {0x51}),
                       Section(RcuSecType::Data, 8, {0x61}), Section(RcuSecType::Bss, 8, {0, 0})};
    return {std::move(first), std::move(second)};
}
} // namespace

TEST_CASE("RCU object layout aligns odd-sized input sections after explicit writer prefixes") {
    const auto objects = OddSizedObjects();
    const std::array prefix = {std::uint8_t{0xA0}, std::uint8_t{0xA1}, std::uint8_t{0xA2}, std::uint8_t{0xA3},
                               std::uint8_t{0xA4}};
    RcuLayoutPrefixes prefixes;
    prefixes.text = prefix;
    prefixes.textPadding = 0xCC;
    const RcuObjectLayout layout = RcuObjectLayout::Build(objects, prefixes);

    CHECK(layout.Section(0, RCU_TEXT_IDX) == (RcuSectionPlacement{RcuMergedSection::Text, 8}));
    CHECK(layout.Section(1, RCU_TEXT_IDX) == (RcuSectionPlacement{RcuMergedSection::Text, 16}));
    CHECK(layout.Section(1, RCU_RODATA_IDX) == (RcuSectionPlacement{RcuMergedSection::RoData, 8}));
    CHECK(layout.Section(1, RCU_DATA_IDX) == (RcuSectionPlacement{RcuMergedSection::Data, 8}));
    CHECK(layout.Section(0, RCU_BSS_IDX) == (RcuSectionPlacement{RcuMergedSection::Bss, 0}));
    CHECK(layout.Section(1, RCU_BSS_IDX) == (RcuSectionPlacement{RcuMergedSection::Bss, 8}));
    CHECK(layout.BssSize() == 10);

    const auto &text = layout.Data(RcuMergedSection::Text);
    REQUIRE(text.size() == 20);
    CHECK(std::ranges::equal(std::span(text).first(prefix.size()), prefix));
    CHECK(text[5] == 0xCC);
    CHECK(text[7] == 0xCC);
    CHECK(text[8] == 0x11);
    CHECK(text[15] == 0xCC);
    CHECK(text[16] == 0x41);
    CHECK(layout.Alignment(RcuMergedSection::Text) == 16);
    CHECK(layout.Alignment(RcuMergedSection::RoData) == 8);
    CHECK(layout.Alignment(RcuMergedSection::Data) == 8);
    CHECK(layout.Alignment(RcuMergedSection::Bss) == 8);
}

TEST_CASE("RCU object layout keeps PE ELF and Mach-O prefix contracts deterministic") {
    const auto objects = OddSizedObjects();
    const auto verify = [&](const std::size_t prefixSize, const std::uint8_t prefixByte,
                            const std::uint8_t paddingByte) {
        const std::vector prefix(prefixSize, prefixByte);
        RcuLayoutPrefixes prefixes;
        prefixes.text = prefix;
        prefixes.textPadding = paddingByte;
        const RcuObjectLayout first = RcuObjectLayout::Build(objects, prefixes);
        const RcuObjectLayout second = RcuObjectLayout::Build(objects, prefixes);
        CHECK(first.Data(RcuMergedSection::Text) == second.Data(RcuMergedSection::Text));
        CHECK(first.Data(RcuMergedSection::RoData) == second.Data(RcuMergedSection::RoData));
        CHECK(first.Data(RcuMergedSection::Data) == second.Data(RcuMergedSection::Data));
        CHECK(first.Section(1, RCU_TEXT_IDX) == second.Section(1, RCU_TEXT_IDX));
        REQUIRE(first.Section(0, RCU_TEXT_IDX).has_value());
        CHECK(first.Section(0, RCU_TEXT_IDX)->offset % 4 == 0);
        REQUIRE(first.Section(1, RCU_TEXT_IDX).has_value());
        CHECK(first.Section(1, RCU_TEXT_IDX)->offset % 16 == 0);
    };

    SUBCASE("PE entry and import stubs") {
        verify(17, 0xE9, 0xCC);
    }
    SUBCASE("ELF entry and PLT preamble") {
        verify(28, 0xD5, 0);
    }
    SUBCASE("Mach-O entry prefix") {
        verify(12, 0x94, 0);
    }
}

TEST_CASE("RCU object layout exposes typed symbol relocation and image addresses") {
    auto objects = OddSizedObjects();
    objects[1].symbols = {{"Function", "", 2, 2, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global},
                          {"Constant", "", 0, 1, RCU_RODATA_IDX, RcuSymKind::Const, RcuSymVis::Local},
                          {"External", "", 0, 0, RCU_SEC_EXTERNAL, RcuSymKind::ExternFunc, RcuSymVis::Global},
                          {"OutOfRange", "", 5, 1, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Local}};
    objects[1].sections[RCU_TEXT_IDX].relocs = {{1, 0, RcuRelType::Rel32, 0}, {4, 0, RcuRelType::Rel32, 0}};
    const RcuObjectLayout layout = RcuObjectLayout::Build(objects);

    const auto function = layout.Symbol({1, 0});
    REQUIRE(function == (RcuSectionPlacement{RcuMergedSection::Text, 18}));
    CHECK(layout.Symbol({1, 1}) == (RcuSectionPlacement{RcuMergedSection::RoData, 8}));
    CHECK_FALSE(layout.Symbol({1, 2}).has_value());
    CHECK_FALSE(layout.Symbol({1, 3}).has_value());
    CHECK_FALSE(layout.Symbol({2, 0}).has_value());

    const auto relocation = layout.Relocation(1, RCU_TEXT_IDX, 0);
    REQUIRE(relocation == (RcuSectionPlacement{RcuMergedSection::Text, 17}));
    RcuLinkReference reference;
    reference.objectIndex = 1;
    reference.sectionIndex = RCU_TEXT_IDX;
    reference.relocationIndex = 0;
    CHECK(layout.Relocation(reference) == relocation);
    CHECK_FALSE(layout.Relocation(1, RCU_TEXT_IDX, 1).has_value());
    CHECK_FALSE(layout.Relocation(1, RCU_TEXT_IDX, 2).has_value());
    CHECK_FALSE(layout.Relocation(1, 99, 0).has_value());

    const RcuSectionBases bases{.text = 0x1000, .rodata = 0x2000, .data = 0x3000, .bss = 0x4000};
    CHECK(RcuObjectLayout::Address(*function, bases) == 0x1012);
    CHECK(RcuObjectLayout::Address(*relocation, bases) == 0x1011);
    CHECK_FALSE(RcuObjectLayout::Address({RcuMergedSection::Data, std::numeric_limits<std::uint64_t>::max()}, bases)
                    .has_value());
}

TEST_CASE("RCU object layout maps fixed symbol section ids across omitted physical sections") {
    RcuFile object;
    object.sections = {Section(RcuSecType::Text, 4, {0x11, 0x12, 0x13, 0x14}),
                       Section(RcuSecType::Data, 8, {0x21, 0x22, 0x23, 0x24}),
                       Section(RcuSecType::Bss, 16, std::vector<std::uint8_t>(8))};
    object.symbols = {{"Entry", {}, 0, 4, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global},
                      {"State", {}, 0, 4, RCU_DATA_IDX, RcuSymKind::Data, RcuSymVis::Global},
                      {"Zeroes", {}, 0, 8, RCU_BSS_IDX, RcuSymKind::Data, RcuSymVis::Global}};

    const std::array objects = {std::move(object)};
    const RcuObjectLayout layout = RcuObjectLayout::Build(objects);

    CHECK(layout.Symbol({0, 0}) == (RcuSectionPlacement{RcuMergedSection::Text, 0}));
    CHECK(layout.Symbol({0, 1}) == (RcuSectionPlacement{RcuMergedSection::Data, 0}));
    CHECK(layout.Symbol({0, 2}) == (RcuSectionPlacement{RcuMergedSection::Bss, 0}));
}
