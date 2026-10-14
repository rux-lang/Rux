#include "Linker/RcuGarbageCollection.h"
#include "Linker/RcuLinkGraph.h"

#include <algorithm>
#include <cstdint>
#include <doctest.h>
#include <string>
#include <vector>

using namespace Rux;

namespace {
const RcuSymbol *Find(const RcuFile &object, const std::string &name) {
    const auto found = std::ranges::find(object.symbols, name, &RcuSymbol::name);
    return found == object.symbols.end() ? nullptr : &*found;
}
} // namespace

TEST_CASE("garbage collection drops unreached functions and keeps what the roots and data reach") {
    RcuFile object;
    RcuSection text;
    text.name = ".text";
    text.type = RcuSecType::Text;
    text.flags = RcuSecFlag::Alloc | RcuSecFlag::Exec | RcuSecFlag::Read;
    text.alignment = 16;
    // Main [0, 8) calls Used; Dead [16, 24) is called by nothing; Used [32, 40); Pointed [48, 56) is only named by
    // data.
    text.data.assign(56, 0x90);
    text.data[0] = 0xE8;
    text.relocs.push_back({1, 2, RcuRelType::Rel32, -4});
    RcuSection rodata;
    rodata.name = ".rodata";
    rodata.type = RcuSecType::RoData;
    rodata.flags = RcuSecFlag::Alloc | RcuSecFlag::Read;
    rodata.alignment = 8;
    // A stack-trace table naming Dead and Used, then a pointer to Pointed.
    rodata.data.assign(24, 0);
    rodata.relocs.push_back({0, 1, RcuRelType::Abs64, 0});
    rodata.relocs.push_back({8, 2, RcuRelType::Abs64, 0});
    rodata.relocs.push_back({16, 3, RcuRelType::Abs64, 0});
    object.sections.push_back(std::move(text));
    object.sections.push_back(std::move(rodata));
    object.symbols.push_back({"Main", "", 0, 8, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});
    object.symbols.push_back({"Dead", "", 16, 8, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Local});
    object.symbols.push_back({"Used", "", 32, 8, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Local});
    object.symbols.push_back({"Pointed", "", 48, 8, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Local});
    object.symbols.push_back(
        {std::string(kFunctionTablePrefix) + "0", "", 0, 16, RCU_RODATA_IDX, RcuSymKind::Const, RcuSymVis::Global});
    object.symbols.push_back({"Pointer", "", 16, 8, RCU_RODATA_IDX, RcuSymKind::Const, RcuSymVis::Local});

    std::vector<RcuFile> objects{object};
    const auto graph = RcuLinkGraph::Build(objects, "Test", ArtifactKind::Executable, Target::Arch::X86_64);
    REQUIRE_FALSE(graph.HasErrors());
    REQUIRE(CollectUnreachableFunctions(objects, graph, 0xCC));

    const RcuFile &kept = objects.front();
    CHECK(Find(kept, "Dead") == nullptr);
    const RcuSymbol *main = Find(kept, "Main");
    const RcuSymbol *used = Find(kept, "Used");
    const RcuSymbol *pointed = Find(kept, "Pointed");
    REQUIRE(main != nullptr);
    REQUIRE(used != nullptr);
    REQUIRE(pointed != nullptr);
    // Each kept function sits at its old offset modulo sixteen, after the one before it.
    CHECK(main->value == 0);
    CHECK(used->value == 16);
    CHECK(pointed->value == 32);
    const RcuSection &text2 = kept.sections[RCU_TEXT_IDX];
    CHECK(text2.data.size() == 40);
    CHECK(text2.data[8] == 0xCC);
    REQUIRE(text2.relocs.size() == 1);
    CHECK(kept.symbols[text2.relocs[0].symbolIndex].name == "Used");
    // The table keeps its entry for Used; the one for Dead is gone and its slot stays zero.
    const RcuSection &rodata2 = kept.sections[RCU_RODATA_IDX];
    REQUIRE(rodata2.relocs.size() == 2);
    CHECK(rodata2.relocs[0].sectionOffset == 8);
    CHECK(kept.symbols[rodata2.relocs[0].symbolIndex].name == "Used");
    CHECK(kept.symbols[rodata2.relocs[1].symbolIndex].name == "Pointed");

    // A second pass over what is left finds nothing more to drop.
    const auto again = RcuLinkGraph::Build(objects, "Test", ArtifactKind::Executable, Target::Arch::X86_64);
    REQUIRE_FALSE(again.HasErrors());
    CHECK_FALSE(CollectUnreachableFunctions(objects, again, 0xCC));
}
