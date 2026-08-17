#include "CodeGen/Layout.h"

#include <doctest.h>
#include <unordered_set>

using namespace Rux;
using namespace Rux::Layout;

namespace {
/// The generic declaration, one instantiation of it at a two-word argument, and the argument itself. A generic struct
/// declares one set of fields for every instantiation, so the declaration's own entry is sized from the unsubstituted
/// parameter and cannot answer for any instantiation.
[[nodiscard]] LayoutMap BoxLayouts() {
    LayoutMap layouts;

    LirStructDecl wide;
    wide.name = "Wide";
    wide.fields = {{"a", TypeRef::MakeInt64()}, {"b", TypeRef::MakeInt64()}};
    layouts["Wide"] = ComputeStructLayout(wide, layouts);

    LirStructDecl generic;
    generic.name = "Box";
    generic.typeParams = {"T"};
    generic.fields = {{"value", TypeRef::MakeTypeParam("T")}};
    layouts["Box"] = ComputeStructLayout(generic, layouts);

    LirStructDecl instantiation;
    instantiation.name = "Box<Wide>";
    instantiation.fields = {{"value", TypeRef::MakeNamed("Wide")}};
    layouts["Box<Wide>"] = ComputeStructLayout(instantiation, layouts);

    return layouts;
}
} // namespace

TEST_CASE("an instantiation is sized by its own layout rather than its declaration's") {
    const LayoutMap layouts = BoxLayouts();

    CHECK_EQ(layouts.at("Box").totalSize, 8);
    CHECK_EQ(layouts.at("Box<Wide>").totalSize, 16);

    // A stack slot is sized by the type the LIR names, and that name carries the arguments.
    CHECK_EQ(RuntimeSizeOf(TypeRef::MakeNamed("Box<Wide>"), layouts, {}), 16);

    // A plain struct's name is its own base name, so the declaration's entry still answers for it.
    CHECK_EQ(RuntimeSizeOf(TypeRef::MakeNamed("Wide"), layouts, {}), 16);
}

TEST_CASE("an instantiation's size reaches the struct and the array holding it") {
    LayoutMap layouts = BoxLayouts();

    LirStructDecl enclosing;
    enclosing.name = "Holder";
    enclosing.fields = {{"first", TypeRef::MakeNamed("Box<Wide>")}, {"second", TypeRef::MakeInt64()}};
    layouts["Holder"] = ComputeStructLayout(enclosing, layouts);

    CHECK_EQ(layouts.at("Holder").totalSize, 24);
    CHECK_EQ(FieldOffsetOf(TypeRef::MakePointer(TypeRef::MakeNamed("Holder")), "second", layouts, {}), 16);
    CHECK_EQ(FieldOffsetOf(TypeRef::MakePointer(TypeRef::MakeNamed("Box<Wide>")), "value", layouts, {}), 0);

    CHECK_EQ(RuntimeSizeOf(TypeRef::MakeArray(TypeRef::MakeNamed("Box<Wide>"), 3), layouts, {}), 48);
}
