#include "Semantic/Type.h"

#include "Target/Layout.h"

namespace Rux {
// TypeRef implementation
bool TypeRef::IsNumeric() const noexcept {
    switch (kind) {
    case Kind::Int8:
    case Kind::Int16:
    case Kind::Int32:
    case Kind::Int64:
    case Kind::Int:
    case Kind::UInt8:
    case Kind::UInt16:
    case Kind::UInt32:
    case Kind::UInt64:
    case Kind::UInt:
    case Kind::Float32:
    case Kind::Float64:
        return true;
    default:
        return false;
    }
}

bool TypeRef::IsInteger() const noexcept {
    switch (kind) {
    case Kind::Int8:
    case Kind::Int16:
    case Kind::Int32:
    case Kind::Int64:
    case Kind::Int:
    case Kind::UInt8:
    case Kind::UInt16:
    case Kind::UInt32:
    case Kind::UInt64:
    case Kind::UInt:
        return true;
    default:
        return false;
    }
}

bool TypeRef::IsFloat() const noexcept {
    return kind == Kind::Float32 || kind == Kind::Float64;
}

bool TypeRef::IsSigned() const noexcept {
    switch (kind) {
    case Kind::Int8:
    case Kind::Int16:
    case Kind::Int32:
    case Kind::Int64:
    case Kind::Int:
        return true;
    default:
        return false;
    }
}

bool TypeRef::IsAssignableTo(const TypeRef &other) const noexcept {
    if (IsUnknown() || other.IsUnknown()) {
        return true;
    }
    if (*this == other) {
        return true;
    }
    // float32 widens implicitly to float64 / float (safe, no precision loss
    // in range)
    if (kind == Kind::Float32 && other.kind == Kind::Float64) {
        return true;
    }
    // char widens implicitly: char8 → char16, char8/char16 → char32
    if (kind == Kind::Char8 && (other.kind == Kind::Char16 || other.kind == Kind::Char32)) {
        return true;
    }
    if (kind == Kind::Char16 && other.kind == Kind::Char32) {
        return true;
    }
    // int/uint interoperate with their fixed-width platform equivalents
    // (x64: 64-bit)
    if (kind == Kind::Int64 && other.kind == Kind::Int) {
        return true;
    }
    if (kind == Kind::Int && other.kind == Kind::Int64) {
        return true;
    }
    if (kind == Kind::UInt64 && other.kind == Kind::UInt) {
        return true;
    }
    if (kind == Kind::UInt && other.kind == Kind::UInt64) {
        return true;
    }
    // smaller fixed-width integers widen implicitly to int/uint
    if (other.kind == Kind::Int && (kind == Kind::Int8 || kind == Kind::Int16 || kind == Kind::Int32)) {
        return true;
    }
    if (other.kind == Kind::UInt && (kind == Kind::UInt8 || kind == Kind::UInt16 || kind == Kind::UInt32)) {
        return true;
    }
    // smaller fixed-width signed integers widen to larger signed integers
    if (other.kind == Kind::Int64 && (kind == Kind::Int8 || kind == Kind::Int16 || kind == Kind::Int32)) {
        return true;
    }
    if (other.kind == Kind::Int32 && (kind == Kind::Int8 || kind == Kind::Int16)) {
        return true;
    }
    if (other.kind == Kind::Int16 && kind == Kind::Int8) {
        return true;
    }
    // smaller fixed-width unsigned integers widen to larger unsigned
    // integers
    if (other.kind == Kind::UInt64 && (kind == Kind::UInt8 || kind == Kind::UInt16 || kind == Kind::UInt32)) {
        return true;
    }
    if (other.kind == Kind::UInt32 && (kind == Kind::UInt8 || kind == Kind::UInt16)) {
        return true;
    }
    if (other.kind == Kind::UInt16 && kind == Kind::UInt8) {
        return true;
    }
    // Numeric types must match exactly unless an explicit cast is used.
    if (IsNumeric() && other.IsNumeric()) {
        return false;
    }
    // Bool types are mutually assignable across widths
    if (IsBool() && other.IsBool()) {
        return true;
    }
    // Any pointer is implicitly assignable to *opaque (like void* in C)
    if (kind == Kind::Pointer && other.kind == Kind::Pointer && !other.inner.empty() && other.inner[0].IsOpaque()) {
        return true;
    }
    return false;
}

std::optional<std::uint64_t> TypeRef::SizeInBytes() const noexcept {
    switch (kind) {
    case Kind::Unknown:
    case Kind::TypeParam:
        return std::nullopt;
    case Kind::Opaque:
        return 0;
    case Kind::Bool8:
    case Kind::Char8:
    case Kind::Int8:
    case Kind::UInt8:
        return 1;
    case Kind::Bool16:
    case Kind::Char16:
    case Kind::Int16:
    case Kind::UInt16:
        return 2;
    case Kind::Bool32:
    case Kind::Char32:
    case Kind::Int32:
    case Kind::UInt32:
    case Kind::Float32:
        return 4;
    case Kind::Int64:
    case Kind::UInt64:
    case Kind::Int:
    case Kind::UInt:
    case Kind::Float64:
    case Kind::Pointer:
    case Kind::Str:
    case Kind::Func:
        return 8;
    case Kind::Slice:
        return 16;
    case Kind::Range: {
        if (inner.empty()) {
            return std::nullopt;
        }
        const auto elemSize = inner[0].SizeInBytes();
        if (!elemSize || *elemSize == 0) {
            return std::nullopt;
        }
        return Layout::AlignUp(2 * *elemSize + 1, *elemSize);
    }
    case Kind::Tuple: {
        const auto layout = Layout::FieldsSizeAndAlign(inner, [](const TypeRef &elem) { return elem.SizeInBytes(); });
        if (!layout) {
            return std::nullopt;
        }
        return layout->first;
    }
    case Kind::Named:
        if (name.starts_with("Slice<") || name == "Slice") {
            return 16;
        }
        if (!inner.empty()) {
            return inner[0].SizeInBytes();
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::string TypeRef::ToString() const {
    switch (kind) {
    case Kind::Unknown:
        return "?";
    case Kind::Opaque:
        return "opaque";
    case Kind::Bool8:
        return "bool8";
    case Kind::Bool16:
        return "bool16";
    case Kind::Bool32:
        return "bool32";
    case Kind::Char8:
        return "char8";
    case Kind::Char16:
        return "char16";
    case Kind::Char32:
        return "char32";
    case Kind::Str:
        return "String";
    case Kind::Int8:
        return "int8";
    case Kind::Int16:
        return "int16";
    case Kind::Int32:
        return "int32";
    case Kind::Int64:
        return "int64";
    case Kind::Int:
        return "int";
    case Kind::UInt8:
        return "uint8";
    case Kind::UInt16:
        return "uint16";
    case Kind::UInt32:
        return "uint32";
    case Kind::UInt64:
        return "uint64";
    case Kind::UInt:
        return "uint";
    case Kind::Float32:
        return "float32";
    case Kind::Float64:
        return "float64";
    case Kind::Named:
        return name;
    case Kind::TypeParam:
        return name;
    case Kind::Pointer:
        return "*" + (inner.empty() ? "?" : inner[0].ToString());
    case Kind::Slice:
        return (inner.empty() ? "?" : inner[0].ToString()) + "[]";
    case Kind::Range:
        return "Range<" + (inner.empty() ? "?" : inner[0].ToString()) + ">";
    case Kind::Tuple: {
        std::string s = "(";
        for (std::size_t i = 0; i < inner.size(); ++i) {
            if (i) {
                s += ", ";
            }
            s += inner[i].ToString();
        }
        return s + ")";
    }
    case Kind::Func: {
        std::string s = "func(";
        for (std::size_t i = 0; i + 1 < inner.size(); ++i) {
            if (i) {
                s += ", ";
            }
            s += inner[i].ToString();
        }
        s += ") -> ";
        s += inner.empty() ? "opaque" : inner.back().ToString();
        return s;
    }
    }
    return "?";
}

bool TypeRef::operator==(const TypeRef &o) const noexcept {
    if (kind != o.kind || name != o.name || inner.size() != o.inner.size()) {
        return false;
    }
    for (std::size_t i = 0; i < inner.size(); ++i) {
        if (inner[i] != o.inner[i]) {
            return false;
        }
    }
    return true;
}
} // namespace Rux
