#include "CodeGen/Layout.h"

#include "Types/PrimitiveCatalog.h"
#include "Unicode/Utf.h"

#include <charconv>

namespace Rux::Layout {
namespace {
/// Offset of tuple element `index`, laid out exactly the way SizeOf lays a tuple out: each element aligned to its own
/// size, capped at a doubleword.
[[nodiscard]] int TupleElementOffset(const TypeRef &tuple, const std::size_t index) {
    int offset = 0;
    for (std::size_t i = 0; i < index; ++i) {
        const int size = SizeOf(tuple.inner[i]);
        offset = AlignUp(offset, size > 0 ? std::min(size, 8) : 1);
        offset += size > 0 ? size : 8;
    }
    const int size = SizeOf(tuple.inner[index]);
    return AlignUp(offset, size > 0 ? std::min(size, 8) : 1);
}
} // namespace

std::string EncodeStringLiteral(const std::string_view value, int elementSize) {
    if (elementSize != 2 && elementSize != 4) {
        elementSize = 1;
    }

    // A literal's value is UTF-8, so the byte-wide encoding is the value itself and a wider one is a real
    // transcoding. Widening each byte on its own would spell a different text: every code point above U+007F would
    // become one bogus code unit per byte of its UTF-8 sequence.
    std::string encoded;
    if (elementSize == 1) {
        encoded = std::string(value);
    }
    else {
        const auto transcoded = elementSize == 2 ? TranscodeUtf8ToUtf16LE(value) : TranscodeUtf8ToUtf32LE(value);
        // The lexer rejects a source file that is not valid UTF-8, and an escape only ever produces well-formed
        // UTF-8, so there is nothing here to transcode from; the empty answer keeps a malformed value from being
        // emitted as text that would read as valid.
        encoded = transcoded.value_or(std::string());
    }
    // Interning writes the last byte of the terminator, so what is appended here is the rest of it: the terminator
    // is one whole code unit however wide the encoding is.
    encoded.append(static_cast<std::size_t>(elementSize - 1), '\0');
    return encoded;
}

int SizeOf(const TypeRef &t) {
    // Every primitive is sized by the catalog, so a width added there needs no case here. The backend compiles for a
    // 64-bit target, which is what a pointer-sized primitive answers at.
    if (const auto primitive = PrimitiveSize(t.kind, 8)) {
        return static_cast<int>(*primitive);
    }
    switch (t.kind) {
    case TypeRef::Kind::Opaque:
        return 0;
    case TypeRef::Kind::Tuple: {
        int offset = 0;
        int maxAlign = 1;
        for (const auto &elem : t.inner) {
            const int sz = SizeOf(elem);
            const int al = sz > 0 ? std::min(sz, 8) : 1;
            if (al > 1) {
                offset = AlignUp(offset, al);
            }
            offset += sz > 0 ? sz : 8;
            maxAlign = std::max(maxAlign, al);
        }
        return AlignUp(offset, maxAlign);
    }
    case TypeRef::Kind::Array:
        if (t.inner.empty() || !t.arrayLength) {
            return 0;
        }
        return SizeOf(t.inner[0]) * static_cast<int>(*t.arrayLength);
    case TypeRef::Kind::Range:
    case TypeRef::Kind::RangeInclusive: {
        const TypeRef &elemType = t.inner.empty() ? TypeRef::MakeInt64() : t.inner[0];
        return 2 * SizeOf(elemType);
    }
    case TypeRef::Kind::RangeFrom:
    case TypeRef::Kind::RangeTo:
    case TypeRef::Kind::RangeToInclusive:
        return t.inner.empty() ? 0 : SizeOf(t.inner[0]);
    case TypeRef::Kind::RangeFull:
        return 0;
    case TypeRef::Kind::Named: {
        const auto baseName = BaseTypeName(t.name);
        if (baseName == "Slice" || baseName.starts_with("Slice<")) {
            return 16;
        }
        if (baseName == "StringArray" || baseName == "SystemTime") {
            return 16;
        }
    }
        if (!t.inner.empty()) {
            return SizeOf(t.inner[0]);
        }
        return 8;
    default:
        return 8; // int, uint, int64, uint64, float64, pointer, str,
        // named, …
    }
}

int AlignOf(const TypeRef &t) {
    if (t.kind == TypeRef::Kind::Array) {
        return t.inner.empty() ? 1 : AlignOf(t.inner[0]);
    }
    // A primitive states its own alignment, which for the wide widths is below its size; everything else takes the
    // record rule's natural alignment, capped at a doubleword.
    if (const auto primitive = PrimitiveAlign(t.kind, 8)) {
        return static_cast<int>(*primitive);
    }
    const int size = SizeOf(t);
    return size > 0 ? std::min(size, 8) : 1;
}

bool IsFloat(const TypeRef &t) {
    // Only the widths a machine floating-point register holds; the software-lowered ones are not register floats.
    return t.kind == TypeRef::Kind::Float32 || t.kind == TypeRef::Kind::Float64;
}

bool IsNativeFloat(const TypeRef &t) {
    return IsFloat(t);
}

bool IsSoftwareFloat(const TypeRef &t) {
    return t.IsFloat() && !IsNativeFloat(t);
}

bool IsWideInteger(const TypeRef &t) {
    return t.IsInteger() && SizeOf(t) > 8;
}

TypeRef UnsignedIntegerType(const TypeRef &type) {
    switch (type.kind) {
    case TypeRef::Kind::Int8:
        return TypeRef::MakeUInt8();
    case TypeRef::Kind::Int16:
        return TypeRef::MakeUInt16();
    case TypeRef::Kind::Int32:
        return TypeRef::MakeUInt32();
    case TypeRef::Kind::Int64:
        return TypeRef::MakeUInt64();
    case TypeRef::Kind::Int:
        return TypeRef::MakeUInt();
    default:
        return type;
    }
}

std::string BaseTypeName(const std::string &name) {
    const std::size_t pos = name.find('<');
    return pos == std::string::npos ? name : name.substr(0, pos);
}

StructLayout ComputeStructLayout(const LirStructDecl &s, const LayoutMap &known,
                                 const std::unordered_set<std::string> &interfaceNames) {
    StructLayout result;
    int offset = 0;
    int maxAlign = 1;
    for (const auto &f : s.fields) {
        int sz = SizeOf(f.type);
        int al = AlignOf(f.type);
        if (f.type.kind == TypeRef::Kind::Named) {
            const auto baseName = BaseTypeName(f.type.name);
            // A generic struct is declared once but laid out once per instantiation, so the instantiation's own entry
            // answers ahead of the declaration's: `Box<int64>` is not the size of `Box<T>`.
            auto it = known.find(f.type.name);
            if (it == known.end()) {
                it = known.find(baseName);
            }
            if (interfaceNames.contains(baseName)) {
                // An interface value is a data pointer and a vtable pointer, under a name that is a declaration
                // rather than a type with a size of its own.
                sz = 16;
                al = 8;
            }
            else if (it != known.end()) {
                sz = it->second.totalSize;
                al = it->second.alignment;
            }
            else if (baseName == "Slice" || baseName.starts_with("Slice<")) {
                sz = 16;
                al = 8;
            }
            else if (baseName == "StringArray") {
                sz = 16;
                al = 8;
            }
            else if (baseName == "SystemTime") {
                sz = 16;
                al = 2;
            }
        }
        if (al > 1) {
            offset = AlignUp(offset, al);
        }
        result.fields.push_back({f.name, offset, sz});
        offset += sz;
        maxAlign = std::max(maxAlign, al);
    }
    result.totalSize = AlignUp(offset, maxAlign);
    result.alignment = maxAlign;
    return result;
}

bool SameLayout(const StructLayout &left, const StructLayout &right) {
    if (left.totalSize != right.totalSize || left.alignment != right.alignment ||
        left.fields.size() != right.fields.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.fields.size(); ++index) {
        if (left.fields[index].offset != right.fields[index].offset ||
            left.fields[index].size != right.fields[index].size) {
            return false;
        }
    }
    return true;
}

void BuildStructLayouts(const std::vector<LirStructDecl> &structs, LayoutMap &layouts,
                        const std::unordered_set<std::string> &interfaceNames) {
    // A struct's field may be a struct declared later -- further down the file, or in a later file of the same
    // package, which is how a package split across files put a type after its user. A single pass in declaration
    // order sized such a field as a word, and everything laid out from that wrote and read the wrong offsets.
    // Each pass resolves one more level of nesting, so the loop runs at most as deep as structs nest by value --
    // and a by-value cycle cannot exist, since a struct cannot contain itself.
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto &s : structs) {
            StructLayout computed = ComputeStructLayout(s, layouts, interfaceNames);
            const auto known = layouts.find(s.name);
            if (known == layouts.end() || !SameLayout(known->second, computed)) {
                layouts[s.name] = std::move(computed);
                changed = true;
            }
        }
    }
}

int RuntimeSizeOf(const TypeRef &t, const LayoutMap &layouts, const std::unordered_set<std::string> &interfaceNames) {
    // A composite's size follows from its elements', and an element may be a struct that only the layout map can size.
    // Recurse here rather than falling through to SizeOf, which has no layout map and charges eight bytes for every
    // struct it cannot measure: an array would then reserve less stack than it occupies and overrun its own frame.
    if (t.kind == TypeRef::Kind::Array) {
        if (t.inner.empty() || !t.arrayLength) {
            return 0;
        }
        return RuntimeSizeOf(t.inner[0], layouts, interfaceNames) * static_cast<int>(*t.arrayLength);
    }
    if (t.kind == TypeRef::Kind::Tuple) {
        int offset = 0;
        int maxAlign = 1;
        for (const auto &element : t.inner) {
            const int size = RuntimeSizeOf(element, layouts, interfaceNames);
            const int alignment = size > 0 ? std::min(size, 8) : 1;
            if (alignment > 1) {
                offset = AlignUp(offset, alignment);
            }
            offset += size > 0 ? size : 8;
            maxAlign = std::max(maxAlign, alignment);
        }
        return AlignUp(offset, maxAlign);
    }
    if (t.kind == TypeRef::Kind::Reference && !t.inner.empty() && t.inner.front().kind == TypeRef::Kind::Named &&
        interfaceNames.contains(BaseTypeName(t.inner.front().name))) {
        return 16;
    }
    if (!t.IsRange() && t.kind == TypeRef::Kind::Named) {
        const std::string base = BaseTypeName(t.name);
        // An interface value is a data pointer and a vtable pointer, and the
        // runtime aggregates below have a shape the declaring module does not
        // get to change; either answers before a struct layout does, so a user
        // type sharing one of those names does not quietly resize it.
        if (interfaceNames.contains(base)) {
            return 16;
        }
        if (base == "Slice" || base == "StringArray" || base == "SystemTime") {
            return 16;
        }
        // An instantiation is sized by its own entry when it has one; the declaration's entry still answers for a plain
        // struct, whose name is its base name.
        if (const auto it = layouts.find(t.name); it != layouts.end()) {
            return it->second.totalSize;
        }
        if (const auto it = layouts.find(base); it != layouts.end()) {
            return it->second.totalSize;
        }
    }
    return SizeOf(t);
}

int FieldOffsetOf(const TypeRef &pointerType, const std::string_view fieldName, const LayoutMap &layouts,
                  const std::unordered_set<std::string> &interfaceNames) {
    if ((pointerType.kind != TypeRef::Kind::Pointer && pointerType.kind != TypeRef::Kind::Reference) ||
        pointerType.inner.empty()) {
        return 0;
    }
    const TypeRef &pointee = pointerType.inner[0];

    // A range is its bounds in declaration order, so `end` follows `start`
    // wherever there is a `start` to follow and sits at the front otherwise.
    if (pointee.IsRange()) {
        if (fieldName != "end" || !pointee.RangeHasEnd() || !pointee.RangeHasStart()) {
            return 0;
        }
        return pointee.inner.empty() ? SizeOf(TypeRef::MakeInt64()) : SizeOf(pointee.inner[0]);
    }

    if (pointee.kind == TypeRef::Kind::Tuple) {
        std::size_t index = 0;
        const char *first = fieldName.data();
        const char *last = first + fieldName.size();
        const auto [stopped, ec] = std::from_chars(first, last, index);
        if (ec != std::errc{} || stopped != last || index >= pointee.inner.size()) {
            return 0;
        }
        return TupleElementOffset(pointee, index);
    }

    // A string is the same {data, length} pair a slice is, under a primitive kind rather than a declared name.
    if (pointee.IsString()) {
        return fieldName == "length" ? 8 : 0;
    }

    if (pointee.kind != TypeRef::Kind::Named) {
        return 0;
    }
    const std::string base = BaseTypeName(pointee.name);
    // An interface value and a slice are both a pointer and a word beside it,
    // under names the runtime fixes rather than a declaration.
    if (interfaceNames.contains(base)) {
        return fieldName == "vtable" ? 8 : 0;
    }
    if (base == "Slice") {
        return fieldName == "length" ? 8 : 0;
    }
    auto layout = layouts.find(pointee.name);
    if (layout == layouts.end()) {
        layout = layouts.find(base);
    }
    if (layout == layouts.end()) {
        return 0;
    }
    for (const auto &field : layout->second.fields) {
        if (field.name == fieldName) {
            return field.offset;
        }
    }
    return 0;
}
} // namespace Rux::Layout
