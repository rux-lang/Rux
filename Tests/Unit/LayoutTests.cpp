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
    CHECK_EQ(FieldOffsetOf(TypeRef::MakeReference(TypeRef::MakeNamed("Holder")), "second", layouts, {}), 16);
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

TEST_CASE("a wider string encoding is transcoded rather than widened byte by byte") {
    // ASCII, a two-byte sequence, a three-byte one, and a supplementary code point that needs a surrogate pair:
    // 'A', U+00A2, U+20AC, U+1F680.
    const std::string_view mixed = "A\xC2\xA2\xE2\x82\xAC\xF0\x9F\x9A\x80";

    // UTF-8 is the value itself, and every encoding leaves room for all but the last byte of the terminator that
    // interning writes.
    CHECK_EQ(EncodeStringLiteral(mixed, 1), std::string(mixed));

    CHECK_EQ(EncodeStringLiteral(mixed, 2), std::string("\x41\x00\xA2\x00\xAC\x20\x3D\xD8\x80\xDE\x00", 11));
    CHECK_EQ(EncodeStringLiteral(mixed, 4), std::string("\x41\x00\x00\x00"
                                                        "\xA2\x00\x00\x00"
                                                        "\xAC\x20\x00\x00"
                                                        "\x80\xF6\x01\x00"
                                                        "\x00\x00\x00",
                                                        19));

    // An unrecognized element size is the byte encoding, as it always was.
    CHECK_EQ(EncodeStringLiteral(mixed, 3), std::string(mixed));
}

TEST_CASE("a string is the same sixteen-byte view a slice is") {
    for (const TypeRef::Kind kind : {TypeRef::Kind::String8, TypeRef::Kind::String16, TypeRef::Kind::String32}) {
        const TypeRef text = TypeRef::MakePrimitive(kind);
        CAPTURE(text.ToString());
        CHECK_EQ(SizeOf(text), 16);
        CHECK_EQ(AlignOf(text), 8);
        CHECK_EQ(RuntimeSizeOf(text, {}, interfaceNames), 16);

        const TypeRef pointer = TypeRef::MakePointer(text);
        CHECK_EQ(FieldOffsetOf(pointer, "data", {}, interfaceNames), 0);
        CHECK_EQ(FieldOffsetOf(pointer, "length", {}, interfaceNames), 8);
    }
}

TEST_CASE("a string field is laid out like the slice it shares a shape with") {
    LirStructDecl header;
    header.name = "Header";
    header.fields.push_back({"tag", TypeRef::MakeInt32()});
    header.fields.push_back({"text", TypeRef::MakeString8()});
    header.fields.push_back({"trailing", TypeRef::MakeInt32()});

    const StructLayout layout = ComputeStructLayout(header, {}, interfaceNames);
    REQUIRE_EQ(layout.fields.size(), 3);
    CHECK_EQ(layout.fields[0].offset, 0);
    CHECK_EQ(layout.fields[1].offset, 8);
    CHECK_EQ(layout.fields[1].size, 16);
    CHECK_EQ(layout.fields[2].offset, 24);
    CHECK_EQ(layout.alignment, 8);
    CHECK_EQ(layout.totalSize, 32);
}
