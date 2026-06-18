// Copyright (c) Rux contributors.
// SPDX-License-Identifier: MIT

#include "Rux/Hir.h"

#include "Rux/Platform/Defines.h"
#include "Rux/Version.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <ctime>
#include <format>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>

namespace Rux {
static bool LocalTime(std::time_t time, std::tm &out) {
#if RUX_OS_WINDOWS
    return localtime_s(&out, &time) == 0;
#else
    return localtime_r(&time, &out) != nullptr;
#endif
}

// Internal: Symbol & Scope
struct HirSymbol {
    enum class Kind {
        Var,
        Func,
        Type,
        Const,
        Interface,
    };

    Kind kind = Kind::Var;
    std::string name;
    TypeRef type;
    bool isMut = false;
    std::vector<FuncDecl const *> funcOverloads;
};

class HirScope {
public:
    explicit HirScope(HirScope *parent = nullptr)
        : parent(parent) {
    }

    void Define(HirSymbol sym) {
        if (auto it = table.find(sym.name); it != table.end()) {
            if (it->second.kind == HirSymbol::Kind::Func && sym.kind == HirSymbol::Kind::Func) {
                it->second.funcOverloads.insert(it->second.funcOverloads.end(),
                                                sym.funcOverloads.begin(), sym.funcOverloads.end());
                if (it->second.type.IsUnknown() && !sym.type.IsUnknown()) {
                    it->second.type = std::move(sym.type);
                }
            }
            return;
        }
        table.emplace(sym.name, std::move(sym));
    }

    HirSymbol *Lookup(std::string const &name) {
        auto it = table.find(name);
        if (it != table.end()) {
            return &it->second;
        }
        if (parent) {
            return parent->Lookup(name);
        }
        return nullptr;
    }

    [[nodiscard]] HirScope *Parent() const {
        return parent;
    }

private:
    HirScope *parent;
    std::unordered_map<std::string, HirSymbol> table;
};

// Operator → string
static std::string_view OpStr(TokenKind op) {
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
    default:
        return "?";
    }
}

// Internal: Lowering
class Lowering {
public:
    explicit Lowering(std::vector<Module const *> &modules)
        : modules(modules)
        , currentScope(&globalScope) {
    }

    HirPackage Run() {
        RegisterBuiltins();
        for (auto *mod : modules) {
            CollectModule(*mod);
        }
        HirPackage pkg;
        for (auto *mod : modules) {
            pkg.modules.push_back(LowerModule(*mod));
        }
        return pkg;
    }

private:
    std::vector<Module const *> &modules;
    HirScope globalScope{nullptr};
    HirScope *currentScope;
    std::vector<std::unique_ptr<HirScope>> ownedScopes;
    std::string currentFile;
    std::string currentFunctionName;
    std::string currentModulePath;
    TypeRef currentReturnType = TypeRef::MakeOpaque();
    bool inImpl = false;
    TypeRef currentSelfType = TypeRef::MakeUnknown();
    std::vector<std::string> currentTypeParams;
    std::unordered_map<std::string, StructDecl const *> structDecls;
    std::unordered_map<std::string, EnumDecl const *> enumDecls;
    std::unordered_map<std::string, std::vector<FuncDecl const *>> functionsByName;
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<FuncDecl const *>>>
        methodsByType;
    std::unordered_map<std::string, InterfaceDecl const *> interfaceDecls;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>>
        typeInterfaceVtables;
    std::vector<std::unordered_map<std::string, std::uint64_t>> constIntegerScopes{{}};

    // Scope management
    void PushScope() {
        ownedScopes.push_back(std::make_unique<HirScope>(currentScope));
        currentScope = ownedScopes.back().get();
        constIntegerScopes.emplace_back();
    }

    void PopScope() {
        assert(currentScope->Parent() != nullptr && "cannot pop global scope");
        currentScope = currentScope->Parent();
        if (constIntegerScopes.size() > 1) {
            constIntegerScopes.pop_back();
        }
    }

    void Define(HirSymbol sym) const {
        currentScope->Define(std::move(sym));
    }

    // Builtins
    void RegisterBuiltins() {
        auto add = [&](char const *name, TypeRef t) {
            HirSymbol sym;
            sym.kind = HirSymbol::Kind::Type;
            sym.name = name;
            sym.type = std::move(t);
            globalScope.Define(std::move(sym));
        };
        add("opaque", TypeRef::MakeOpaque());
        add("bool8", TypeRef::MakeBool8());
        add("bool16", TypeRef::MakeBool16());
        add("bool32", TypeRef::MakeBool32());
        add("bool", TypeRef::MakeBool());
        add("char8", TypeRef::MakeChar8());
        add("char16", TypeRef::MakeChar16());
        add("char32", TypeRef::MakeChar32());
        add("char", TypeRef::MakeChar());
        add("int8", TypeRef::MakeInt8());
        add("int16", TypeRef::MakeInt16());
        add("int32", TypeRef::MakeInt32());
        add("int64", TypeRef::MakeInt64());
        add("int", TypeRef::MakeInt());
        add("uint8", TypeRef::MakeUInt8());
        add("uint16", TypeRef::MakeUInt16());
        add("uint32", TypeRef::MakeUInt32());
        add("uint64", TypeRef::MakeUInt64());
        add("uint", TypeRef::MakeUInt());
        add("float32", TypeRef::MakeFloat32());
        add("float64", TypeRef::MakeFloat64());
        add("float", TypeRef::MakeFloat());
    }

    // First pass: collect global names
    void CollectModule(Module const &mod) {
        currentFile = mod.name;
        for (auto const &decl : mod.items) {
            CollectDecl(*decl);
        }
    }

    TypeRef MakeFuncType(std::vector<Param> const &params,
                         std::optional<TypeExprPtr> const &returnType,
                         std::vector<std::string> const &typeParams = {}) {
        auto savedTypeParams = currentTypeParams;
        currentTypeParams = typeParams;

        std::vector<TypeRef> paramTypes;
        for (auto const &param : params) {
            if (!param.isVariadic) {
                paramTypes.push_back(ResolveType(*param.type));
            }
        }
        TypeRef ret = returnType ? ResolveType(*returnType->get()) : TypeRef::MakeOpaque();

        currentTypeParams = savedTypeParams;
        return TypeRef::MakeFunc(std::move(paramTypes), std::move(ret));
    }

    void CollectDecl(Decl const &decl) {
        auto simple = [&](HirSymbol::Kind k, std::string const &name, TypeRef t = {}) {
            HirSymbol sym;
            sym.kind = k;
            sym.name = name;
            sym.type = std::move(t);
            globalScope.Define(std::move(sym));
        };
        if (auto *d = dynamic_cast<FuncDecl const *>(&decl)) {
            functionsByName[d->name].push_back(d);
            HirSymbol sym;
            sym.kind = HirSymbol::Kind::Func;
            sym.name = d->name;
            sym.type = MakeFuncType(d->params, d->returnType, d->typeParams);
            sym.funcOverloads.push_back(d);
            globalScope.Define(std::move(sym));
        }
        else if (auto *d = dynamic_cast<StructDecl const *>(&decl)) {
            structDecls[d->name] = d;
            simple(HirSymbol::Kind::Type, d->name, TypeRef::MakeNamed(d->name));
        }
        else if (auto *d = dynamic_cast<EnumDecl const *>(&decl)) {
            enumDecls[d->name] = d;
            simple(HirSymbol::Kind::Type, d->name, EnumType(*d));
        }
        else if (auto *d = dynamic_cast<UnionDecl const *>(&decl)) {
            simple(HirSymbol::Kind::Type, d->name, TypeRef::MakeNamed(d->name));
        }
        else if (auto *d = dynamic_cast<InterfaceDecl const *>(&decl)) {
            simple(HirSymbol::Kind::Interface, d->name, TypeRef::MakeNamed(d->name));
            interfaceDecls[d->name] = d;
        }
        else if (auto *d = dynamic_cast<ConstDecl const *>(&decl)) {
            TypeRef constType;
            if (d->type) {
                constType = ResolveType(*d->type->get());
            }
            simple(HirSymbol::Kind::Const, d->name, constType);
        }
        else if (auto *d = dynamic_cast<TypeAliasDecl const *>(&decl)) {
            simple(HirSymbol::Kind::Type, d->name, ResolveType(*d->type));
        }
        else if (auto *d = dynamic_cast<ExternFuncDecl const *>(&decl)) {
            simple(HirSymbol::Kind::Func, d->name, MakeFuncType(d->params, d->returnType));
        }
        else if (auto *d = dynamic_cast<ExternVarDecl const *>(&decl)) {
            HirSymbol sym;
            sym.kind = HirSymbol::Kind::Var;
            sym.name = d->name;
            sym.isMut = true;
            globalScope.Define(std::move(sym));
        }
        else if (auto *d = dynamic_cast<ExternBlockDecl const *>(&decl)) {
            for (auto &item : d->items) {
                CollectDecl(*item);
            }
        }
        else if (auto *d = dynamic_cast<ModuleDecl const *>(&decl)) {
            for (auto &item : d->items) {
                CollectDecl(*item);
            }
        }
        else if (auto *d = dynamic_cast<ImplDecl const *>(&decl)) {
            for (auto const &method : d->methods) {
                methodsByType[d->typeName][method->name].push_back(method.get());
            }
            if (d->interfaceName) {
                typeInterfaceVtables[d->typeName][*d->interfaceName] =
                    "__vtable__" + d->typeName + "__" + *d->interfaceName;
            }
        }
    }

    // Type resolution
    std::string GenericTypeName(NamedTypeExpr const &type) {
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

    std::string GenericStructInitName(StructInitExpr const &expr) {
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

    std::pair<EnumDecl const *, EnumDecl::Variant const *>
    LookupEnumVariantInitializer(std::string const &typeName) const {
        std::size_t const sep = typeName.find("::");
        if (sep == std::string::npos || typeName.find("::", sep + 2) != std::string::npos) {
            return {nullptr, nullptr};
        }

        std::string const enumName = typeName.substr(0, sep);
        std::string const variantName = typeName.substr(sep + 2);
        auto const enumIt = enumDecls.find(enumName);
        if (enumIt == enumDecls.end()) {
            return {nullptr, nullptr};
        }
        for (auto const &variant : enumIt->second->variants) {
            if (variant.name == variantName) {
                return {enumIt->second, &variant};
            }
        }
        return {enumIt->second, nullptr};
    }

    static std::string SliceTypeName(TypeRef const &elemType) {
        return "Slice<" + elemType.ToString() + ">";
    }

    static std::string BaseTypeName(std::string const &name) {
        std::size_t const pos = name.find('<');
        return pos == std::string::npos ? name : name.substr(0, pos);
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
        if (str == "uint8") {
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
            return TypeRef::MakeSlice(ParseTypeRefFromString(str.substr(0, str.size() - 2)));
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

        if (str.rfind("Range<", 0) == 0 && str.back() == '>') {
            return TypeRef::MakeRange(ParseTypeRefFromString(str.substr(6, str.size() - 7)));
        }

        return TypeRef::MakeNamed(str);
    }

    static std::vector<TypeRef> ParseTypeArgsFromTypeName(std::string const &typeName) {
        std::vector<TypeRef> args;
        std::size_t const pos = typeName.find('<');
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

    static std::uint64_t AlignUp(std::uint64_t const value, std::uint64_t const align) {
        return (value + align - 1) & ~(align - 1);
    }

    static TypeRef StringLiteralElementType(Token const &tok) {
        if (tok.text.starts_with("c16\"")) {
            return TypeRef::MakeChar16();
        }
        if (tok.text.starts_with("c32\"")) {
            return TypeRef::MakeChar32();
        }
        return TypeRef::MakeChar8();
    }

    static TypeRef StringLiteralType(Token const &tok) {
        return TypeRef::MakeNamed(SliceTypeName(StringLiteralElementType(tok)));
    }

    static TypeRef CharLiteralType(Token const &tok) {
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

    static std::string StripNumericLiteralSuffix(std::string const &text) {
        std::string const suffix = NumericLiteralSuffix(text);
        if (suffix.empty()) {
            return text;
        }
        return text.substr(0, text.size() - suffix.size());
    }

    static std::optional<std::uint64_t> ParseUnsuffixedIntegerLiteral(Token const &tok) {
        if (tok.kind != TokenKind::IntLiteral || !NumericLiteralSuffix(tok.text).empty()) {
            return std::nullopt;
        }

        std::string text;
        text.reserve(tok.text.size());
        for (char const c : tok.text) {
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
        auto const *first = digits.data();
        auto const *last = first + digits.size();
        auto const [ptr, ec] = std::from_chars(first, last, value, base);
        if (ec != std::errc{} || ptr != last) {
            return std::nullopt;
        }
        return value;
    }

    static std::optional<std::uint64_t> ParseUnsignedIntegerText(std::string const &rawText) {
        std::string text = StripNumericLiteralSuffix(rawText);
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
        auto const *first = digits.data();
        auto const *last = first + digits.size();
        auto const [ptr, ec] = std::from_chars(first, last, value, base);
        if (ec != std::errc{} || ptr != last) {
            return std::nullopt;
        }
        return value;
    }

    std::optional<std::uint64_t> LookupConstInteger(std::string const &name) const {
        for (auto it = constIntegerScopes.rbegin(); it != constIntegerScopes.rend(); ++it) {
            if (auto const valueIt = it->find(name); valueIt != it->end()) {
                return valueIt->second;
            }
        }
        return std::nullopt;
    }

    void RegisterConstInteger(std::string const &name, HirExpr const &value) {
        auto const *literal = dynamic_cast<HirLiteralExpr const *>(&value);
        if (!literal) {
            return;
        }
        if (auto parsed = ParseUnsignedIntegerText(literal->value)) {
            constIntegerScopes.back()[name] = *parsed;
        }
    }

    static std::optional<std::int64_t> ParseEnumDiscriminant(std::string const &text) {
        std::string cleaned = StripNumericLiteralSuffix(text);
        bool const negative = !cleaned.empty() && cleaned[0] == '-';
        if (negative) {
            cleaned.erase(cleaned.begin());
        }

        std::string digitsText;
        digitsText.reserve(cleaned.size());
        for (char const c : cleaned) {
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
        auto const *first = digits.data();
        auto const *last = first + digits.size();
        auto const [ptr, ec] = std::from_chars(first, last, parsed, base);
        if (ec != std::errc{} || ptr != last) {
            return std::nullopt;
        }
        if (negative) {
            constexpr auto maxMagnitude =
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1;
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

    static std::optional<std::uint64_t> UnsignedIntegerMax(TypeRef const &type) {
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

    static std::optional<std::pair<std::int64_t, std::int64_t>>
    SignedIntegerRange(TypeRef const &type) {
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
            return std::pair{std::numeric_limits<std::int64_t>::min(),
                             std::numeric_limits<std::int64_t>::max()};
        default:
            return std::nullopt;
        }
    }

    static bool UnsuffixedIntegerLiteralFits(Expr const &expr, TypeRef const &target) {
        bool negative = false;
        LiteralExpr const *literal = dynamic_cast<LiteralExpr const *>(&expr);
        if (!literal) {
            if (auto const *unary = dynamic_cast<UnaryExpr const *>(&expr);
                unary && unary->op == TokenKind::Minus) {
                literal = dynamic_cast<LiteralExpr const *>(unary->operand.get());
            }
            if (!literal) {
                return false;
            }
            negative = true;
        }

        auto const value = ParseUnsuffixedIntegerLiteral(literal->token);
        if (!value) {
            return false;
        }

        if (negative) {
            auto const range = SignedIntegerRange(target);
            if (!range) {
                return false;
            }
            auto const minMagnitude = static_cast<std::uint64_t>(-(range->first + 1)) + 1;
            return *value <= minMagnitude;
        }

        if (auto const max = UnsignedIntegerMax(target)) {
            return *value <= *max;
        }
        if (auto const range = SignedIntegerRange(target)) {
            return *value <= static_cast<std::uint64_t>(range->second);
        }
        return false;
    }

    static bool IsNullLiteral(Expr const &expr) {
        auto const *literal = dynamic_cast<LiteralExpr const *>(&expr);
        return literal && literal->token.kind == TokenKind::NullKeyword;
    }

    static std::string NamedBaseTypeName(TypeRef const &type) {
        TypeRef const *named = &type;
        if (type.kind == TypeRef::Kind::Pointer && !type.inner.empty()) {
            named = &type.inner[0];
        }
        if (named->kind == TypeRef::Kind::Named) {
            return BaseTypeName(named->name);
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
    StructTypeSubstitutions(StructDecl const &decl, std::vector<TypeExprPtr> const &typeArgs) {
        std::unordered_map<std::string, TypeRef> substitutions;
        std::size_t const count = std::min(decl.typeParams.size(), typeArgs.size());
        for (std::size_t i = 0; i < count; ++i) {
            substitutions.emplace(decl.typeParams[i], ResolveType(*typeArgs[i]));
        }
        return substitutions;
    }

    static TypeRef SuffixedLiteralType(Token const &tok) {
        std::string const suffix = NumericLiteralSuffix(tok.text);
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

    static std::optional<TypeRef> BuiltinTypeFromName(std::string const &name) {
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
        if (name == "uint8") {
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

    static std::optional<TypeRef> SliceElementType(TypeRef const &type) {
        if (type.kind == TypeRef::Kind::Slice && !type.inner.empty()) {
            return type.inner[0];
        }
        if (type.kind != TypeRef::Kind::Named) {
            return std::nullopt;
        }
        constexpr std::string_view prefix = "Slice<";
        if (!type.name.starts_with(prefix) || type.name.back() != '>') {
            return std::nullopt;
        }
        std::string elemName =
            type.name.substr(prefix.size(), type.name.size() - prefix.size() - 1);
        if (auto builtin = BuiltinTypeFromName(elemName)) {
            return *builtin;
        }
        return TypeRef::MakeNamed(elemName);
    }

    static std::optional<TypeRef> IndexElementType(TypeRef const &type) {
        if (auto elemType = SliceElementType(type)) {
            return elemType;
        }
        if (type.kind == TypeRef::Kind::Pointer && !type.inner.empty()) {
            return type.inner[0];
        }
        return std::nullopt;
    }

    TypeRef ResolveType(TypeExpr const &expr) {
        if (auto *t = dynamic_cast<NamedTypeExpr const *>(&expr)) {
            if (t->typeArgs.empty()) {
                for (auto const &tp : currentTypeParams) {
                    if (tp == t->name) {
                        return TypeRef::MakeTypeParam(t->name);
                    }
                }
            }
            HirSymbol *sym = currentScope->Lookup(t->name);
            if (sym &&
                (sym->kind == HirSymbol::Kind::Type || sym->kind == HirSymbol::Kind::Interface)) {
                if (t->typeArgs.empty() && !sym->type.IsUnknown()) {
                    return sym->type;
                }
                if (t->typeArgs.empty()) {
                    if (auto const enumIt = enumDecls.find(t->name); enumIt != enumDecls.end()) {
                        return EnumType(*enumIt->second);
                    }
                }
                return TypeRef::MakeNamed(GenericTypeName(*t));
            }
            return TypeRef::MakeNamed(GenericTypeName(*t)); // best-effort for unresolved names
        }
        if (auto *t = dynamic_cast<PathTypeExpr const *>(&expr)) {
            return TypeRef::MakeNamed(t->segments.back());
        }
        if (auto *t = dynamic_cast<PointerTypeExpr const *>(&expr)) {
            return TypeRef::MakePointer(ResolveType(*t->pointee));
        }
        if (auto *t = dynamic_cast<SliceTypeExpr const *>(&expr)) {
            return TypeRef::MakeNamed(SliceTypeName(ResolveType(*t->element)));
        }
        if (auto *t = dynamic_cast<TupleTypeExpr const *>(&expr)) {
            std::vector<TypeRef> elems;
            for (auto &e : t->elements) {
                elems.push_back(ResolveType(*e));
            }
            return TypeRef::MakeTuple(std::move(elems));
        }
        if (dynamic_cast<SelfTypeExpr const *>(&expr)) {
            return currentSelfType.IsUnknown() ? TypeRef::MakeNamed("self") : currentSelfType;
        }
        return TypeRef::MakeUnknown();
    }

    std::optional<std::uint64_t> FixedSliceTypeSize(TypeExpr const &expr) {
        auto const *slice = dynamic_cast<SliceTypeExpr const *>(&expr);
        if (!slice || !slice->size) {
            return std::nullopt;
        }
        if (auto const *literal = dynamic_cast<LiteralExpr const *>(slice->size.get())) {
            return ParseUnsuffixedIntegerLiteral(literal->token);
        }
        if (auto const *ident = dynamic_cast<IdentExpr const *>(slice->size.get())) {
            return LookupConstInteger(ident->name);
        }
        return std::nullopt;
    }

    TypeRef FixedSliceElementType(TypeExpr const &expr) {
        auto const *slice = dynamic_cast<SliceTypeExpr const *>(&expr);
        if (!slice) {
            return TypeRef::MakeUnknown();
        }
        return ResolveType(*slice->element);
    }

    TypeRef
    ResolveTypeWithSubstitution(TypeExpr const &expr,
                                std::unordered_map<std::string, TypeRef> const &substitutions) {
        if (auto *t = dynamic_cast<NamedTypeExpr const *>(&expr)) {
            if (t->typeArgs.empty()) {
                if (auto it = substitutions.find(t->name); it != substitutions.end()) {
                    return it->second;
                }
                return ResolveType(expr);
            }

            TypeRef named = TypeRef::MakeNamed(t->name);
            named.name += "<";
            for (std::size_t i = 0; i < t->typeArgs.size(); ++i) {
                if (i) {
                    named.name += ", ";
                }
                named.name +=
                    ResolveTypeWithSubstitution(*t->typeArgs[i], substitutions).ToString();
            }
            named.name += ">";
            return named;
        }
        if (auto *t = dynamic_cast<PointerTypeExpr const *>(&expr)) {
            return TypeRef::MakePointer(ResolveTypeWithSubstitution(*t->pointee, substitutions));
        }
        if (auto *t = dynamic_cast<SliceTypeExpr const *>(&expr)) {
            return TypeRef::MakeNamed(
                SliceTypeName(ResolveTypeWithSubstitution(*t->element, substitutions)));
        }
        if (auto *t = dynamic_cast<TupleTypeExpr const *>(&expr)) {
            std::vector<TypeRef> elems;
            for (auto &elem : t->elements) {
                elems.push_back(ResolveTypeWithSubstitution(*elem, substitutions));
            }
            return TypeRef::MakeTuple(std::move(elems));
        }
        return ResolveType(expr);
    }

    TypeRef StructFieldType(TypeRef const &objectType, std::string const &fieldName) {
        std::string const typeName = NamedBaseTypeName(objectType);
        if (typeName.empty()) {
            return TypeRef::MakeUnknown();
        }
        auto const structIt = structDecls.find(typeName);
        if (structIt == structDecls.end()) {
            return TypeRef::MakeUnknown();
        }

        std::unordered_map<std::string, TypeRef> substitutions;
        std::vector<TypeRef> typeArgs = ParseTypeArgsFromTypeName(objectType.name);
        auto const &params = structIt->second->typeParams;
        std::size_t const count = std::min(params.size(), typeArgs.size());
        for (std::size_t i = 0; i < count; ++i) {
            substitutions.emplace(params[i], typeArgs[i]);
        }

        for (auto const &field : structIt->second->fields) {
            if (field.name == fieldName) {
                if (!substitutions.empty()) {
                    return ResolveTypeWithSubstitution(*field.type, substitutions);
                }
                return ResolveType(*field.type);
            }
        }
        return TypeRef::MakeUnknown();
    }

    TypeRef MethodType(TypeRef const &receiverType, FuncDecl const &method) {
        std::vector<TypeRef> params;
        params.push_back(receiverType);
        for (auto const &param : method.params) {
            if (param.isVariadic || param.name == "self") {
                continue;
            }
            params.push_back(ResolveType(*param.type));
        }
        TypeRef ret =
            method.returnType ? ResolveType(*method.returnType->get()) : TypeRef::MakeOpaque();
        return TypeRef::MakeFunc(std::move(params), std::move(ret));
    }

    TypeRef AssociatedFunctionType(TypeRef const &receiverType, FuncDecl const &method) {
        TypeRef savedSelfType = currentSelfType;
        currentSelfType = receiverType.kind == TypeRef::Kind::Pointer
                            ? receiverType
                            : TypeRef::MakePointer(receiverType);
        std::vector<TypeRef> params;
        for (auto const &param : method.params) {
            if (param.isVariadic) {
                continue;
            }
            params.push_back(ResolveType(*param.type));
        }
        TypeRef ret =
            method.returnType ? ResolveType(*method.returnType->get()) : TypeRef::MakeOpaque();
        currentSelfType = savedSelfType;
        return TypeRef::MakeFunc(std::move(params), std::move(ret));
    }

    bool MethodIsOverloaded(std::string const &typeName, std::string const &methodName) const {
        auto const typeIt = methodsByType.find(typeName);
        if (typeIt == methodsByType.end()) {
            return false;
        }
        auto const methodIt = typeIt->second.find(methodName);
        return methodIt != typeIt->second.end() && methodIt->second.size() > 1;
    }

    bool FunctionIsOverloaded(std::string const &name) const {
        auto const it = functionsByName.find(name);
        return it != functionsByName.end() && it->second.size() > 1;
    }

    static std::string MangleTypeName(TypeRef const &type) {
        std::string out;
        for (char const c : type.ToString()) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
                out += c;
            }
            else {
                out += '_';
            }
        }
        return out.empty() ? "_" : out;
    }

    std::string FunctionCalleeName(std::string const &name, FuncDecl const &decl) {
        if (!FunctionIsOverloaded(name)) {
            return name;
        }
        std::string out = name + "__";
        bool first = true;
        for (auto const &param : decl.params) {
            TypeRef paramType = param.isVariadic
                                  ? TypeRef::MakeNamed(SliceTypeName(ResolveType(*param.type)))
                                  : ResolveType(*param.type);
            if (!first) {
                out += "_";
            }
            out += MangleTypeName(paramType);
            first = false;
        }
        return out;
    }

    FuncDecl const *LookupFunction(std::string const &name, std::vector<TypeRef> const &argTypes) {
        auto const it = functionsByName.find(name);
        if (it == functionsByName.end() || it->second.empty()) {
            return nullptr;
        }
        if (it->second.size() == 1) {
            // Single-candidate validation. We must still verify arity and
            // assignability to prevent bogus calls from silently bypassing
            // the type-checker.
            auto const *decl = it->second[0];
            TypeRef ft = MakeFuncType(decl->params, decl->returnType, decl->typeParams);
            if (ft.kind != TypeRef::Kind::Func || ft.inner.empty()) {
                return decl;
            }
            std::size_t const paramCount = ft.inner.size() - 1;
            bool const isVariadic = !decl->params.empty() && decl->params.back().isVariadic;
            std::size_t requiredCount = 0;
            for (auto const &p : decl->params) {
                if (!p.isVariadic && !p.defaultValue) {
                    ++requiredCount;
                }
            }
            bool const arityOk =
                isVariadic ? argTypes.size() >= requiredCount
                           : (argTypes.size() >= requiredCount && argTypes.size() <= paramCount);
            if (!arityOk) {
                return nullptr;
            }
            for (std::size_t i = 0; i < std::min(argTypes.size(), paramCount); ++i) {
                if (argTypes[i].IsUnknown() || ft.inner[i].IsUnknown()) {
                    continue;
                }
                if (!argTypes[i].IsAssignableTo(ft.inner[i]) &&
                    !(argTypes[i].IsInteger() && ft.inner[i].IsInteger())) {
                    return nullptr;
                }
            }
            return decl;
        }
        for (bool const allowVariadic : {false, true}) {
            for (bool const exactOnly : {true, false}) {
                for (auto const *decl : it->second) {
                    TypeRef ft = MakeFuncType(decl->params, decl->returnType, decl->typeParams);
                    if (ft.kind != TypeRef::Kind::Func || ft.inner.empty()) {
                        continue;
                    }
                    std::size_t const paramCount = ft.inner.size() - 1;
                    bool const isVariadic = !decl->params.empty() && decl->params.back().isVariadic;
                    if (isVariadic != allowVariadic) {
                        continue;
                    }
                    std::size_t requiredCount = 0;
                    for (auto const &p : decl->params) {
                        if (!p.isVariadic && !p.defaultValue) {
                            ++requiredCount;
                        }
                    }
                    bool const arityOk = isVariadic ? argTypes.size() >= requiredCount
                                                    : (argTypes.size() >= requiredCount &&
                                                       argTypes.size() <= paramCount);
                    if (!arityOk) {
                        continue;
                    }
                    bool match = true;
                    for (std::size_t i = 0; i < std::min(argTypes.size(), paramCount); ++i) {
                        TypeRef const &paramType = ft.inner[i];
                        if (argTypes[i].IsUnknown() || paramType.IsUnknown()) {
                            continue;
                        }
                        if (exactOnly ? !(argTypes[i] == paramType)
                                      : !(argTypes[i].IsAssignableTo(paramType) ||
                                          (argTypes[i].IsInteger() && paramType.IsInteger()))) {
                            match = false;
                            break;
                        }
                    }
                    if (match) {
                        return decl;
                    }
                }
            }
        }
        return nullptr;
    }

    EnumDecl::Variant const *LookupEnumVariant(std::string const &enumName,
                                               std::string const &variantName) const {
        auto const enumIt = enumDecls.find(enumName);
        if (enumIt == enumDecls.end()) {
            return nullptr;
        }
        for (auto const &variant : enumIt->second->variants) {
            if (variant.name == variantName) {
                return &variant;
            }
        }
        return nullptr;
    }

    std::optional<std::string> LookupEnumVariantDiscriminant(std::string const &enumName,
                                                             std::string const &variantName) const {
        auto const enumIt = enumDecls.find(enumName);
        if (enumIt == enumDecls.end()) {
            return std::nullopt;
        }
        auto const &variants = enumIt->second->variants;
        std::int64_t next = 0;
        for (std::size_t i = 0; i < variants.size(); ++i) {
            std::int64_t value = next;
            if (variants[i].discriminant) {
                if (auto const parsed = ParseEnumDiscriminant(*variants[i].discriminant)) {
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

    TypeRef EnumVariantConstructorType(EnumDecl const &decl, EnumDecl::Variant const &variant) {
        std::vector<TypeRef> params;
        params.reserve(variant.fields.size() + variant.namedFields.size());
        for (auto const &field : variant.fields) {
            params.push_back(ResolveType(*field));
        }
        for (auto const &field : variant.namedFields) {
            params.push_back(ResolveType(*field.type));
        }
        return TypeRef::MakeFunc(std::move(params), EnumType(decl));
    }

    // Returns the mangled callee name: "Type::method__p1_p2" for overloads,
    // "Type::method" for single-dispatch methods.
    std::string CalleeName(std::string const &typeName, std::string const &methodName,
                           TypeRef const &receiverType, FuncDecl const &decl) {
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

    FuncDecl const *LookupMethod(TypeRef const &receiverType, std::string const &methodName,
                                 std::vector<TypeRef> const &argTypes = {}) {
        std::string const typeName = NamedBaseTypeName(receiverType);
        if (typeName.empty()) {
            return nullptr;
        }
        auto const typeIt = methodsByType.find(typeName);
        if (typeIt == methodsByType.end()) {
            return nullptr;
        }
        auto const methodIt = typeIt->second.find(methodName);
        if (methodIt == typeIt->second.end()) {
            return nullptr;
        }
        auto const &overloads = methodIt->second;
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
            auto const *decl = overloads[0];
            TypeRef ft = MethodType(receiverType, *decl);
            std::size_t const paramCount = ft.inner.size() >= 2 ? ft.inner.size() - 2 : 0;
            if (paramCount != argTypes.size()) {
                return nullptr;
            }
            for (std::size_t i = 0; i < argTypes.size(); ++i) {
                TypeRef const &paramType = ft.inner[i + 1];
                if (argTypes[i].IsUnknown() || paramType.IsUnknown()) {
                    continue;
                }
                if (!argTypes[i].IsAssignableTo(paramType) &&
                    !(argTypes[i].IsInteger() && paramType.IsInteger())) {
                    return nullptr;
                }
            }
            return decl;
        }
        for (auto const *decl : overloads) {
            TypeRef ft = MethodType(receiverType, *decl);
            // ft.inner = [selfType, param1, ..., retType]
            std::size_t const paramCount = ft.inner.size() >= 2 ? ft.inner.size() - 2 : 0;
            if (paramCount != argTypes.size()) {
                continue;
            }
            bool match = true;
            for (std::size_t i = 0; i < argTypes.size(); ++i) {
                TypeRef const &paramType = ft.inner[i + 1];
                if (!argTypes[i].IsUnknown() && !paramType.IsUnknown() &&
                    !argTypes[i].IsAssignableTo(paramType) &&
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

    int InterfaceMethodIndex(std::string const &ifaceName, std::string const &methodName) const {
        auto it = interfaceDecls.find(ifaceName);
        if (it == interfaceDecls.end()) {
            return -1;
        }
        auto const &methods = it->second->methods;
        for (int i = 0; i < static_cast<int>(methods.size()); ++i) {
            if (methods[i]->name == methodName) {
                return i;
            }
        }
        return -1;
    }

    TypeRef InterfaceMethodReturnType(std::string const &ifaceName, std::string const &methodName) {
        auto it = interfaceDecls.find(ifaceName);
        if (it == interfaceDecls.end()) {
            return TypeRef::MakeUnknown();
        }
        for (auto const &m : it->second->methods) {
            if (m->name == methodName) {
                return m->returnType ? ResolveType(**m->returnType) : TypeRef::MakeOpaque();
            }
        }
        return TypeRef::MakeUnknown();
    }

    std::vector<TypeRef> InterfaceMethodParamTypes(std::string const &ifaceName,
                                                   std::string const &methodName) {
        std::vector<TypeRef> params;
        auto it = interfaceDecls.find(ifaceName);
        if (it == interfaceDecls.end()) {
            return params;
        }
        for (auto const &m : it->second->methods) {
            if (m->name != methodName) {
                continue;
            }
            for (auto const &param : m->params) {
                if (param.isVariadic) {
                    continue;
                }
                params.push_back(ResolveType(*param.type));
            }
            return params;
        }
        return params;
    }

    std::optional<TypeRef> InterfaceImplementationType(TypeRef const &exprType,
                                                       TypeRef const &targetType) const {
        if (targetType.kind != TypeRef::Kind::Named) {
            return std::nullopt;
        }
        auto hasVtable = [&](TypeRef const &type) {
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

    std::optional<std::uint64_t>
    SizeOfTypeRef(TypeRef const &type,
                  std::unordered_map<std::string, TypeRef> const &substitutions = {}) {
        if (type.kind == TypeRef::Kind::Named) {
            if (type.name.starts_with("Slice<")) {
                return 16;
            }
            if (auto it = substitutions.find(type.name); it != substitutions.end()) {
                return SizeOfTypeRef(it->second, substitutions);
            }
            std::string const baseName = BaseTypeName(type.name);
            std::unordered_map<std::string, TypeRef> localSubs = substitutions;
            auto const structIt = structDecls.find(baseName);
            if (structIt != structDecls.end()) {
                std::vector<TypeRef> typeArgs = ParseTypeArgsFromTypeName(type.name);
                auto const &params = structIt->second->typeParams;
                std::size_t const count = std::min(params.size(), typeArgs.size());
                for (std::size_t i = 0; i < count; ++i) {
                    localSubs[params[i]] = typeArgs[i];
                }
            }

            if (auto const enumIt = enumDecls.find(baseName); enumIt != enumDecls.end()) {
                return SizeOfEnum(*enumIt->second, localSubs);
            }
            if (interfaceDecls.contains(baseName)) {
                return 16;
            }
            return SizeOfStruct(baseName, localSubs);
        }

        if (type.kind == TypeRef::Kind::Range) {
            if (type.inner.empty()) {
                return std::nullopt;
            }
            auto const elemSize = SizeOfTypeRef(type.inner[0], substitutions);
            if (!elemSize || *elemSize == 0) {
                return std::nullopt;
            }
            return AlignUp(2 * *elemSize + 1, *elemSize);
        }

        if (type.kind == TypeRef::Kind::Tuple) {
            auto alignUp = [](std::uint64_t v, std::uint64_t a) { return (v + a - 1) & ~(a - 1); };
            std::uint64_t offset = 0;
            std::uint64_t maxAlign = 1;
            for (auto const &elem : type.inner) {
                auto const elemSize = SizeOfTypeRef(elem, substitutions);
                if (!elemSize) {
                    return std::nullopt;
                }
                std::uint64_t const al = *elemSize > 0 ? std::min(*elemSize, std::uint64_t(8)) : 1;
                if (al > 1) {
                    offset = alignUp(offset, al);
                }
                offset += *elemSize > 0 ? *elemSize : 8;
                maxAlign = std::max(maxAlign, al);
            }
            return alignUp(offset, maxAlign);
        }

        return type.SizeInBytes();
    }

    std::optional<std::uint64_t>
    SizeOfEnum(EnumDecl const &decl,
               std::unordered_map<std::string, TypeRef> const &substitutions = {}) {
        auto const tagSize = SizeOfTypeRef(EnumBaseType(decl), substitutions);
        if (!tagSize) {
            return std::nullopt;
        }

        bool hasPayload = false;
        std::uint64_t maxPayloadSize = 0;
        std::uint64_t maxPayloadAlign = 1;

        auto fieldLayout =
            [&](auto const &fields) -> std::optional<std::pair<std::uint64_t, std::uint64_t>> {
            std::uint64_t offset = 0;
            std::uint64_t maxAlign = 1;
            for (auto const &field : fields) {
                auto const fieldSize = SizeOfTypeExprWithSubstitution(*field, substitutions);
                if (!fieldSize) {
                    return std::nullopt;
                }
                std::uint64_t const align =
                    *fieldSize > 0 ? std::min<std::uint64_t>(*fieldSize, 8) : 1;
                if (align > 1) {
                    offset = AlignUp(offset, align);
                }
                offset += *fieldSize > 0 ? *fieldSize : 8;
                maxAlign = std::max(maxAlign, align);
            }
            return std::pair{AlignUp(offset, maxAlign), maxAlign};
        };

        auto namedFieldLayout =
            [&](auto const &fields) -> std::optional<std::pair<std::uint64_t, std::uint64_t>> {
            std::uint64_t offset = 0;
            std::uint64_t maxAlign = 1;
            for (auto const &field : fields) {
                auto const fieldSize = SizeOfTypeExprWithSubstitution(*field.type, substitutions);
                if (!fieldSize) {
                    return std::nullopt;
                }
                std::uint64_t const align =
                    *fieldSize > 0 ? std::min<std::uint64_t>(*fieldSize, 8) : 1;
                if (align > 1) {
                    offset = AlignUp(offset, align);
                }
                offset += *fieldSize > 0 ? *fieldSize : 8;
                maxAlign = std::max(maxAlign, align);
            }
            return std::pair{AlignUp(offset, maxAlign), maxAlign};
        };

        for (auto const &variant : decl.variants) {
            if (variant.fields.empty() && variant.namedFields.empty()) {
                continue;
            }

            hasPayload = true;
            auto payload = !variant.fields.empty() ? fieldLayout(variant.fields)
                                                   : namedFieldLayout(variant.namedFields);
            if (!payload) {
                return std::nullopt;
            }
            maxPayloadSize = std::max(maxPayloadSize, payload->first);
            maxPayloadAlign = std::max(maxPayloadAlign, payload->second);
        }

        if (!hasPayload) {
            return tagSize;
        }

        std::uint64_t const tagAlign = *tagSize > 0 ? std::min<std::uint64_t>(*tagSize, 8) : 1;
        std::uint64_t const align = std::max(tagAlign, maxPayloadAlign);
        std::uint64_t offset = *tagSize;
        if (maxPayloadAlign > 1) {
            offset = AlignUp(offset, maxPayloadAlign);
        }
        offset += maxPayloadSize;
        return AlignUp(offset, align);
    }

    TypeRef EnumBaseType(EnumDecl const &decl) {
        return decl.baseType ? ResolveType(*decl.baseType) : TypeRef::MakeInt();
    }

    TypeRef EnumType(EnumDecl const &decl) {
        TypeRef type = TypeRef::MakeNamed(decl.name);
        type.inner.push_back(EnumBaseType(decl));
        return type;
    }

    std::optional<std::uint64_t>
    SizeOfStruct(std::string const &name,
                 std::unordered_map<std::string, TypeRef> const &substitutions = {}) {
        auto const structIt = structDecls.find(name);
        if (structIt == structDecls.end()) {
            return std::nullopt;
        }

        std::uint64_t offset = 0;
        std::uint64_t maxAlign = 1;
        for (auto const &field : structIt->second->fields) {
            auto const fieldSize = SizeOfTypeExprWithSubstitution(*field.type, substitutions);
            if (!fieldSize) {
                return std::nullopt;
            }
            std::uint64_t const align = *fieldSize > 0 ? std::min<std::uint64_t>(*fieldSize, 8) : 1;
            if (align > 1) {
                offset = AlignUp(offset, align);
            }
            offset += *fieldSize > 0 ? *fieldSize : 8;
            maxAlign = std::max(maxAlign, align);
        }
        return AlignUp(offset, maxAlign);
    }

    std::optional<std::uint64_t> SizeOfTypeExprWithSubstitution(
        TypeExpr const &expr, std::unordered_map<std::string, TypeRef> const &substitutions = {}) {
        if (auto *t = dynamic_cast<NamedTypeExpr const *>(&expr)) {
            auto const structIt = structDecls.find(t->name);
            if (structIt != structDecls.end()) {
                std::unordered_map<std::string, TypeRef> fieldSubstitutions = substitutions;
                auto const &params = structIt->second->typeParams;
                for (std::size_t i = 0; i < params.size() && i < t->typeArgs.size(); ++i) {
                    fieldSubstitutions[params[i]] =
                        ResolveTypeWithSubstitution(*t->typeArgs[i], substitutions);
                }
                return SizeOfStruct(t->name, fieldSubstitutions);
            }
        }

        return SizeOfTypeRef(ResolveTypeWithSubstitution(expr, substitutions), substitutions);
    }

    std::optional<std::uint64_t> SizeOfTypeExpr(TypeExpr const &expr) {
        return SizeOfTypeExprWithSubstitution(expr);
    }

    static std::uint32_t DecodeUtf8CodePoint(std::string const &text, std::size_t i) {
        auto const byte = [&](std::size_t offset) {
            return static_cast<std::uint32_t>(static_cast<unsigned char>(text[i + offset]));
        };

        std::uint32_t const b0 = byte(0);
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
            return ((b0 & 0x07u) << 18) | ((byte(1) & 0x3Fu) << 12) | ((byte(2) & 0x3Fu) << 6) |
                   (byte(3) & 0x3Fu);
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
    static std::size_t ParseUnicodeEscape(std::string const &text, std::size_t uPos,
                                          std::uint32_t &cp) {
        std::size_t j = uPos + 1;
        if (j >= text.size() || text[j] != '{') {
            return uPos;
        }
        ++j;
        std::uint32_t value = 0;
        std::size_t digits = 0;
        for (; j < text.size() && text[j] != '}'; ++j, ++digits) {
            char const h = text[j];
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

    static std::string DecodeCharLiteral(std::string const &text) {
        // text is raw source like 'A' or '\n'; strip quotes and decode.
        std::uint32_t cp = 0;
        std::size_t const quote = text.find('\'');
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

    static std::string DecodeStringLiteral(std::string const &text) {
        // text is raw source like "hello\n" — strip quotes and decode
        // escapes
        std::string out;
        if (text.size() < 2) {
            return out;
        }
        std::size_t const quote = text.find('"');
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
                if (std::size_t const end = ParseUnicodeEscape(text, i, u); end != i) {
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

    static TypeRef LiteralType(Token const &tok) {
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

    std::vector<HirParam> LowerParams(std::vector<Param> const &params) {
        std::vector<HirParam> out;
        out.reserve(params.size());
        for (auto const &p : params) {
            HirParam hp;
            hp.name = p.name;
            hp.isVariadic = p.isVariadic;
            hp.type = p.isVariadic ? TypeRef::MakeNamed(SliceTypeName(ResolveType(*p.type)))
                                   : ResolveType(*p.type);
            out.push_back(std::move(hp));
        }
        return out;
    }

    // Derives the Rux module path (e.g. "Std::Io") from a source file path.
    // Finds the "Src" directory component and uses the relative path below
    // it.
    static std::string FilePathToModulePath(std::string const &filePath) {
        std::string const generic = std::filesystem::path(filePath).generic_string();
        std::vector<std::string> parts;
        std::string cur;
        for (char const c : generic) {
            if (c == '/') {
                if (!cur.empty()) {
                    parts.push_back(cur);
                    cur.clear();
                }
            }
            else {
                cur += c;
            }
        }
        if (!cur.empty()) {
            parts.push_back(cur);
        }

        std::size_t srcIdx = std::string::npos;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (parts[i] == "Src" || parts[i] == "src") {
                srcIdx = i;
            }
        }

        std::vector<std::string> mod;
        if (srcIdx != std::string::npos && srcIdx + 1 < parts.size()) {
            for (std::size_t i = srcIdx + 1; i < parts.size(); ++i) {
                std::string s = parts[i];
                if (i + 1 == parts.size()) {
                    auto const dot = s.rfind('.');
                    if (dot != std::string::npos) {
                        s = s.substr(0, dot);
                    }
                }
                mod.push_back(s);
            }
        }
        else {
            std::string stem = parts.empty() ? filePath : parts.back();
            auto const dot = stem.rfind('.');
            if (dot != std::string::npos) {
                stem = stem.substr(0, dot);
            }
            mod.push_back(stem);
        }

        std::string result;
        for (std::size_t i = 0; i < mod.size(); ++i) {
            if (i) {
                result += "::";
            }
            result += mod[i];
        }
        return result;
    }

    // Module lowering
    HirModule LowerModule(Module const &mod) {
        currentFile = mod.name;
        currentModulePath = FilePathToModulePath(mod.name);
        HirModule hmod;
        hmod.name = mod.name;
        for (auto const &decl : mod.items) {
            LowerTopLevelDecl(*decl, hmod);
        }
        return hmod;
    }

    void LowerTopLevelDecl(Decl const &decl, HirModule &hmod) {
        if (auto *d = dynamic_cast<FuncDecl const *>(&decl)) {
            HirFunc hf = LowerFunc(*d);
            hf.name = FunctionCalleeName(d->name, *d);
            hmod.funcs.push_back(std::move(hf));
        }
        else if (auto *d = dynamic_cast<StructDecl const *>(&decl)) {
            hmod.structs.push_back(LowerStruct(*d));
        }
        else if (auto *d = dynamic_cast<EnumDecl const *>(&decl)) {
            hmod.enums.push_back(LowerEnum(*d));
        }
        else if (auto *d = dynamic_cast<UnionDecl const *>(&decl)) {
            hmod.unions.push_back(LowerUnion(*d));
        }
        else if (auto *d = dynamic_cast<InterfaceDecl const *>(&decl)) {
            hmod.interfaces.push_back(LowerInterface(*d));
        }
        else if (auto *d = dynamic_cast<ImplDecl const *>(&decl)) {
            hmod.impls.push_back(LowerImpl(*d));
        }
        else if (auto *d = dynamic_cast<ConstDecl const *>(&decl)) {
            hmod.consts.push_back(LowerConst(*d));
        }
        else if (auto *d = dynamic_cast<ExternFuncDecl const *>(&decl)) {
            hmod.externFuncs.push_back(LowerExternFunc(*d));
        }
        else if (auto *d = dynamic_cast<ExternVarDecl const *>(&decl)) {
            hmod.externVars.push_back(LowerExternVar(*d));
        }
        else if (auto *d = dynamic_cast<ExternBlockDecl const *>(&decl)) {
            for (auto &item : d->items) {
                LowerTopLevelDecl(*item, hmod);
            }
        }
        else if (auto *d = dynamic_cast<TypeAliasDecl const *>(&decl)) {
            hmod.typeAliases.push_back(LowerTypeAlias(*d));
        }
        else if (auto *d = dynamic_cast<ModuleDecl const *>(&decl)) {
            auto const savedModulePath = currentModulePath;
            currentModulePath =
                currentModulePath.empty() ? d->name : currentModulePath + "::" + d->name;
            for (auto &item : d->items) {
                LowerTopLevelDecl(*item, hmod);
            }
            currentModulePath = savedModulePath;
        }
        // Import declarations are resolved by sema and have no HIR
        // representation.
    }

    // Declaration lowering

    HirFunc LowerFunc(FuncDecl const &d, bool isMethod = false) {
        auto savedTypeParams = currentTypeParams;
        currentTypeParams = d.typeParams;
        TypeRef retType = d.returnType ? ResolveType(**d.returnType) : TypeRef::MakeOpaque();
        auto savedRet = currentReturnType;
        currentReturnType = retType;
        auto savedFuncName = currentFunctionName;
        currentFunctionName = d.name;
        PushScope();
        for (auto const &tp : d.typeParams) {
            HirSymbol sym;
            sym.kind = HirSymbol::Kind::Type;
            sym.name = tp;
            sym.type = TypeRef::MakeTypeParam(tp);
            Define(sym);
        }
        if (isMethod) {
            HirSymbol self;
            self.kind = HirSymbol::Kind::Var;
            self.name = "self";
            self.type = currentSelfType.IsUnknown() ? TypeRef::MakeNamed("self") : currentSelfType;
            self.isMut = true;
            Define(self);
        }
        for (auto const &param : d.params) {
            if (param.name == "self") {
                continue;
            }
            HirSymbol sym;
            sym.kind = HirSymbol::Kind::Var;
            sym.name = param.name;
            sym.type = param.isVariadic
                         ? TypeRef::MakeNamed(SliceTypeName(ResolveType(*param.type)))
                         : ResolveType(*param.type);
            Define(sym);
        }
        std::optional<HirBlock> body;
        if (d.body) {
            body = LowerBlock(*d.body);
        }
        PopScope();
        currentReturnType = savedRet;
        currentTypeParams = savedTypeParams;
        currentFunctionName = savedFuncName;
        HirFunc hf;
        hf.name = d.name;
        hf.isPublic = d.isPublic;
        hf.isAsm = d.isAsm;
        hf.callConv = d.callConv;
        hf.typeParams = d.typeParams;
        hf.params = LowerParams(d.params);
        hf.returnType = retType;
        hf.body = std::move(body);
        hf.location = d.location;
        return hf;
    }

    HirStruct LowerStruct(StructDecl const &d) {
        auto savedTypeParams = currentTypeParams;
        currentTypeParams = d.typeParams;
        PushScope();
        for (auto const &tp : d.typeParams) {
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
        for (auto const &f : d.fields) {
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

    HirEnum LowerEnum(EnumDecl const &d) {
        HirEnum he;
        he.name = d.name;
        he.isPublic = d.isPublic;
        he.baseType = EnumBaseType(d);
        he.location = d.location;
        std::int64_t next = 0;
        for (auto const &v : d.variants) {
            HirEnumVariant hv;
            hv.name = v.name;
            std::int64_t value = next;
            if (v.discriminant) {
                if (auto const parsed = ParseEnumDiscriminant(*v.discriminant)) {
                    value = *parsed;
                }
            }
            hv.discriminant = std::to_string(value);
            next = value + 1;
            for (auto const &f : v.fields) {
                hv.fields.push_back(ResolveType(*f));
            }
            for (auto const &f : v.namedFields) {
                hv.fields.push_back(ResolveType(*f.type));
            }
            he.variants.push_back(std::move(hv));
        }
        return he;
    }

    HirUnion LowerUnion(UnionDecl const &d) {
        HirUnion hu;
        hu.name = d.name;
        hu.isPublic = d.isPublic;
        hu.location = d.location;
        for (auto const &f : d.fields) {
            HirUnionField hf;
            hf.name = f.name;
            hf.type = ResolveType(*f.type);
            hu.fields.push_back(std::move(hf));
        }
        return hu;
    }

    HirInterface LowerInterface(InterfaceDecl const &d) {
        HirInterface hi;
        hi.name = d.name;
        hi.isPublic = d.isPublic;
        hi.location = d.location;
        for (auto const &m : d.methods) {
            HirInterfaceMethod hm;
            hm.name = m->name;
            hm.location = m->location;
            hm.returnType = m->returnType ? ResolveType(**m->returnType) : TypeRef::MakeOpaque();
            hm.params = LowerParams(m->params);
            hi.methods.push_back(std::move(hm));
        }
        return hi;
    }

    HirImplBlock LowerImpl(ImplDecl const &d) {
        bool savedInImpl = inImpl;
        TypeRef savedSelfType = currentSelfType;
        inImpl = true;
        TypeRef selfBase;
        if (HirSymbol *sym = currentScope->Lookup(d.typeName); sym && !sym->type.IsUnknown()) {
            selfBase = sym->type;
        }
        else {
            selfBase = TypeRef::MakeNamed(d.typeName);
        }
        currentSelfType = TypeRef::MakePointer(selfBase);

        HirImplBlock hib;
        hib.typeName = d.typeName;
        hib.interfaceName = d.interfaceName;
        hib.location = d.location;
        for (auto const &m : d.methods) {
            HirFunc hf = LowerFunc(*m, /*isMethod=*/true);
            if (MethodIsOverloaded(d.typeName, m->name)) {
                TypeRef selfType = TypeRef::MakePointer(TypeRef::MakeNamed(d.typeName));
                hf.name =
                    CalleeName(d.typeName, m->name, selfType, *m).substr(d.typeName.size() + 2);
            }
            hib.methods.push_back(std::move(hf));
        }

        currentSelfType = savedSelfType;
        inImpl = savedInImpl;
        return hib;
    }

    HirConst LowerConst(ConstDecl const &d) {
        HirConst hc;
        hc.name = d.name;
        hc.isPublic = d.isPublic;
        std::optional<TypeRef> const explicitType =
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

    HirExternFunc LowerExternFunc(ExternFuncDecl const &d) {
        HirExternFunc hef;
        hef.name = d.name;
        hef.dll = d.dll;
        hef.isPublic = d.isPublic;
        hef.callConv = d.callConv;
        hef.isVariadic = d.isVariadic;
        hef.returnType = d.returnType ? ResolveType(**d.returnType) : TypeRef::MakeOpaque();
        hef.params = LowerParams(d.params);
        hef.location = d.location;
        return hef;
    }

    HirExternVar LowerExternVar(ExternVarDecl const &d) {
        HirExternVar hev;
        hev.name = d.name;
        hev.isPublic = d.isPublic;
        hev.type = ResolveType(*d.type);
        hev.location = d.location;
        return hev;
    }

    HirTypeAlias LowerTypeAlias(TypeAliasDecl const &d) {
        HirTypeAlias hta;
        hta.name = d.name;
        hta.isPublic = d.isPublic;
        hta.type = ResolveType(*d.type);
        hta.location = d.location;
        return hta;
    }

    // Block & statement lowering

    HirBlock LowerBlock(Block const &block) {
        HirBlock hb;
        hb.location = block.location;
        PushScope();
        for (auto const &stmt : block.stmts) {
            hb.stmts.push_back(LowerStmt(*stmt));
        }
        PopScope();
        return hb;
    }

    HirStmtPtr LowerStmt(Stmt const &stmt) {
        if (auto *s = dynamic_cast<ExprStmt const *>(&stmt)) {
            auto hs = std::make_unique<HirExprStmt>();
            hs->location = s->location;
            hs->expr = LowerExpr(*s->expr);
            return hs;
        }

        if (auto *s = dynamic_cast<LetStmt const *>(&stmt)) {
            auto hs = std::make_unique<HirLetStmt>();
            hs->location = s->location;
            hs->isMut = s->isMut;
            hs->name = s->name;
            std::optional<TypeRef> const explicitType =
                s->type ? std::optional<TypeRef>(ResolveType(**s->type)) : std::nullopt;
            if (s->init) {
                hs->init =
                    explicitType ? LowerExprAs(*s->init, *explicitType) : LowerExpr(*s->init);
            }
            hs->type =
                explicitType ? *explicitType : (hs->init ? hs->init->type : TypeRef::MakeUnknown());
            if (s->type) {
                if (auto const size = FixedSliceTypeSize(**s->type)) {
                    hs->stackBufferLength = *size;
                    hs->stackBufferElementType = FixedSliceElementType(**s->type);
                }
            }

            if (s->pattern) {
                hs->pattern = LowerLetPattern(*s->pattern, hs->type, s->isMut);
                return hs;
            }

            HirSymbol sym;
            sym.kind = HirSymbol::Kind::Var;
            sym.name = s->name;
            sym.type = hs->type;
            sym.isMut = s->isMut;
            Define(sym);
            return hs;
        }

        if (auto *s = dynamic_cast<IfStmt const *>(&stmt)) {
            auto hs = std::make_unique<HirIfStmt>();
            hs->location = s->location;
            hs->condition = LowerExpr(*s->condition);
            hs->thenBlock = LowerBlock(*s->thenBlock);
            for (auto const &elif : s->elseIfs) {
                HirIfStmt::ElseIf hElif;
                hElif.location = elif.location;
                hElif.condition = LowerExpr(*elif.condition);
                hElif.block = LowerBlock(*elif.block);
                hs->elseIfs.push_back(std::move(hElif));
            }
            if (s->elseBlock) {
                hs->elseBlock = LowerBlock(*s->elseBlock);
            }
            return hs;
        }

        if (auto *s = dynamic_cast<WhileStmt const *>(&stmt)) {
            auto hs = std::make_unique<HirWhileStmt>();
            hs->location = s->location;
            hs->label = s->label;
            hs->condition = LowerExpr(*s->condition);
            hs->body = LowerBlock(*s->body);
            return hs;
        }

        if (auto *s = dynamic_cast<DoWhileStmt const *>(&stmt)) {
            auto hs = std::make_unique<HirDoWhileStmt>();
            hs->location = s->location;
            hs->label = s->label;
            hs->body = LowerBlock(*s->body);
            hs->condition = LowerExpr(*s->condition);
            return hs;
        }

        if (auto *s = dynamic_cast<LoopStmt const *>(&stmt)) {
            auto hs = std::make_unique<HirLoopStmt>();
            hs->location = s->location;
            hs->label = s->label;
            hs->body = LowerBlock(*s->body);
            return hs;
        }

        if (auto *s = dynamic_cast<ForStmt const *>(&stmt)) {
            auto hs = std::make_unique<HirForStmt>();
            hs->location = s->location;
            hs->label = s->label;
            hs->variable = s->variable;
            hs->iterable = LowerExpr(*s->iterable);
            TypeRef elemType = TypeRef::MakeUnknown();
            if (hs->iterable->type.IsRange() && !hs->iterable->type.inner.empty()) {
                elemType = hs->iterable->type.inner[0];
            }
            else if (auto sliceElem = SliceElementType(hs->iterable->type)) {
                elemType = *sliceElem;
            }
            hs->varType = elemType;
            PushScope();
            HirSymbol var;
            var.kind = HirSymbol::Kind::Var;
            var.name = s->variable;
            var.type = elemType;
            Define(var);
            hs->body = LowerBlock(*s->body);
            PopScope();
            return hs;
        }

        if (auto *s = dynamic_cast<MatchStmt const *>(&stmt)) {
            auto hs = std::make_unique<HirMatchStmt>();
            hs->location = s->location;
            hs->subject = LowerExpr(*s->subject);
            for (auto const &arm : s->arms) {
                HirMatchArm ha;
                ha.location = arm.location;
                PushScope();
                ha.pattern = LowerPattern(*arm.pattern);
                ha.body = LowerExpr(*arm.body);
                PopScope();
                hs->arms.push_back(std::move(ha));
            }
            return hs;
        }

        if (auto *s = dynamic_cast<ReturnStmt const *>(&stmt)) {
            auto hs = std::make_unique<HirReturnStmt>();
            hs->location = s->location;
            if (s->value) {
                hs->value = LowerExprAs(**s->value, currentReturnType);
            }
            return hs;
        }

        if (auto *s = dynamic_cast<BreakStmt const *>(&stmt)) {
            auto hs = std::make_unique<HirBreakStmt>();
            hs->location = stmt.location;
            hs->label = s->label;
            return hs;
        }

        if (auto *s = dynamic_cast<ContinueStmt const *>(&stmt)) {
            auto hs = std::make_unique<HirContinueStmt>();
            hs->location = stmt.location;
            hs->label = s->label;
            return hs;
        }

        if (auto *s = dynamic_cast<DeclStmt const *>(&stmt)) {
            auto hs = std::make_unique<HirLocalDecl>();
            hs->location = s->location;
            CollectDecl(*s->decl);
            if (auto *fd = dynamic_cast<FuncDecl const *>(s->decl.get())) {
                hs->description = std::format("func {}", fd->name);
            }
            else if (auto *cd = dynamic_cast<ConstDecl const *>(s->decl.get())) {
                hs->description = std::format("const {}", cd->name);
                HirConst constant = LowerConst(*cd);
                hs->hasConstant = true;
                hs->constantName = std::move(constant.name);
                hs->constantType = std::move(constant.type);
                hs->constantValue = std::move(constant.value);
            }
            else if (auto *ta = dynamic_cast<TypeAliasDecl const *>(s->decl.get())) {
                hs->description = std::format("type {}", ta->name);
            }
            else {
                hs->description = "<local decl>";
            }
            return hs;
        }
        // Unreachable in valid AST
        auto hs = std::make_unique<HirLocalDecl>();
        hs->location = stmt.location;
        hs->description = "<unknown stmt>";
        return hs;
    }

    // Expression lowering
    HirExprPtr LowerExprAs(Expr const &expr, TypeRef const &targetType) {
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
                std::optional<TypeRef> implementationType =
                    InterfaceImplementationType(lowered->type, targetType);
                if (!implementationType) {
                    implementationType = lowered->type;
                }
                std::string const typeName = implementationType->ToString();
                if (UnsuffixedIntegerLiteralFits(expr, *implementationType)) {
                    lowered->type = *implementationType;
                }
                auto coerce = std::make_unique<HirCoerceToInterfaceExpr>();
                coerce->location = expr.location;
                coerce->type = targetType;
                // Only reference a vtable when there are methods to
                // dispatch. Empty interfaces have nothing to dispatch, so
                // no vtable is generated.
                auto const ifaceIt = interfaceDecls.find(targetType.name);
                if (ifaceIt != interfaceDecls.end() && !ifaceIt->second->methods.empty()) {
                    coerce->vtableLabel = "__vtable__" + typeName + "__" + targetType.name;
                }
                coerce->value = std::move(lowered);
                return coerce;
            }
        }
        return lowered;
    }

    // Like LowerExprAs but, for intrinsic defaults, evaluates at
    // callSiteLoc rather than at the declaration site (call-site builtins:
    // #line, #column, #file, #ruxVersion etc.).
    HirExprPtr LowerDefaultArg(Expr const &defaultExpr, TypeRef const &targetType,
                               SourceLocation const &callSiteLoc) {
        if (auto const *intr = dynamic_cast<IntrinsicExpr const *>(&defaultExpr)) {
            IntrinsicExpr tmp;
            tmp.location = callSiteLoc;
            tmp.kind = intr->kind;
            return LowerExprAs(tmp, targetType);
        }
        return LowerExprAs(defaultExpr, targetType);
    }

    TypeRef StructInitFieldType(StructInitExpr const &expr, std::string const &fieldName) {
        auto const structIt = structDecls.find(expr.typeName);
        if (structIt == structDecls.end()) {
            if (auto const [enumDecl, variant] = LookupEnumVariantInitializer(expr.typeName);
                enumDecl && variant) {
                for (auto const &field : variant->namedFields) {
                    if (field.name == fieldName) {
                        return ResolveType(*field.type);
                    }
                }
            }
            return TypeRef::MakeUnknown();
        }

        auto const substitutions = StructTypeSubstitutions(*structIt->second, expr.typeArgs);
        for (auto const &field : structIt->second->fields) {
            if (field.name == fieldName) {
                return ResolveTypeWithSubstitution(*field.type, substitutions);
            }
        }
        return TypeRef::MakeUnknown();
    }

    HirExprPtr LowerExpr(Expr const &expr) {
        if (auto *e = dynamic_cast<LiteralExpr const *>(&expr)) {
            auto he = std::make_unique<HirLiteralExpr>();
            he->location = e->location;
            he->type = LiteralType(e->token);
            if (e->token.kind == TokenKind::CharLiteral) {
                he->value = DecodeCharLiteral(e->token.text);
            }
            else if (e->token.kind == TokenKind::StringLiteral) {
                he->value = DecodeStringLiteral(e->token.text);
            }
            else if (e->token.kind == TokenKind::IntLiteral ||
                     e->token.kind == TokenKind::FloatLiteral) {
                he->value = StripNumericLiteralSuffix(e->token.text);
            }
            else {
                he->value = e->token.text;
            }
            return he;
        }
        if (auto *e = dynamic_cast<IdentExpr const *>(&expr)) {
            auto he = std::make_unique<HirVarExpr>();
            he->location = e->location;
            he->name = e->name;
            if (HirSymbol *sym = currentScope->Lookup(e->name)) {
                he->type = sym->type;
            }
            return he;
        }
        if (dynamic_cast<SelfExpr const *>(&expr)) {
            auto he = std::make_unique<HirSelfExpr>();
            he->location = expr.location;
            he->type = currentSelfType.IsUnknown() ? TypeRef::MakeNamed("self") : currentSelfType;
            return he;
        }
        if (auto *e = dynamic_cast<PathExpr const *>(&expr)) {
            if (e->segments.size() == 2) {
                if (HirSymbol *first = currentScope->Lookup(e->segments[0]);
                    first && (first->kind == HirSymbol::Kind::Type ||
                              first->kind == HirSymbol::Kind::Interface)) {
                    if (first->kind == HirSymbol::Kind::Type) {
                        if (auto const discriminant =
                                LookupEnumVariantDiscriminant(e->segments[0], e->segments[1])) {
                            auto const *variant = LookupEnumVariant(e->segments[0], e->segments[1]);
                            if (variant &&
                                (!variant->fields.empty() || !variant->namedFields.empty())) {
                                auto he = std::make_unique<HirPathExpr>();
                                he->location = e->location;
                                he->segments = e->segments;
                                he->type = EnumVariantConstructorType(*enumDecls.at(e->segments[0]),
                                                                      *variant);
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
                    TypeRef receiverType =
                        first->type.IsUnknown() ? TypeRef::MakeNamed(first->name) : first->type;
                    if (FuncDecl const *method = LookupMethod(receiverType, e->segments[1])) {
                        auto he = std::make_unique<HirVarExpr>();
                        he->location = e->location;
                        he->name =
                            CalleeName(e->segments[0], e->segments[1], receiverType, *method);
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
        if (auto *e = dynamic_cast<SizeOfExpr const *>(&expr)) {
            auto he = std::make_unique<HirLiteralExpr>();
            he->location = e->location;
            he->type = TypeRef::MakeUInt64();
            he->value = std::to_string(SizeOfTypeExpr(*e->type).value_or(0));
            return he;
        }
        if (auto *e = dynamic_cast<IntrinsicExpr const *>(&expr)) {
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
                he->type = TypeRef::MakeNamed(SliceTypeName(TypeRef::MakeChar8()));
                he->value = std::filesystem::path(currentFile).filename().string();
                break;
            case K::Function:
                he->type = TypeRef::MakeNamed(SliceTypeName(TypeRef::MakeChar8()));
                he->value = currentFunctionName;
                break;
            case K::Date: {
                std::time_t t = std::time(nullptr);
                std::tm tm{};
                LocalTime(t, tm);
                char buf[12];
                std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
                he->type = TypeRef::MakeNamed(SliceTypeName(TypeRef::MakeChar8()));
                he->value = buf;
                break;
            }
            case K::Time: {
                std::time_t t = std::time(nullptr);
                std::tm tm{};
                LocalTime(t, tm);
                char buf[9];
                std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
                he->type = TypeRef::MakeNamed(SliceTypeName(TypeRef::MakeChar8()));
                he->value = buf;
                break;
            }
            case K::Module: {
                he->type = TypeRef::MakeNamed(SliceTypeName(TypeRef::MakeChar8()));
                he->value = currentModulePath;
                break;
            }
            case K::RuxVersion: {
                he->type = TypeRef::MakeNamed(SliceTypeName(TypeRef::MakeChar8()));
                he->value = RUX_VERSION;
                break;
            }
            case K::Os: {
                he->type = TypeRef::MakeNamed(SliceTypeName(TypeRef::MakeChar8()));
                std::string os;
#if RUX_OS_WINDOWS
                os = "Windows";
#elif RUX_OS_LINUX
                os = "Linux";
#elif RUX_OS_MACOS
                os = "macOS";
#endif
                he->value = os;
                break;
            }
            }
            return he;
        }
        if (auto *e = dynamic_cast<UnaryExpr const *>(&expr)) {
            auto he = std::make_unique<HirUnaryExpr>();
            he->location = e->location;
            he->op = e->op;
            he->operand = LowerExpr(*e->operand);
            he->type = InferUnaryType(e->op, he->operand->type);
            return he;
        }
        if (auto *e = dynamic_cast<PostfixExpr const *>(&expr)) {
            auto he = std::make_unique<HirPostfixExpr>();
            he->location = e->location;
            he->op = e->op;
            he->operand = LowerExpr(*e->operand);
            he->type = he->operand->type;
            return he;
        }
        if (auto *e = dynamic_cast<BinaryExpr const *>(&expr)) {
            HirExprPtr left = LowerExpr(*e->left);
            HirExprPtr right = LowerExpr(*e->right);
            std::string const opName = std::string(OpStr(e->op));
            if (FuncDecl const *method = LookupMethod(left->type, opName, {right->type})) {
                std::string const receiverBase = NamedBaseTypeName(left->type);
                HirExprPtr selfArg;
                if (left->type.kind == TypeRef::Kind::Pointer) {
                    selfArg = std::move(left);
                }
                else {
                    auto addr = std::make_unique<HirUnaryExpr>();
                    addr->location = left->location;
                    addr->op = TokenKind::Amp;
                    addr->type = TypeRef::MakePointer(left->type);
                    addr->operand = std::move(left);
                    selfArg = std::move(addr);
                }

                auto callee = std::make_unique<HirVarExpr>();
                callee->location = e->location;
                callee->name = CalleeName(receiverBase, opName, selfArg->type, *method);
                callee->type = MethodType(selfArg->type, *method);

                auto call = std::make_unique<HirCallExpr>();
                call->location = e->location;
                call->type =
                    callee->type.inner.empty() ? TypeRef::MakeUnknown() : callee->type.inner.back();
                call->callee = std::move(callee);
                call->args.push_back(std::move(selfArg));
                if (call->callee->type.inner.size() > 2) {
                    TypeRef const &expectedType = call->callee->type.inner[1];
                    if (UnsuffixedIntegerLiteralFits(*e->right, expectedType)) {
                        right->type = expectedType;
                    }
                    else if (IsNullLiteral(*e->right) &&
                             expectedType.kind == TypeRef::Kind::Pointer) {
                        right->type = expectedType;
                        if (auto *lit = dynamic_cast<HirLiteralExpr *>(right.get())) {
                            lit->value = "0";
                        }
                    }
                }
                call->args.push_back(std::move(right));
                return call;
            }

            auto he = std::make_unique<HirBinaryExpr>();
            he->location = e->location;
            he->op = e->op;
            he->left = std::move(left);
            he->right = std::move(right);
            he->type = InferBinaryType(e->op, he->left->type, he->right->type);
            return he;
        }
        if (auto *e = dynamic_cast<AssignExpr const *>(&expr)) {
            auto he = std::make_unique<HirAssignExpr>();
            he->location = e->location;
            he->op = e->op;
            he->target = LowerExpr(*e->target);
            he->value = LowerExprAs(*e->value, he->target->type);
            he->type = he->target->type;
            return he;
        }
        if (auto *e = dynamic_cast<TernaryExpr const *>(&expr)) {
            auto he = std::make_unique<HirTernaryExpr>();
            he->location = e->location;
            he->condition = LowerExpr(*e->condition);
            he->thenExpr = LowerExpr(*e->thenExpr);
            he->elseExpr = LowerExpr(*e->elseExpr);
            he->type = he->thenExpr->type.IsUnknown() ? he->elseExpr->type : he->thenExpr->type;
            return he;
        }
        if (auto *e = dynamic_cast<RangeExpr const *>(&expr)) {
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
            if (elemType.IsUnknown()) {
                elemType = TypeRef::MakeInt64();
            }
            he->type = TypeRef::MakeRange(elemType);
            return he;
        }
        if (auto *e = dynamic_cast<CallExpr const *>(&expr)) {
            if (auto *path = dynamic_cast<PathExpr const *>(e->callee.get());
                path && path->segments.size() == 2) {
                auto const *variant = LookupEnumVariant(path->segments[0], path->segments[1]);
                if (variant && (!variant->fields.empty() || !variant->namedFields.empty())) {
                    TypeExpr const *singlePayloadType = nullptr;
                    if (variant->fields.size() == 1 && variant->namedFields.empty()) {
                        singlePayloadType = variant->fields[0].get();
                    }
                    else if (variant->fields.empty() && variant->namedFields.size() == 1) {
                        singlePayloadType = variant->namedFields[0].type.get();
                    }

                    if (singlePayloadType && e->args.size() == 1) {
                        auto he = std::make_unique<HirEnumConstructExpr>();
                        he->location = e->location;
                        he->type = EnumType(*enumDecls.at(path->segments[0]));
                        he->payloads.push_back(
                            LowerExprAs(*e->args[0], ResolveType(*singlePayloadType)));
                        he->discriminant =
                            LookupEnumVariantDiscriminant(path->segments[0], path->segments[1])
                                .value_or("0");
                        return he;
                    }

                    auto he = std::make_unique<HirLiteralExpr>();
                    he->location = e->location;
                    he->type = EnumType(*enumDecls.at(path->segments[0]));
                    he->value = LookupEnumVariantDiscriminant(path->segments[0], path->segments[1])
                                    .value_or("0");
                    return he;
                }
            }

            if (auto *path = dynamic_cast<PathExpr const *>(e->callee.get());
                path && path->segments.size() == 2) {
                HirSymbol *first = currentScope->Lookup(path->segments[0]);
                if (first &&
                    (first->kind == HirSymbol::Kind::Type ||
                     first->kind == HirSymbol::Kind::Interface) &&
                    !LookupEnumVariant(path->segments[0], path->segments[1])) {
                    TypeRef receiverType =
                        first->type.IsUnknown() ? TypeRef::MakeNamed(first->name) : first->type;
                    std::vector<HirExprPtr> args;
                    std::vector<TypeRef> argTypes;
                    args.reserve(e->args.size());
                    argTypes.reserve(e->args.size());
                    for (auto const &arg : e->args) {
                        auto lowered = LowerExpr(*arg);
                        argTypes.push_back(lowered->type);
                        args.push_back(std::move(lowered));
                    }
                    if (FuncDecl const *method =
                            LookupMethod(receiverType, path->segments[1], argTypes)) {
                        TypeRef funcType = AssociatedFunctionType(receiverType, *method);
                        auto callee = std::make_unique<HirVarExpr>();
                        callee->location = path->location;
                        callee->name =
                            CalleeName(path->segments[0], path->segments[1], receiverType, *method);
                        callee->type = funcType;
                        auto he = std::make_unique<HirCallExpr>();
                        he->location = e->location;
                        he->callee = std::move(callee);
                        for (std::size_t i = 0; i < args.size(); ++i) {
                            if (i + 1 < funcType.inner.size() &&
                                UnsuffixedIntegerLiteralFits(*e->args[i], funcType.inner[i])) {
                                args[i]->type = funcType.inner[i];
                            }
                            he->args.push_back(std::move(args[i]));
                        }
                        he->type =
                            funcType.inner.empty() ? TypeRef::MakeUnknown() : funcType.inner.back();
                        return he;
                    }
                }
            }

            if (auto *path = dynamic_cast<PathExpr const *>(e->callee.get());
                path && path->segments.size() >= 2) {
                std::vector<HirExprPtr> args;
                std::vector<TypeRef> argTypes;
                args.reserve(e->args.size());
                argTypes.reserve(e->args.size());
                for (auto const &arg : e->args) {
                    auto lowered = LowerExpr(*arg);
                    argTypes.push_back(lowered->type);
                    args.push_back(std::move(lowered));
                }
                std::string const &funcName = path->segments.back();
                HirSymbol *sym = currentScope->Lookup(funcName);
                if (sym && sym->kind == HirSymbol::Kind::Func && !sym->funcOverloads.empty()) {
                    if (FuncDecl const *decl = LookupFunction(funcName, argTypes)) {
                        TypeRef funcType =
                            MakeFuncType(decl->params, decl->returnType, decl->typeParams);
                        if (funcType.kind == TypeRef::Kind::Func && !funcType.inner.empty()) {
                            bool const isVariadic =
                                !decl->params.empty() && decl->params.back().isVariadic;
                            std::size_t const fixedCount =
                                decl->params.size() - (isVariadic ? 1 : 0);
                            for (std::size_t i = args.size(); i < fixedCount; ++i) {
                                if (decl->params[i].defaultValue) {
                                    TypeRef pt = (i + 1 < funcType.inner.size())
                                                   ? funcType.inner[i]
                                                   : TypeRef::MakeUnknown();
                                    args.push_back(LowerDefaultArg(**decl->params[i].defaultValue,
                                                                   pt, e->location));
                                }
                            }
                            if (isVariadic) {
                                TypeRef varElemType = ResolveType(*decl->params.back().type);
                                bool const isSingleSpread =
                                    (e->args.size() == fixedCount + 1 &&
                                     dynamic_cast<SpreadExpr const *>(e->args[fixedCount].get()));
                                if (isSingleSpread) {
                                    HirExprPtr sliceArg = std::move(args[fixedCount]);
                                    sliceArg->type = TypeRef::MakeNamed(SliceTypeName(varElemType));
                                    args.resize(fixedCount);
                                    args.push_back(std::move(sliceArg));
                                }
                                else {
                                    auto slice = std::make_unique<HirSliceExpr>();
                                    slice->location = e->location;
                                    slice->elementType = varElemType;
                                    slice->type = TypeRef::MakeNamed(SliceTypeName(varElemType));
                                    for (std::size_t i = fixedCount; i < e->args.size(); ++i) {
                                        slice->elements.push_back(
                                            LowerExprAs(*e->args[i], varElemType));
                                    }
                                    args.resize(fixedCount);
                                    args.push_back(std::move(slice));
                                }
                            }

                            auto callee = std::make_unique<HirVarExpr>();
                            callee->location = path->location;
                            callee->name = FunctionCalleeName(funcName, *decl);
                            callee->type = funcType;

                            auto he = std::make_unique<HirCallExpr>();
                            he->location = e->location;
                            he->type = funcType.inner.back();
                            he->callee = std::move(callee);
                            for (std::size_t i = 0; i < args.size(); ++i) {
                                if (i < e->args.size() && i + 1 < funcType.inner.size() &&
                                    UnsuffixedIntegerLiteralFits(*e->args[i], funcType.inner[i])) {
                                    args[i]->type = funcType.inner[i];
                                }
                                he->args.push_back(std::move(args[i]));
                            }
                            return he;
                        }
                    }
                }
            }

            if (auto *ident = dynamic_cast<IdentExpr const *>(e->callee.get())) {
                std::vector<HirExprPtr> args;
                std::vector<TypeRef> argTypes;
                args.reserve(e->args.size());
                argTypes.reserve(e->args.size());
                for (auto const &arg : e->args) {
                    auto lowered = LowerExpr(*arg);
                    argTypes.push_back(lowered->type);
                    args.push_back(std::move(lowered));
                }
                HirSymbol *sym = currentScope->Lookup(ident->name);
                if (sym && sym->kind == HirSymbol::Kind::Func && !sym->funcOverloads.empty()) {
                    if (FuncDecl const *decl = LookupFunction(ident->name, argTypes)) {
                        TypeRef funcType =
                            MakeFuncType(decl->params, decl->returnType, decl->typeParams);
                        if (funcType.kind == TypeRef::Kind::Func && !funcType.inner.empty()) {
                            bool const isVariadic =
                                !decl->params.empty() && decl->params.back().isVariadic;
                            std::size_t const fixedCount =
                                decl->params.size() - (isVariadic ? 1 : 0);
                            // Inject default arguments for omitted fixed
                            // parameters
                            for (std::size_t i = args.size(); i < fixedCount; ++i) {
                                if (decl->params[i].defaultValue) {
                                    TypeRef pt = (i + 1 < funcType.inner.size())
                                                   ? funcType.inner[i]
                                                   : TypeRef::MakeUnknown();
                                    args.push_back(LowerDefaultArg(**decl->params[i].defaultValue,
                                                                   pt, e->location));
                                }
                            }
                            if (isVariadic) {
                                TypeRef varElemType = ResolveType(*decl->params.back().type);
                                bool const isSingleSpread =
                                    (e->args.size() == fixedCount + 1 &&
                                     dynamic_cast<SpreadExpr const *>(e->args[fixedCount].get()));
                                if (isSingleSpread) {
                                    // Pass the already-lowered slice
                                    // through directly
                                    HirExprPtr sliceArg = std::move(args[fixedCount]);
                                    sliceArg->type = TypeRef::MakeNamed(SliceTypeName(varElemType));
                                    args.resize(fixedCount);
                                    args.push_back(std::move(sliceArg));
                                }
                                else {
                                    auto slice = std::make_unique<HirSliceExpr>();
                                    slice->location = e->location;
                                    slice->elementType = varElemType;
                                    slice->type = TypeRef::MakeNamed(SliceTypeName(varElemType));
                                    for (std::size_t i = fixedCount; i < e->args.size(); ++i) {
                                        slice->elements.push_back(
                                            LowerExprAs(*e->args[i], varElemType));
                                    }
                                    args.resize(fixedCount);
                                    args.push_back(std::move(slice));
                                }
                            }

                            auto callee = std::make_unique<HirVarExpr>();
                            callee->location = ident->location;
                            callee->name = FunctionCalleeName(ident->name, *decl);
                            callee->type = funcType;

                            auto he = std::make_unique<HirCallExpr>();
                            he->location = e->location;
                            he->type = funcType.inner.back();
                            he->callee = std::move(callee);
                            for (std::size_t i = 0; i < args.size(); ++i) {
                                if (i < e->args.size() && i + 1 < funcType.inner.size() &&
                                    UnsuffixedIntegerLiteralFits(*e->args[i], funcType.inner[i])) {
                                    args[i]->type = funcType.inner[i];
                                }
                                he->args.push_back(std::move(args[i]));
                            }
                            return he;
                        }
                    }
                }
            }

            auto he = std::make_unique<HirCallExpr>();
            he->location = e->location;
            if (auto *field = dynamic_cast<FieldExpr const *>(e->callee.get())) {
                HirExprPtr receiver = LowerExpr(*field->object);
                std::string const receiverBase = NamedBaseTypeName(receiver->type);
                // Pre-lower args when we have overloads so we can pick the
                // right one.
                std::vector<HirExprPtr> preArgs;
                std::vector<TypeRef> argTypes;
                if (MethodIsOverloaded(receiverBase, field->field)) {
                    for (auto const &arg : e->args) {
                        preArgs.push_back(LowerExpr(*arg));
                        argTypes.push_back(preArgs.back()->type);
                    }
                }
                if (FuncDecl const *method = LookupMethod(receiver->type, field->field, argTypes)) {
                    HirExprPtr selfArg;
                    if (receiver->type.kind == TypeRef::Kind::Pointer) {
                        selfArg = std::move(receiver);
                    }
                    else {
                        auto addr = std::make_unique<HirUnaryExpr>();
                        addr->location = receiver->location;
                        addr->op = TokenKind::Amp;
                        addr->type = TypeRef::MakePointer(receiver->type);
                        addr->operand = std::move(receiver);
                        selfArg = std::move(addr);
                    }
                    auto callee = std::make_unique<HirVarExpr>();
                    callee->location = field->location;
                    callee->name = CalleeName(receiverBase, field->field, selfArg->type, *method);
                    callee->type = MethodType(selfArg->type, *method);
                    he->callee = std::move(callee);
                    he->args.push_back(std::move(selfArg));
                    if (!preArgs.empty()) {
                        for (std::size_t i = 0; i < preArgs.size(); ++i) {
                            if (i + 1 < he->callee->type.inner.size()) {
                                TypeRef const &expectedType = he->callee->type.inner[i + 1];
                                if (UnsuffixedIntegerLiteralFits(*e->args[i], expectedType)) {
                                    preArgs[i]->type = expectedType;
                                }
                                else if (IsNullLiteral(*e->args[i]) &&
                                         expectedType.kind == TypeRef::Kind::Pointer) {
                                    preArgs[i]->type = expectedType;
                                    if (auto *lit =
                                            dynamic_cast<HirLiteralExpr *>(preArgs[i].get())) {
                                        lit->value = "0";
                                    }
                                }
                            }
                            he->args.push_back(std::move(preArgs[i]));
                        }
                    }
                    else {
                        for (std::size_t i = 0; i < e->args.size(); ++i) {
                            he->args.push_back(
                                i + 1 < he->callee->type.inner.size()
                                    ? LowerExprAs(*e->args[i], he->callee->type.inner[i + 1])
                                    : LowerExpr(*e->args[i]));
                        }
                    }
                    he->type = he->callee->type.inner.back();
                    return he;
                }
                // Interface dispatch: receiver type is a known interface
                if (receiver && receiver->type.kind == TypeRef::Kind::Named) {
                    std::string const receiverName = BaseTypeName(receiver->type.name);
                    if (HirSymbol *sym = currentScope->Lookup(receiverName);
                        sym && sym->kind == HirSymbol::Kind::Interface) {
                        int const idx = InterfaceMethodIndex(receiverName, field->field);
                        if (idx >= 0) {
                            auto ic = std::make_unique<HirInterfaceCallExpr>();
                            ic->location = e->location;
                            ic->methodIdx = idx;
                            ic->type = InterfaceMethodReturnType(receiverName, field->field);
                            ic->fatPtrExpr = std::move(receiver);
                            if (!preArgs.empty()) {
                                for (auto &a : preArgs) {
                                    ic->args.push_back(std::move(a));
                                }
                            }
                            else {
                                std::vector<TypeRef> const paramTypes =
                                    InterfaceMethodParamTypes(receiverName, field->field);
                                for (std::size_t i = 0; i < e->args.size(); ++i) {
                                    ic->args.push_back(i < paramTypes.size()
                                                           ? LowerExprAs(*e->args[i], paramTypes[i])
                                                           : LowerExpr(*e->args[i]));
                                }
                            }
                            return ic;
                        }
                    }
                }
            }

            he->callee = LowerExpr(*e->callee);
            bool const hasParamTypes = he->callee->type.kind == TypeRef::Kind::Func &&
                                       he->callee->type.inner.size() == e->args.size() + 1;
            for (std::size_t i = 0; i < e->args.size(); ++i) {
                he->args.push_back(hasParamTypes
                                       ? LowerExprAs(*e->args[i], he->callee->type.inner[i])
                                       : LowerExpr(*e->args[i]));
            }
            // Propagate return type if callee is a known func type
            if (he->callee->type.kind == TypeRef::Kind::Func && !he->callee->type.inner.empty()) {
                he->type = he->callee->type.inner.back();
            }
            return he;
        }
        if (auto *e = dynamic_cast<IndexExpr const *>(&expr)) {
            auto he = std::make_unique<HirIndexExpr>();
            he->location = e->location;
            he->object = LowerExpr(*e->object);
            he->index = LowerExpr(*e->index);
            if (auto elemType = IndexElementType(he->object->type)) {
                he->type = *elemType;
            }
            return he;
        }
        if (auto *e = dynamic_cast<FieldExpr const *>(&expr)) {
            auto he = std::make_unique<HirFieldExpr>();
            he->location = e->location;
            he->object = LowerExpr(*e->object);
            he->field = e->field;
            if (auto elemType = SliceElementType(he->object->type)) {
                if (e->field == "data") {
                    he->type = TypeRef::MakePointer(*elemType);
                }
                else if (e->field == "length") {
                    he->type = TypeRef::MakeUInt64();
                }
            }
            else if (he->object->type.IsRange()) {
                TypeRef elemType = he->object->type.inner.empty() ? TypeRef::MakeInt64()
                                                                  : he->object->type.inner[0];
                if (e->field == "lo" || e->field == "hi") {
                    he->type = elemType;
                }
                else if (e->field == "inclusive") {
                    he->type = TypeRef::MakeBool();
                }
            }
            else if (he->object->type.kind == TypeRef::Kind::Tuple) {
                try {
                    std::size_t const idx = std::stoul(e->field);
                    if (idx < he->object->type.inner.size()) {
                        he->type = he->object->type.inner[idx];
                    }
                }
                catch (...) {
                }
            }
            else if (std::string const ifaceName = NamedBaseTypeName(he->object->type);
                     !ifaceName.empty() && interfaceDecls.contains(ifaceName)) {
                if (e->field == "data" || e->field == "vtable") {
                    he->type = TypeRef::MakePointer(TypeRef::MakeOpaque());
                }
            }
            else {
                he->type = StructFieldType(he->object->type, e->field);
            }
            return he;
        }
        if (auto *e = dynamic_cast<StructInitExpr const *>(&expr)) {
            if (auto const [enumDecl, variant] = LookupEnumVariantInitializer(e->typeName);
                enumDecl && variant) {
                if (!variant->namedFields.empty()) {
                    auto he = std::make_unique<HirEnumConstructExpr>();
                    he->location = e->location;
                    he->type = EnumType(*enumDecl);
                    std::size_t const sep = e->typeName.find("::");
                    he->discriminant = LookupEnumVariantDiscriminant(e->typeName.substr(0, sep),
                                                                     e->typeName.substr(sep + 2))
                                           .value_or("0");
                    for (auto const &field : variant->namedFields) {
                        StructInitExpr::Field const *initField = nullptr;
                        for (auto const &f : e->fields) {
                            if (f.name == field.name) {
                                initField = &f;
                                break;
                            }
                        }
                        if (initField) {
                            he->payloads.push_back(
                                LowerExprAs(*initField->value, ResolveType(*field.type)));
                        }
                    }
                    return he;
                }

                auto he = std::make_unique<HirLiteralExpr>();
                he->location = e->location;
                he->type = EnumType(*enumDecl);
                std::size_t const sep = e->typeName.find("::");
                he->value = LookupEnumVariantDiscriminant(e->typeName.substr(0, sep),
                                                          e->typeName.substr(sep + 2))
                                .value_or("0");
                return he;
            }

            auto he = std::make_unique<HirStructInitExpr>();
            he->location = e->location;
            he->typeName = GenericStructInitName(*e);
            he->type = TypeRef::MakeNamed(he->typeName);
            for (auto const &f : e->fields) {
                HirStructInitField hf;
                hf.name = f.name;
                hf.value = LowerExprAs(*f.value, StructInitFieldType(*e, f.name));
                he->fields.push_back(std::move(hf));
            }
            return he;
        }
        if (auto *e = dynamic_cast<SliceExpr const *>(&expr)) {
            auto he = std::make_unique<HirSliceExpr>();
            he->location = e->location;
            TypeRef elemType = TypeRef::MakeUnknown();
            for (auto const &el : e->elements) {
                he->elements.push_back(LowerExpr(*el));
                if (elemType.IsUnknown()) {
                    elemType = he->elements.back()->type;
                }
            }
            he->elementType = elemType;
            he->type = TypeRef::MakeNamed(SliceTypeName(elemType));
            return he;
        }
        if (auto *e = dynamic_cast<TupleExpr const *>(&expr)) {
            auto he = std::make_unique<HirTupleExpr>();
            he->location = e->location;
            std::vector<TypeRef> elemTypes;
            for (auto const &el : e->elements) {
                he->elements.push_back(LowerExpr(*el));
                elemTypes.push_back(he->elements.back()->type);
            }
            he->type = TypeRef::MakeTuple(std::move(elemTypes));
            return he;
        }
        if (auto *e = dynamic_cast<CastExpr const *>(&expr)) {
            auto he = std::make_unique<HirCastExpr>();
            he->location = e->location;
            he->operand = LowerExpr(*e->operand);
            he->targetType = ResolveType(*e->type);
            he->type = he->targetType;
            return he;
        }
        if (auto *e = dynamic_cast<IsExpr const *>(&expr)) {
            // The answer is statically known for all non-interface types.
            // Interface types are rejected by Sema, so this path never
            // reaches Lir.
            auto he = std::make_unique<HirLiteralExpr>();
            he->value = LowerExpr(*e->operand)->type == ResolveType(*e->type) ? "true" : "false";
            he->type = TypeRef::MakeBool();
            return he;
        }
        if (auto *e = dynamic_cast<MatchExpr const *>(&expr)) {
            auto he = std::make_unique<HirMatchExpr>();
            he->location = e->location;
            he->subject = LowerExpr(*e->subject);
            for (auto const &arm : e->arms) {
                HirMatchArm ha;
                ha.location = arm.location;
                PushScope();
                ha.pattern = LowerPattern(*arm.pattern);
                ha.body = LowerExpr(*arm.body);
                PopScope();
                if (he->type.IsUnknown()) {
                    he->type = ha.body->type;
                }
                he->arms.push_back(std::move(ha));
            }
            return he;
        }
        if (auto *e = dynamic_cast<BlockExpr const *>(&expr)) {
            auto he = std::make_unique<HirBlockExpr>();
            he->location = e->location;
            he->block = LowerBlock(*e->block);
            return he;
        }
        if (auto *e = dynamic_cast<SpreadExpr const *>(&expr)) {
            return LowerExpr(*e->operand);
        }

        // Fallback for unrecognized expression kinds
        auto he = std::make_unique<HirLiteralExpr>();
        he->location = expr.location;
        he->value = "<expr>";
        return he;
    }

    static TypeRef InferUnaryType(TokenKind op, TypeRef const &t) {
        switch (op) {
        case TokenKind::Bang:
            return TypeRef::MakeBool();
        case TokenKind::Amp:
            return TypeRef::MakePointer(t);
        case TokenKind::Star:
            return t.inner.empty() ? TypeRef::MakeUnknown() : t.inner[0];
        default:
            return t;
        }
    }

    static TypeRef InferBinaryType(TokenKind op, TypeRef const &l, TypeRef const &r) {
        using TK = TokenKind;
        switch (op) {
        case TK::Plus:
            if (l.kind == TypeRef::Kind::Pointer && r.IsInteger()) {
                return l;
            }
            if (l.IsInteger() && r.kind == TypeRef::Kind::Pointer) {
                return r;
            }
            return l.IsUnknown() ? r : l;
        case TK::Minus:
            if (l.kind == TypeRef::Kind::Pointer && r.IsInteger()) {
                return l;
            }
            return l.IsUnknown() ? r : l;
        case TK::AmpAmp:
        case TK::PipePipe:
        case TK::Equal:
        case TK::BangEqual:
        case TK::Less:
        case TK::LessEqual:
        case TK::Greater:
        case TK::GreaterEqual:
            return TypeRef::MakeBool();
        default:
            return l.IsUnknown() ? r : l;
        }
    }

    HirPatternPtr LowerLetPattern(Pattern const &pat, TypeRef const &type, bool isMut) {
        if (dynamic_cast<WildcardPattern const *>(&pat)) {
            auto hp = std::make_unique<HirWildcardPattern>();
            hp->location = pat.location;
            return hp;
        }
        if (auto *p = dynamic_cast<IdentPattern const *>(&pat)) {
            auto hp = std::make_unique<HirBindingPattern>();
            hp->location = p->location;
            hp->name = p->name;
            hp->type = type;

            HirSymbol sym;
            sym.kind = HirSymbol::Kind::Var;
            sym.name = p->name;
            sym.type = type;
            sym.isMut = isMut;
            Define(sym);
            return hp;
        }
        if (auto *p = dynamic_cast<TuplePattern const *>(&pat)) {
            auto hp = std::make_unique<HirTuplePattern>();
            hp->location = p->location;
            for (std::size_t i = 0; i < p->elements.size(); ++i) {
                TypeRef elemType = TypeRef::MakeUnknown();
                if (type.kind == TypeRef::Kind::Tuple && i < type.inner.size()) {
                    elemType = type.inner[i];
                }
                hp->elements.push_back(LowerLetPattern(*p->elements[i], elemType, isMut));
            }
            return hp;
        }
        return LowerPattern(pat);
    }

    // Pattern lowering
    HirPatternPtr LowerPattern(Pattern const &pat) {
        if (dynamic_cast<WildcardPattern const *>(&pat)) {
            auto hp = std::make_unique<HirWildcardPattern>();
            hp->location = pat.location;
            return hp;
        }
        if (auto *p = dynamic_cast<LiteralPattern const *>(&pat)) {
            auto hp = std::make_unique<HirLiteralPattern>();
            hp->location = p->location;
            hp->type = LiteralType(p->value);
            if (p->value.kind == TokenKind::IntLiteral ||
                p->value.kind == TokenKind::FloatLiteral) {
                hp->value = StripNumericLiteralSuffix(p->value.text);
            }
            else {
                hp->value = p->value.text;
            }
            return hp;
        }
        if (auto *p = dynamic_cast<IdentPattern const *>(&pat)) {
            auto hp = std::make_unique<HirBindingPattern>();
            hp->location = p->location;
            hp->name = p->name;
            HirSymbol sym;
            sym.kind = HirSymbol::Kind::Var;
            sym.name = p->name;
            sym.type = TypeRef::MakeUnknown();
            Define(sym);
            return hp;
        }
        if (auto *p = dynamic_cast<RangePattern const *>(&pat)) {
            auto hp = std::make_unique<HirRangePattern>();
            hp->location = p->location;
            hp->inclusive = p->inclusive;
            hp->lo = LowerPattern(*p->lo);
            hp->hi = LowerPattern(*p->hi);
            return hp;
        }
        if (auto *p = dynamic_cast<EnumPattern const *>(&pat)) {
            auto hp = std::make_unique<HirEnumPattern>();
            hp->location = p->location;
            hp->path = p->path;
            EnumDecl::Variant const *variant = nullptr;
            if (!p->path.empty()) {
                if (HirSymbol *sym = currentScope->Lookup(p->path[0])) {
                    hp->resolvedType = sym->type;
                }
                if (p->path.size() >= 2) {
                    hp->discriminant = LookupEnumVariantDiscriminant(p->path[0], p->path[1]);
                    variant = LookupEnumVariant(p->path[0], p->path[1]);
                    if (variant) {
                        hp->hasPayload = !variant->fields.empty() || !variant->namedFields.empty();
                    }
                    if (auto const enumIt = enumDecls.find(p->path[0]); enumIt != enumDecls.end()) {
                        for (auto const &variant : enumIt->second->variants) {
                            if (variant.fields.empty() && variant.namedFields.empty()) {
                                if (auto disc =
                                        LookupEnumVariantDiscriminant(p->path[0], variant.name)) {
                                    hp->unitDiscriminants.push_back(*disc);
                                }
                            }
                        }
                    }
                }
            }
            std::unordered_map<std::string, Pattern const *> namedArgs;
            for (auto const &arg : p->namedArgs) {
                namedArgs.emplace(arg.name, arg.pattern.get());
            }
            if (variant) {
                for (auto const &field : variant->namedFields) {
                    if (auto const it = namedArgs.find(field.name); it != namedArgs.end()) {
                        hp->argIndices.push_back(&field - variant->namedFields.data());
                        hp->args.push_back(
                            LowerLetPattern(*it->second, ResolveType(*field.type), false));
                    }
                }
            }
            else {
                for (auto const &arg : p->namedArgs) {
                    hp->args.push_back(LowerPattern(*arg.pattern));
                }
            }
            for (std::size_t i = 0; i < p->args.size(); ++i) {
                if (variant && i < variant->fields.size()) {
                    hp->argIndices.push_back(i);
                    hp->args.push_back(
                        LowerLetPattern(*p->args[i], ResolveType(*variant->fields[i]), false));
                }
                else if (variant && i - variant->fields.size() < variant->namedFields.size()) {
                    hp->argIndices.push_back(i);
                    hp->args.push_back(LowerLetPattern(
                        *p->args[i],
                        ResolveType(*variant->namedFields[i - variant->fields.size()].type),
                        false));
                }
                else {
                    hp->args.push_back(LowerPattern(*p->args[i]));
                }
            }
            return hp;
        }
        if (auto *p = dynamic_cast<StructPattern const *>(&pat)) {
            auto hp = std::make_unique<HirStructPattern>();
            hp->location = p->location;
            hp->typeName = p->typeName;
            if (HirSymbol *sym = currentScope->Lookup(p->typeName)) {
                hp->resolvedType = sym->type;
            }
            for (auto const &f : p->fields) {
                HirStructPatternField hf;
                hf.name = f.name;
                hf.pattern = LowerPattern(*f.pattern);
                hp->fields.push_back(std::move(hf));
            }
            return hp;
        }
        if (auto *p = dynamic_cast<TuplePattern const *>(&pat)) {
            auto hp = std::make_unique<HirTuplePattern>();
            hp->location = p->location;
            for (auto const &e : p->elements) {
                hp->elements.push_back(LowerPattern(*e));
            }
            return hp;
        }
        if (auto *p = dynamic_cast<GuardedPattern const *>(&pat)) {
            auto hp = std::make_unique<HirGuardedPattern>();
            hp->location = p->location;
            hp->inner = LowerPattern(*p->inner);
            hp->guard = LowerExpr(*p->guard);
            return hp;
        }
        auto hp = std::make_unique<HirWildcardPattern>();
        hp->location = pat.location;
        return hp;
    }
};

// Hir public API
Hir::Hir(std::vector<Module const *> modules)
    : modules_(std::move(modules)) {
}

HirPackage Hir::Generate() {
    Lowering lowering(modules_);
    return lowering.Run();
}

// Dump
static std::string PrintPattern(HirPattern const &pat);
static std::string PrintExpr(HirExpr const &expr);

static std::string PrintExpr(HirExpr const &expr) {
    if (auto *e = dynamic_cast<HirLiteralExpr const *>(&expr)) {
        return e->value;
    }
    if (auto *e = dynamic_cast<HirVarExpr const *>(&expr)) {
        return e->name;
    }
    if (dynamic_cast<HirSelfExpr const *>(&expr)) {
        return "self";
    }
    if (auto *e = dynamic_cast<HirPathExpr const *>(&expr)) {
        std::string s;
        for (std::size_t i = 0; i < e->segments.size(); ++i) {
            if (i) {
                s += "::";
            }
            s += e->segments[i];
        }
        return s;
    }
    if (auto *e = dynamic_cast<HirUnaryExpr const *>(&expr)) {
        return std::string(OpStr(e->op)) + PrintExpr(*e->operand);
    }
    if (auto *e = dynamic_cast<HirPostfixExpr const *>(&expr)) {
        return PrintExpr(*e->operand) + (e->op == TokenKind::PlusPlus ? "++" : "--");
    }
    if (auto *e = dynamic_cast<HirBinaryExpr const *>(&expr)) {
        return std::format("({} {} {})", PrintExpr(*e->left), OpStr(e->op), PrintExpr(*e->right));
    }
    if (auto *e = dynamic_cast<HirAssignExpr const *>(&expr)) {
        return std::format("{} {} {}", PrintExpr(*e->target), OpStr(e->op), PrintExpr(*e->value));
    }
    if (auto *e = dynamic_cast<HirTernaryExpr const *>(&expr)) {
        return std::format("{} ? {} : {}", PrintExpr(*e->condition), PrintExpr(*e->thenExpr),
                           PrintExpr(*e->elseExpr));
    }
    if (auto *e = dynamic_cast<HirRangeExpr const *>(&expr)) {
        std::string lo = e->lo ? PrintExpr(*e->lo) : "";
        std::string hi = e->hi ? PrintExpr(*e->hi) : "";
        return lo + (e->inclusive ? "..." : "..") + hi;
    }
    if (auto *e = dynamic_cast<HirCallExpr const *>(&expr)) {
        std::string s = PrintExpr(*e->callee) + "(";
        for (std::size_t i = 0; i < e->args.size(); ++i) {
            if (i) {
                s += ", ";
            }
            s += PrintExpr(*e->args[i]);
        }
        return s + ")";
    }
    if (auto *e = dynamic_cast<HirIndexExpr const *>(&expr)) {
        return std::format("{}[{}]", PrintExpr(*e->object), PrintExpr(*e->index));
    }
    if (auto *e = dynamic_cast<HirFieldExpr const *>(&expr)) {
        return std::format("{}.{}", PrintExpr(*e->object), e->field);
    }
    if (auto *e = dynamic_cast<HirStructInitExpr const *>(&expr)) {
        std::string s = e->typeName + " { ";
        for (std::size_t i = 0; i < e->fields.size(); ++i) {
            if (i) {
                s += ", ";
            }
            s += e->fields[i].name + ": " + PrintExpr(*e->fields[i].value);
        }
        return s + " }";
    }
    if (auto *e = dynamic_cast<HirSliceExpr const *>(&expr)) {
        std::string s = "[";
        for (std::size_t i = 0; i < e->elements.size(); ++i) {
            if (i) {
                s += ", ";
            }
            s += PrintExpr(*e->elements[i]);
        }
        return s + "]";
    }
    if (auto *e = dynamic_cast<HirTupleExpr const *>(&expr)) {
        std::string s = "(";
        for (std::size_t i = 0; i < e->elements.size(); ++i) {
            if (i) {
                s += ", ";
            }
            s += PrintExpr(*e->elements[i]);
        }
        return s + ")";
    }
    if (auto *e = dynamic_cast<HirCastExpr const *>(&expr)) {
        return std::format("{} as {}", PrintExpr(*e->operand), e->targetType.ToString());
    }
    if (auto *e = dynamic_cast<HirIsExpr const *>(&expr)) {
        return std::format("{} is {}", PrintExpr(*e->operand), e->checkType.ToString());
    }
    if (auto *e = dynamic_cast<HirMatchExpr const *>(&expr)) {
        std::string s = "match " + PrintExpr(*e->subject) + " { ";
        for (std::size_t i = 0; i < e->arms.size(); ++i) {
            if (i) {
                s += ", ";
            }
            s += PrintPattern(*e->arms[i].pattern) + " => " + PrintExpr(*e->arms[i].body);
        }
        return s + " }";
    }
    if (auto *e = dynamic_cast<HirEnumConstructExpr const *>(&expr)) {
        std::string s = "#(";
        for (std::size_t i = 0; i < e->payloads.size(); ++i) {
            if (i) {
                s += ", ";
            }
            s += PrintExpr(*e->payloads[i]);
        }
        return s + ")#" + e->discriminant;
    }
    if (dynamic_cast<HirBlockExpr const *>(&expr)) {
        return "{ ... }";
    }
    return "<expr>";
}

static std::string PrintPattern(HirPattern const &pat) {
    if (dynamic_cast<HirWildcardPattern const *>(&pat)) {
        return "_";
    }
    if (auto *p = dynamic_cast<HirLiteralPattern const *>(&pat)) {
        return p->value;
    }
    if (auto *p = dynamic_cast<HirBindingPattern const *>(&pat)) {
        return p->name;
    }
    if (auto *p = dynamic_cast<HirRangePattern const *>(&pat)) {
        std::string lo = p->lo ? PrintPattern(*p->lo) : "";
        std::string hi = p->hi ? PrintPattern(*p->hi) : "";
        return lo + (p->inclusive ? "..." : "..") + hi;
    }
    if (auto *p = dynamic_cast<HirEnumPattern const *>(&pat)) {
        std::string s;
        for (std::size_t i = 0; i < p->path.size(); ++i) {
            if (i) {
                s += "::";
            }
            s += p->path[i];
        }
        if (!p->args.empty()) {
            s += "(";
            for (std::size_t i = 0; i < p->args.size(); ++i) {
                if (i) {
                    s += ", ";
                }
                s += PrintPattern(*p->args[i]);
            }
            s += ")";
        }
        return s;
    }

    if (auto *p = dynamic_cast<HirStructPattern const *>(&pat)) {
        std::string s = p->typeName + " { ";
        for (std::size_t i = 0; i < p->fields.size(); ++i) {
            if (i) {
                s += ", ";
            }
            s += p->fields[i].name + ": " + PrintPattern(*p->fields[i].pattern);
        }
        return s + " }";
    }
    if (auto *p = dynamic_cast<HirTuplePattern const *>(&pat)) {
        std::string s = "(";
        for (std::size_t i = 0; i < p->elements.size(); ++i) {
            if (i) {
                s += ", ";
            }
            s += PrintPattern(*p->elements[i]);
        }
        return s + ")";
    }
    if (auto *p = dynamic_cast<HirGuardedPattern const *>(&pat)) {
        return PrintPattern(*p->inner) + " if " + PrintExpr(*p->guard);
    }
    return "_";
}

static void DumpBlock(std::ostream &out, HirBlock const &block, std::string const &indent);

static void DumpStmt(std::ostream &out, HirStmt const &stmt, std::string const &indent);

static void DumpBlock(std::ostream &out, HirBlock const &block, std::string const &indent) {
    for (auto const &stmt : block.stmts) {
        DumpStmt(out, *stmt, indent);
    }
}

static void DumpStmt(std::ostream &out, HirStmt const &stmt, std::string const &indent) {
    if (auto *s = dynamic_cast<HirExprStmt const *>(&stmt)) {
        out << indent << PrintExpr(*s->expr) << '\n';
        return;
    }
    if (auto *s = dynamic_cast<HirLetStmt const *>(&stmt)) {
        out << std::format("{}{} {}: {}", indent, s->isMut ? "var" : "let",
                           s->pattern ? PrintPattern(*s->pattern) : s->name, s->type.ToString());
        if (s->stackBufferLength != 0) {
            out << std::format("[{}]", s->stackBufferLength);
        }
        if (s->init) {
            out << " = " << PrintExpr(*s->init);
        }
        out << '\n';
        return;
    }
    if (auto *s = dynamic_cast<HirIfStmt const *>(&stmt)) {
        out << std::format("{}if {}\n", indent, PrintExpr(*s->condition));
        DumpBlock(out, s->thenBlock, indent + "  ");
        for (auto const &elif : s->elseIfs) {
            out << std::format("{}else if {}\n", indent, PrintExpr(*elif.condition));
            DumpBlock(out, elif.block, indent + "  ");
        }
        if (s->elseBlock) {
            out << indent << "else\n";
            DumpBlock(out, *s->elseBlock, indent + "  ");
        }
        return;
    }
    if (auto *s = dynamic_cast<HirWhileStmt const *>(&stmt)) {
        out << std::format("{}while {}\n", indent, PrintExpr(*s->condition));
        DumpBlock(out, s->body, indent + "  ");
        return;
    }
    if (auto *s = dynamic_cast<HirDoWhileStmt const *>(&stmt)) {
        out << std::format("{}do\n", indent);
        DumpBlock(out, s->body, indent + "  ");
        out << std::format("{}while {}\n", indent, PrintExpr(*s->condition));
        return;
    }
    if (auto *s = dynamic_cast<HirLoopStmt const *>(&stmt)) {
        out << std::format("{}loop\n", indent);
        DumpBlock(out, s->body, indent + "  ");
        return;
    }
    if (auto *s = dynamic_cast<HirForStmt const *>(&stmt)) {
        out << std::format("{}for {} in {}\n", indent, s->variable, PrintExpr(*s->iterable));
        DumpBlock(out, s->body, indent + "  ");
        return;
    }
    if (auto *s = dynamic_cast<HirMatchStmt const *>(&stmt)) {
        out << std::format("{}match {}\n", indent, PrintExpr(*s->subject));
        for (auto const &arm : s->arms) {
            out << std::format("{}  {} =>\n", indent, PrintPattern(*arm.pattern));
            out << std::format("{}    {}\n", indent, PrintExpr(*arm.body));
        }
        return;
    }
    if (auto *s = dynamic_cast<HirReturnStmt const *>(&stmt)) {
        if (s->value) {
            out << std::format("{}return {}\n", indent, PrintExpr(**s->value));
        }
        else {
            out << indent << "return\n";
        }
        return;
    }
    if (auto *s = dynamic_cast<HirBreakStmt const *>(&stmt)) {
        out << indent << (s->label.empty() ? "break" : "break " + s->label) << "\n";
        return;
    }
    if (auto *s = dynamic_cast<HirContinueStmt const *>(&stmt)) {
        out << indent << (s->label.empty() ? "continue" : "continue " + s->label) << "\n";
        return;
    }
    if (auto *s = dynamic_cast<HirLocalDecl const *>(&stmt)) {
        out << std::format("{}[local {}]\n", indent, s->description);
        return;
    }
}

static void DumpFuncSignature(std::ostream &out, HirFunc const &f, std::string const &prefix = "") {
    std::string pub = f.isPublic ? "pub " : "";
    std::string asm_ = f.isAsm ? "asm " : "";
    std::string tps;
    if (!f.typeParams.empty()) {
        tps = "<";
        for (std::size_t i = 0; i < f.typeParams.size(); ++i) {
            if (i) {
                tps += ", ";
            }
            tps += f.typeParams[i];
        }
        tps += ">";
    }
    std::string params;
    for (std::size_t i = 0; i < f.params.size(); ++i) {
        if (i) {
            params += ", ";
        }
        if (f.params[i].isVariadic) {
            params += "...";
        }
        else {
            params += f.params[i].name + ": " + f.params[i].type.ToString();
        }
    }
    std::string ret = f.returnType.IsOpaque() ? "" : " -> " + f.returnType.ToString();
    out << std::format("{}{}{}func {}{}{}{}\n", prefix, pub, asm_, f.name, tps,
                       params.empty() ? "()" : "(" + params + ")", ret);
}

bool Hir::Dump(HirPackage const &package, std::filesystem::path const &path) {
    std::ofstream out(path);
    if (!out) {
        return false;
    }
    out << "=== High-level Intermediate Representation ===\n";
    for (auto const &mod : package.modules) {
        out << '\n';
        out << std::format("Module \"{}\"\n", mod.name);
        out << std::string(std::min<std::size_t>(mod.name.size() + 9, 72), '-') << '\n';
        for (auto const &c : mod.consts) {
            std::string pub = c.isPublic ? "pub " : "";
            out << std::format("\n{}const {}: {} = {}\n", pub, c.name, c.type.ToString(),
                               PrintExpr(*c.value));
        }
        for (auto const &ta : mod.typeAliases) {
            std::string pub = ta.isPublic ? "pub " : "";
            out << std::format("\n{}type {} = {}\n", pub, ta.name, ta.type.ToString());
        }
        for (auto const &ev : mod.externVars) {
            std::string pub = ev.isPublic ? "pub " : "";
            out << std::format("\nextern {}{}: {}\n", pub, ev.name, ev.type.ToString());
        }
        for (auto const &ef : mod.externFuncs) {
            std::string pub = ef.isPublic ? "pub " : "";
            std::string params;
            for (std::size_t i = 0; i < ef.params.size(); ++i) {
                if (i) {
                    params += ", ";
                }
                if (ef.params[i].isVariadic) {
                    params += "...";
                }
                else {
                    params += ef.params[i].name + ": " + ef.params[i].type.ToString();
                }
            }
            if (ef.isVariadic && !ef.params.empty()) {
                params += ", ...";
            }
            std::string ret = ef.returnType.IsOpaque() ? "" : " -> " + ef.returnType.ToString();
            std::string attr;
            if (!ef.dll.empty()) {
                attr += std::format("@[Import(lib: \"{}\")]\n", ef.dll);
            }
            if (ef.callConv == CallingConvention::Win64) {
                attr += "@[Call(.Win64)]\n";
            }
            out << std::format("\n{}extern {}func {}({}){}\n", attr, pub, ef.name, params, ret);
        }
        for (auto const &s : mod.structs) {
            std::string pub = s.isPublic ? "pub " : "";
            std::string typeParams;
            if (!s.typeParams.empty()) {
                typeParams = "<";
                for (std::size_t i = 0; i < s.typeParams.size(); ++i) {
                    if (i) {
                        typeParams += ", ";
                    }
                    typeParams += s.typeParams[i];
                }
                typeParams += ">";
            }
            out << std::format("\n{}struct {}{}\n", pub, s.name, typeParams);
            for (auto const &f : s.fields) {
                std::string fpub = f.isPublic ? "pub " : "";
                out << std::format("  {}{}: {}\n", fpub, f.name, f.type.ToString());
            }
        }
        for (auto const &e : mod.enums) {
            std::string pub = e.isPublic ? "pub " : "";
            out << std::format("\n{}enum {}: {}\n", pub, e.name, e.baseType.ToString());
            for (auto const &v : e.variants) {
                if (v.fields.empty()) {
                    out << std::format("  {} = {}\n", v.name, v.discriminant.value_or("0"));
                }
                else {
                    std::string fields;
                    for (std::size_t i = 0; i < v.fields.size(); ++i) {
                        if (i) {
                            fields += ", ";
                        }
                        fields += v.fields[i].ToString();
                    }
                    if (v.discriminant) {
                        out << std::format("  {}({}) = {}\n", v.name, fields, *v.discriminant);
                    }
                    else {
                        out << std::format("  {}({})\n", v.name, fields);
                    }
                }
            }
        }
        for (auto const &u : mod.unions) {
            std::string pub = u.isPublic ? "pub " : "";
            out << std::format("\n{}union {}\n", pub, u.name);
            for (auto const &f : u.fields) {
                out << std::format("  {}: {}\n", f.name, f.type.ToString());
            }
        }
        for (auto const &iface : mod.interfaces) {
            std::string pub = iface.isPublic ? "pub " : "";
            out << std::format("\n{}interface {}\n", pub, iface.name);
            for (auto const &m : iface.methods) {
                std::string params;
                for (std::size_t i = 0; i < m.params.size(); ++i) {
                    if (i) {
                        params += ", ";
                    }
                    if (m.params[i].isVariadic) {
                        params += "...";
                    }
                    else {
                        params += m.params[i].name + ": " + m.params[i].type.ToString();
                    }
                }
                std::string ret = m.returnType.IsOpaque() ? "" : " -> " + m.returnType.ToString();
                out << std::format("  func {}({}){}\n", m.name, params, ret);
            }
        }
        for (auto const &impl : mod.impls) {
            out << '\n';
            if (impl.interfaceName) {
                out << std::format("extend {} for {}\n", impl.typeName, *impl.interfaceName);
            }
            else {
                out << std::format("extend {}\n", impl.typeName);
            }
            for (auto const &m : impl.methods) {
                DumpFuncSignature(out, m, "  ");
                if (m.body) {
                    DumpBlock(out, *m.body, "    ");
                }
            }
        }
        for (auto const &f : mod.funcs) {
            out << '\n';
            DumpFuncSignature(out, f);
            if (f.body) {
                DumpBlock(out, *f.body, "  ");
            }
        }
    }
    return out.good();
}
} // namespace Rux
