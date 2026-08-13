#include "Lowering/AstToHir/AstToHir.h"

#include "Ir/Hir/HirInternal.h"
#include "Lowering/AstToHir/Detail/AstToHirContext.h"
#include "Semantic/PrimitiveConstants.h"
#include "Semantic/SemanticVersion.h"
#include "Target/Platform.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace Rux {
using AstToHirDetail::AstToHirContext;
using AstToHirDetail::HirScope;
using AstToHirDetail::HirSymbol;

static bool UtcTime(std::time_t time, std::tm &out) {
#if RUX_OS_WINDOWS
    return gmtime_s(&out, &time) == 0;
#else
    return gmtime_r(&time, &out) != nullptr;
#endif
}

// Operator → string
std::string_view OpStr(TokenKind op) {
    using TK = TokenKind;
    switch (op) {
    case TK::Plus:
        return "+";
    case TK::Minus:
        return "-";
    case TK::Star:
        return "*";
    case TK::Slash:
        return "/";
    case TK::Percent:
        return "%";
    case TK::StarStar:
        return "**";
    case TK::PlusPlus:
        return "++";
    case TK::MinusMinus:
        return "--";
    case TK::Amp:
        return "&";
    case TK::At:
        return "@";
    case TK::Pipe:
        return "|";
    case TK::Caret:
        return "^";
    case TK::Tilde:
        return "~";
    case TK::LessLess:
        return "<<";
    case TK::GreaterGreater:
        return ">>";
    case TK::GreaterGreaterGreater:
        return ">>>";
    case TK::AmpAmp:
        return "&&";
    case TK::PipePipe:
        return "||";
    case TK::Bang:
        return "!";
    case TK::Equal:
        return "==";
    case TK::BangEqual:
        return "!=";
    case TK::Less:
        return "<";
    case TK::LessEqual:
        return "<=";
    case TK::Greater:
        return ">";
    case TK::GreaterEqual:
        return ">=";
    case TK::Assign:
        return "=";
    case TK::PlusAssign:
        return "+=";
    case TK::MinusAssign:
        return "-=";
    case TK::StarAssign:
        return "*=";
    case TK::SlashAssign:
        return "/=";
    case TK::PercentAssign:
        return "%=";
    case TK::AmpAssign:
        return "&=";
    case TK::PipeAssign:
        return "|=";
    case TK::CaretAssign:
        return "^=";
    case TK::LessLessAssign:
        return "<<=";
    case TK::GreaterGreaterAssign:
        return ">>=";
    case TK::GreaterGreaterGreaterAssign:
        return ">>>=";
    default:
        return "?";
    }
}

// Internal: Lowering
class Lowering final : public AstToHirContext {
public:
    explicit Lowering(const SemanticModel &inputModel)
        : AstToHirContext(inputModel, inputModel.modules, inputModel.compileTimeContext) {
    }

private:
    template <typename Fact>
    [[nodiscard]] static const Fact &RequireSemanticFact(const Fact *fact) {
        assert(fact != nullptr && "accepted AST node is missing a required semantic fact");
        if (!fact) {
            std::abort();
        }
        return *fact;
    }

    [[nodiscard]] const TypeRef &ResolvedType(const TypeExpr &type) const {
        return RequireSemanticFact(model.TryGetType(type));
    }

    [[nodiscard]] const ResolvedTypeLayout &ResolvedLayout(const TypeRef &type) const {
        return RequireSemanticFact(model.TryGetLayout(type));
    }

    [[nodiscard]] std::uint64_t ResolvedSizeOf(const SizeOfExpr &expression) {
        if (const std::uint64_t *value = model.TryGetSizeOfValue(expression)) {
            return *value;
        }

        const TypeRef type = ResolveTypeWithSubstitution(*expression.type, currentSubstitutions);
        if (type.kind == TypeRef::Kind::TypeParam) {
            // Generic templates are retained in HIR but never emitted. Keep
            // their historical placeholder; each concrete instance below is
            // required to have a validated semantic layout fact.
            assert(currentSubstitutions.empty());
            return 0;
        }
        return ResolvedLayout(type).size;
    }

    TypeRef MakeFuncTypeWithSubstitution(const std::vector<Param> &params, const std::optional<TypeExprPtr> &returnType,
                                         const std::unordered_map<std::string, TypeRef> &substitutions,
                                         const std::vector<std::string> &typeParams = {}) override {
        auto savedTypeParams = currentTypeParams;
        currentTypeParams = typeParams;
        auto savedSubstitutions = currentSubstitutions;
        currentSubstitutions = substitutions;

        std::vector<TypeRef> paramTypes;
        for (const auto &param : params) {
            if (!param.isVariadic) {
                paramTypes.push_back(ResolveType(*param.type));
            }
        }
        TypeRef ret = returnType ? ResolveType(*returnType->get()) : TypeRef::MakeOpaque();

        currentTypeParams = savedTypeParams;
        currentSubstitutions = savedSubstitutions;
        return TypeRef::MakeFunc(std::move(paramTypes), std::move(ret));
    }

    // Type resolution
    std::string GenericTypeName(const NamedTypeExpr &type) {
        std::string name = type.name;
        if (!type.typeArgs.empty()) {
            name += "<";
            for (std::size_t i = 0; i < type.typeArgs.size(); ++i) {
                if (i) {
                    name += ", ";
                }
                name += ResolveType(*type.typeArgs[i]).ToString();
            }
            name += ">";
        }
        return name;
    }

    std::string GenericStructInitName(const StructInitExpr &expr) {
        std::string name = expr.typeName;
        if (!expr.typeArgs.empty()) {
            name += "<";
            for (std::size_t i = 0; i < expr.typeArgs.size(); ++i) {
                if (i) {
                    name += ", ";
                }
                name += ResolveType(*expr.typeArgs[i]).ToString();
            }
            name += ">";
        }
        return name;
    }

    std::pair<const EnumDecl *, const EnumDecl::Variant *>
    LookupEnumVariantInitializer(const std::string &typeName) const {
        const std::size_t sep = typeName.find("::");
        if (sep == std::string::npos || typeName.find("::", sep + 2) != std::string::npos) {
            return {nullptr, nullptr};
        }

        const std::string enumName = typeName.substr(0, sep);
        const std::string variantName = typeName.substr(sep + 2);
        const auto enumIt = enumDecls.find(enumName);
        if (enumIt == enumDecls.end()) {
            return {nullptr, nullptr};
        }
        for (const auto &variant : enumIt->second->variants) {
            if (variant.name == variantName) {
                return {enumIt->second, &variant};
            }
        }
        return {enumIt->second, nullptr};
    }

    static std::string SliceTypeName(const TypeRef &elemType) {
        return "Slice<" + elemType.ToString() + ">";
    }

    static std::string BaseTypeNameImpl(const std::string &name) {
        const std::size_t pos = name.find('<');
        return pos == std::string::npos ? name : name.substr(0, pos);
    }

    std::string BaseTypeName(const std::string &name) const override {
        return BaseTypeNameImpl(name);
    }

    std::vector<std::string> ImplTypeParams(const ImplDecl &decl) const override {
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
            if (arg && arg->typeArgs.empty() && arg->name == structParams[i]) {
                params.push_back(arg->name);
            }
        }
        return params;
    }

    static TypeRef ParseTypeRefFromString(std::string str) {
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
        if (str == "bool8" || str == "bool") {
            return TypeRef::MakeBool8();
        }
        if (str == "bool16") {
            return TypeRef::MakeBool16();
        }
        if (str == "bool32") {
            return TypeRef::MakeBool32();
        }
        if (str == "char8") {
            return TypeRef::MakeChar8();
        }
        if (str == "char16") {
            return TypeRef::MakeChar16();
        }
        if (str == "char32" || str == "char") {
            return TypeRef::MakeChar32();
        }
        if (str == "String") {
            return TypeRef::MakeStr();
        }
        if (str == "int8") {
            return TypeRef::MakeInt8();
        }
        if (str == "int16") {
            return TypeRef::MakeInt16();
        }
        if (str == "int32") {
            return TypeRef::MakeInt32();
        }
        if (str == "int64") {
            return TypeRef::MakeInt64();
        }
        if (str == "int") {
            return TypeRef::MakeInt();
        }
        if (str == "byte" || str == "uint8") {
            return TypeRef::MakeUInt8();
        }
        if (str == "uint16") {
            return TypeRef::MakeUInt16();
        }
        if (str == "uint32") {
            return TypeRef::MakeUInt32();
        }
        if (str == "uint64") {
            return TypeRef::MakeUInt64();
        }
        if (str == "uint") {
            return TypeRef::MakeUInt();
        }
        if (str == "float32") {
            return TypeRef::MakeFloat32();
        }
        if (str == "float64" || str == "float") {
            return TypeRef::MakeFloat64();
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

    std::vector<TypeRef> ParseTypeArgsFromTypeName(const std::string &typeName) const override {
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

    static TypeRef StringLiteralElementType(const Token &tok) {
        if (tok.text.starts_with("c16\"")) {
            return TypeRef::MakeChar16();
        }
        if (tok.text.starts_with("c32\"")) {
            return TypeRef::MakeChar32();
        }
        return TypeRef::MakeChar8();
    }

    static TypeRef StringLiteralType(const Token &tok) {
        return TypeRef::MakeNamed(SliceTypeName(StringLiteralElementType(tok)));
    }

    static TypeRef CharLiteralType(const Token &tok) {
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

    static std::string NumericLiteralSuffix(std::string_view text) {
        static constexpr std::string_view suffixes[] = {"i8",  "i16", "i32", "i64", "u8", "u16",
                                                        "u32", "u64", "f32", "f64", "i",  "u"};
        for (auto suffix : suffixes) {
            if (text.size() > suffix.size() && text.substr(text.size() - suffix.size()) == suffix) {
                return std::string(suffix);
            }
        }
        return {};
    }

    static std::string StripNumericLiteralSuffixImpl(const std::string &text) {
        const std::string suffix = NumericLiteralSuffix(text);
        if (suffix.empty()) {
            return text;
        }
        return text.substr(0, text.size() - suffix.size());
    }

    std::string StripNumericLiteralSuffix(const std::string &text) const override {
        return StripNumericLiteralSuffixImpl(text);
    }

    static std::optional<std::uint64_t> ParseUnsuffixedIntegerLiteral(const Token &tok) {
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

    static std::optional<std::uint64_t> ParseUnsignedIntegerText(const std::string &rawText) {
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

    std::optional<std::uint64_t> LookupConstInteger(const std::string &name) const {
        for (auto it = constIntegerScopes.rbegin(); it != constIntegerScopes.rend(); ++it) {
            if (const auto valueIt = it->find(name); valueIt != it->end()) {
                return valueIt->second;
            }
        }
        return std::nullopt;
    }

    void RegisterConstInteger(const std::string &name, const HirExpr &value) {
        const auto *literal = dynamic_cast<const HirLiteralExpr *>(&value);
        if (!literal) {
            return;
        }
        if (auto parsed = ParseUnsignedIntegerText(literal->value)) {
            constIntegerScopes.back()[name] = *parsed;
        }
    }

    static std::optional<std::int64_t> ParseEnumDiscriminant(const std::string &text) {
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

    static std::optional<std::uint64_t> UnsignedIntegerMax(const TypeRef &type) {
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

    static std::optional<std::pair<std::int64_t, std::int64_t>> SignedIntegerRange(const TypeRef &type) {
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

    static bool UnsuffixedIntegerLiteralFits(const Expr &expr, const TypeRef &target) {
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

        const auto value = ParseUnsuffixedIntegerLiteral(literal->token);
        if (!value) {
            return false;
        }

        if (negative) {
            const auto range = SignedIntegerRange(target);
            if (!range) {
                return false;
            }
            const auto minMagnitude = static_cast<std::uint64_t>(-(range->first + 1)) + 1;
            return *value <= minMagnitude;
        }

        if (const auto max = UnsignedIntegerMax(target)) {
            return *value <= *max;
        }
        if (const auto range = SignedIntegerRange(target)) {
            return *value <= static_cast<std::uint64_t>(range->second);
        }
        return false;
    }

    static bool IsNullLiteral(const Expr &expr) {
        const auto *literal = dynamic_cast<const LiteralExpr *>(&expr);
        return literal && literal->token.kind == TokenKind::NullKeyword;
    }

    static std::string NamedBaseTypeName(const TypeRef &type) {
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

    std::unordered_map<std::string, TypeRef> StructTypeSubstitutions(const StructDecl &decl,
                                                                     const std::vector<TypeExprPtr> &typeArgs) {
        std::unordered_map<std::string, TypeRef> substitutions;
        const std::size_t count = std::min(decl.typeParams.size(), typeArgs.size());
        for (std::size_t i = 0; i < count; ++i) {
            substitutions.emplace(decl.typeParams[i], ResolveType(*typeArgs[i]));
        }
        return substitutions;
    }

    static TypeRef SuffixedLiteralType(const Token &tok) {
        const std::string suffix = NumericLiteralSuffix(tok.text);
        if (suffix == "i8") {
            return TypeRef::MakeInt8();
        }
        if (suffix == "i16") {
            return TypeRef::MakeInt16();
        }
        if (suffix == "i32") {
            return TypeRef::MakeInt32();
        }
        if (suffix == "i64") {
            return TypeRef::MakeInt64();
        }
        if (suffix == "i") {
            return TypeRef::MakeInt();
        }
        if (suffix == "u8") {
            return TypeRef::MakeUInt8();
        }
        if (suffix == "u16") {
            return TypeRef::MakeUInt16();
        }
        if (suffix == "u32") {
            return TypeRef::MakeUInt32();
        }
        if (suffix == "u64") {
            return TypeRef::MakeUInt64();
        }
        if (suffix == "u") {
            return TypeRef::MakeUInt();
        }
        if (suffix == "f32") {
            return TypeRef::MakeFloat32();
        }
        if (suffix == "f64") {
            return TypeRef::MakeFloat64();
        }
        return tok.kind == TokenKind::FloatLiteral ? TypeRef::MakeFloat64() : TypeRef::MakeInt();
    }

    static std::optional<TypeRef> BuiltinTypeFromName(const std::string &name) {
        if (name == "opaque") {
            return TypeRef::MakeOpaque();
        }
        if (name == "bool" || name == "bool8") {
            return TypeRef::MakeBool8();
        }
        if (name == "bool16") {
            return TypeRef::MakeBool16();
        }
        if (name == "bool32") {
            return TypeRef::MakeBool32();
        }
        if (name == "char" || name == "char32") {
            return TypeRef::MakeChar32();
        }
        if (name == "char8") {
            return TypeRef::MakeChar8();
        }
        if (name == "char16") {
            return TypeRef::MakeChar16();
        }
        if (name == "int8") {
            return TypeRef::MakeInt8();
        }
        if (name == "int16") {
            return TypeRef::MakeInt16();
        }
        if (name == "int32") {
            return TypeRef::MakeInt32();
        }
        if (name == "int64") {
            return TypeRef::MakeInt64();
        }
        if (name == "int") {
            return TypeRef::MakeInt();
        }
        if (name == "byte" || name == "uint8") {
            return TypeRef::MakeUInt8();
        }
        if (name == "uint16") {
            return TypeRef::MakeUInt16();
        }
        if (name == "uint32") {
            return TypeRef::MakeUInt32();
        }
        if (name == "uint64") {
            return TypeRef::MakeUInt64();
        }
        if (name == "uint") {
            return TypeRef::MakeUInt();
        }
        if (name == "float32") {
            return TypeRef::MakeFloat32();
        }
        if (name == "float64") {
            return TypeRef::MakeFloat64();
        }
        if (name == "float") {
            return TypeRef::MakeFloat();
        }
        return std::nullopt;
    }

    static std::optional<TypeRef> SliceElementType(const TypeRef &type) {
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

    std::optional<TypeRef> IndexElementType(const TypeRef &type) const override {
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

    TypeRef ResolveType(const TypeExpr &expr) override {
        return currentSubstitutions.empty() ? ResolvedType(expr)
                                            : ResolveTypeWithSubstitution(expr, currentSubstitutions);
    }

    TypeRef ResolveTypeWithSubstitution(const TypeExpr &expr,
                                        const std::unordered_map<std::string, TypeRef> &substitutions) override {
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

            TypeRef named = TypeRef::MakeNamed(t->name);
            named.name += "<";
            for (std::size_t i = 0; i < t->typeArgs.size(); ++i) {
                if (i) {
                    named.name += ", ";
                }
                named.name += ResolveTypeWithSubstitution(*t->typeArgs[i], substitutions).ToString();
            }
            named.name += ">";
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
            TypeRef returnType = t->returnType ? ResolveTypeWithSubstitution(*t->returnType->get(), substitutions)
                                               : TypeRef::MakeOpaque();
            TypeRef functionType = TypeRef::MakeFunc(std::move(paramTypes), std::move(returnType));
            functionType.isVariadic = t->isVariadic;
            return functionType;
        }
        return ResolvedType(expr);
    }

    TypeRef StructFieldType(const TypeRef &objectType, const std::string &fieldName) {
        const std::string typeName = NamedBaseTypeName(objectType);
        if (typeName.empty()) {
            return TypeRef::MakeUnknown();
        }
        const auto structIt = structDecls.find(typeName);
        if (structIt == structDecls.end()) {
            return TypeRef::MakeUnknown();
        }

        std::unordered_map<std::string, TypeRef> substitutions;
        std::vector<TypeRef> typeArgs = ParseTypeArgsFromTypeName(objectType.name);
        const auto &params = structIt->second->typeParams;
        const std::size_t count = std::min(params.size(), typeArgs.size());
        for (std::size_t i = 0; i < count; ++i) {
            substitutions.emplace(params[i], typeArgs[i]);
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

    std::unordered_map<std::string, TypeRef> MethodTypeSubstitutions(const TypeRef &receiverType) const {
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
            substitutions.emplace(params[i], args[i]);
        }
        return substitutions;
    }

    TypeRef MethodType(const TypeRef &receiverType, const FuncDecl &method) override {
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

    TypeRef AssociatedFunctionType(const TypeRef &receiverType, const FuncDecl &method) override {
        TypeRef savedSelfType = currentSelfType;
        currentSelfType =
            receiverType.kind == TypeRef::Kind::Pointer ? receiverType : TypeRef::MakePointer(receiverType);
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

    bool MethodIsOverloaded(const std::string &typeName, const std::string &methodName) const {
        const auto typeIt = methodsByType.find(typeName);
        if (typeIt == methodsByType.end()) {
            return false;
        }
        const auto methodIt = typeIt->second.find(methodName);
        return methodIt != typeIt->second.end() && methodIt->second.size() > 1;
    }

    static std::string MangleTypeName(const TypeRef &type) {
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

    // Whether the `extend` block a method was declared in already names every
    // type argument its receiver has — `extend Slice<int>` rather than
    // `extend List<T>`. Such a block is lowered as it stands, so its methods
    // need no monomorphization even though the receiver substitutes a struct's
    // type parameter.
    [[nodiscard]] bool MethodIsFromConcreteImpl(const FuncDecl &method) const override {
        const auto it = methodImpl.find(&method);
        return it != methodImpl.end() && ImplTypeParams(*it->second).empty();
    }

    std::string ConcreteMethodCalleeName(const std::string &typeName, const TypeRef &receiverType,
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
                if (const auto it = substitutions.find(param); it != substitutions.end()) {
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

    const std::string &FunctionCalleeName(const FuncDecl &decl) const override {
        return RequireSemanticFact(model.TryGetSymbolIdentity(decl)).linkerName;
    }

    const EnumDecl::Variant *LookupEnumVariant(const std::string &enumName,
                                               const std::string &variantName) const override {
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

    std::optional<std::string> LookupEnumVariantDiscriminant(const std::string &enumName,
                                                             const std::string &variantName) const override {
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

    TypeRef EnumVariantConstructorType(const EnumDecl &decl, const EnumDecl::Variant &variant,
                                       const std::vector<TypeRef> &typeArgs = {}) override {
        std::unordered_map<std::string, TypeRef> substitutions;
        const std::size_t count = std::min(decl.typeParams.size(), typeArgs.size());
        for (std::size_t i = 0; i < count; ++i) {
            substitutions.emplace(decl.typeParams[i], typeArgs[i]);
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

    // Returns the mangled callee name: "Type::method__p1_p2" for overloads,
    // "Type::method" for single-dispatch methods.
    std::string CalleeName(const std::string &typeName, const std::string &methodName, const TypeRef &receiverType,
                           const FuncDecl &decl) {
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

    const FuncDecl *LookupMethod(const TypeRef &receiverType, const std::string &methodName,
                                 const std::vector<TypeRef> &argTypes = {}) {
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

    std::optional<TypeRef> InterfaceImplementationType(const TypeRef &exprType, const TypeRef &targetType) const {
        if (targetType.kind != TypeRef::Kind::Named) {
            return std::nullopt;
        }
        auto hasVtable = [&](const TypeRef &type) {
            auto typeIt = typeInterfaceVtables.find(type.ToString());
            return typeIt != typeInterfaceVtables.end() && typeIt->second.contains(targetType.name);
        };
        if (hasVtable(exprType)) {
            return exprType;
        }
        if (exprType.kind == TypeRef::Kind::Int && hasVtable(TypeRef::MakeInt64())) {
            return TypeRef::MakeInt64();
        }
        if (exprType.kind == TypeRef::Kind::Int64 && hasVtable(TypeRef::MakeInt())) {
            return TypeRef::MakeInt();
        }
        if (exprType.kind == TypeRef::Kind::UInt && hasVtable(TypeRef::MakeUInt64())) {
            return TypeRef::MakeUInt64();
        }
        if (exprType.kind == TypeRef::Kind::UInt64 && hasVtable(TypeRef::MakeUInt())) {
            return TypeRef::MakeUInt();
        }
        return std::nullopt;
    }

    TypeRef EnumBaseType(const EnumDecl &decl) {
        return decl.baseType ? ResolveType(*decl.baseType) : TypeRef::MakeInt();
    }

    TypeRef EnumType(const EnumDecl &decl, const std::vector<TypeRef> &typeArgs = {}) override {
        std::string name = decl.name;
        if (!typeArgs.empty()) {
            name += '<';
            for (std::size_t i = 0; i < typeArgs.size(); ++i) {
                if (i) {
                    name += ", ";
                }
                name += typeArgs[i].ToString();
            }
            name += '>';
        }

        TypeRef type = TypeRef::MakeNamed(std::move(name));
        if (decl.typeParams.empty()) {
            type.inner.push_back(EnumBaseType(decl));
            return type;
        }
        if (typeArgs.size() == decl.typeParams.size()) {
            type.inner.push_back(TypeRef::MakeArray(TypeRef::MakeChar8(), ResolvedLayout(type).size));
        }
        return type;
    }

    static std::uint32_t DecodeUtf8CodePoint(const std::string &text, std::size_t i) {
        const auto byte = [&](std::size_t offset) {
            return static_cast<std::uint32_t>(static_cast<unsigned char>(text[i + offset]));
        };

        const std::uint32_t b0 = byte(0);
        if ((b0 & 0x80u) == 0) {
            return b0;
        }
        if ((b0 & 0xE0u) == 0xC0u && i + 1 < text.size()) {
            return ((b0 & 0x1Fu) << 6) | (byte(1) & 0x3Fu);
        }
        if ((b0 & 0xF0u) == 0xE0u && i + 2 < text.size()) {
            return ((b0 & 0x0Fu) << 12) | ((byte(1) & 0x3Fu) << 6) | (byte(2) & 0x3Fu);
        }
        if ((b0 & 0xF8u) == 0xF0u && i + 3 < text.size()) {
            return ((b0 & 0x07u) << 18) | ((byte(1) & 0x3Fu) << 12) | ((byte(2) & 0x3Fu) << 6) | (byte(3) & 0x3Fu);
        }
        return b0;
    }

    // Appends `cp` to `out` encoded as UTF-8.
    static void AppendUtf8(std::string &out, std::uint32_t cp) {
        if (cp <= 0x7F) {
            out += static_cast<char>(cp);
        }
        else if (cp <= 0x7FF) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else if (cp <= 0xFFFF) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    // Decodes a `\u{...}` escape body. `uPos` is the index of the 'u'. On
    // success writes the code point to `cp` and returns the index of the
    // closing '}'; on a malformed body it returns `uPos` unchanged. The
    // lexer already validates these escapes, so the failure path is purely
    // defensive.
    static std::size_t ParseUnicodeEscape(const std::string &text, std::size_t uPos, std::uint32_t &cp) {
        std::size_t j = uPos + 1;
        if (j >= text.size() || text[j] != '{') {
            return uPos;
        }
        ++j;
        std::uint32_t value = 0;
        std::size_t digits = 0;
        for (; j < text.size() && text[j] != '}'; ++j, ++digits) {
            const char h = text[j];
            if (h >= '0' && h <= '9') {
                value = (value << 4) | static_cast<std::uint32_t>(h - '0');
            }
            else if (h >= 'a' && h <= 'f') {
                value = (value << 4) | static_cast<std::uint32_t>(h - 'a' + 10);
            }
            else if (h >= 'A' && h <= 'F') {
                value = (value << 4) | static_cast<std::uint32_t>(h - 'A' + 10);
            }
            else {
                return uPos;
            }
        }
        if (digits == 0 || j >= text.size() || text[j] != '}') {
            return uPos;
        }
        cp = value;
        return j;
    }

    static std::string DecodeCharLiteral(const std::string &text) {
        // text is raw source like 'A' or '\n'; strip quotes and decode.
        std::uint32_t cp = 0;
        const std::size_t quote = text.find('\'');
        if (quote != std::string::npos && quote + 1 < text.size()) {
            std::size_t i = quote + 1; // skip opening '
            if (text[i] == '\\' && i + 1 < text.size()) {
                switch (text[i + 1]) {
                case 'n':
                    cp = '\n';
                    break;
                case 't':
                    cp = '\t';
                    break;
                case 'r':
                    cp = '\r';
                    break;
                case 'a':
                    cp = '\a';
                    break;
                case 'b':
                    cp = '\b';
                    break;
                case 'f':
                    cp = '\f';
                    break;
                case 'v':
                    cp = '\v';
                    break;
                case '0':
                    cp = 0;
                    break;
                case '\\':
                    cp = '\\';
                    break;
                case '\'':
                    cp = '\'';
                    break;
                case '"':
                    cp = '"';
                    break;
                case 'u': {
                    // \u{XXXX} — Unicode escape ('u' sits at i + 1)
                    std::uint32_t u = 0;
                    if (ParseUnicodeEscape(text, i + 1, u) != i + 1) {
                        cp = u;
                    }
                    break;
                }
                default:
                    cp = static_cast<unsigned char>(text[i + 1]);
                    break;
                }
            }
            else if (text[i] != '\'') {
                cp = DecodeUtf8CodePoint(text, i);
            }
        }
        return std::to_string(cp);
    }

    static std::string DecodeStringLiteral(const std::string &text) {
        // text is raw source like "hello\n" — strip quotes and decode
        // escapes
        std::string out;
        if (text.size() < 2) {
            return out;
        }
        const std::size_t quote = text.find('"');
        if (quote == std::string::npos) {
            return out;
        }
        for (std::size_t i = quote + 1; i + 1 < text.size(); ++i) {
            if (text[i] != '\\') {
                out += text[i];
                continue;
            }
            if (++i + 1 > text.size()) {
                break;
            }
            switch (text[i]) {
            case 'n':
                out += '\n';
                break;
            case 't':
                out += '\t';
                break;
            case 'r':
                out += '\r';
                break;
            case 'a':
                out += '\a';
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'v':
                out += '\v';
                break;
            case '0':
                out += '\0';
                break;
            case '\\':
                out += '\\';
                break;
            case '\'':
                out += '\'';
                break;
            case '"':
                out += '"';
                break;
            case 'u': {
                // \u{XXXX} — Unicode escape, encoded as UTF-8 ('u' sits at
                // i)
                std::uint32_t u = 0;
                if (const std::size_t end = ParseUnicodeEscape(text, i, u); end != i) {
                    AppendUtf8(out, u);
                    i = end; // the loop's ++i then steps past the closing
                    // '}'
                }
                break;
            }
            default:
                break;
            }
        }
        return out;
    }

    TypeRef LiteralType(const Token &tok) const override {
        switch (tok.kind) {
        case TokenKind::IntLiteral:
        case TokenKind::FloatLiteral:
            return SuffixedLiteralType(tok);
        case TokenKind::StringLiteral:
            return StringLiteralType(tok);
        case TokenKind::CharLiteral:
            return CharLiteralType(tok);
        case TokenKind::BoolLiteral:
            return TypeRef::MakeBool();
        default:
            return TypeRef::MakeUnknown();
        }
    }

    std::vector<HirParam> LowerParams(const std::vector<Param> &params) {
        std::vector<HirParam> out;
        out.reserve(params.size());
        for (const auto &p : params) {
            HirParam hp;
            hp.name = p.name;
            hp.isVariadic = p.isVariadic;
            hp.type = p.isVariadic ? TypeRef::MakeNamed(SliceTypeName(ResolveType(*p.type))) : ResolveType(*p.type);
            out.push_back(std::move(hp));
        }
        return out;
    }

    // Declaration lowering

    HirFunc LowerFunc(const FuncDecl &d, bool isMethod = false,
                      const std::unordered_map<std::string, TypeRef> &substitutions = {},
                      const std::string &overrideName = "") override {
        auto savedTypeParams = currentTypeParams;
        currentTypeParams = substitutions.empty() ? d.typeParams : std::vector<std::string>{};
        auto savedSubstitutions = currentSubstitutions;
        currentSubstitutions = substitutions;
        TypeRef retType = d.returnType ? ResolveType(**d.returnType) : TypeRef::MakeOpaque();
        auto savedRet = currentReturnType;
        currentReturnType = retType;
        auto savedFuncName = currentFunctionName;
        if (isMethod) {
            currentFunctionName = NamedBaseTypeName(currentSelfType) + "::" + d.name;
        }
        else {
            currentFunctionName = declModulePath.empty() ? d.name : declModulePath + "::" + d.name;
        }
        PushScope();
        if (substitutions.empty()) {
            for (const auto &tp : d.typeParams) {
                HirSymbol sym;
                sym.kind = HirSymbol::Kind::Type;
                sym.name = tp;
                sym.type = TypeRef::MakeTypeParam(tp);
                Define(sym);
            }
        }
        if (isMethod) {
            HirSymbol self;
            self.kind = HirSymbol::Kind::Var;
            self.name = "self";
            self.type = currentSelfType.IsUnknown() ? TypeRef::MakeNamed("self") : currentSelfType;
            self.isMut = true;
            Define(self);
        }
        for (const auto &param : d.params) {
            if (param.name == "self") {
                continue;
            }
            HirSymbol sym;
            sym.kind = HirSymbol::Kind::Var;
            sym.name = param.name;
            sym.type = param.isVariadic ? TypeRef::MakeNamed(SliceTypeName(ResolveType(*param.type)))
                                        : ResolveType(*param.type);
            sym.isMut = param.isMut;
            Define(sym);
        }
        std::optional<HirBlock> body;
        if (d.body) {
            body = LowerBlock(*d.body);
        }
        HirFunc hf;
        hf.name = overrideName.empty() ? d.name : overrideName;
        hf.isPublic = d.isPublic;
        hf.isAsm = d.isAsm;
        hf.isNoReturn = d.isNoReturn;
        hf.asmBody = d.asmBody;
        hf.callConv = d.callConv;
        hf.typeParams = substitutions.empty() ? d.typeParams : std::vector<std::string>{};
        hf.params = LowerParams(d.params);
        hf.returnType = retType;
        hf.body = std::move(body);
        hf.location = d.location;

        PopScope();
        currentReturnType = savedRet;
        currentTypeParams = savedTypeParams;
        currentSubstitutions = savedSubstitutions;
        currentFunctionName = savedFuncName;
        return hf;
    }

    HirStruct LowerStruct(const StructDecl &d) override {
        auto savedTypeParams = currentTypeParams;
        currentTypeParams = d.typeParams;
        PushScope();
        for (const auto &tp : d.typeParams) {
            HirSymbol sym;
            sym.kind = HirSymbol::Kind::Type;
            sym.name = tp;
            sym.type = TypeRef::MakeTypeParam(tp);
            Define(sym);
        }
        HirStruct hs;
        hs.name = d.name;
        hs.isPublic = d.isPublic;
        hs.typeParams = d.typeParams;
        hs.location = d.location;
        for (const auto &f : d.fields) {
            HirStructField hf;
            hf.name = f.name;
            hf.isPublic = f.isPublic;
            hf.type = ResolveType(*f.type);
            hs.fields.push_back(std::move(hf));
        }
        PopScope();
        currentTypeParams = savedTypeParams;
        return hs;
    }

    HirEnum LowerEnum(const EnumDecl &d) override {
        const auto savedTypeParams = currentTypeParams;
        currentTypeParams.insert(currentTypeParams.end(), d.typeParams.begin(), d.typeParams.end());
        HirEnum he;
        he.name = d.name;
        he.isPublic = d.isPublic;
        he.typeParams = d.typeParams;
        he.baseType = EnumBaseType(d);
        he.location = d.location;
        std::int64_t next = 0;
        for (const auto &v : d.variants) {
            HirEnumVariant hv;
            hv.name = v.name;
            std::int64_t value = next;
            if (v.discriminant) {
                if (const auto parsed = ParseEnumDiscriminant(*v.discriminant)) {
                    value = *parsed;
                }
            }
            hv.discriminant = std::to_string(value);
            next = value + 1;
            for (const auto &f : v.fields) {
                hv.fields.push_back(ResolveType(*f));
            }
            for (const auto &f : v.namedFields) {
                hv.fields.push_back(ResolveType(*f.type));
            }
            he.variants.push_back(std::move(hv));
        }
        currentTypeParams = savedTypeParams;
        return he;
    }

    HirUnion LowerUnion(const UnionDecl &d) override {
        HirUnion hu;
        hu.name = d.name;
        hu.isPublic = d.isPublic;
        hu.location = d.location;
        for (const auto &f : d.fields) {
            HirUnionField hf;
            hf.name = f.name;
            hf.type = ResolveType(*f.type);
            hu.fields.push_back(std::move(hf));
        }
        return hu;
    }

    HirInterface LowerInterface(const InterfaceDecl &d) override {
        HirInterface hi;
        hi.name = d.name;
        hi.isPublic = d.isPublic;
        hi.location = d.location;
        for (const auto &m : d.methods) {
            HirInterfaceMethod hm;
            hm.name = m->name;
            hm.location = m->location;
            hm.returnType = m->returnType ? ResolveType(**m->returnType) : TypeRef::MakeOpaque();
            hm.params = LowerParams(m->params);
            hi.methods.push_back(std::move(hm));
        }
        return hi;
    }

    HirImplBlock LowerImpl(const ImplDecl &d) override {
        bool savedInImpl = inImpl;
        TypeRef savedSelfType = currentSelfType;
        inImpl = true;
        TypeRef extendedType = d.extendedType ? ResolveType(*d.extendedType) : TypeRef::MakeUnknown();
        const bool isSliceReceiver =
            extendedType.kind == TypeRef::Kind::Array ||
            (extendedType.kind == TypeRef::Kind::Named && extendedType.name.starts_with("Slice<"));
        if (isSliceReceiver) {
            // `self` is the slice value; the slice ABI passes its address, so
            // slice indexing and iteration inside the method work as usual.
            currentSelfType = extendedType;
        }
        else {
            TypeRef selfBase = extendedType.IsUnknown() ? TypeRef::MakeNamed(d.typeName) : extendedType;
            currentSelfType = TypeRef::MakePointer(selfBase);
        }

        HirImplBlock hib;
        hib.typeName = d.typeName.starts_with("Slice<") ? d.typeName : BaseTypeName(d.typeName);
        hib.interfaceName = d.interfaceName;
        hib.location = d.location;
        for (const auto &m : d.methods) {
            if (!m->intrinsicName.empty() && !m->body && !m->isAsm) {
                continue;
            }
            HirFunc hf = LowerFunc(*m, /*isMethod=*/true);
            const auto *identity = model.TryGetSymbolIdentity(*m);
            assert(identity && "method declaration is missing its semantic symbol identity");
            if (!identity) {
                continue;
            }
            const std::string prefix = hib.typeName + "::";
            assert(identity->linkerName.starts_with(prefix));
            hf.name = identity->linkerName.substr(prefix.size());
            hib.methodLinkerNames.push_back(identity->linkerName);
            hib.methods.push_back(std::move(hf));
        }
        if (const auto *identity = model.TryGetVtableIdentity(d)) {
            hib.vtableLabel = identity->linkerName;
            hib.vtableEntries = identity->entries;
        }

        currentSelfType = savedSelfType;
        inImpl = savedInImpl;
        return hib;
    }

    HirConst LowerConst(const ConstDecl &d) override {
        HirConst hc;
        hc.name = d.name;
        hc.isPublic = d.isPublic;
        const std::optional<TypeRef> explicitType =
            d.type ? std::optional<TypeRef>(ResolveType(*d.type->get())) : std::nullopt;
        hc.value = explicitType ? LowerExprAs(*d.value, *explicitType) : LowerExpr(*d.value);
        hc.type = explicitType ? *explicitType : hc.value->type;
        if (HirSymbol *sym = currentScope->Lookup(d.name)) {
            sym->type = hc.type;
        }
        hc.location = d.location;
        RegisterConstInteger(hc.name, *hc.value);
        return hc;
    }

    HirExternFunc LowerExternFunc(const ExternFuncDecl &d) override {
        HirExternFunc hef;
        hef.name = d.name;
        hef.dll = d.dll;
        if (const auto *identity = model.TryGetSymbolIdentity(d); identity && identity->linkerName != d.name) {
            hef.symbolName = identity->linkerName;
        }
        hef.isPublic = d.isPublic;
        hef.isNoReturn = d.isNoReturn;
        hef.callConv = d.callConv;
        hef.isVariadic = d.isVariadic;
        hef.returnType = d.returnType ? ResolveType(**d.returnType) : TypeRef::MakeOpaque();
        hef.params = LowerParams(d.params);
        hef.location = d.location;
        return hef;
    }

    HirExternVar LowerExternVar(const ExternVarDecl &d) override {
        HirExternVar hev;
        hev.name = d.name;
        hev.isPublic = d.isPublic;
        hev.type = ResolveType(*d.type);
        hev.location = d.location;
        return hev;
    }

    HirTypeAlias LowerTypeAlias(const TypeAliasDecl &d) override {
        HirTypeAlias hta;
        hta.name = d.name;
        hta.isPublic = d.isPublic;
        hta.type = ResolveType(*d.type);
        hta.location = d.location;
        return hta;
    }

    // Expression lowering
    HirExprPtr LowerExprAs(const Expr &expr, const TypeRef &targetType) override {
        if (IsNullLiteral(expr) && targetType.kind == TypeRef::Kind::Pointer) {
            auto loweredNull = std::make_unique<HirLiteralExpr>();
            loweredNull->location = expr.location;
            loweredNull->type = targetType;
            loweredNull->value = "0";
            return loweredNull;
        }

        if (const auto *array = dynamic_cast<const ArrayExpr *>(&expr);
            array && targetType.kind == TypeRef::Kind::Array && targetType.arrayLength && !targetType.inner.empty()) {
            auto loweredArray = std::make_unique<HirArrayExpr>();
            loweredArray->location = array->location;
            loweredArray->elementType = targetType.inner[0];
            for (const auto &element : array->elements) {
                loweredArray->elements.push_back(LowerExprAs(*element, targetType.inner[0]));
            }
            loweredArray->type = targetType;
            return loweredArray;
        }

        if (const auto *array = dynamic_cast<const ArrayExpr *>(&expr)) {
            if (const auto sliceElement = SliceElementType(targetType)) {
                auto loweredArray = std::make_unique<HirArrayExpr>();
                loweredArray->location = array->location;
                loweredArray->elementType = *sliceElement;
                for (const auto &element : array->elements) {
                    loweredArray->elements.push_back(LowerExprAs(*element, *sliceElement));
                }
                loweredArray->type = TypeRef::MakeArray(*sliceElement, array->elements.size());

                auto view = std::make_unique<HirArrayToSliceExpr>();
                view->location = array->location;
                view->type = targetType;
                view->elementType = *sliceElement;
                view->length = array->elements.size();
                view->value = std::move(loweredArray);
                return view;
            }
        }

        // Preserve the destination tuple type by contextually lowering every
        // tuple-literal element. Besides producing the right aggregate type,
        // this applies any required scalar coercion recursively.
        if (const auto *tuple = dynamic_cast<const TupleExpr *>(&expr);
            tuple && targetType.kind == TypeRef::Kind::Tuple && tuple->elements.size() == targetType.inner.size()) {
            auto loweredTuple = std::make_unique<HirTupleExpr>();
            loweredTuple->location = tuple->location;
            for (std::size_t i = 0; i < tuple->elements.size(); ++i) {
                loweredTuple->elements.push_back(LowerExprAs(*tuple->elements[i], targetType.inner[i]));
            }
            loweredTuple->type = targetType;
            return loweredTuple;
        }

        HirExprPtr lowered = LowerExpr(expr);
        if (UnsuffixedIntegerLiteralFits(expr, targetType)) {
            lowered->type = targetType;
        }
        else if (IsNullLiteral(expr) && targetType.kind == TypeRef::Kind::Pointer) {
            lowered->type = targetType;
            if (auto *literal = dynamic_cast<HirLiteralExpr *>(lowered.get())) {
                literal->value = "0";
            }
        }
        else if (targetType.kind == TypeRef::Kind::Named) {
            if (HirSymbol *sym = currentScope->Lookup(targetType.name);
                sym && sym->kind == HirSymbol::Kind::Interface && lowered->type != targetType) {
                std::optional<TypeRef> implementationType = InterfaceImplementationType(lowered->type, targetType);
                if (!implementationType) {
                    implementationType = lowered->type;
                }
                const std::string typeName = implementationType->ToString();
                if (UnsuffixedIntegerLiteralFits(expr, *implementationType)) {
                    lowered->type = *implementationType;
                }
                auto coerce = std::make_unique<HirCoerceToInterfaceExpr>();
                coerce->location = expr.location;
                coerce->type = targetType;
                // Only reference a vtable when there are methods to
                // dispatch. Empty interfaces have nothing to dispatch, so
                // no vtable is generated.
                const auto ifaceIt = interfaceDecls.find(targetType.name);
                if (ifaceIt != interfaceDecls.end() && !ifaceIt->second->methods.empty()) {
                    if (const auto type = typeInterfaceVtables.find(typeName); type != typeInterfaceVtables.end()) {
                        if (const auto interface = type->second.find(targetType.name);
                            interface != type->second.end()) {
                            coerce->vtableLabel = interface->second;
                        }
                    }
                }
                coerce->value = std::move(lowered);
                return coerce;
            }
        }
        if (lowered->type.kind == TypeRef::Kind::Array && lowered->type.arrayLength && !lowered->type.inner.empty() &&
            SliceElementType(targetType)) {
            auto coerce = std::make_unique<HirArrayToSliceExpr>();
            coerce->location = expr.location;
            coerce->type = targetType;
            coerce->elementType = lowered->type.inner[0];
            coerce->length = *lowered->type.arrayLength;
            coerce->value = std::move(lowered);
            return coerce;
        }
        return lowered;
    }

    template <typename T>
    HirExprPtr CompilerParamLiteral(const SourceLocation location, TypeRef type, T value) const {
        auto literal = std::make_unique<HirLiteralExpr>();
        literal->location = location;
        literal->type = std::move(type);
        if constexpr (std::is_same_v<std::decay_t<T>, bool>) {
            literal->value = value ? "true" : "false";
        }
        else {
            literal->value = std::to_string(value);
        }
        return literal;
    }

    HirExprPtr CompilerParamString(const SourceLocation location, std::string value) const {
        auto literal = std::make_unique<HirLiteralExpr>();
        literal->location = location;
        literal->type = TypeRef::MakeNamed(SliceTypeName(TypeRef::MakeChar8()));
        literal->value = std::move(value);
        return literal;
    }

    HirExprPtr CompilerParamEnum(const SourceLocation location, const std::string &typeName,
                                 const std::int64_t discriminant) {
        TypeRef type = TypeRef::MakeNamed(typeName);
        if (const auto it = enumDecls.find(typeName); it != enumDecls.end()) {
            type = EnumType(*it->second);
        }
        return CompilerParamLiteral(location, std::move(type), discriminant);
    }

    static std::optional<std::string> CompilerParamArgument(const Expr &expr) {
        if (const auto *variant = dynamic_cast<const EnumShorthandExpr *>(&expr)) {
            return variant->variant;
        }
        if (const auto *path = dynamic_cast<const PathExpr *>(&expr); path && path->segments.size() == 2) {
            return path->segments[1];
        }
        if (const auto *literal = dynamic_cast<const LiteralExpr *>(&expr);
            literal && literal->token.kind == TokenKind::StringLiteral) {
            return DecodeStringLiteral(literal->token.text);
        }
        return std::nullopt;
    }

    HirExprPtr LowerCompilerParamField(const std::string &root, const std::string &field,
                                       const SourceLocation location) {
        if (root == "Target") {
            if (field == "os")
                return CompilerParamEnum(location, "OperatingSystem", static_cast<std::int64_t>(context.target.os));
            if (field == "arch")
                return CompilerParamEnum(location, "Architecture", static_cast<std::int64_t>(context.target.arch));
            if (field == "abi")
                return CompilerParamEnum(location, "ApplicationBinaryInterface",
                                         static_cast<std::int64_t>(context.target.abi));
            if (field == "endian")
                return CompilerParamEnum(location, "Endianness", static_cast<std::int64_t>(context.target.endianness));
            if (field == "pointerBits")
                return CompilerParamLiteral(location, TypeRef::MakeUInt(), context.target.pointer_size * 8);
            if (field == "dataModel")
                return CompilerParamEnum(location, "DataModel", static_cast<std::int64_t>(context.target.data_model));
            if (field == "objectFormat")
                return CompilerParamEnum(location, "ObjectFormat",
                                         static_cast<std::int64_t>(context.target.object_format));
            if (field == "triple")
                return CompilerParamString(location, context.targetTriple);
        }
        if (root == "Build") {
            if (field == "profile")
                return CompilerParamString(location, std::string(context.ProfileName()));
            if (field == "mode")
                return CompilerParamEnum(location, "BuildMode", static_cast<std::int64_t>(context.BuildMode()));
            if (field == "optimization")
                return CompilerParamEnum(location, "OptimizationMode",
                                         static_cast<std::int64_t>(context.Optimization()));
            if (field == "debugAssertions")
                return CompilerParamLiteral(location, TypeRef::MakeBool(), context.DebugAssertions());
            if (field == "debugInfo")
                return CompilerParamLiteral(location, TypeRef::MakeBool(), context.DebugInfo());
            if (field == "isTest")
                return CompilerParamLiteral(location, TypeRef::MakeBool(), context.isTest);
            if (field == "outputKind")
                return CompilerParamEnum(location, "OutputKind", static_cast<std::int64_t>(context.outputKind));
            if (field == "timestamp")
                return CompilerParamLiteral(location, TypeRef::MakeUInt64(), context.buildInfo.Timestamp());
            if (field == "date")
                return CompilerParamString(location, FormatBuildTime("%Y-%m-%d"));
            if (field == "time")
                return CompilerParamString(location, FormatBuildTime("%H:%M:%S"));
        }
        if (root == "Compiler" && field == "version") {
            const std::string &text = context.buildInfo.CompilerVersion();
            const ParsedSemanticVersion version = ParseSemanticVersion(text).value_or(ParsedSemanticVersion{});
            auto object = std::make_unique<HirStructInitExpr>();
            object->location = location;
            object->type = TypeRef::MakeNamed("SemanticVersion");
            object->typeName = "SemanticVersion";

            auto addField = [&](std::string name, HirExprPtr value) {
                HirStructInitField fieldValue;
                fieldValue.name = std::move(name);
                fieldValue.value = std::move(value);
                object->fields.push_back(std::move(fieldValue));
            };
            addField("major", CompilerParamLiteral(location, TypeRef::MakeUInt(), version.major));
            addField("minor", CompilerParamLiteral(location, TypeRef::MakeUInt(), version.minor));
            addField("patch", CompilerParamLiteral(location, TypeRef::MakeUInt(), version.patch));
            return object;
        }
        if (root == "Source") {
            if (field == "line")
                return CompilerParamLiteral(location, TypeRef::MakeUInt(), location.line);
            if (field == "column")
                return CompilerParamLiteral(location, TypeRef::MakeUInt(), location.column);
            if (field == "file" || field == "fileName")
                return CompilerParamString(location, std::filesystem::path(currentFile).filename().string());
            if (field == "filePath")
                return CompilerParamString(location, LogicalCurrentFilePath());
            if (field == "function")
                return CompilerParamString(location, currentFunctionName);
            if (field == "module")
                return CompilerParamString(location, currentModulePath);
        }
        return nullptr;
    }

    HirExprPtr LowerCompilerParamObject(const std::string &root, const TypeRef &type, const SourceLocation location) {
        if (root != "Target" && root != "Build" && root != "Compiler" && root != "Source" && root != "Config")
            return nullptr;

        auto object = std::make_unique<HirStructInitExpr>();
        object->location = location;
        object->type = type;
        object->typeName = type.name.empty() ? root : type.name;
        const auto declaration = structDecls.find(object->typeName);
        if (declaration == structDecls.end()) {
            return nullptr;
        }
        for (const StructDecl::Field &field : declaration->second->fields) {
            HirStructInitField value;
            value.name = field.name;
            value.value = LowerCompilerParamField(root, field.name, location);
            if (!value.value) {
                return nullptr;
            }
            object->fields.push_back(std::move(value));
        }
        return object;
    }

    HirExprPtr LowerCompilerParamCall(const std::string &root, const std::string &member,
                                      const CallExpr &call) const override {
        if (call.args.size() != 1 || !call.args[0]) {
            return nullptr;
        }
        const auto argument = CompilerParamArgument(*call.args[0]);
        if (!argument) {
            return nullptr;
        }
        // These match the method names the Rux package declares, which are
        // functions and so PascalCase; only the fields are lowerCamelCase.
        if (root == "Target" && member == "HasFeature")
            return CompilerParamLiteral(call.location, TypeRef::MakeBool(), TargetHasFeature(*argument));
        if (root == "Compiler" && member == "HasFeature")
            return CompilerParamLiteral(call.location, TypeRef::MakeBool(), CompilerHasFeature(*argument));
        if (root == "Config" && member == "Get") {
            const auto it = context.config.find(*argument);
            return CompilerParamString(call.location, it == context.config.end() ? std::string{} : it->second);
        }
        if (root == "Config" && member == "Has")
            return CompilerParamLiteral(call.location, TypeRef::MakeBool(), context.config.contains(*argument));
        return nullptr;
    }

    std::string IntrinsicArgument(const IntrinsicExpr &expr) const {
        if (expr.args.size() != 1 || !expr.args[0]) {
            return {};
        }
        if (const auto *variant = dynamic_cast<const EnumShorthandExpr *>(expr.args[0].get())) {
            return variant->variant;
        }
        if (const auto *literal = dynamic_cast<const LiteralExpr *>(expr.args[0].get());
            literal && literal->token.kind == TokenKind::StringLiteral) {
            return DecodeStringLiteral(literal->token.text);
        }
        return {};
    }

    bool TargetHasFeature(const std::string_view name) const {
        const Target::CpuFeatures features = context.target.cpu_features;
        if (name == "SSE2")
            return features.Has(Target::CpuFeature::SSE2);
        if (name == "SSE3")
            return features.Has(Target::CpuFeature::SSE3);
        if (name == "SSSE3")
            return features.Has(Target::CpuFeature::SSSE3);
        if (name == "SSE41")
            return features.Has(Target::CpuFeature::SSE41);
        if (name == "SSE42")
            return features.Has(Target::CpuFeature::SSE42);
        if (name == "AVX")
            return features.Has(Target::CpuFeature::AVX);
        if (name == "AVX2")
            return features.Has(Target::CpuFeature::AVX2);
        if (name == "AVX512")
            return features.Has(Target::CpuFeature::AVX512);
        if (name == "NEON")
            return features.Has(Target::CpuFeature::NEON);
        if (name == "SVE")
            return features.Has(Target::CpuFeature::SVE);
        return false;
    }

    static bool CompilerHasFeature(const std::string_view feature) {
        static constexpr std::array features{
            "conditional-compilation", "namespaced-intrinsics",      "target-intrinsics",
            "build-intrinsics",        "compiler-feature-detection", "source-location-defaults",
            "extern-symbol-names",     "no-return-attribute",        "when-attribute",
            "link-attribute"};
        return std::ranges::contains(features, feature);
    }

    std::string FormatBuildTime(const char *format) const {
        const std::time_t value = static_cast<std::time_t>(context.buildInfo.Timestamp());
        std::tm utc{};
        if (!UtcTime(value, utc)) {
            return {};
        }
        char buffer[32]{};
        return std::strftime(buffer, sizeof(buffer), format, &utc) == 0 ? std::string{} : std::string(buffer);
    }

    TypeRef StructInitFieldType(const StructInitExpr &expr, const std::string &fieldName) {
        const auto structIt = structDecls.find(expr.typeName);
        if (structIt == structDecls.end()) {
            if (const auto [enumDecl, variant] = LookupEnumVariantInitializer(expr.typeName); enumDecl && variant) {
                for (const auto &field : variant->namedFields) {
                    if (field.name == fieldName) {
                        return ResolveType(*field.type);
                    }
                }
            }
            return TypeRef::MakeUnknown();
        }

        const auto substitutions = StructTypeSubstitutions(*structIt->second, expr.typeArgs);
        for (const auto &field : structIt->second->fields) {
            if (field.name == fieldName) {
                return ResolveTypeWithSubstitution(*field.type, substitutions);
            }
        }
        return TypeRef::MakeUnknown();
    }

    std::string LowerLiteralValue(const LiteralExpr &expression) const override {
        if (expression.token.kind == TokenKind::CharLiteral) {
            return DecodeCharLiteral(expression.token.text);
        }
        if (expression.token.kind == TokenKind::StringLiteral) {
            return DecodeStringLiteral(expression.token.text);
        }
        if (expression.token.kind == TokenKind::IntLiteral || expression.token.kind == TokenKind::FloatLiteral) {
            return StripNumericLiteralSuffix(expression.token.text);
        }
        return expression.token.text;
    }

    HirExprPtr LowerCompilerParamIdentifier(const IdentExpr &expression) override {
        if (HirSymbol *symbol = currentScope->Lookup(expression.name);
            symbol && symbol->kind == HirSymbol::Kind::Const && !symbol->intrinsicName.empty()) {
            return LowerCompilerParamObject(symbol->intrinsicName, symbol->type, expression.location);
        }
        return nullptr;
    }

    HirExprPtr LowerCompilerParamFieldExpression(const FieldExpr &expression) override {
        if (const auto root = CompilerParamRoot(*expression.object)) {
            return LowerCompilerParamField(*root, expression.field, expression.location);
        }
        return nullptr;
    }

    HirExprPtr TryLowerOverloadedBinary(const BinaryExpr &expression, HirExprPtr &left, HirExprPtr &right) override {
        const std::string opName = std::string(OpStr(expression.op));
        const FuncDecl *method = LookupMethod(left->type, opName, {right->type});
        if (!method) {
            return nullptr;
        }

        const std::string receiverBase = NamedBaseTypeName(left->type);
        HirExprPtr selfArg;
        if (left->type.kind == TypeRef::Kind::Pointer) {
            selfArg = std::move(left);
        }
        else {
            auto address = std::make_unique<HirUnaryExpr>();
            address->location = left->location;
            address->op = TokenKind::At;
            address->type = TypeRef::MakePointer(left->type);
            address->operand = std::move(left);
            selfArg = std::move(address);
        }

        auto callee = std::make_unique<HirVarExpr>();
        callee->location = expression.location;
        callee->name = ConcreteMethodCalleeName(receiverBase, selfArg->type, *method);
        callee->type = MethodType(selfArg->type, *method);

        auto call = std::make_unique<HirCallExpr>();
        call->location = expression.location;
        call->isNoReturn = method->isNoReturn;
        call->type = callee->type.inner.empty() ? TypeRef::MakeUnknown() : callee->type.inner.back();
        call->callee = std::move(callee);
        call->args.push_back(std::move(selfArg));
        if (call->callee->type.inner.size() > 2) {
            const TypeRef &expectedType = call->callee->type.inner[1];
            if (UnsuffixedIntegerLiteralFits(*expression.right, expectedType)) {
                right->type = expectedType;
            }
            else if (IsNullLiteral(*expression.right) && expectedType.kind == TypeRef::Kind::Pointer) {
                right->type = expectedType;
                if (auto *literal = dynamic_cast<HirLiteralExpr *>(right.get())) {
                    literal->value = "0";
                }
            }
        }
        call->args.push_back(std::move(right));
        return call;
    }

    HirExprPtr LowerExpr(const Expr &expr) override {
        if (HirExprPtr basic = LowerBasicExpr(expr)) {
            return basic;
        }
        if (auto *e = dynamic_cast<const PathExpr *>(&expr)) {
            if (e->segments.size() == 2) {
                if (HirSymbol *first = currentScope->Lookup(e->segments[0]);
                    first && (first->kind == HirSymbol::Kind::Type || first->kind == HirSymbol::Kind::Interface)) {
                    if (first->kind == HirSymbol::Kind::Type) {
                        if (const auto constant = LookupPrimitiveConstant(first->type, e->segments[1], context)) {
                            auto he = std::make_unique<HirLiteralExpr>();
                            he->location = e->location;
                            he->type = constant->type;
                            he->value = constant->value;
                            return he;
                        }
                        if (const auto discriminant = LookupEnumVariantDiscriminant(e->segments[0], e->segments[1])) {
                            const auto *variant = LookupEnumVariant(e->segments[0], e->segments[1]);
                            if (variant && (!variant->fields.empty() || !variant->namedFields.empty())) {
                                auto he = std::make_unique<HirPathExpr>();
                                he->location = e->location;
                                he->segments = e->segments;
                                he->type = EnumVariantConstructorType(*enumDecls.at(e->segments[0]), *variant);
                                return he;
                            }
                            else {
                                auto he = std::make_unique<HirLiteralExpr>();
                                he->location = e->location;
                                he->type = EnumType(*enumDecls.at(e->segments[0]));
                                he->value = *discriminant;
                                return he;
                            }
                        }
                    }
                    TypeRef receiverType = first->type.IsUnknown() ? TypeRef::MakeNamed(first->name) : first->type;
                    if (const FuncDecl *method = LookupMethod(receiverType, e->segments[1])) {
                        auto he = std::make_unique<HirVarExpr>();
                        he->location = e->location;
                        if (const auto *identity = model.TryGetSymbolIdentity(*method)) {
                            he->name = identity->linkerName;
                        }
                        else {
                            he->name = CalleeName(e->segments[0], e->segments[1], receiverType, *method);
                        }
                        he->type = AssociatedFunctionType(receiverType, *method);
                        return he;
                    }
                }
            }

            auto he = std::make_unique<HirPathExpr>();
            he->location = e->location;
            he->segments = e->segments;
            // Resolve type through the final segment so module-qualified
            // paths (e.g. Math::Add) carry the correct function type.
            if (!e->segments.empty()) {
                if (HirSymbol *sym = currentScope->Lookup(e->segments.back())) {
                    he->type = sym->type;
                }
            }
            return he;
        }
        if (auto *e = dynamic_cast<const SizeOfExpr *>(&expr)) {
            auto he = std::make_unique<HirLiteralExpr>();
            he->location = e->location;
            he->type = RequireSemanticFact(model.TryGetType(*e));
            he->value = std::to_string(ResolvedSizeOf(*e));
            return he;
        }
        if (auto *e = dynamic_cast<const IntrinsicExpr *>(&expr)) {
            auto he = std::make_unique<HirLiteralExpr>();
            he->location = e->location;
            using K = IntrinsicExpr::Kind;
            switch (e->kind) {
            case K::Line:
                he->type = TypeRef::MakeUInt();
                he->value = std::to_string(e->location.line);
                break;
            case K::Column:
                he->type = TypeRef::MakeUInt();
                he->value = std::to_string(e->location.column);
                break;
            case K::File:
            case K::FileName:
                he->type = TypeRef::MakeNamed(SliceTypeName(TypeRef::MakeChar8()));
                he->value = std::filesystem::path(currentFile).filename().string();
                break;
            case K::FilePath:
                he->type = TypeRef::MakeNamed(SliceTypeName(TypeRef::MakeChar8()));
                he->value = LogicalCurrentFilePath();
                break;
            case K::Function:
                he->type = TypeRef::MakeNamed(SliceTypeName(TypeRef::MakeChar8()));
                he->value = currentFunctionName;
                break;
            case K::Date: {
                he->type = TypeRef::MakeNamed(SliceTypeName(TypeRef::MakeChar8()));
                he->value = FormatBuildTime("%Y-%m-%d");
                break;
            }
            case K::Time: {
                he->type = TypeRef::MakeNamed(SliceTypeName(TypeRef::MakeChar8()));
                he->value = FormatBuildTime("%H:%M:%S");
                break;
            }
            case K::Module: {
                he->type = TypeRef::MakeNamed(SliceTypeName(TypeRef::MakeChar8()));
                he->value = currentModulePath;
                break;
            }
            case K::CompilerVersion: {
                he->type = TypeRef::MakeNamed(SliceTypeName(TypeRef::MakeChar8()));
                he->value = context.buildInfo.CompilerVersion();
                break;
            }
            case K::Os:
            case K::Arch:
            case K::Abi:
            case K::Endian:
            case K::DataModel:
            case K::ObjectFormat:
            case K::BuildMode:
            case K::Optimization:
            case K::OutputKind:
                // Compile-time-only enums are folded away by conditional
                // compilation and rejected by semantic analysis elsewhere.
                he->type = TypeRef::MakeUnknown();
                break;
            case K::PointerBits:
                he->type = TypeRef::MakeUInt();
                he->value = std::to_string(context.target.pointer_size * 8);
                break;
            case K::TargetTriple:
                he->type = TypeRef::MakeNamed(SliceTypeName(TypeRef::MakeChar8()));
                he->value = context.targetTriple;
                break;
            case K::TargetFeature:
                he->type = TypeRef::MakeBool();
                he->value = TargetHasFeature(IntrinsicArgument(*e)) ? "true" : "false";
                break;
            case K::BuildProfile:
                he->type = TypeRef::MakeNamed(SliceTypeName(TypeRef::MakeChar8()));
                he->value = context.ProfileName();
                break;
            case K::DebugAssertions:
                he->type = TypeRef::MakeBool();
                he->value = context.DebugAssertions() ? "true" : "false";
                break;
            case K::DebugInfo:
                he->type = TypeRef::MakeBool();
                he->value = context.DebugInfo() ? "true" : "false";
                break;
            case K::IsTest:
                he->type = TypeRef::MakeBool();
                he->value = context.isTest ? "true" : "false";
                break;
            case K::BuildTimestamp:
                he->type = TypeRef::MakeUInt64();
                he->value = std::to_string(context.buildInfo.Timestamp());
                break;
            case K::CompilerHasFeature:
                he->type = TypeRef::MakeBool();
                he->value = CompilerHasFeature(IntrinsicArgument(*e)) ? "true" : "false";
                break;
            case K::Config: {
                he->type = TypeRef::MakeNamed(SliceTypeName(TypeRef::MakeChar8()));
                const auto it = context.config.find(IntrinsicArgument(*e));
                he->value = it == context.config.end() ? std::string{} : it->second;
                break;
            }
            case K::HasConfig:
                he->type = TypeRef::MakeBool();
                he->value = context.config.contains(IntrinsicArgument(*e)) ? "true" : "false";
                break;
            }
            return he;
        }
        if (auto *e = dynamic_cast<const RangeExpr *>(&expr)) {
            auto he = std::make_unique<HirRangeExpr>();
            he->location = e->location;
            he->inclusive = e->inclusive;
            if (e->lo) {
                he->lo = LowerExpr(*e->lo);
            }
            if (e->hi) {
                he->hi = LowerExpr(*e->hi);
            }
            TypeRef elemType = he->lo ? he->lo->type : he->hi ? he->hi->type : TypeRef::MakeInt64();
            if (e->lo && he->hi && he->hi->type.IsInteger() && UnsuffixedIntegerLiteralFits(*e->lo, he->hi->type)) {
                elemType = he->hi->type;
                he->lo = LowerExprAs(*e->lo, elemType);
            }
            else if (e->hi && he->lo && he->lo->type.IsInteger() &&
                     UnsuffixedIntegerLiteralFits(*e->hi, he->lo->type)) {
                elemType = he->lo->type;
                he->hi = LowerExprAs(*e->hi, elemType);
            }
            if (!he->lo && !he->hi) {
                he->type = TypeRef::MakeRangeFull();
            }
            else {
                if (elemType.IsUnknown()) {
                    elemType = TypeRef::MakeInt64();
                }
                he->type = TypeRef::MakeRange(elemType, he->lo != nullptr, he->hi != nullptr, he->inclusive);
            }
            return he;
        }
        if (auto *e = dynamic_cast<const CallExpr *>(&expr)) {
            return LowerCallExpr(*e);
        }
        if (auto *e = dynamic_cast<const StructInitExpr *>(&expr)) {
            if (const auto [enumDecl, variant] = LookupEnumVariantInitializer(e->typeName); enumDecl && variant) {
                if (!variant->namedFields.empty()) {
                    auto he = std::make_unique<HirEnumConstructExpr>();
                    he->location = e->location;
                    he->type = EnumType(*enumDecl);
                    const std::size_t sep = e->typeName.find("::");
                    he->discriminant =
                        LookupEnumVariantDiscriminant(e->typeName.substr(0, sep), e->typeName.substr(sep + 2))
                            .value_or("0");
                    for (const auto &field : variant->namedFields) {
                        const StructInitExpr::Field *initField = nullptr;
                        for (const auto &f : e->fields) {
                            if (f.name == field.name) {
                                initField = &f;
                                break;
                            }
                        }
                        if (initField) {
                            he->payloads.push_back(LowerExprAs(*initField->value, ResolveType(*field.type)));
                        }
                    }
                    return he;
                }

                auto he = std::make_unique<HirLiteralExpr>();
                he->location = e->location;
                he->type = EnumType(*enumDecl);
                const std::size_t sep = e->typeName.find("::");
                he->value = LookupEnumVariantDiscriminant(e->typeName.substr(0, sep), e->typeName.substr(sep + 2))
                                .value_or("0");
                return he;
            }

            auto he = std::make_unique<HirStructInitExpr>();
            he->location = e->location;
            he->typeName = GenericStructInitName(*e);
            TypeRef type = ParseTypeRefFromString(he->typeName);
            he->type = type.IsRange() ? type : TypeRef::MakeNamed(he->typeName);
            for (const auto &f : e->fields) {
                HirStructInitField hf;
                hf.name = f.name;
                hf.value = LowerExprAs(*f.value, StructInitFieldType(*e, f.name));
                he->fields.push_back(std::move(hf));
            }
            return he;
        }
        if (auto *e = dynamic_cast<const ArrayExpr *>(&expr)) {
            auto he = std::make_unique<HirArrayExpr>();
            he->location = e->location;
            TypeRef elemType = TypeRef::MakeUnknown();
            for (const auto &el : e->elements) {
                he->elements.push_back(LowerExpr(*el));
                if (elemType.IsUnknown()) {
                    elemType = he->elements.back()->type;
                }
            }
            he->elementType = elemType;
            he->type = TypeRef::MakeArray(elemType, e->elements.size());
            return he;
        }
        if (auto *e = dynamic_cast<const TupleExpr *>(&expr)) {
            auto he = std::make_unique<HirTupleExpr>();
            he->location = e->location;
            std::vector<TypeRef> elemTypes;
            for (const auto &el : e->elements) {
                he->elements.push_back(LowerExpr(*el));
                elemTypes.push_back(he->elements.back()->type);
            }
            he->type = TypeRef::MakeTuple(std::move(elemTypes));
            return he;
        }
        if (auto *e = dynamic_cast<const MatchExpr *>(&expr)) {
            auto he = std::make_unique<HirMatchExpr>();
            he->location = e->location;
            he->subject = LowerExpr(*e->subject);
            for (const auto &arm : e->arms) {
                HirMatchArm ha;
                ha.location = arm.location;
                PushScope();
                ha.pattern = LowerPattern(*arm.pattern, he->subject->type);
                ha.body = LowerExpr(*arm.body);
                PopScope();
                if (he->type.IsUnknown()) {
                    he->type = ha.body->type;
                }
                he->arms.push_back(std::move(ha));
            }
            return he;
        }
        if (auto *e = dynamic_cast<const BlockExpr *>(&expr)) {
            auto he = std::make_unique<HirBlockExpr>();
            he->location = e->location;
            he->block = LowerBlock(*e->block);
            return he;
        }
        if (auto *e = dynamic_cast<const SpreadExpr *>(&expr)) {
            return LowerExpr(*e->operand);
        }

        // Fallback for unrecognized expression kinds
        auto he = std::make_unique<HirLiteralExpr>();
        he->location = expr.location;
        he->value = "<expr>";
        return he;
    }
};

// Hir public API
AstToHirLowering::AstToHirLowering(const SemanticModel &model)
    : semanticModel_(model) {
}

HirPackage AstToHirLowering::Generate() {
    Lowering lowering(semanticModel_);
    return lowering.Run();
}
} // namespace Rux
