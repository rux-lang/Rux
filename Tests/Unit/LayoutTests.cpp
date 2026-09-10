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

TEST_CASE("tuple projections preserve concrete aggregate widths and alignments") {
    LayoutMap layouts = BoxLayouts();
    const TypeRef wide = TypeRef::MakeNamed("Box<Wide>");
    const TypeRef tuple = TypeRef::MakeTuple({wide, wide, TypeRef::MakeInt32()});
    const TypeRef pointer = TypeRef::MakePointer(tuple);
    CHECK_EQ(RuntimeSizeOf(tuple, layouts, {}), 40);
    CHECK_EQ(FieldOffsetOf(pointer, "0", layouts, {}), 0);
    CHECK_EQ(FieldOffsetOf(pointer, "1", layouts, {}), 16);
    CHECK_EQ(FieldOffsetOf(pointer, "2", layouts, {}), 32);
    LirStructDecl bytes;
    bytes.name = "Bytes";
    bytes.fields = {{"data", TypeRef::MakeArray(TypeRef::MakeChar8(), 9)}};
    layouts[bytes.name] = ComputeStructLayout(bytes, layouts, {});
    const TypeRef unaligned = TypeRef::MakeTuple({TypeRef::MakeChar8(), TypeRef::MakeNamed("Bytes"), tuple});
    const TypeRef unalignedPointer = TypeRef::MakePointer(unaligned);
    CHECK_EQ(FieldOffsetOf(unalignedPointer, "1", layouts, {}), 1);
    CHECK_EQ(FieldOffsetOf(unalignedPointer, "2", layouts, {}), 16);
    CHECK_EQ(RuntimeSizeOf(unaligned, layouts, {}), 56);
    LirStructDecl holder;
    holder.name = "Holder";
    holder.fields = {{"tuple", tuple}, {"array", TypeRef::MakeArray(wide, 2)}, {"tail", TypeRef::MakeInt64()}};
    const StructLayout held = ComputeStructLayout(holder, layouts, {});
    REQUIRE_EQ(held.fields.size(), 3);
    CHECK_EQ(held.fields[1].offset, 40);
    CHECK_EQ(held.fields[2].offset, 72);
    CHECK_EQ(held.totalSize, 80);
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

TEST_CASE("text in every encoding is the sixteen-byte view a slice is") {
    for (const TypeRef::Kind unit : {TypeRef::Kind::Char8, TypeRef::Kind::Char16, TypeRef::Kind::Char32}) {
        const TypeRef text = TypeRef::MakeText(unit);
        CAPTURE(text.ToString());
        CHECK_EQ(SizeOf(text), 16);
        CHECK_EQ(AlignOf(text), 8);
        CHECK_EQ(RuntimeSizeOf(text, {}, interfaceNames), 16);

        const TypeRef pointer = TypeRef::MakePointer(text);
        CHECK_EQ(FieldOffsetOf(pointer, "data", {}, interfaceNames), 0);
        CHECK_EQ(FieldOffsetOf(pointer, "length", {}, interfaceNames), 8);
    }
}

TEST_CASE("a text field is laid out like any other slice field") {
    LirStructDecl header;
    header.name = "Header";
    header.fields.push_back({"tag", TypeRef::MakeInt32()});
    header.fields.push_back({"text", TypeRef::MakeText(TypeRef::Kind::Char8)});
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

TEST_CASE("slice writability does not change layout or enclosing field offsets") {
    for (const bool writable : {false, true}) {
        for (const TypeRef &element :
             {TypeRef::MakeInt32(), TypeRef::MakeInt64(), TypeRef::MakeText(TypeRef::Kind::Char16)}) {
            const TypeRef slice = TypeRef::MakeSlice(element, writable);
            CAPTURE(slice.ToString());
            CHECK_EQ(SizeOf(slice), 16);
            CHECK_EQ(AlignOf(slice), 8);
            CHECK_EQ(RuntimeSizeOf(slice, {}, interfaceNames), 16);
            CHECK_EQ(FieldOffsetOf(TypeRef::MakePointer(slice), "data", {}, interfaceNames), 0);
            CHECK_EQ(FieldOffsetOf(TypeRef::MakePointer(slice), "length", {}, interfaceNames), 8);
            LirStructDecl holder;
            holder.name = "Views";
            holder.fields = {{"first", slice}, {"second", slice}, {"count", TypeRef::MakeInt32()}};
            const StructLayout layout = ComputeStructLayout(holder, {}, interfaceNames);
            REQUIRE_EQ(layout.fields.size(), 3);
            CHECK_EQ(layout.fields[1].offset, 16);
            CHECK_EQ(layout.fields[2].offset, 32);
            CHECK_EQ(layout.totalSize, 40);
        }
    }
}
