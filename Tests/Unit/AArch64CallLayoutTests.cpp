#include "CodeGen/AArch64/CallLayout.h"

#include <doctest.h>
#include <vector>

using namespace Rux;
using namespace Rux::Layout;

namespace {
[[nodiscard]] AArch64CallPlanner PlannerFor(const Target::OS os, const LayoutMap &layouts,
                                            const std::vector<LirStructDecl> &declarations) {
    static const std::unordered_set<std::string> noInterfaces;
    return AArch64CallPlanner(layouts, noInterfaces, declarations, os);
}

[[nodiscard]] AArch64CallPlanner PlannerFor(const Target::OS os, const LayoutMap &layouts) {
    static const std::vector<LirStructDecl> noDeclarations;
    return PlannerFor(os, layouts, noDeclarations);
}

[[nodiscard]] AArch64CallPlanner PlannerFor(const Target::OS os) {
    static const LayoutMap noLayouts;
    return PlannerFor(os, noLayouts);
}
} // namespace

TEST_CASE("AArch64 call planner keeps Apple stack slots exact width") {
    const std::vector<TypeRef> types = {TypeRef::MakeInt64(),  TypeRef::MakeInt64(), TypeRef::MakeInt64(),
                                        TypeRef::MakeInt64(),  TypeRef::MakeInt64(), TypeRef::MakeInt64(),
                                        TypeRef::MakeInt64(),  TypeRef::MakeInt64(), TypeRef::MakeInt8(),
                                        TypeRef::MakeUInt16(), TypeRef::MakeInt32(), TypeRef::MakeInt64()};

    const AArch64CallLayout generic = PlannerFor(Target::OS::Linux).PlanArguments(types);
    const AArch64CallLayout apple = PlannerFor(Target::OS::MacOS).PlanArguments(types);

    REQUIRE_EQ(generic.args.size(), types.size());
    REQUIRE_EQ(apple.args.size(), types.size());
    for (std::size_t index = 8; index < types.size(); ++index) {
        CHECK_EQ(generic.args[index].kind, AArch64ArgumentLocation::Kind::Stack);
        CHECK_EQ(generic.args[index].offset, static_cast<std::int32_t>((index - 8) * 8));
        CHECK_EQ(generic.args[index].bytes, 8);
    }
    CHECK_EQ(generic.areaBytes, 32);

    CHECK_EQ(apple.args[8].offset, 0);
    CHECK_EQ(apple.args[8].bytes, 1);
    CHECK_EQ(apple.args[9].offset, 2);
    CHECK_EQ(apple.args[9].bytes, 2);
    CHECK_EQ(apple.args[10].offset, 4);
    CHECK_EQ(apple.args[10].bytes, 4);
    CHECK_EQ(apple.args[11].offset, 8);
    CHECK_EQ(apple.args[11].bytes, 8);
    CHECK_EQ(apple.areaBytes, 16);
}

TEST_CASE("AArch64 call planner applies target rules to 16-byte values") {
    const TypeRef wide = TypeRef::MakeNamed("Wide");
    LayoutMap layouts;
    layouts["Wide"] = StructLayout{.fields = {}, .totalSize = 16, .alignment = 16};

    const std::vector<TypeRef> types = {TypeRef::MakeInt64(), wide};
    const AArch64CallLayout generic = PlannerFor(Target::OS::FreeBSD, layouts).PlanArguments(types);
    const AArch64CallLayout apple = PlannerFor(Target::OS::MacOS, layouts).PlanArguments(types);

    CHECK_EQ(generic.args[1].kind, AArch64ArgumentLocation::Kind::General);
    CHECK_EQ(generic.args[1].first, 2);
    CHECK_EQ(generic.args[1].count, 2);
    CHECK_EQ(apple.args[1].kind, AArch64ArgumentLocation::Kind::General);
    CHECK_EQ(apple.args[1].first, 1);
    CHECK_EQ(apple.args[1].count, 2);
}

TEST_CASE("AArch64 call planner preserves variadic boundaries across target profiles") {
    // Lowering has applied the C default promotions to the anonymous tail: its
    // source float32 and narrow integer therefore reach this planner as f64 and
    // i32. The boundary decides where those promoted values travel.
    const std::vector<TypeRef> types = {TypeRef::MakeInt64(), TypeRef::MakeFloat64(), TypeRef::MakeFloat64(),
                                        TypeRef::MakeInt32()};

    const AArch64CallLayout generic = PlannerFor(Target::OS::FreeBSD).PlanArguments(types, 2);
    REQUIRE_EQ(generic.args.size(), types.size());
    CHECK_EQ(generic.args[0].kind, AArch64ArgumentLocation::Kind::General);
    CHECK_EQ(generic.args[0].first, 0);
    CHECK_EQ(generic.args[1].kind, AArch64ArgumentLocation::Kind::Vector);
    CHECK_EQ(generic.args[1].first, 0);
    CHECK_EQ(generic.args[2].kind, AArch64ArgumentLocation::Kind::Vector);
    CHECK_EQ(generic.args[2].first, 1);
    CHECK_EQ(generic.args[3].kind, AArch64ArgumentLocation::Kind::General);
    CHECK_EQ(generic.args[3].first, 1);
    CHECK_EQ(generic.areaBytes, 0);

    const AArch64CallLayout apple = PlannerFor(Target::OS::MacOS).PlanArguments(types, 2);
    CHECK_EQ(apple.args[0].kind, AArch64ArgumentLocation::Kind::General);
    CHECK_EQ(apple.args[1].kind, AArch64ArgumentLocation::Kind::Vector);
    CHECK_EQ(apple.args[2].kind, AArch64ArgumentLocation::Kind::Stack);
    CHECK_EQ(apple.args[2].offset, 0);
    CHECK_EQ(apple.args[3].kind, AArch64ArgumentLocation::Kind::Stack);
    CHECK_EQ(apple.args[3].offset, 8);
    CHECK_EQ(apple.areaBytes, 16);

    const AArch64CallLayout windows = PlannerFor(Target::OS::Windows).PlanArguments(types, 2);
    CHECK(windows.windowsVariadic);
    for (std::size_t index = 0; index < types.size(); ++index) {
        CHECK_EQ(windows.args[index].kind, AArch64ArgumentLocation::Kind::Slots);
        CHECK_EQ(windows.args[index].offset, static_cast<std::int32_t>(index * 8));
    }
}

TEST_CASE("Windows AArch64 variadic planning sizes stack tails and by-reference copies") {
    const TypeRef pair = TypeRef::MakeNamed("Pair");
    const TypeRef triple = TypeRef::MakeNamed("Triple");
    LayoutMap layouts;
    layouts["Pair"] = StructLayout{.fields = {}, .totalSize = 16, .alignment = 8};
    layouts["Triple"] = StructLayout{.fields = {}, .totalSize = 24, .alignment = 8};

    std::vector<TypeRef> types(7, TypeRef::MakeInt64());
    types.push_back(pair);
    types.push_back(triple);
    const AArch64CallLayout layout = PlannerFor(Target::OS::Windows, layouts).PlanArguments(types, 1);

    CHECK(layout.windowsVariadic);
    CHECK_EQ(layout.args[7].offset, 56);
    CHECK_EQ(layout.args[7].bytes, 16);
    CHECK_EQ(layout.args[8].offset, 72);
    CHECK_EQ(layout.args[8].bytes, 8);
    CHECK(layout.args[8].byReference);
    CHECK_EQ(layout.args[8].copyOffset, 16);
    CHECK_EQ(layout.args[8].copyBytes, 24);
    CHECK_EQ(layout.areaBytes, 48);
}

TEST_CASE("AArch64 call planner classifies register and indirect results") {
    const TypeRef pair = TypeRef::MakeNamed("Pair");
    const TypeRef triple = TypeRef::MakeNamed("Triple");
    const TypeRef quad = TypeRef::MakeNamed("Quad");
    LayoutMap layouts;
    layouts["Pair"] = StructLayout{.fields = {}, .totalSize = 16, .alignment = 8};
    layouts["Triple"] = StructLayout{.fields = {}, .totalSize = 24, .alignment = 8};
    layouts["Quad"] = StructLayout{.fields = {}, .totalSize = 32, .alignment = 8};
    const std::vector<LirStructDecl> declarations = {
        {.name = "Pair",
         .isPublic = false,
         .typeParams = {},
         .fields = {{"first", TypeRef::MakeInt64()}, {"second", TypeRef::MakeInt64()}}},
        {.name = "Triple",
         .isPublic = false,
         .typeParams = {},
         .fields = {{"first", TypeRef::MakeInt64()},
                    {"second", TypeRef::MakeInt64()},
                    {"third", TypeRef::MakeInt64()}}},
        {.name = "Quad",
         .isPublic = false,
         .typeParams = {},
         .fields = {{"a", TypeRef::MakeFloat64()},
                    {"b", TypeRef::MakeFloat64()},
                    {"c", TypeRef::MakeFloat64()},
                    {"d", TypeRef::MakeFloat64()}}},
    };
    const AArch64CallPlanner planner = PlannerFor(Target::OS::Linux, layouts, declarations);

    const AArch64ArgumentLocation pairResult = planner.PlanResult(pair);
    CHECK_EQ(pairResult.kind, AArch64ArgumentLocation::Kind::General);
    CHECK_EQ(pairResult.count, 2);
    CHECK_FALSE(planner.ReturnsInMemory(pair));

    CHECK(planner.ReturnsInMemory(triple));

    const AArch64ArgumentLocation quadResult = planner.PlanResult(quad);
    CHECK_EQ(quadResult.kind, AArch64ArgumentLocation::Kind::Vector);
    CHECK_EQ(quadResult.count, 4);
    CHECK_EQ(quadResult.memberBytes, 8);
    CHECK_FALSE(planner.ReturnsInMemory(quad));
}
