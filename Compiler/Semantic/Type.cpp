#include "Semantic/Type.h"

#include "Semantic/PrimitiveCatalog.h"
#include "Target/Layout.h"

namespace Rux {
namespace {
/// The size a type of unknown target has. `TypeRef` is target-agnostic, so a pointer-sized primitive answers for the
/// 64-bit targets the compiler is hosted and tested on; a caller that has a target reads `PrimitiveSize` directly.
constexpr std::uint32_t DefaultPointerSize = 8;

/// @return the family `kind` belongs to, or nullopt when `kind` is not a primitive
std::optional<PrimitiveCategory> CategoryOf(const TypeRef::Kind kind) noexcept {
    const PrimitiveInfo *info = FindPrimitive(kind);
    return info ? std::optional{info->category} : std::nullopt;
}

bool IsCategory(const TypeRef::Kind kind, const PrimitiveCategory category) noexcept {
    return CategoryOf(kind) == category;
}
} // namespace

// TypeRef implementation
bool TypeRef::IsBool() const noexcept {
    return IsCategory(kind, PrimitiveCategory::Bool);
}

bool TypeRef::IsChar() const noexcept {
    return IsCategory(kind, PrimitiveCategory::Char);
}

bool TypeRef::IsNumeric() const noexcept {
    return IsInteger() || IsFloat();
}

bool TypeRef::IsInteger() const noexcept {
    const auto category = CategoryOf(kind);
    return category == PrimitiveCategory::SignedInt || category == PrimitiveCategory::UnsignedInt;
}

bool TypeRef::IsFloat() const noexcept {
    return IsCategory(kind, PrimitiveCategory::Float);
}

bool TypeRef::IsSigned() const noexcept {
    return IsCategory(kind, PrimitiveCategory::SignedInt);
}

bool TypeRef::IsPrimitive() const noexcept {
    return FindPrimitive(kind) != nullptr;
}

bool TypeRef::IsAssignableTo(const TypeRef &other) const noexcept {
    if (IsUnknown() || other.IsUnknown()) {
        return true;
    }
    // A read-only pointer (*T) cannot be coerced into a writable one (*var T):
    // that would silently grant write access. The reverse (*var T -> *T) is a
    // permitted weakening.
    if (kind == Kind::Pointer && other.kind == Kind::Pointer && !inner.empty() && !other.inner.empty() &&
        !inner[0].isMut && other.inner[0].isMut) {
        return false;
    }
    if (*this == other) {
        return true;
    }
    if (kind == Kind::Array && arrayLength && !inner.empty() && other.kind == Kind::Named &&
        other.name == "Slice<" + inner[0].ToString() + ">") {
        return true;
    }
    // float32 widens implicitly to float64 / float (safe, no precision loss
    // in range)
    if (kind == Kind::Float32 && other.kind == Kind::Float64) {
        return true;
    }
    // A character widens implicitly only to a wider character carrying the same thing. Every scalar-valued width
    // holds the same code points, so widening one is exact. A code unit does not widen at all: `char8` and `char16`
    // are units of different encodings, and a UTF-8 byte above 0x7F is not the UTF-16 word of the same number.
    if (IsChar() && other.IsChar()) {
        return CharacterDomainOf(kind) == CharacterDomain::ScalarValue &&
               CharacterDomainOf(other.kind) == CharacterDomain::ScalarValue &&
               PrimitiveSize(kind, DefaultPointerSize) <= PrimitiveSize(other.kind, DefaultPointerSize);
    }
    // int/uint interoperate with their fixed-width platform equivalents
    // (x86-64: 64-bit)
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
    if (const auto primitive = PrimitiveSize(kind, DefaultPointerSize)) {
        return *primitive;
    }
    switch (kind) {
    case Kind::Unknown:
    case Kind::TypeParam:
        return std::nullopt;
    case Kind::Opaque:
        return 0;
    case Kind::Pointer:
    case Kind::Str:
    case Kind::Func:
        return 8;
    case Kind::Array: {
        if (inner.empty()) {
            return std::nullopt;
        }
        if (!arrayLength) {
            return 0;
        }
        const auto elemSize = inner[0].SizeInBytes();
        if (!elemSize) {
            return std::nullopt;
        }
        return *elemSize * *arrayLength;
    }
    case Kind::Range:
    case Kind::RangeInclusive: {
        if (inner.empty()) {
            return std::nullopt;
        }
        const auto elemSize = inner[0].SizeInBytes();
        if (!elemSize || *elemSize == 0) {
            return std::nullopt;
        }
        return 2 * *elemSize;
    }
    case Kind::RangeFrom:
    case Kind::RangeTo:
    case Kind::RangeToInclusive: {
        if (inner.empty()) {
            return std::nullopt;
        }
        return inner[0].SizeInBytes();
    }
    case Kind::RangeFull:
        return 0;
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
    default:
        // Every primitive was already answered from the catalog above.
        return std::nullopt;
    }
}

std::string TypeRef::InstantiationName(const std::string_view base, const std::vector<TypeRef> &typeArgs) {
    std::string name(base);
    if (typeArgs.empty()) {
        return name;
    }
    name += '<';
    for (std::size_t i = 0; i < typeArgs.size(); ++i) {
        if (i) {
            name += ", ";
        }
        name += typeArgs[i].ToString();
    }
    name += '>';
    return name;
}

std::string TypeRef::ToString() const {
    if (const PrimitiveInfo *primitive = FindPrimitive(kind)) {
        return std::string(primitive->name);
    }
    switch (kind) {
    case Kind::Unknown:
        return "?";
    case Kind::Opaque:
        return "opaque";
    case Kind::Str:
        return "String";
    case Kind::Named:
        return name;
    case Kind::TypeParam:
        return name;
    case Kind::Pointer: {
        if (inner.empty()) {
            return "*?";
        }
        std::string pointee = inner[0].ToString();
        if (inner[0].kind == Kind::Array) {
            pointee = "(" + pointee + ")";
        }
        return (inner[0].isMut ? "*var " : "*") + pointee;
    }
    case Kind::Array: {
        std::string element = inner.empty() ? "?" : inner[0].ToString();
        if (!inner.empty() && inner[0].kind == Kind::Pointer) {
            element = "(" + element + ")";
        }
        return element + (arrayLength ? "[" + std::to_string(*arrayLength) + "]" : "[]");
    }
    case Kind::Range:
        return "Range<" + (inner.empty() ? "?" : inner[0].ToString()) + ">";
    case Kind::RangeInclusive:
        return "RangeInclusive<" + (inner.empty() ? "?" : inner[0].ToString()) + ">";
    case Kind::RangeFrom:
        return "RangeFrom<" + (inner.empty() ? "?" : inner[0].ToString()) + ">";
    case Kind::RangeTo:
        return "RangeTo<" + (inner.empty() ? "?" : inner[0].ToString()) + ">";
    case Kind::RangeToInclusive:
        return "RangeToInclusive<" + (inner.empty() ? "?" : inner[0].ToString()) + ">";
    case Kind::RangeFull:
        return "RangeFull";
    case Kind::Tuple: {
        std::string s = "(";
        for (std::size_t i = 0; i < inner.size(); ++i) {
            if (i) {
                s += ", ";
            }
            s += inner[i].ToString();
        }
        if (inner.size() == 1) {
            s += ",";
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
    default:
        // Every primitive was already spelled from the catalog above.
        return "?";
    }
}

bool TypeRef::operator==(const TypeRef &o) const noexcept {
    if (kind != o.kind || name != o.name) {
        return false;
    }
    // Named types are identified by their concrete name. Their `inner` value
    // may carry layout metadata (for example an enum's storage type), which is
    // not part of the source-language type identity.
    if (kind == Kind::Named) {
        return true;
    }
    if (arrayLength != o.arrayLength || inner.size() != o.inner.size()) {
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
