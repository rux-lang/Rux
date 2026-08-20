#include "CodeGen/Layout.h"

#include <doctest.h>
#include <unordered_set>

using namespace Rux;
using namespace Rux::Layout;

namespace {
/// No interfaces are in scope for these layouts; the cases that need one name it explicitly.
const std::unordered_set<std::string> interfaceNames;

/// The generic declaration, one instantiation of it at a two-word argument, and the argument itself. A generic struct
/// declares one set of fields for every instantiation, so the declaration's own entry is sized from the unsubstituted
/// parameter and cannot answer for any instantiation.
[[nodiscard]] LayoutMap BoxLayouts() {
    LayoutMap layouts;

    LirStructDecl wide;
    wide.name = "Wide";
    wide.fields = {{"a", TypeRef::MakeInt64()}, {"b", TypeRef::MakeInt64()}};
    layouts["Wide"] = ComputeStructLayout(wide, layouts, interfaceNames);

    LirStructDecl generic;
    generic.name = "Box";
    generic.typeParams = {"T"};
    generic.fields = {{"value", TypeRef::MakeTypeParam("T")}};
    layouts["Box"] = ComputeStructLayout(generic, layouts, interfaceNames);

    LirStructDecl instantiation;
    instantiation.name = "Box<Wide>";
    instantiation.fields = {{"value", TypeRef::MakeNamed("Wide")}};
    layouts["Box<Wide>"] = ComputeStructLayout(instantiation, layouts, interfaceNames);

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
    layouts["Holder"] = ComputeStructLayout(enclosing, layouts, interfaceNames);

    CHECK_EQ(layouts.at("Holder").totalSize, 24);
    CHECK_EQ(FieldOffsetOf(TypeRef::MakePointer(TypeRef::MakeNamed("Holder")), "second", layouts, {}), 16);
    CHECK_EQ(FieldOffsetOf(TypeRef::MakePointer(TypeRef::MakeNamed("Box<Wide>")), "value", layouts, {}), 0);

    CHECK_EQ(RuntimeSizeOf(TypeRef::MakeArray(TypeRef::MakeNamed("Box<Wide>"), 3), layouts, {}), 48);
}

TEST_CASE("an interface-typed field occupies a fat pointer rather than one word") {
    // An interface value is a data pointer and a vtable pointer under a name the declaration gives no size for.
    // Sized as an ordinary named type it took eight bytes, and the vtable word landed on whatever followed it --
    // which made a call through an interface stored in a struct jump into the data pointer.
    const std::unordered_set<std::string> interfaces{"Allocator"};
    LayoutMap layouts;

    LirStructDecl holder;
    holder.name = "Holder";
    holder.fields = {{"pointer", TypeRef::MakePointer(TypeRef::MakeInt64())},
                     {"allocator", TypeRef::MakeNamed("Allocator")},
                     {"tag", TypeRef::MakeInt64()}};
    const StructLayout layout = ComputeStructLayout(holder, layouts, interfaces);

    REQUIRE_EQ(layout.fields.size(), 3);
    CHECK_EQ(layout.fields[0].offset, 0);
    CHECK_EQ(layout.fields[1].offset, 8);
    CHECK_EQ(layout.fields[1].size, 16);
    // The field after it starts past both words, which is what the old sizing got wrong.
    CHECK_EQ(layout.fields[2].offset, 24);
    CHECK_EQ(layout.totalSize, 32);
}
