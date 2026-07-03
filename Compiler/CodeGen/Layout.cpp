#include "CodeGen/Layout.h"

namespace Rux::Layout {

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
    case TypeRef::Kind::Named:
        if (!t.inner.empty()) {
            return SizeOf(t.inner[0]);
        }
        if (t.name == "Slice" || t.name.starts_with("Slice<")) {
            return 16;
        }
        return 8;
    default:
        return 8; // int, uint, int64, uint64, float64, pointer, str,
                  // named, …
    }
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
        int al = sz > 0 ? std::min(sz, 8) : 1;
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
