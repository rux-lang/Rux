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

TEST_CASE("garbage collection drops unreached functions and data and keeps what the roots reach") {
    RcuFile object;
    RcuSection text;
    text.name = ".text";
    text.type = RcuSecType::Text;
    text.flags = RcuSecFlag::Alloc | RcuSecFlag::Exec | RcuSecFlag::Read;
    text.alignment = 16;
    // Main [0, 16) calls Used and reads Pointer and the function table; Dead [16, 24) is called by nothing; Used
    // [32, 40); Pointed [48, 56) is only named by Pointer.
    text.data.assign(56, 0x90);
    text.relocs.push_back({1, 2, RcuRelType::Rel32, -4});
    text.relocs.push_back({6, 5, RcuRelType::Rel32, -4});
    text.relocs.push_back({11, 4, RcuRelType::Rel32, -4});
    RcuSection rodata;
    rodata.name = ".rodata";
    rodata.type = RcuSecType::RoData;
    rodata.flags = RcuSecFlag::Alloc | RcuSecFlag::Read;
    rodata.alignment = 8;
    // A stack-trace table naming Dead and Used, a pointer to Pointed, a constant nothing reads that points at Dead,
    // and a literal under a label of no size.
    rodata.data.assign(36, 0);
    rodata.data[32] = 'a';
    rodata.relocs.push_back({0, 1, RcuRelType::Abs64, 0});
    rodata.relocs.push_back({8, 2, RcuRelType::Abs64, 0});
    rodata.relocs.push_back({16, 3, RcuRelType::Abs64, 0});
    rodata.relocs.push_back({24, 1, RcuRelType::Abs64, 0});
    object.sections.push_back(std::move(text));
    object.sections.push_back(std::move(rodata));
    object.symbols.push_back({"Main", "", 0, 16, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Global});
    object.symbols.push_back({"Dead", "", 16, 8, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Local});
    object.symbols.push_back({"Used", "", 32, 8, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Local});
    object.symbols.push_back({"Pointed", "", 48, 8, RCU_TEXT_IDX, RcuSymKind::Func, RcuSymVis::Local});
    object.symbols.push_back(
        {std::string(kFunctionTablePrefix) + "0", "", 0, 16, RCU_RODATA_IDX, RcuSymKind::Const, RcuSymVis::Global});
    object.symbols.push_back({"Pointer", "", 16, 8, RCU_RODATA_IDX, RcuSymKind::Const, RcuSymVis::Local});
    object.symbols.push_back({"Unread", "", 24, 8, RCU_RODATA_IDX, RcuSymKind::Const, RcuSymVis::Global});
    object.symbols.push_back({"__str0", "", 32, 0, RCU_RODATA_IDX, RcuSymKind::Const, RcuSymVis::Local});

    std::vector<RcuFile> objects{object};
    const auto graph = RcuLinkGraph::Build(objects, "Test", ArtifactKind::Executable, Target::Arch::X86_64);
    REQUIRE_FALSE(graph.HasErrors());
    REQUIRE(CollectUnreachable(objects, graph, 0xCC));

    const RcuFile &kept = objects.front();
    CHECK(Find(kept, "Dead") == nullptr);
    CHECK(Find(kept, "Unread") == nullptr);
    const RcuSymbol *main = Find(kept, "Main");
    const RcuSymbol *used = Find(kept, "Used");
    const RcuSymbol *pointed = Find(kept, "Pointed");
    const RcuSymbol *pointer = Find(kept, "Pointer");
    const RcuSymbol *literal = Find(kept, "__str0");
    REQUIRE(main != nullptr);
    REQUIRE(used != nullptr);
    REQUIRE(pointed != nullptr);
    REQUIRE(pointer != nullptr);
    REQUIRE(literal != nullptr);
    // Each kept atom sits at its old offset modulo sixteen, after the one before it.
    CHECK(main->value == 0);
    CHECK(used->value == 16);
    CHECK(pointed->value == 32);
    CHECK(pointer->value == 16);
    CHECK(literal->value == 32);
    const RcuSection &text2 = kept.sections[RCU_TEXT_IDX];
    CHECK(text2.data.size() == 40);
    CHECK(text2.data[24] == 0xCC);
    REQUIRE(text2.relocs.size() == 3);
    CHECK(kept.symbols[text2.relocs[0].symbolIndex].name == "Used");
    // The table keeps its entry for Used; the one for Dead is gone and its slot stays zero. The unread constant and
    // its pointer to Dead are gone; the literal stays.
    const RcuSection &rodata2 = kept.sections[RCU_RODATA_IDX];
    CHECK(rodata2.data.size() == 36);
    CHECK(rodata2.data[32] == 'a');
    REQUIRE(rodata2.relocs.size() == 2);
    CHECK(rodata2.relocs[0].sectionOffset == 8);
    CHECK(kept.symbols[rodata2.relocs[0].symbolIndex].name == "Used");
    CHECK(rodata2.relocs[1].sectionOffset == 16);
    CHECK(kept.symbols[rodata2.relocs[1].symbolIndex].name == "Pointed");

    // A second pass over what is left finds nothing more to drop.
    const auto again = RcuLinkGraph::Build(objects, "Test", ArtifactKind::Executable, Target::Arch::X86_64);
    REQUIRE_FALSE(again.HasErrors());
    CHECK_FALSE(CollectUnreachable(objects, again, 0xCC));
}
