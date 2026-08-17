#include "CodeGen/Layout.h"

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

    std::string encoded;
    encoded.reserve(value.size() * static_cast<std::size_t>(elementSize) + static_cast<std::size_t>(elementSize - 1));
    for (const unsigned char byte : value) {
        encoded.push_back(static_cast<char>(byte));
        encoded.append(static_cast<std::size_t>(elementSize - 1), '\0');
    }
    encoded.append(static_cast<std::size_t>(elementSize - 1), '\0');
    return encoded;
}

int SizeOf(const TypeRef &t) {
    switch (t.kind) {
    case TypeRef::Kind::Bool8: // Bool == Bool8
    case TypeRef::Kind::Char8:
    case TypeRef::Kind::Int8:
    case TypeRef::Kind::UInt8:
        return 1;
    case TypeRef::Kind::Bool16:
    case TypeRef::Kind::Char16:
    case TypeRef::Kind::Int16:
    case TypeRef::Kind::UInt16:
        return 2;
    case TypeRef::Kind::Bool32:
    case TypeRef::Kind::Char32: // Char == Char32
    case TypeRef::Kind::Int32:
    case TypeRef::Kind::UInt32:
    case TypeRef::Kind::Float32:
        return 4;
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
        if (baseName == "String" || baseName == "StringArray" || baseName == "SystemTime") {
            return 16;
        }
        if (baseName == "StringBuilder") {
            return 24;
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
    const int size = SizeOf(t);
    return size > 0 ? std::min(size, 8) : 1;
}

bool IsFloat(const TypeRef &t) {
    return t.kind == TypeRef::Kind::Float32 || t.kind == TypeRef::Kind::Float64;
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

StructLayout ComputeStructLayout(const LirStructDecl &s, const LayoutMap &known) {
    StructLayout result;
    int offset = 0;
    int maxAlign = 1;
    for (const auto &f : s.fields) {
        int sz = SizeOf(f.type);
        int al = AlignOf(f.type);
        if (f.type.kind == TypeRef::Kind::Named) {
            const auto baseName = BaseTypeName(f.type.name);
            if (auto it = known.find(baseName); it != known.end()) {
                sz = it->second.totalSize;
                al = it->second.alignment;
            }
            else if (baseName == "Slice" || baseName.starts_with("Slice<")) {
                sz = 16;
                al = 8;
            }
            else if (baseName == "String" || baseName == "StringArray") {
                sz = 16;
                al = 8;
            }
            else if (baseName == "SystemTime") {
                sz = 16;
                al = 2;
            }
            else if (baseName == "StringBuilder") {
                sz = 24;
                al = 8;
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
    if (!t.IsRange() && t.kind == TypeRef::Kind::Named) {
        const std::string base = BaseTypeName(t.name);
        // An interface value is a data pointer and a vtable pointer, and the
        // runtime aggregates below have a shape the declaring module does not
        // get to change; either answers before a struct layout does, so a user
        // type sharing one of those names does not quietly resize it.
        if (interfaceNames.contains(base)) {
            return 16;
        }
        if (base == "Slice" || base == "String" || base == "StringArray" || base == "SystemTime") {
            return 16;
        }
        if (base == "StringBuilder") {
            return 24;
        }
        if (const auto it = layouts.find(base); it != layouts.end()) {
            return it->second.totalSize;
        }
    }
    return SizeOf(t);
}

int FieldOffsetOf(const TypeRef &pointerType, const std::string_view fieldName, const LayoutMap &layouts,
                  const std::unordered_set<std::string> &interfaceNames) {
    if (pointerType.kind != TypeRef::Kind::Pointer || pointerType.inner.empty()) {
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
    const auto layout = layouts.find(base);
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
