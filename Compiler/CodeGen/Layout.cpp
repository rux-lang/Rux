#include "CodeGen/Layout.h"

namespace Rux::Layout {
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
} // namespace Rux::Layout
