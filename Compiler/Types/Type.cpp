#include "Types/Type.h"

#include "Target/Layout.h"
#include "Types/NativeConversion.h"
#include "Types/PrimitiveCatalog.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <memory>
#include <mutex>
#include <unordered_map>

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

/// Wrap `spelling` in parentheses when `grouped` says the surrounding operator would otherwise read it differently.
std::string Grouped(std::string spelling, const bool grouped) {
    return grouped ? "(" + spelling + ")" : spelling;
}

/// Whether `type` is one of the native composite forms whose operator binds more loosely than a postfix suffix, so it
/// is grouped wherever a suffix or a prefix would otherwise claim part of it.
bool IsLooseNativeForm(const TypeRef &type) noexcept {
    return type.IsSum() || type.IsFallible();
}

/// A tuple's {size, alignment}, its elements laid out by the record rule each at its own alignment.
std::optional<std::pair<std::uint64_t, std::uint64_t>> TupleLayout(const std::vector<TypeRef> &elements) noexcept {
    return Layout::FieldsSizeAndAlign(
        elements, [](const TypeRef &element) -> std::optional<std::pair<std::uint64_t, std::uint64_t>> {
            const auto size = element.SizeInBytes();
            const auto alignment = element.AlignInBytes();
            if (!size || !alignment) {
                return std::nullopt;
            }
            return std::pair{*size, *alignment};
        });
}
} // namespace

TypeRef TypeRef::MakeSum(const std::span<const TypeRef> members) {
    std::vector<TypeRef> flattened;
    for (const TypeRef &member : members) {
        if (member.IsUnknown()) {
            return MakeUnknown();
        }
        if (member.IsSum()) {
            flattened.insert(flattened.end(), member->inner.begin(), member->inner.end());
        }
        else {
            flattened.push_back(member);
        }
    }
    // Order by spelling first so the member kept for a duplicate, and therefore the canonical spelling, never depends
    // on the order the source wrote.
    std::ranges::stable_sort(flattened, {},
                             [](const TypeRef &member) -> const std::string & { return member.ToString(); });
    std::vector<TypeRef> unique;
    for (const TypeRef &member : flattened) {
        if (std::ranges::find(unique, member) == unique.end()) {
            unique.push_back(member);
        }
    }
    if (unique.empty()) {
        return MakeUnknown();
    }
    if (unique.size() == 1) {
        return unique.front();
    }
    return Intern(Kind::Sum, {}, unique);
}

TypeRef TypeRef::Renormalize(const TypeRef &type) {
    return type.IsSum() ? MakeSum(type->inner) : type;
}

TypeRef TypeRef::WithInner(const std::span<const TypeRef> replacement) const {
    return With(node->name, replacement);
}

TypeRef TypeRef::WithWritableInner(const bool writable) const {
    if (node->inner.empty() || node->inner[0].isMut == writable) {
        return *this;
    }
    std::vector<TypeRef> inner = node->inner;
    inner[0].isMut = writable;
    return WithInner(inner);
}

TypeRef TypeRef::With(const std::string_view replacementName, const std::span<const TypeRef> replacement) const {
    const auto same = [](const TypeRef &left, const TypeRef &right) {
        return left.node == right.node && left.isMut == right.isMut && left.isVariadic == right.isVariadic;
    };
    if (replacementName == node->name && std::ranges::equal(replacement, node->inner, same)) {
        return *this;
    }
    TypeRef type = Intern(node->kind, replacementName, replacement, node->arrayLength);
    type.isMut = isMut;
    type.isVariadic = isVariadic;
    return type;
}

// TypeRef implementation
bool TypeRef::IsBool() const noexcept {
    return IsCategory(node->kind, PrimitiveCategory::Bool);
}

bool TypeRef::IsChar() const noexcept {
    return IsCategory(node->kind, PrimitiveCategory::Char);
}

bool TypeRef::IsNumeric() const noexcept {
    return IsInteger() || IsFloat();
}

bool TypeRef::IsInteger() const noexcept {
    const auto category = CategoryOf(node->kind);
    return category == PrimitiveCategory::SignedInt || category == PrimitiveCategory::UnsignedInt;
}

bool TypeRef::IsFloat() const noexcept {
    return IsCategory(node->kind, PrimitiveCategory::Float);
}

bool TypeRef::IsSigned() const noexcept {
    return IsCategory(node->kind, PrimitiveCategory::SignedInt);
}

bool TypeRef::IsPrimitive() const noexcept {
    return FindPrimitive(node->kind) != nullptr;
}

bool TypeRef::IsAssignableTo(const TypeRef &other) const noexcept {
    if (IsUnknown() || other.IsUnknown()) {
        return true;
    }
    // A pointer, reference, or slice of native values converts by the view rules below, which compare the elements as
    // they are: an `int32?[4]` array is viewed as an `int32?[..]` slice exactly as an `int32[4]` one is.
    const bool viewConversion = (node->kind == Kind::Pointer && other->kind == Kind::Pointer) ||
                                (node->kind == Kind::Reference && other->kind == Kind::Reference) ||
                                (node->kind == Kind::Slice && other->kind == Kind::Slice) ||
                                (node->kind == Kind::Array && other->kind == Kind::Slice);
    // A native sum, optional, or fallible on either side follows the native conversion rules: identity, injection,
    // subset widening, and the presence and success routes, accepted only when exactly one route applies.
    if (!viewConversion && (MentionsNativeType(*this) || MentionsNativeType(other))) {
        return ClassifyNativeConversion(*this, other).Accepted();
    }
    // A read-only pointer (*T) cannot be coerced into a writable one (*var T):
    // that would silently grant write access. The reverse (*var T -> *T) is a
    // permitted weakening.
    if (node->kind == Kind::Pointer && other->kind == Kind::Pointer && !node->inner.empty() && !other->inner.empty() &&
        !node->inner[0].isMut && other->inner[0].isMut) {
        return false;
    }
    // References preserve mutability in their type identity. An exclusive reference may be weakened to an immutable
    // reference to the same referent, but the reverse would grant write access.
    if (node->kind == Kind::Reference && other->kind == Kind::Reference && !node->inner.empty() &&
        !other->inner.empty()) {
        if (!node->inner[0].isMut && other->inner[0].isMut) {
            return false;
        }
        TypeRef source = node->inner[0];
        TypeRef target = other->inner[0];
        source.isMut = false;
        target.isMut = false;
        return source == target;
    }
    // A slice carries its writability on its element the way a pointer does. A writable view may be weakened to a
    // read-only view of the same elements, but the reverse would grant write access through a read-only view.
    if (node->kind == Kind::Slice && other->kind == Kind::Slice && !node->inner.empty() && !other->inner.empty()) {
        if (!node->inner[0].isMut && other->inner[0].isMut) {
            return false;
        }
        TypeRef source = node->inner[0];
        TypeRef target = other->inner[0];
        source.isMut = false;
        target.isMut = false;
        return source == target;
    }
    if (*this == other) {
        return true;
    }
    // A fixed array value coerces to a read-only view of its elements. A writable view needs a writable place, which
    // is a question about the expression rather than its type, so it is answered where the expression is checked.
    if (node->kind == Kind::Array && node->arrayLength && !node->inner.empty() && other->kind == Kind::Slice &&
        !other->inner.empty() && !other->inner[0].isMut) {
        TypeRef element = node->inner[0];
        element.isMut = false;
        return element == other->inner[0];
    }
    // float32 widens implicitly to float64 / float (safe, no precision loss
    // in range)
    if (node->kind == Kind::Float32 && other->kind == Kind::Float64) {
        return true;
    }
    // A character widens implicitly only to a wider character carrying the same thing. Every scalar-valued width
    // holds the same code points, so widening one is exact. A code unit does not widen at all: `char8` and `char16`
    // are units of different encodings, and a UTF-8 byte above 0x7F is not the UTF-16 word of the same number.
    if (IsChar() && other.IsChar()) {
        return CharacterDomainOf(node->kind) == CharacterDomain::ScalarValue &&
               CharacterDomainOf(other->kind) == CharacterDomain::ScalarValue &&
               PrimitiveSize(node->kind, DefaultPointerSize) <= PrimitiveSize(other->kind, DefaultPointerSize);
    }
    // int/uint interoperate with their fixed-width platform equivalents
    // (x86-64: 64-bit)
    if (node->kind == Kind::Int64 && other->kind == Kind::Int) {
        return true;
    }
    if (node->kind == Kind::Int && other->kind == Kind::Int64) {
        return true;
    }
    if (node->kind == Kind::UInt64 && other->kind == Kind::UInt) {
        return true;
    }
    if (node->kind == Kind::UInt && other->kind == Kind::UInt64) {
        return true;
    }
    // smaller fixed-width integers widen implicitly to int/uint
    if (other->kind == Kind::Int &&
        (node->kind == Kind::Int8 || node->kind == Kind::Int16 || node->kind == Kind::Int32)) {
        return true;
    }
    if (other->kind == Kind::UInt &&
        (node->kind == Kind::UInt8 || node->kind == Kind::UInt16 || node->kind == Kind::UInt32)) {
        return true;
    }
    // smaller fixed-width signed integers widen to larger signed integers
    if (other->kind == Kind::Int64 &&
        (node->kind == Kind::Int8 || node->kind == Kind::Int16 || node->kind == Kind::Int32)) {
        return true;
    }
    if (other->kind == Kind::Int32 && (node->kind == Kind::Int8 || node->kind == Kind::Int16)) {
        return true;
    }
    if (other->kind == Kind::Int16 && node->kind == Kind::Int8) {
        return true;
    }
    // smaller fixed-width unsigned integers widen to larger unsigned
    // integers
    if (other->kind == Kind::UInt64 &&
        (node->kind == Kind::UInt8 || node->kind == Kind::UInt16 || node->kind == Kind::UInt32)) {
        return true;
    }
    if (other->kind == Kind::UInt32 && (node->kind == Kind::UInt8 || node->kind == Kind::UInt16)) {
        return true;
    }
    if (other->kind == Kind::UInt16 && node->kind == Kind::UInt8) {
        return true;
    }
    // The same rule continues past 64 bits: an integer widens to any strictly wider integer of the same signedness,
    // so an `int64` or an `int` reaches `int128` the way an `int32` reaches `int64`.
    if (IsInteger() && other.IsInteger() && IsSigned() == other.IsSigned()) {
        const auto bits = PrimitiveBits(node->kind, DefaultPointerSize * 8);
        const auto otherBits = PrimitiveBits(other->kind, DefaultPointerSize * 8);
        if (bits && otherBits && *bits < *otherBits) {
            return true;
        }
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
    if (node->kind == Kind::Pointer && other->kind == Kind::Pointer && !other->inner.empty() &&
        other->inner[0].IsOpaque()) {
        return true;
    }
    return false;
}

bool TypeRef::CanReadScalarTo(const TypeRef &other) const noexcept {
    if (node->kind != Kind::Reference || node->inner.empty() || other->kind == Kind::Reference) {
        return false;
    }
    TypeRef value = node->inner.front();
    value.isMut = false;
    return (value.IsNumeric() || value.IsBool() || value.IsChar() || value->kind == Kind::TypeParam) &&
           value.IsAssignableTo(other);
}

bool TypeRef::CanImplicitlyBorrowTo(const TypeRef &other) const noexcept {
    if (other->kind != Kind::Reference || other->inner.empty()) {
        return false;
    }
    if (node->kind == Kind::Reference) {
        return IsAssignableTo(other);
    }
    TypeRef value = *this;
    TypeRef referent = other->inner.front();
    value.isMut = false;
    referent.isMut = false;
    return value == referent;
}

std::optional<std::uint64_t> TypeRef::SizeInBytes() const noexcept {
    if (const auto primitive = PrimitiveSize(node->kind, DefaultPointerSize)) {
        return *primitive;
    }
    switch (node->kind) {
    case Kind::Unknown:
    case Kind::TypeParam:
        return std::nullopt;
    case Kind::Opaque:
        return 0;
    case Kind::Pointer:
    case Kind::Reference:
    case Kind::Func:
        return 8;
    case Kind::Array: {
        if (node->inner.empty()) {
            return std::nullopt;
        }
        if (!node->arrayLength) {
            return 0;
        }
        const auto elemSize = node->inner[0].SizeInBytes();
        if (!elemSize) {
            return std::nullopt;
        }
        return *elemSize * *node->arrayLength;
    }
    case Kind::Range:
    case Kind::RangeInclusive: {
        if (node->inner.empty()) {
            return std::nullopt;
        }
        const auto elemSize = node->inner[0].SizeInBytes();
        if (!elemSize || *elemSize == 0) {
            return std::nullopt;
        }
        return 2 * *elemSize;
    }
    case Kind::RangeFrom:
    case Kind::RangeTo:
    case Kind::RangeToInclusive: {
        if (node->inner.empty()) {
            return std::nullopt;
        }
        return node->inner[0].SizeInBytes();
    }
    case Kind::RangeFull:
        return 0;
    case Kind::Tuple: {
        const auto layout = TupleLayout(node->inner);
        if (!layout) {
            return std::nullopt;
        }
        return layout->first;
    }
    case Kind::Slice:
        return 16;
    case Kind::Sum:
    case Kind::Optional:
    case Kind::Fallible:
        // A native tagged layout is published by the shared layout facts, not guessed from the members here.
        return std::nullopt;
    case Kind::Named:
        if (!node->inner.empty()) {
            return node->inner[0].SizeInBytes();
        }
        return std::nullopt;
    default:
        // Every primitive was already answered from the catalog above.
        return std::nullopt;
    }
}

std::optional<std::uint64_t> TypeRef::AlignInBytes() const noexcept {
    if (const auto primitive = PrimitiveAlign(node->kind, DefaultPointerSize)) {
        return *primitive;
    }
    switch (node->kind) {
    case Kind::Opaque:
    case Kind::RangeFull:
        return 1;
    case Kind::Pointer:
    case Kind::Reference:
    case Kind::Func:
    case Kind::Slice:
        return Layout::WordAlign(DefaultPointerSize);
    case Kind::Array:
    case Kind::Range:
    case Kind::RangeInclusive:
    case Kind::RangeFrom:
    case Kind::RangeTo:
    case Kind::RangeToInclusive:
    case Kind::Named:
        // An array and a range are their elements laid end to end, and a named type is its layout marker.
        if (node->inner.empty()) {
            return std::nullopt;
        }
        return node->inner[0].AlignInBytes();
    case Kind::Tuple: {
        const auto layout = TupleLayout(node->inner);
        if (!layout) {
            return std::nullopt;
        }
        return layout->second;
    }
    default:
        // Unknown, a type parameter, and a native tagged layout, which the shared layout facts publish.
        return std::nullopt;
    }
}

TypeRef TypeRef::MakeStorageMarker(const std::uint64_t size, const std::uint64_t alignment) {
    TypeRef word;
    switch (alignment) {
    case 2:
        word = MakeUInt16();
        break;
    case 4:
        word = MakeUInt32();
        break;
    case 8:
        word = MakeUInt64();
        break;
    case 16:
        word = MakePrimitive(Kind::UInt128);
        break;
    default:
        return MakeArray(MakeUInt8(), size);
    }
    return MakeArray(word, size / alignment);
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

std::optional<TypeRef::RangeSpelling> TypeRef::SplitRangeSpelling(const std::string_view text) {
    int depth = 0;
    for (std::size_t index = 0; index + 1 < text.size(); ++index) {
        const char character = text[index];
        if (character == '<' || character == '(' || character == '[') {
            ++depth;
        }
        else if (character == '>' || character == ')' || character == ']') {
            --depth;
        }
        else if (depth == 0 && character == '.' && text[index + 1] == '.') {
            RangeSpelling spelling;
            spelling.inclusive = index + 2 < text.size() && text[index + 2] == '=';
            spelling.start = std::string(text.substr(0, index));
            spelling.end = std::string(text.substr(index + (spelling.inclusive ? 3 : 2)));
            return spelling;
        }
    }
    return std::nullopt;
}

namespace {
/// Visit `text` with the bracket depth of each character, treating the `>` of a function's `->` as no bracket.
template <typename Visitor>
void ForEachTopLevelCharacter(const std::string_view text, Visitor &&visit) {
    int depth = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char character = text[index];
        if (character == '<' || character == '(' || character == '[') {
            ++depth;
        }
        else if ((character == '>' && (index == 0 || text[index - 1] != '-')) || character == ')' || character == ']') {
            --depth;
        }
        else if (depth == 0) {
            visit(index);
        }
    }
}
} // namespace

std::optional<TypeRef::NativeSpelling> TypeRef::SplitNativeSpelling(const std::string_view text) {
    if (text.starts_with("! ")) {
        return NativeSpelling{NativeSpelling::Form::Fallible, {"", std::string(text.substr(2))}};
    }
    std::optional<std::size_t> bang;
    std::vector<std::size_t> pipes;
    ForEachTopLevelCharacter(text, [&](const std::size_t index) {
        if (index + 2 < text.size() && text[index] == ' ' && text[index + 2] == ' ') {
            if (text[index + 1] == '!' && !bang) {
                bang = index;
            }
            else if (text[index + 1] == '|') {
                pipes.push_back(index);
            }
        }
    });
    if (bang) {
        return NativeSpelling{NativeSpelling::Form::Fallible,
                              {std::string(text.substr(0, *bang)), std::string(text.substr(*bang + 3))}};
    }
    if (pipes.empty()) {
        return std::nullopt;
    }
    NativeSpelling sum{NativeSpelling::Form::Sum, {}};
    std::size_t start = 0;
    for (const std::size_t pipe : pipes) {
        sum.parts.emplace_back(text.substr(start, pipe - start));
        start = pipe + 3;
    }
    sum.parts.emplace_back(text.substr(start));
    return sum;
}

std::string TypeRef::MangledSpelling() const {
    std::string mangled;
    for (const char character : ToString()) {
        if (std::isalnum(static_cast<unsigned char>(character)) || character == '_') {
            mangled += character;
        }
        else if (character == '?') {
            mangled += "_O";
        }
        else if (character == '|') {
            mangled += "_S";
        }
        else if (character == '!') {
            mangled += "_F";
        }
        else {
            mangled += '_';
        }
    }
    return mangled;
}

namespace {
using Kind = TypeRef::Kind;

/// The canonical spelling of a node whose inner types are already interned, so each of them is spelled already.
std::string Spell(const TypeNode &type) {
    if (const PrimitiveInfo *primitive = FindPrimitive(type.kind)) {
        return std::string(primitive->name);
    }
    const std::vector<TypeRef> &inner = type.inner;
    switch (type.kind) {
    case Kind::Unknown:
        return "?";
    case Kind::Opaque:
        return "opaque";
    case Kind::Named:
        return type.name;
    case Kind::TypeParam:
        return type.name;
    case Kind::Pointer: {
        if (inner.empty()) {
            return "*?";
        }
        std::string pointee = inner[0].ToString();
        if (inner[0]->kind == Kind::Array || inner[0]->kind == Kind::Slice || inner[0].IsOptional() ||
            IsLooseNativeForm(inner[0])) {
            pointee = "(" + pointee + ")";
        }
        return (inner[0].isMut ? "*var " : "*") + pointee;
    }
    case Kind::Reference: {
        if (inner.empty()) {
            return "&?";
        }
        std::string referent = inner[0].ToString();
        if (inner[0]->kind == Kind::Array || inner[0]->kind == Kind::Slice || inner[0].IsOptional() ||
            IsLooseNativeForm(inner[0])) {
            referent = "(" + referent + ")";
        }
        return (inner[0].isMut ? "&var " : "&") + referent;
    }
    case Kind::Array: {
        std::string element = inner.empty() ? "?" : inner[0].ToString();
        if (!inner.empty() &&
            (inner[0]->kind == Kind::Pointer || inner[0]->kind == Kind::Reference || IsLooseNativeForm(inner[0]))) {
            element = "(" + element + ")";
        }
        return element + (type.arrayLength ? "[" + std::to_string(*type.arrayLength) + "]" : "[]");
    }
    case Kind::Slice: {
        // The bracket binds tighter than a pointer, reference or range operator, so an element of one of those
        // shapes is parenthesized to keep the bracket on the outside when the spelling is read back.
        std::string element = inner.empty() ? "?" : inner[0].ToString();
        if (!inner.empty() && (inner[0]->kind == Kind::Pointer || inner[0]->kind == Kind::Reference ||
                               inner[0].IsRange() || IsLooseNativeForm(inner[0]))) {
            element = "(" + element + ")";
        }
        const bool writable = !inner.empty() && inner[0].isMut;
        return (writable ? "var " : "") + element + "[..]";
    }
    case Kind::Range:
    case Kind::RangeInclusive:
    case Kind::RangeFrom:
    case Kind::RangeTo:
    case Kind::RangeToInclusive:
    case Kind::RangeFull: {
        // A bound spells the element; a pointer, function or range element takes parentheses because the range
        // operator would otherwise be read as part of it.
        std::string element = inner.empty() ? "?" : inner[0].ToString();
        if (!inner.empty() && (inner[0]->kind == Kind::Pointer || inner[0]->kind == Kind::Reference ||
                               inner[0]->kind == Kind::Func || inner[0].IsRange() || IsLooseNativeForm(inner[0]))) {
            element = "(" + element + ")";
        }
        const bool hasStart =
            type.kind == Kind::Range || type.kind == Kind::RangeInclusive || type.kind == Kind::RangeFrom;
        const bool hasEnd = type.kind == Kind::Range || type.kind == Kind::RangeInclusive ||
                            type.kind == Kind::RangeTo || type.kind == Kind::RangeToInclusive;
        const bool inclusive = type.kind == Kind::RangeInclusive || type.kind == Kind::RangeToInclusive;
        return (hasStart ? element : "") + (inclusive ? "..=" : "..") + (hasEnd ? element : "");
    }
    case Kind::Sum: {
        // `|` binds more tightly than `!` and more loosely than a suffix, so a fallible, optional, function, or range
        // member is grouped; a written `A | B?` is rejected as ambiguous, so its canonical spelling is `A | (B?)`.
        std::string s;
        for (std::size_t i = 0; i < inner.size(); ++i) {
            if (i) {
                s += " | ";
            }
            const TypeRef &member = inner[i];
            s += Grouped(member.ToString(),
                         member.IsFallible() || member.IsOptional() || member->kind == Kind::Func || member.IsRange());
        }
        return s;
    }
    case Kind::Optional: {
        if (inner.empty()) {
            return "??";
        }
        // The suffix binds to the type immediately before it, so a payload whose own operator is looser, or whose
        // prefix would otherwise swallow the suffix, is grouped: `(A | B)?`, `(*T)?`, `(var T[..])?`.
        const TypeRef &payload = inner[0];
        return Grouped(payload.ToString(), IsLooseNativeForm(payload) || payload->kind == Kind::Pointer ||
                                               payload->kind == Kind::Reference || payload->kind == Kind::Func ||
                                               payload.IsRange() || payload.IsWritableSlice()) +
               "?";
    }
    case Kind::Fallible: {
        if (inner.size() != 2) {
            return "? ! ?";
        }
        const TypeRef &success = inner[0];
        const TypeRef &error = inner[1];
        // One unparenthesized `!` per type: a nested fallible on either side is grouped, as is a function type, whose
        // return would otherwise extend over the `!`. A directly written optional error is grouped too, `T ! (E?)`.
        // A sum on either side binds tighter than `!` and needs no parentheses, but is grouped anyway, as the language
        // reference writes it, because `int ! A | B` reads as a different grouping.
        const std::string errorSpelling = Grouped(error.ToString(), error.IsFallible() || error.IsOptional() ||
                                                                        error->kind == Kind::Func || error.IsSum());
        if (success.IsUnit()) {
            return "! " + errorSpelling;
        }
        return Grouped(success.ToString(), success.IsFallible() || success->kind == Kind::Func || success.IsSum()) +
               " ! " + errorSpelling;
    }
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

/// Fold `value` into `seed`, the 64-bit mix every type hash is built from.
constexpr std::size_t MixHash(const std::size_t seed, const std::size_t value) noexcept {
    std::uint64_t mixed = seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
    mixed ^= mixed >> 31;
    mixed *= 0xbf58476d1ce4e5b9ULL;
    mixed ^= mixed >> 29;
    return static_cast<std::size_t>(mixed);
}

/// One structure to intern: the node's fields, with the inner types still in the caller's storage.
struct TypeKey {
    Kind kind = Kind::Unknown;
    std::string_view name;
    std::span<const TypeRef> inner;
    std::optional<std::uint64_t> arrayLength;

    [[nodiscard]] bool Matches(const TypeNode &node) const noexcept {
        return node.kind == kind && node.name == name && node.arrayLength == arrayLength &&
               std::ranges::equal(node.inner, inner, [](const TypeRef &left, const TypeRef &right) {
                   return left.IsSameNode(right) && left.isMut == right.isMut && left.isVariadic == right.isVariadic;
               });
    }

    /// The hash the table finds a node by. Inner types are interned already, so their addresses identify them; the
    /// table is never iterated, so the addresses never decide an order.
    [[nodiscard]] std::size_t TableHash() const noexcept {
        std::size_t hash = MixHash(static_cast<std::size_t>(kind), std::hash<std::string_view>{}(name));
        hash = MixHash(hash, arrayLength ? *arrayLength + 1 : 0);
        for (const TypeRef &type : inner) {
            hash = MixHash(hash, std::bit_cast<std::uintptr_t>(type.operator->()));
            hash = MixHash(hash, (type.isMut ? 1U : 0U) | (type.isVariadic ? 2U : 0U));
        }
        return hash;
    }
};

/// Whether the identity of a type of `kind` keeps the writability of its inner type at `index`: a reference's referent
/// and a slice's elements are part of what the type is, while writability anywhere else, behind a pointer included,
/// never reaches `operator==`.
bool KeepsInnerWritability(const Kind kind, const std::size_t index) noexcept {
    return index == 0 && (kind == Kind::Reference || kind == Kind::Slice);
}

/// Every node interned so far. Sharded by hash so threads interning different types rarely wait on each other; a
/// node is built outside the lock and published only if no other thread published the same structure first.
class TypeTable {
public:
    const TypeNode *Intern(const TypeKey &key) {
        const std::size_t hash = key.TableHash();
        Shard &shard = shards[hash % ShardCount];
        {
            const std::scoped_lock lock(shard.mutex);
            if (const TypeNode *found = shard.Find(hash, key)) {
                return found;
            }
        }
        auto node = std::make_unique<TypeNode>();
        node->kind = key.kind;
        node->name = key.name;
        node->inner.assign(key.inner.begin(), key.inner.end());
        node->arrayLength = key.arrayLength;
        node->spelling = Spell(*node);
        const TypeNode *identity = IdentityOf(*node);
        node->hash = identity ? identity->hash : IdentityHash(*node);
        const std::scoped_lock lock(shard.mutex);
        if (const TypeNode *found = shard.Find(hash, key)) {
            return found;
        }
        TypeNode *published = shard.nodes.emplace_back(std::move(node)).get();
        published->identity = identity ? identity : published;
        shard.index.emplace(hash, published);
        return published;
    }

private:
    static constexpr std::size_t ShardCount = 64;

    struct Shard {
        std::mutex mutex;
        std::unordered_multimap<std::size_t, const TypeNode *> index;
        std::vector<std::unique_ptr<TypeNode>> nodes;

        [[nodiscard]] const TypeNode *Find(const std::size_t hash, const TypeKey &key) const noexcept {
            const auto [first, last] = index.equal_range(hash);
            for (auto entry = first; entry != last; ++entry) {
                if (key.Matches(*entry->second)) {
                    return entry->second;
                }
            }
            return nullptr;
        }
    };

    /// The node `node`'s identity is, interned, or null when `node` is already in identity form.
    const TypeNode *IdentityOf(const TypeNode &node) {
        if (node.kind == Kind::Named) {
            if (node.inner.empty() && !node.arrayLength) {
                return nullptr;
            }
            return Intern({Kind::Named, node.name, {}, std::nullopt});
        }
        std::vector<TypeRef> inner;
        bool changed = false;
        for (std::size_t index = 0; index < node.inner.size(); ++index) {
            const TypeRef &type = node.inner[index];
            TypeRef identity = type.Identity();
            identity.isMut = KeepsInnerWritability(node.kind, index) && type.isMut;
            changed = changed || !identity.IsSameNode(type) || identity.isMut != type.isMut || type.isVariadic;
            inner.push_back(identity);
        }
        if (!changed) {
            return nullptr;
        }
        return Intern({node.kind, node.name, inner, node.arrayLength});
    }

    /// The deterministic hash of a node in identity form, built from its fields and its inner types' hashes.
    static std::size_t IdentityHash(const TypeNode &node) noexcept {
        std::size_t hash = MixHash(static_cast<std::size_t>(node.kind), std::hash<std::string_view>{}(node.name));
        hash = MixHash(hash, node.arrayLength ? *node.arrayLength + 1 : 0);
        for (const TypeRef &type : node.inner) {
            hash = MixHash(hash, type.Hash());
            hash = MixHash(hash, type.isMut ? 1U : 0U);
        }
        return hash;
    }

    std::array<Shard, ShardCount> shards;
};

TypeTable &Table() {
    // Never destroyed: a node must outlive every handle, including those in other static objects.
    static TypeTable *const table = new TypeTable;
    return *table;
}
} // namespace

TypeRef TypeRef::Intern(const Kind kind, const std::string_view name, const std::span<const TypeRef> inner,
                        const std::optional<std::uint64_t> arrayLength) {
    if (name.empty() && inner.empty() && !arrayLength) {
        const TypeNode &leaf = Detail::leafTypeNodes[static_cast<std::size_t>(kind)];
        if (leaf.identity) {
            return TypeRef(&leaf);
        }
    }
    return TypeRef(Table().Intern({kind, name, inner, arrayLength}));
}

TypeRef TypeRef::Identity() const noexcept {
    return TypeRef(node->identity);
}
} // namespace Rux
