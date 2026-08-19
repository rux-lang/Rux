// Type lowering: resolving written types to HIR types, and composing the names
// generic instantiations are identified by.

#include "Ir/Hir/HirInternal.h"
#include "Lowering/AstToHir/Detail/AstToHirContext.h"
#include "Numeric/IntegerLiteral.h"
#include "Semantic/PrimitiveCatalog.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Rux::AstToHirDetail {
namespace {
template <typename Fact>
[[nodiscard]] const Fact &RequireSemanticFact(const Fact *fact) {
    assert(fact != nullptr && "accepted AST node is missing a required semantic fact");
    if (!fact) {
        std::abort();
    }
    return *fact;
}
} // namespace

[[nodiscard]] const TypeRef &AstToHirContext::ResolvedType(const TypeExpr &type) const {
    const TypeRef &resolved = RequireSemanticFact(model.TryGetType(type));
    NoteStructInstantiation(resolved);
    return resolved;
}

[[nodiscard]] const ResolvedTypeLayout &AstToHirContext::ResolvedLayout(const TypeRef &type) const {
    return RequireSemanticFact(model.TryGetLayout(type));
}

TypeRef AstToHirContext::MakeFuncTypeWithSubstitution(const std::vector<Param> &params,
                                                      const std::optional<TypeExprPtr> &returnType,
                                                      const std::unordered_map<std::string, TypeRef> &substitutions,
                                                      const std::vector<std::string> &typeParams) {
    auto savedTypeParams = currentTypeParams;
    currentTypeParams = typeParams;
    auto savedSubstitutions = currentSubstitutions;
    currentSubstitutions = substitutions;

    std::vector<TypeRef> paramTypes;
    for (const auto &param : params) {
        // A receiver is not one of the arguments a call site writes: it reaches the function on its own, as the value
        // to the left of the dot or, for an interface, as the data half of the fat pointer.
        if (!param.isVariadic && !param.IsReceiver()) {
            paramTypes.push_back(ResolveType(*param.type));
        }
    }
    TypeRef ret = returnType ? ResolveType(*returnType->get()) : TypeRef::MakeOpaque();

    currentTypeParams = savedTypeParams;
    currentSubstitutions = savedSubstitutions;
    return TypeRef::MakeFunc(std::move(paramTypes), std::move(ret));
}

std::string AstToHirContext::GenericTypeName(const NamedTypeExpr &type) {
    std::vector<TypeRef> typeArgs;
    typeArgs.reserve(type.typeArgs.size());
    for (const auto &typeArg : type.typeArgs) {
        typeArgs.push_back(ResolveType(*typeArg));
    }
    return TypeRef::InstantiationName(type.name, typeArgs);
}

std::string AstToHirContext::SliceTypeName(const TypeRef &elemType) {
    return "Slice<" + elemType.ToString() + ">";
}

std::string AstToHirContext::BaseTypeNameImpl(const std::string &name) {
    const std::size_t pos = name.find('<');
    return pos == std::string::npos ? name : name.substr(0, pos);
}

std::string AstToHirContext::BaseTypeName(const std::string &name) const {
    return BaseTypeNameImpl(name);
}

std::vector<std::string> AstToHirContext::ImplTypeParams(const ImplDecl &decl) const {
    std::vector<std::string> params;
    const auto *target = dynamic_cast<const NamedTypeExpr *>(decl.extendedType.get());
    if (!target) {
        return params;
    }
    const auto structIt = structDecls.find(target->name);
    if (structIt == structDecls.end()) {
        return params;
    }

    const auto &structParams = structIt->second->typeParams;
    const std::size_t count = std::min(structParams.size(), target->typeArgs.size());
    for (std::size_t i = 0; i < count; ++i) {
        const auto *arg = dynamic_cast<const NamedTypeExpr *>(target->typeArgs[i].get());
        if (arg && arg->typeArgs.empty() && arg->name == structParams[i].name) {
            params.push_back(arg->name);
        }
    }
    return params;
}

TypeRef AstToHirContext::ParseTypeRefFromString(std::string str) {
    auto trim = [](std::string &s) {
        s.erase(0, s.find_first_not_of(" \t\r\n"));
        s.erase(s.find_last_not_of(" \t\r\n") + 1);
    };
    trim(str);
    if (str.empty()) {
        return TypeRef::MakeUnknown();
    }

    if (str == "?") {
        return TypeRef::MakeUnknown();
    }
    if (str == "opaque") {
        return TypeRef::MakeOpaque();
    }
    if (str == "String") {
        return TypeRef::MakeStr();
    }
    if (const auto primitive = PrimitiveTypeFromName(str)) {
        return *primitive;
    }

    if (str[0] == '*') {
        return TypeRef::MakePointer(ParseTypeRefFromString(str.substr(1)));
    }

    if (str.size() >= 2 && str.compare(str.size() - 2, 2, "[]") == 0) {
        return TypeRef::MakeArray(ParseTypeRefFromString(str.substr(0, str.size() - 2)));
    }
    if (str.back() == ']') {
        const std::size_t open = str.rfind('[');
        if (open != std::string::npos && open + 1 < str.size() - 1) {
            try {
                const auto length = std::stoull(str.substr(open + 1, str.size() - open - 2));
                return TypeRef::MakeArray(ParseTypeRefFromString(str.substr(0, open)), length);
            }
            catch (...) {
            }
        }
    }

    if (str[0] == '(' && str.back() == ')') {
        std::vector<TypeRef> elems;
        std::string content = str.substr(1, str.size() - 2);
        std::size_t start = 0;
        int depth = 0;
        for (std::size_t i = 0; i < content.size(); ++i) {
            if (content[i] == '<' || content[i] == '(') {
                depth++;
            }
            else if (content[i] == '>' || content[i] == ')') {
                depth--;
            }
            else if (content[i] == ',' && depth == 0) {
                elems.push_back(ParseTypeRefFromString(content.substr(start, i - start)));
                start = i + 1;
            }
        }
        if (start < content.size()) {
            elems.push_back(ParseTypeRefFromString(content.substr(start)));
        }
        return TypeRef::MakeTuple(elems);
    }

    const auto rangeElement = [&](const std::string_view prefix) {
        return ParseTypeRefFromString(str.substr(prefix.size(), str.size() - prefix.size() - 1));
    };
    if (str.rfind("Range<", 0) == 0 && str.back() == '>') {
        return TypeRef::MakeRange(rangeElement("Range<"));
    }
    if (str.rfind("RangeInclusive<", 0) == 0 && str.back() == '>') {
        return TypeRef::MakeRange(rangeElement("RangeInclusive<"), true, true, true);
    }
    if (str.rfind("RangeFrom<", 0) == 0 && str.back() == '>') {
        return TypeRef::MakeRange(rangeElement("RangeFrom<"), true, false);
    }
    if (str.rfind("RangeTo<", 0) == 0 && str.back() == '>') {
        return TypeRef::MakeRange(rangeElement("RangeTo<"), false, true);
    }
    if (str.rfind("RangeToInclusive<", 0) == 0 && str.back() == '>') {
        return TypeRef::MakeRange(rangeElement("RangeToInclusive<"), false, true, true);
    }
    if (str == "RangeFull") {
        return TypeRef::MakeRangeFull();
    }

    return TypeRef::MakeNamed(str);
}

std::vector<TypeRef> AstToHirContext::ParseTypeArgsFromTypeName(const std::string &typeName) const {
    std::vector<TypeRef> args;
    const std::size_t pos = typeName.find('<');
    if (pos == std::string::npos || typeName.back() != '>') {
        return args;
    }
    std::string content = typeName.substr(pos + 1, typeName.size() - pos - 2);
    std::size_t start = 0;
    int depth = 0;
    for (std::size_t i = 0; i < content.size(); ++i) {
        if (content[i] == '<' || content[i] == '(') {
            depth++;
        }
        else if (content[i] == '>' || content[i] == ')') {
            depth--;
        }
        else if (content[i] == ',' && depth == 0) {
            args.push_back(ParseTypeRefFromString(content.substr(start, i - start)));
            start = i + 1;
        }
    }
    if (start < content.size()) {
        args.push_back(ParseTypeRefFromString(content.substr(start)));
    }
    return args;
}

TypeRef AstToHirContext::StringLiteralElementType(const Token &tok) {
    if (tok.text.starts_with("c16\"")) {
        return TypeRef::MakeChar16();
    }
    if (tok.text.starts_with("c32\"")) {
        return TypeRef::MakeChar32();
    }
    return TypeRef::MakeChar8();
}

TypeRef AstToHirContext::StringLiteralType(const Token &tok) {
    return TypeRef::MakeNamed(SliceTypeName(StringLiteralElementType(tok)));
}

TypeRef AstToHirContext::CharLiteralType(const Token &tok) {
    if (tok.text.starts_with("c8'")) {
        return TypeRef::MakeChar8();
    }
    if (tok.text.starts_with("c16'")) {
        return TypeRef::MakeChar16();
    }
    if (tok.text.starts_with("c32'")) {
        return TypeRef::MakeChar32();
    }
    return TypeRef::MakeChar();
}

std::string AstToHirContext::NumericLiteralSuffix(const std::string_view text) {
    return std::string(NumericLiteralSuffixOf(text));
}

std::string AstToHirContext::StripNumericLiteralSuffixImpl(const std::string &text) {
    const std::string suffix = NumericLiteralSuffix(text);
    if (suffix.empty()) {
        return text;
    }
    return text.substr(0, text.size() - suffix.size());
}

std::string AstToHirContext::StripNumericLiteralSuffix(const std::string &text) const {
    return StripNumericLiteralSuffixImpl(text);
}

std::optional<std::uint64_t> AstToHirContext::ParseUnsuffixedIntegerLiteral(const Token &tok) {
    if (tok.kind != TokenKind::IntLiteral || !NumericLiteralSuffix(tok.text).empty()) {
        return std::nullopt;
    }

    std::string text;
    text.reserve(tok.text.size());
    for (const char c : tok.text) {
        if (c != '_') {
            text.push_back(c);
        }
    }

    int base = 10;
    std::string_view digits(text);
    if (digits.size() > 2 && digits[0] == '0') {
        switch (digits[1]) {
        case 'x':
        case 'X':
            base = 16;
            digits.remove_prefix(2);
            break;
        case 'b':
        case 'B':
            base = 2;
            digits.remove_prefix(2);
            break;
        case 'o':
        case 'O':
            base = 8;
            digits.remove_prefix(2);
            break;
        default:
            break;
        }
    }
    if (digits.empty()) {
        return std::nullopt;
    }

    std::uint64_t value = 0;
    const auto *first = digits.data();
    const auto *last = first + digits.size();
    const auto [ptr, ec] = std::from_chars(first, last, value, base);
    if (ec != std::errc{} || ptr != last) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::uint64_t> AstToHirContext::ParseUnsignedIntegerText(const std::string &rawText) {
    std::string text = StripNumericLiteralSuffixImpl(rawText);
    text.erase(std::remove(text.begin(), text.end(), '_'), text.end());
    if (text.empty() || text[0] == '-') {
        return std::nullopt;
    }

    int base = 10;
    std::string_view digits(text);
    if (digits.size() > 2 && digits[0] == '0') {
        switch (digits[1]) {
        case 'x':
        case 'X':
            base = 16;
            digits.remove_prefix(2);
            break;
        case 'b':
        case 'B':
            base = 2;
            digits.remove_prefix(2);
            break;
        case 'o':
        case 'O':
            base = 8;
            digits.remove_prefix(2);
            break;
        default:
            break;
        }
    }
    if (digits.empty()) {
        return std::nullopt;
    }

    std::uint64_t value = 0;
    const auto *first = digits.data();
    const auto *last = first + digits.size();
    const auto [ptr, ec] = std::from_chars(first, last, value, base);
    if (ec != std::errc{} || ptr != last) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::uint64_t> AstToHirContext::LookupConstInteger(const std::string &name) const {
    for (auto it = constIntegerScopes.rbegin(); it != constIntegerScopes.rend(); ++it) {
        if (const auto valueIt = it->find(name); valueIt != it->end()) {
            return valueIt->second;
        }
    }
    return std::nullopt;
}

void AstToHirContext::RegisterConstInteger(const std::string &name, const HirExpr &value) {
    const auto *literal = dynamic_cast<const HirLiteralExpr *>(&value);
    if (!literal) {
        return;
    }
    if (auto parsed = ParseUnsignedIntegerText(literal->value)) {
        constIntegerScopes.back()[name] = *parsed;
    }
}

std::optional<std::int64_t> AstToHirContext::ParseEnumDiscriminant(const std::string &text) {
    std::string cleaned = StripNumericLiteralSuffixImpl(text);
    const bool negative = !cleaned.empty() && cleaned[0] == '-';
    if (negative) {
        cleaned.erase(cleaned.begin());
    }

    std::string digitsText;
    digitsText.reserve(cleaned.size());
    for (const char c : cleaned) {
        if (c != '_') {
            digitsText.push_back(c);
        }
    }

    int base = 10;
    std::string_view digits(digitsText);
    if (digits.size() > 2 && digits[0] == '0') {
        switch (digits[1]) {
        case 'x':
        case 'X':
            base = 16;
            digits.remove_prefix(2);
            break;
        case 'b':
        case 'B':
            base = 2;
            digits.remove_prefix(2);
            break;
        case 'o':
        case 'O':
            base = 8;
            digits.remove_prefix(2);
            break;
        default:
            break;
        }
    }
    if (digits.empty()) {
        return std::nullopt;
    }

    std::uint64_t parsed = 0;
    const auto *first = digits.data();
    const auto *last = first + digits.size();
    const auto [ptr, ec] = std::from_chars(first, last, parsed, base);
    if (ec != std::errc{} || ptr != last) {
        return std::nullopt;
    }
    if (negative) {
        constexpr auto maxMagnitude = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1;
        if (parsed > maxMagnitude) {
            return std::nullopt;
        }
        if (parsed == maxMagnitude) {
            return std::numeric_limits<std::int64_t>::min();
        }
        return -static_cast<std::int64_t>(parsed);
    }
    if (parsed > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(parsed);
}

std::optional<std::uint64_t> AstToHirContext::UnsignedIntegerMax(const TypeRef &type) {
    switch (type.kind) {
    case TypeRef::Kind::UInt8:
        return std::numeric_limits<std::uint8_t>::max();
    case TypeRef::Kind::UInt16:
        return std::numeric_limits<std::uint16_t>::max();
    case TypeRef::Kind::UInt32:
        return std::numeric_limits<std::uint32_t>::max();
    case TypeRef::Kind::UInt64:
    case TypeRef::Kind::UInt:
        return std::numeric_limits<std::uint64_t>::max();
    default:
        return std::nullopt;
    }
}

std::optional<std::pair<std::int64_t, std::int64_t>> AstToHirContext::SignedIntegerRange(const TypeRef &type) {
    switch (type.kind) {
    case TypeRef::Kind::Int8:
        return std::pair{static_cast<std::int64_t>(std::numeric_limits<std::int8_t>::min()),
                         static_cast<std::int64_t>(std::numeric_limits<std::int8_t>::max())};
    case TypeRef::Kind::Int16:
        return std::pair{static_cast<std::int64_t>(std::numeric_limits<std::int16_t>::min()),
                         static_cast<std::int64_t>(std::numeric_limits<std::int16_t>::max())};
    case TypeRef::Kind::Int32:
        return std::pair{static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()),
                         static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())};
    case TypeRef::Kind::Int64:
    case TypeRef::Kind::Int:
        return std::pair{std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()};
    default:
        return std::nullopt;
    }
}

/// Whether an unsuffixed literal is one `target` holds.
///
/// Lowering repeats this check because it re-derives a literal's type without analysis alongside it; both ask the one
/// decoder, so both are exact at every width.
bool AstToHirContext::UnsuffixedIntegerLiteralFits(const Expr &expr, const TypeRef &target) const {
    bool negative = false;
    const LiteralExpr *literal = dynamic_cast<const LiteralExpr *>(&expr);
    if (!literal) {
        if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expr); unary && unary->op == TokenKind::Minus) {
            literal = dynamic_cast<const LiteralExpr *>(unary->operand.get());
        }
        if (!literal) {
            return false;
        }
        negative = true;
    }
    if (literal->token.kind != TokenKind::IntLiteral || !NumericLiteralSuffixOf(literal->token.text).empty()) {
        return false;
    }
    if (!target.IsInteger()) {
        return false;
    }
    const auto bits = PrimitiveBits(target.kind, static_cast<std::uint32_t>(context.target.pointer_size * 8));
    const auto magnitude = DecodeIntegerLiteral(literal->token.text, WideInteger::MaxBits);
    if (!bits || !magnitude) {
        return false;
    }
    return IntegerLiteralFits(*magnitude, negative, *bits, target.IsSigned());
}

bool AstToHirContext::IsNullLiteral(const Expr &expr) {
    const auto *literal = dynamic_cast<const LiteralExpr *>(&expr);
    return literal && literal->token.kind == TokenKind::NullKeyword;
}

std::string AstToHirContext::NamedBaseTypeName(const TypeRef &type) {
    const TypeRef *named = &type;
    if (type.kind == TypeRef::Kind::Pointer && !type.inner.empty()) {
        named = &type.inner[0];
    }
    if (named->kind == TypeRef::Kind::Named) {
        // Keep the full element-specific name for slices so `extend int[]`
        // methods are found on `int[]` receivers (see SemanticAnalyzer).
        if (named->name.starts_with("Slice<")) {
            return named->name;
        }
        return BaseTypeNameImpl(named->name);
    }
    switch (named->kind) {
    case TypeRef::Kind::Bool8:
    case TypeRef::Kind::Bool16:
    case TypeRef::Kind::Bool32:
    case TypeRef::Kind::Char8:
    case TypeRef::Kind::Char16:
    case TypeRef::Kind::Char32:
    case TypeRef::Kind::Int8:
    case TypeRef::Kind::Int16:
    case TypeRef::Kind::Int32:
    case TypeRef::Kind::Int64:
    case TypeRef::Kind::UInt8:

    case TypeRef::Kind::UInt16:
    case TypeRef::Kind::UInt32:
    case TypeRef::Kind::UInt64:
    case TypeRef::Kind::Int:
    case TypeRef::Kind::UInt:
    case TypeRef::Kind::Float32:
    case TypeRef::Kind::Float64:
    case TypeRef::Kind::Str:
        return named->ToString();
    default:
        return {};
    }
}

std::unordered_map<std::string, TypeRef>
AstToHirContext::StructTypeSubstitutions(const StructDecl &decl, const std::vector<TypeExprPtr> &typeArgs) {
    std::unordered_map<std::string, TypeRef> substitutions;
    const std::size_t count = std::min(decl.typeParams.size(), typeArgs.size());
    for (std::size_t i = 0; i < count; ++i) {
        substitutions.emplace(decl.typeParams[i].name, ResolveType(*typeArgs[i]));
    }
    return substitutions;
}

TypeRef AstToHirContext::SuffixedLiteralType(const Token &tok) {
    const NumericLiteralSuffixInfo *suffix = FindNumericLiteralSuffix(NumericLiteralSuffixOf(tok.text));
    if (!suffix) {
        return tok.kind == TokenKind::FloatLiteral ? TypeRef::MakeFloat64() : TypeRef::MakeInt();
    }
    if (suffix->bits == 0) {
        return suffix->isSigned ? TypeRef::MakeInt() : TypeRef::MakeUInt();
    }
    const PrimitiveCategory category = suffix->isFloat  ? PrimitiveCategory::Float
                                     : suffix->isSigned ? PrimitiveCategory::SignedInt
                                                        : PrimitiveCategory::UnsignedInt;
    for (const PrimitiveInfo &primitive : PrimitiveCatalog()) {
        if (primitive.bits == suffix->bits && primitive.category == category) {
            return TypeRef::MakePrimitive(primitive.kind);
        }
    }
    return tok.kind == TokenKind::FloatLiteral ? TypeRef::MakeFloat64() : TypeRef::MakeInt();
}

std::optional<TypeRef> AstToHirContext::BuiltinTypeFromName(const std::string &name) {
    if (name == "opaque") {
        return TypeRef::MakeOpaque();
    }
    return PrimitiveTypeFromName(name);
}

std::optional<TypeRef> AstToHirContext::SliceElementType(const TypeRef &type) const {
    if (type.kind != TypeRef::Kind::Named) {
        return std::nullopt;
    }
    constexpr std::string_view prefix = "Slice<";
    if (!type.name.starts_with(prefix) || type.name.back() != '>') {
        return std::nullopt;
    }
    std::string elemName = type.name.substr(prefix.size(), type.name.size() - prefix.size() - 1);
    if (auto builtin = BuiltinTypeFromName(elemName)) {
        return *builtin;
    }
    return TypeRef::MakeNamed(elemName);
}

std::optional<TypeRef> AstToHirContext::IndexElementType(const TypeRef &type) const {
    if (type.kind == TypeRef::Kind::Array && !type.inner.empty()) {
        return type.inner[0];
    }
    if (auto elemType = SliceElementType(type)) {
        return elemType;
    }
    if (type.kind == TypeRef::Kind::Pointer && !type.inner.empty()) {
        return type.inner[0];
    }
    return std::nullopt;
}

TypeRef AstToHirContext::ResolveType(const TypeExpr &expr) {
    return currentSubstitutions.empty() ? ResolvedType(expr) : ResolveTypeWithSubstitution(expr, currentSubstitutions);
}

TypeRef AstToHirContext::ResolveTypeWithSubstitution(const TypeExpr &expr,
                                                     const std::unordered_map<std::string, TypeRef> &substitutions) {
    if (auto *t = dynamic_cast<const NamedTypeExpr *>(&expr)) {
        if (t->typeArgs.empty()) {
            if (auto it = substitutions.find(t->name); it != substitutions.end()) {
                return it->second;
            }
            return ResolvedType(expr);
        }

        if (t->typeArgs.size() == 1) {
            TypeRef elemType = ResolveTypeWithSubstitution(*t->typeArgs[0], substitutions);
            if (t->name == "Range") {
                return TypeRef::MakeRange(std::move(elemType));
            }
            if (t->name == "RangeInclusive") {
                return TypeRef::MakeRange(std::move(elemType), true, true, true);
            }
            if (t->name == "RangeFrom") {
                return TypeRef::MakeRange(std::move(elemType), true, false);
            }
            if (t->name == "RangeTo") {
                return TypeRef::MakeRange(std::move(elemType), false, true);
            }
            if (t->name == "RangeToInclusive") {
                return TypeRef::MakeRange(std::move(elemType), false, true, true);
            }
        }

        std::vector<TypeRef> resolvedArgs;
        resolvedArgs.reserve(t->typeArgs.size());
        for (const auto &typeArg : t->typeArgs) {
            resolvedArgs.push_back(ResolveTypeWithSubstitution(*typeArg, substitutions));
        }

        // An enum instantiation is composed in exactly one place, so that a type reached through a substitution
        // carries the same layout marker as one resolved directly. Spelling the name out here instead dropped that
        // marker, and lowering then read one enum two ways: an aggregate where a variant was built, a tag and payload
        // packed into a single word where the value was matched.
        if (const auto enumIt = enumDecls.find(t->name); enumIt != enumDecls.end()) {
            return EnumType(*enumIt->second, resolvedArgs);
        }

        TypeRef named = TypeRef::MakeNamed(TypeRef::InstantiationName(t->name, resolvedArgs));
        NoteStructInstantiation(named);
        return named;
    }
    if (auto *t = dynamic_cast<const PointerTypeExpr *>(&expr)) {
        TypeRef pointee = ResolveTypeWithSubstitution(*t->pointee, substitutions);
        pointee.isMut = pointee.isMut || t->pointeeMut;
        return TypeRef::MakePointer(std::move(pointee));
    }
    if (auto *t = dynamic_cast<const ArrayTypeExpr *>(&expr)) {
        return TypeRef::MakeArray(ResolveTypeWithSubstitution(*t->element, substitutions),
                                  ResolvedType(expr).arrayLength);
    }
    if (auto *t = dynamic_cast<const TupleTypeExpr *>(&expr)) {
        std::vector<TypeRef> elems;
        for (auto &elem : t->elements) {
            elems.push_back(ResolveTypeWithSubstitution(*elem, substitutions));
        }
        return TypeRef::MakeTuple(std::move(elems));
    }
    if (auto *t = dynamic_cast<const FunctionTypeExpr *>(&expr)) {
        std::vector<TypeRef> paramTypes;
        paramTypes.reserve(t->params.size());
        for (const auto &param : t->params) {
            paramTypes.push_back(ResolveTypeWithSubstitution(*param, substitutions));
        }
        TypeRef returnType =
            t->returnType ? ResolveTypeWithSubstitution(*t->returnType->get(), substitutions) : TypeRef::MakeOpaque();
        TypeRef functionType = TypeRef::MakeFunc(std::move(paramTypes), std::move(returnType));
        functionType.isVariadic = t->isVariadic;
        return functionType;
    }
    return ResolvedType(expr);
}

TypeRef AstToHirContext::StructFieldType(const TypeRef &objectType, const std::string &fieldName) {
    const std::string typeName = NamedBaseTypeName(objectType);
    if (typeName.empty()) {
        return TypeRef::MakeUnknown();
    }
    const auto structIt = structDecls.find(typeName);
    if (structIt == structDecls.end()) {
        if (const auto unionType = unionDecls.find(typeName); unionType != unionDecls.end()) {
            for (const auto &field : unionType->second->fields) {
                if (field.name == fieldName) {
                    return ResolveType(*field.type);
                }
            }
        }
        return TypeRef::MakeUnknown();
    }

    std::unordered_map<std::string, TypeRef> substitutions;
    std::vector<TypeRef> typeArgs = ParseTypeArgsFromTypeName(objectType.name);
    const auto &params = structIt->second->typeParams;
    const std::size_t count = std::min(params.size(), typeArgs.size());
    for (std::size_t i = 0; i < count; ++i) {
        substitutions.emplace(params[i].name, typeArgs[i]);
    }

    for (const auto &field : structIt->second->fields) {
        if (field.name == fieldName) {
            if (!substitutions.empty()) {
                return ResolveTypeWithSubstitution(*field.type, substitutions);
            }
            return ResolveType(*field.type);
        }
    }
    return TypeRef::MakeUnknown();
}

std::unordered_map<std::string, TypeRef> AstToHirContext::MethodTypeSubstitutions(const TypeRef &receiverType) const {
    const TypeRef *receiver = &receiverType;
    if (receiver->kind == TypeRef::Kind::Pointer && !receiver->inner.empty()) {
        receiver = &receiver->inner[0];
    }
    if (receiver->kind != TypeRef::Kind::Named) {
        return {};
    }

    const auto structIt = structDecls.find(BaseTypeName(receiver->name));
    if (structIt == structDecls.end()) {
        return {};
    }
    const std::vector<TypeRef> args = ParseTypeArgsFromTypeName(receiver->name);
    std::unordered_map<std::string, TypeRef> substitutions;
    const auto &params = structIt->second->typeParams;
    const std::size_t count = std::min(params.size(), args.size());
    for (std::size_t i = 0; i < count; ++i) {
        substitutions.emplace(params[i].name, args[i]);
    }
    return substitutions;
}

TypeRef AstToHirContext::MethodType(const TypeRef &receiverType, const FuncDecl &method) {
    const auto substitutions = MethodTypeSubstitutions(receiverType);
    std::vector<TypeRef> params;
    params.push_back(receiverType);
    for (const auto &param : method.params) {
        if (param.isVariadic || param.name == "self") {
            continue;
        }
        params.push_back(ResolveTypeWithSubstitution(*param.type, substitutions));
    }
    TypeRef ret = method.returnType ? ResolveTypeWithSubstitution(*method.returnType->get(), substitutions)
                                    : TypeRef::MakeOpaque();
    return TypeRef::MakeFunc(std::move(params), std::move(ret));
}

TypeRef AstToHirContext::AssociatedFunctionType(const TypeRef &receiverType, const FuncDecl &method) {
    TypeRef savedSelfType = currentSelfType;
    currentSelfType = receiverType.kind == TypeRef::Kind::Pointer ? receiverType : TypeRef::MakePointer(receiverType);
    const auto substitutions = MethodTypeSubstitutions(receiverType);
    std::vector<TypeRef> params;
    for (const auto &param : method.params) {
        if (param.isVariadic) {
            continue;
        }
        params.push_back(ResolveTypeWithSubstitution(*param.type, substitutions));
    }
    TypeRef ret = method.returnType ? ResolveTypeWithSubstitution(*method.returnType->get(), substitutions)
                                    : TypeRef::MakeOpaque();
    currentSelfType = savedSelfType;
    return TypeRef::MakeFunc(std::move(params), std::move(ret));
}

bool AstToHirContext::MethodIsOverloaded(const std::string &typeName, const std::string &methodName) const {
    const auto typeIt = methodsByType.find(typeName);
    if (typeIt == methodsByType.end()) {
        return false;
    }
    const auto methodIt = typeIt->second.find(methodName);
    return methodIt != typeIt->second.end() && methodIt->second.size() > 1;
}

std::string AstToHirContext::MangleTypeName(const TypeRef &type) {
    std::string out;
    for (const char c : type.ToString()) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            out += c;
        }
        else {
            out += '_';
        }
    }
    return out.empty() ? "_" : out;
}

[[nodiscard]] bool AstToHirContext::MethodIsFromConcreteImpl(const FuncDecl &method) const {
    const auto it = methodImpl.find(&method);
    return it != methodImpl.end() && ImplTypeParams(*it->second).empty();
}

std::string AstToHirContext::ConcreteMethodCalleeName(const std::string &typeName, const TypeRef &receiverType,
                                                      const FuncDecl &method) {
    const auto substitutions = MethodTypeSubstitutions(receiverType);
    if (substitutions.empty() || MethodIsFromConcreteImpl(method)) {
        if (const auto *identity = model.TryGetSymbolIdentity(method)) {
            return identity->linkerName;
        }
        return CalleeName(typeName, method.name, receiverType, method);
    }

    std::string name = CalleeName(typeName, method.name, receiverType, method);
    const auto structIt = structDecls.find(typeName);
    if (structIt != structDecls.end()) {
        for (const auto &param : structIt->second->typeParams) {
            if (const auto it = substitutions.find(param.name); it != substitutions.end()) {
                name += "_" + MangleTypeName(it->second);
            }
        }
    }

    if (generatedMonomorphizedFuncNames.insert(name).second) {
        const TypeRef savedSelfType = currentSelfType;
        currentSelfType =
            receiverType.kind == TypeRef::Kind::Pointer ? receiverType : TypeRef::MakePointer(receiverType);
        monomorphizedFuncs.push_back(LowerFunc(method, /*isMethod=*/true, substitutions, name));
        currentSelfType = savedSelfType;
    }
    return name;
}

const std::string &AstToHirContext::FunctionCalleeName(const FuncDecl &decl) const {
    return RequireSemanticFact(model.TryGetSymbolIdentity(decl)).linkerName;
}

const EnumDecl::Variant *AstToHirContext::LookupEnumVariant(const std::string &enumName,
                                                            const std::string &variantName) const {
    const auto enumIt = enumDecls.find(enumName);
    if (enumIt == enumDecls.end()) {
        return nullptr;
    }
    for (const auto &variant : enumIt->second->variants) {
        if (variant.name == variantName) {
            return &variant;
        }
    }
    return nullptr;
}

std::optional<std::string> AstToHirContext::LookupEnumVariantDiscriminant(const std::string &enumName,
                                                                          const std::string &variantName) const {
    const auto enumIt = enumDecls.find(enumName);
    if (enumIt == enumDecls.end()) {
        return std::nullopt;
    }
    const auto &variants = enumIt->second->variants;
    std::int64_t next = 0;
    for (std::size_t i = 0; i < variants.size(); ++i) {
        std::int64_t value = next;
        if (variants[i].discriminant) {
            if (const auto parsed = ParseEnumDiscriminant(*variants[i].discriminant)) {
                value = *parsed;
            }
        }
        if (variants[i].name == variantName) {
            return std::to_string(value);
        }
        next = value + 1;
    }
    return std::nullopt;
}

TypeRef AstToHirContext::EnumVariantConstructorType(const EnumDecl &decl, const EnumDecl::Variant &variant,
                                                    const std::vector<TypeRef> &typeArgs) {
    std::unordered_map<std::string, TypeRef> substitutions;
    const std::size_t count = std::min(decl.typeParams.size(), typeArgs.size());
    for (std::size_t i = 0; i < count; ++i) {
        substitutions.emplace(decl.typeParams[i].name, typeArgs[i]);
    }
    std::vector<TypeRef> params;
    params.reserve(variant.fields.size() + variant.namedFields.size());
    for (const auto &field : variant.fields) {
        params.push_back(ResolveTypeWithSubstitution(*field, substitutions));
    }
    for (const auto &field : variant.namedFields) {
        params.push_back(ResolveTypeWithSubstitution(*field.type, substitutions));
    }
    return TypeRef::MakeFunc(std::move(params), EnumType(decl, typeArgs));
}

std::string AstToHirContext::CalleeName(const std::string &typeName, const std::string &methodName,
                                        const TypeRef &receiverType, const FuncDecl &decl) {
    if (!MethodIsOverloaded(typeName, methodName)) {
        return typeName + "::" + methodName;
    }
    TypeRef ft = MethodType(receiverType, decl);
    // ft.inner = [selfType, param1, ..., retType]
    std::string name = typeName + "::" + methodName + "__";
    for (std::size_t i = 1; i + 1 < ft.inner.size(); ++i) {
        if (i > 1) {
            name += "_";
        }
        name += MangleTypeName(ft.inner[i]);
    }
    return name;
}

const FuncDecl *AstToHirContext::LookupMethod(const TypeRef &receiverType, const std::string &methodName,
                                              const std::vector<TypeRef> &argTypes) {
    const std::string typeName = NamedBaseTypeName(receiverType);
    if (typeName.empty()) {
        return nullptr;
    }
    const auto typeIt = methodsByType.find(typeName);
    if (typeIt == methodsByType.end()) {
        return nullptr;
    }
    const auto methodIt = typeIt->second.find(methodName);
    if (methodIt == typeIt->second.end()) {
        return nullptr;
    }
    const auto &overloads = methodIt->second;
    if (overloads.empty()) {
        return nullptr;
    }
    // Best-effort scrape for property access (missing args).
    if (argTypes.empty()) {
        return overloads[0];
    }
    if (overloads.size() == 1) {
        // Single candidate: strictly enforce arity/types to prevent
        // silent AST corruption.
        const auto *decl = overloads[0];
        TypeRef ft = MethodType(receiverType, *decl);
        const std::size_t paramCount = ft.inner.size() >= 2 ? ft.inner.size() - 2 : 0;
        if (paramCount != argTypes.size()) {
            return nullptr;
        }
        for (std::size_t i = 0; i < argTypes.size(); ++i) {
            const TypeRef &paramType = ft.inner[i + 1];
            if (argTypes[i].IsUnknown() || paramType.IsUnknown()) {
                continue;
            }
            if (!argTypes[i].IsAssignableTo(paramType) && !(argTypes[i].IsInteger() && paramType.IsInteger())) {
                return nullptr;
            }
        }
        return decl;
    }
    for (const auto *decl : overloads) {
        TypeRef ft = MethodType(receiverType, *decl);
        // ft.inner = [selfType, param1, ..., retType]
        const std::size_t paramCount = ft.inner.size() >= 2 ? ft.inner.size() - 2 : 0;
        if (paramCount != argTypes.size()) {
            continue;
        }
        bool match = true;
        for (std::size_t i = 0; i < argTypes.size(); ++i) {
            const TypeRef &paramType = ft.inner[i + 1];
            if (!argTypes[i].IsUnknown() && !paramType.IsUnknown() && !argTypes[i].IsAssignableTo(paramType) &&
                !(argTypes[i].IsInteger() && paramType.IsInteger())) {
                match = false;
                break;
            }
        }
        if (match) {
            return decl;
        }
    }
    return nullptr;
}

TypeRef AstToHirContext::EnumBaseType(const EnumDecl &decl) {
    return decl.baseType ? ResolveType(*decl.baseType) : TypeRef::MakeInt();
}

TypeRef AstToHirContext::EnumType(const EnumDecl &decl, const std::vector<TypeRef> &typeArgs) {
    TypeRef type = TypeRef::MakeNamed(TypeRef::InstantiationName(decl.name, typeArgs));
    if (decl.typeParams.empty()) {
        // Same rule as SemanticAnalyzer::EnumType, the other builder of this type: an enum that is only a
        // discriminant is the size of that discriminant, and one carrying a payload is wider than its tag, so the
        // marker rather than the tag has to say how large the value is.
        const bool carriesPayload = std::ranges::any_of(decl.variants, [](const EnumDecl::Variant &variant) {
            return !variant.fields.empty() || !variant.namedFields.empty();
        });
        if (carriesPayload) {
            if (const ResolvedTypeLayout *layout = model.TryGetLayout(type)) {
                type.inner.push_back(TypeRef::MakeArray(TypeRef::MakeChar8(), layout->size));
                return type;
            }
        }
        type.inner.push_back(EnumBaseType(decl));
        return type;
    }
    if (typeArgs.size() == decl.typeParams.size()) {
        // The marker is how the instantiation's size travels to lowering, which reads it to decide whether the enum
        // is wider than the word a tag and payload are packed into. Mirrors SemanticAnalyzer::EnumType, the other
        // builder of this type: an instantiation whose layout is not knowable yet -- a type argument still a
        // parameter -- carries no marker from either, and is not lowered until it has been substituted.
        if (const ResolvedTypeLayout *layout = model.TryGetLayout(type)) {
            type.inner.push_back(TypeRef::MakeArray(TypeRef::MakeChar8(), layout->size));
        }
    }
    return type;
}

} // namespace Rux::AstToHirDetail
