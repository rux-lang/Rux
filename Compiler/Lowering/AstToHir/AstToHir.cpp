#include "Lowering/AstToHir/AstToHir.h"

#include "Driver/Version.h"
#include "Ir/Hir/HirInternal.h"
#include "Semantic/PrimitiveConstants.h"
#include "Target/Layout.h"
#include "Target/Platform.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace Rux {
using Layout::AlignUp;

static bool UtcTime(std::time_t time, std::tm &out) {
#if RUX_OS_WINDOWS
    return gmtime_s(&out, &time) == 0;
#else
    return gmtime_r(&time, &out) != nullptr;
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
    bool isNoReturn = false;
    std::vector<const FuncDecl *> funcOverloads;
};

class HirScope {
public:
    explicit HirScope(HirScope *parentScope = nullptr)
        : parent(parentScope) {
    }

    void Define(HirSymbol sym) {
        if (auto it = table.find(sym.name); it != table.end()) {
            if (it->second.kind == HirSymbol::Kind::Func && sym.kind == HirSymbol::Kind::Func) {
                it->second.funcOverloads.insert(it->second.funcOverloads.end(), sym.funcOverloads.begin(),
                                                sym.funcOverloads.end());
                if (it->second.type.IsUnknown() && !sym.type.IsUnknown()) {
                    it->second.type = std::move(sym.type);
                }
            }
            return;
        }
        table.emplace(sym.name, std::move(sym));
    }

    HirSymbol *Lookup(const std::string &name) {
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
    Lowering(std::vector<const Module *> &inputModules, const CompileTimeContext &inputContext)
        : modules(inputModules)
        , context(inputContext)
        , currentScope(&globalScope) {
    }

    HirPackage Run() {
        RegisterBuiltins();
        for (auto *mod : modules) {
            CollectModule(*mod);
        }
        BuildFunctionSymbolNames();
        HirPackage pkg;
        for (auto *mod : modules) {
            pkg.modules.push_back(LowerModule(*mod));
        }
        return pkg;
    }

private:
    std::vector<const Module *> &modules;
    const CompileTimeContext &context;
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
    std::unordered_map<std::string, TypeRef> currentSubstitutions;
    std::vector<HirFunc> monomorphizedFuncs;
    std::unordered_set<std::string> generatedMonomorphizedFuncNames;
    std::unordered_map<std::string, const StructDecl *> structDecls;
    std::unordered_map<std::string, const EnumDecl *> enumDecls;
    std::unordered_map<std::string, std::vector<const FuncDecl *>> functionsByName;
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<const FuncDecl *>>> methodsByType;
    std::unordered_map<std::string, const InterfaceDecl *> interfaceDecls;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> typeInterfaceVtables;
    std::vector<std::unordered_map<std::string, std::uint64_t>> constIntegerScopes{{}};

    // Free functions live in a single flat lookup table, so a module path is
    // kept alongside each one: it decides which candidates a call site can see
    // and keeps same-named functions from different modules apart in the object
    // file. Empty means the function was declared outside any `module`.
    std::unordered_map<const FuncDecl *, std::string> funcModulePath;
    std::unordered_map<const FuncDecl *, std::string> funcSymbolName;
    // Module paths named by each source file's `import` declarations.
    std::unordered_map<std::string, std::vector<std::string>> fileImports;
    // Declared module path of the enclosing `module`, during collection and
    // during lowering respectively. Distinct from currentModulePath, which is
    // derived from the file name for the `#source.module` intrinsic.
    std::string collectModulePath;
    std::string declModulePath;

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
        auto add = [&](const char *name, TypeRef t) {
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

        const auto addAssert = [&](const char *name) {
            HirSymbol sym;
            sym.kind = HirSymbol::Kind::Const;
            sym.name = name;
            sym.type = TypeRef::MakeFunc({TypeRef::MakeBool(), TypeRef::MakeNamed(SliceTypeName(TypeRef::MakeChar8()))},
                                         TypeRef::MakeOpaque());
            globalScope.Define(std::move(sym));
        };
        addAssert("Assert");
        addAssert("DebugAssert");

        HirSymbol panic;
        panic.kind = HirSymbol::Kind::Const;
        panic.name = "Panic";
        panic.type =
            TypeRef::MakeFunc({TypeRef::MakeNamed(SliceTypeName(TypeRef::MakeChar8()))}, TypeRef::MakeOpaque());
        globalScope.Define(std::move(panic));
    }

    // First pass: collect global names
    void CollectModule(const Module &mod) {
        currentFile = mod.name;
        collectModulePath.clear();
        for (const auto &decl : mod.items) {
            CollectDecl(*decl);
        }
    }

    // The module a bare `import` makes visible: `import Std::Io::Print` names
    // module Std::Io, `import Std::{ Assert }` and `import Std.Io.*` name the
    // module they are rooted at. Mirrors SemanticAnalyzer::LogicalModulePathForImport.
    static std::string ImportedModulePath(const UseDecl &d) {
        if (d.path.empty()) {
            return "";
        }
        // A Single import ends in the imported name, not in a module segment.
        const std::size_t end = d.kind == UseDecl::Kind::Single ? d.path.size() - 1 : d.path.size();
        if (end == 0) {
            return "";
        }
        std::string out = d.path[0];
        for (std::size_t i = 1; i < end; ++i) {
            out += "::" + d.path[i];
        }
        return out;
    }

    TypeRef MakeFuncType(const std::vector<Param> &params, const std::optional<TypeExprPtr> &returnType,
                         const std::vector<std::string> &typeParams = {}) {
        auto savedTypeParams = currentTypeParams;
        currentTypeParams = typeParams;

        std::vector<TypeRef> paramTypes;
        for (const auto &param : params) {
            if (!param.isVariadic) {
                paramTypes.push_back(ResolveType(*param.type));
            }
        }
        TypeRef ret = returnType ? ResolveType(*returnType->get()) : TypeRef::MakeOpaque();

        currentTypeParams = savedTypeParams;
        return TypeRef::MakeFunc(std::move(paramTypes), std::move(ret));
    }

    TypeRef MakeFuncTypeWithSubstitution(const std::vector<Param> &params, const std::optional<TypeExprPtr> &returnType,
                                         const std::unordered_map<std::string, TypeRef> &substitutions,
                                         const std::vector<std::string> &typeParams = {}) {
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

    void CollectDecl(const Decl &decl) {
        auto simple = [&](HirSymbol::Kind k, const std::string &name, TypeRef t = {}) {
            HirSymbol sym;
            sym.kind = k;
            sym.name = name;
            sym.type = std::move(t);
            globalScope.Define(std::move(sym));
        };
        if (auto *fn = dynamic_cast<const FuncDecl *>(&decl)) {
            functionsByName[fn->name].push_back(fn);
            funcModulePath[fn] = collectModulePath;
            HirSymbol sym;
            sym.kind = HirSymbol::Kind::Func;
            sym.name = fn->name;
            sym.type = MakeFuncType(fn->params, fn->returnType, fn->typeParams);
            sym.isNoReturn = fn->isNoReturn;
            sym.funcOverloads.push_back(fn);
            globalScope.Define(std::move(sym));
        }
        else if (auto *useDecl = dynamic_cast<const UseDecl *>(&decl)) {
            if (std::string path = ImportedModulePath(*useDecl); !path.empty()) {
                fileImports[currentFile].push_back(std::move(path));
            }
        }
        else if (auto *structDecl = dynamic_cast<const StructDecl *>(&decl)) {
            structDecls[structDecl->name] = structDecl;
            simple(HirSymbol::Kind::Type, structDecl->name, TypeRef::MakeNamed(structDecl->name));
        }
        else if (auto *enumDecl = dynamic_cast<const EnumDecl *>(&decl)) {
            enumDecls[enumDecl->name] = enumDecl;
            simple(HirSymbol::Kind::Type, enumDecl->name, EnumType(*enumDecl));
        }
        else if (auto *unionDecl = dynamic_cast<const UnionDecl *>(&decl)) {
            simple(HirSymbol::Kind::Type, unionDecl->name, TypeRef::MakeNamed(unionDecl->name));
        }
        else if (auto *ifaceDecl = dynamic_cast<const InterfaceDecl *>(&decl)) {
            simple(HirSymbol::Kind::Interface, ifaceDecl->name, TypeRef::MakeNamed(ifaceDecl->name));
            interfaceDecls[ifaceDecl->name] = ifaceDecl;
        }
        else if (auto *constDecl = dynamic_cast<const ConstDecl *>(&decl)) {
            TypeRef constType;
            if (constDecl->type) {
                constType = ResolveType(*constDecl->type->get());
            }
            simple(HirSymbol::Kind::Const, constDecl->name, constType);
        }
        else if (auto *aliasDecl = dynamic_cast<const TypeAliasDecl *>(&decl)) {
            simple(HirSymbol::Kind::Type, aliasDecl->name, ResolveType(*aliasDecl->type));
        }
        else if (auto *externFn = dynamic_cast<const ExternFuncDecl *>(&decl)) {
            HirSymbol sym;
            sym.kind = HirSymbol::Kind::Func;
            sym.name = externFn->name;
            sym.type = MakeFuncType(externFn->params, externFn->returnType);
            sym.isNoReturn = externFn->isNoReturn;
            globalScope.Define(std::move(sym));
        }
        else if (auto *externVar = dynamic_cast<const ExternVarDecl *>(&decl)) {
            HirSymbol sym;
            sym.kind = HirSymbol::Kind::Var;
            sym.name = externVar->name;
            sym.isMut = true;
            globalScope.Define(std::move(sym));
        }
        else if (auto *externBlock = dynamic_cast<const ExternBlockDecl *>(&decl)) {
            for (auto &item : externBlock->items) {
                CollectDecl(*item);
            }
        }
        else if (auto *modDecl = dynamic_cast<const ModuleDecl *>(&decl)) {
            const auto saved = collectModulePath;
            collectModulePath = collectModulePath.empty() ? modDecl->name : collectModulePath + "::" + modDecl->name;
            for (auto &item : modDecl->items) {
                CollectDecl(*item);
            }
            collectModulePath = saved;
        }
        else if (auto *implDecl = dynamic_cast<const ImplDecl *>(&decl)) {
            for (const auto &method : implDecl->methods) {
                methodsByType[implDecl->typeName][method->name].push_back(method.get());
            }
            if (implDecl->interfaceName) {
                typeInterfaceVtables[implDecl->typeName][*implDecl->interfaceName] =
                    "__vtable__" + implDecl->typeName + "__" + *implDecl->interfaceName;
            }
        }
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

    static std::string BaseTypeName(const std::string &name) {
        const std::size_t pos = name.find('<');
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

    static std::vector<TypeRef> ParseTypeArgsFromTypeName(const std::string &typeName) {
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

    static std::string StripNumericLiteralSuffix(const std::string &text) {
        const std::string suffix = NumericLiteralSuffix(text);
        if (suffix.empty()) {
            return text;
        }
        return text.substr(0, text.size() - suffix.size());
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
        std::string cleaned = StripNumericLiteralSuffix(text);
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

    static std::optional<TypeRef> SliceElementType(const TypeRef &type) {
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
        std::string elemName = type.name.substr(prefix.size(), type.name.size() - prefix.size() - 1);
        if (auto builtin = BuiltinTypeFromName(elemName)) {
            return *builtin;
        }
        return TypeRef::MakeNamed(elemName);
    }

    static std::optional<TypeRef> IndexElementType(const TypeRef &type) {
        if (auto elemType = SliceElementType(type)) {
            return elemType;
        }
        if (type.kind == TypeRef::Kind::Pointer && !type.inner.empty()) {
            return type.inner[0];
        }
        return std::nullopt;
    }

    TypeRef ResolveType(const TypeExpr &expr) {
        if (auto *t = dynamic_cast<const NamedTypeExpr *>(&expr)) {
            if (t->typeArgs.empty()) {
                if (auto it = currentSubstitutions.find(t->name); it != currentSubstitutions.end()) {
                    return it->second;
                }
                for (const auto &tp : currentTypeParams) {
                    if (tp == t->name) {
                        return TypeRef::MakeTypeParam(t->name);
                    }
                }
            }
            HirSymbol *sym = currentScope->Lookup(t->name);
            if (sym && (sym->kind == HirSymbol::Kind::Type || sym->kind == HirSymbol::Kind::Interface)) {
                if (t->typeArgs.empty() && !sym->type.IsUnknown()) {
                    return sym->type;
                }
                if (t->typeArgs.empty()) {
                    if (const auto enumIt = enumDecls.find(t->name); enumIt != enumDecls.end()) {
                        return EnumType(*enumIt->second);
                    }
                }
                return TypeRef::MakeNamed(GenericTypeName(*t));
            }
            return TypeRef::MakeNamed(GenericTypeName(*t)); // best-effort for unresolved names
        }
        if (auto *t = dynamic_cast<const PathTypeExpr *>(&expr)) {
            return TypeRef::MakeNamed(t->segments.back());
        }
        if (auto *t = dynamic_cast<const PointerTypeExpr *>(&expr)) {
            return TypeRef::MakePointer(ResolveType(*t->pointee));
        }
        if (auto *t = dynamic_cast<const SliceTypeExpr *>(&expr)) {
            return TypeRef::MakeNamed(SliceTypeName(ResolveType(*t->element)));
        }
        if (auto *t = dynamic_cast<const TupleTypeExpr *>(&expr)) {
            std::vector<TypeRef> elems;
            for (auto &e : t->elements) {
                elems.push_back(ResolveType(*e));
            }
            return TypeRef::MakeTuple(std::move(elems));
        }
        if (dynamic_cast<const SelfTypeExpr *>(&expr)) {
            return currentSelfType.IsUnknown() ? TypeRef::MakeNamed("self") : currentSelfType;
        }
        if (auto *t = dynamic_cast<const FunctionTypeExpr *>(&expr)) {
            std::vector<TypeRef> paramTypes;
            paramTypes.reserve(t->params.size());
            for (const auto &p : t->params) {
                paramTypes.push_back(ResolveType(*p));
            }
            TypeRef ret = t->returnType ? ResolveType(*t->returnType->get()) : TypeRef::MakeOpaque();
            TypeRef funcType = TypeRef::MakeFunc(std::move(paramTypes), std::move(ret));
            funcType.isVariadic = t->isVariadic;
            return funcType;
        }
        return TypeRef::MakeUnknown();
    }

    std::optional<std::uint64_t> FixedSliceTypeSize(const TypeExpr &expr) {
        const auto *slice = dynamic_cast<const SliceTypeExpr *>(&expr);
        if (!slice || !slice->size) {
            return std::nullopt;
        }
        if (const auto *literal = dynamic_cast<const LiteralExpr *>(slice->size.get())) {
            return ParseUnsuffixedIntegerLiteral(literal->token);
        }
        if (const auto *ident = dynamic_cast<const IdentExpr *>(slice->size.get())) {
            return LookupConstInteger(ident->name);
        }
        return std::nullopt;
    }

    TypeRef FixedSliceElementType(const TypeExpr &expr) {
        const auto *slice = dynamic_cast<const SliceTypeExpr *>(&expr);
        if (!slice) {
            return TypeRef::MakeUnknown();
        }
        return ResolveType(*slice->element);
    }

    TypeRef ResolveTypeWithSubstitution(const TypeExpr &expr,
                                        const std::unordered_map<std::string, TypeRef> &substitutions) {
        if (auto *t = dynamic_cast<const NamedTypeExpr *>(&expr)) {
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
                named.name += ResolveTypeWithSubstitution(*t->typeArgs[i], substitutions).ToString();
            }
            named.name += ">";
            return named;
        }
        if (auto *t = dynamic_cast<const PointerTypeExpr *>(&expr)) {
            return TypeRef::MakePointer(ResolveTypeWithSubstitution(*t->pointee, substitutions));
        }
        if (auto *t = dynamic_cast<const SliceTypeExpr *>(&expr)) {
            return TypeRef::MakeNamed(SliceTypeName(ResolveTypeWithSubstitution(*t->element, substitutions)));
        }
        if (auto *t = dynamic_cast<const TupleTypeExpr *>(&expr)) {
            std::vector<TypeRef> elems;
            for (auto &elem : t->elements) {
                elems.push_back(ResolveTypeWithSubstitution(*elem, substitutions));
            }
            return TypeRef::MakeTuple(std::move(elems));
        }
        return ResolveType(expr);
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

    TypeRef MethodType(const TypeRef &receiverType, const FuncDecl &method) {
        std::vector<TypeRef> params;
        params.push_back(receiverType);
        for (const auto &param : method.params) {
            if (param.isVariadic || param.name == "self") {
                continue;
            }
            params.push_back(ResolveType(*param.type));
        }
        TypeRef ret = method.returnType ? ResolveType(*method.returnType->get()) : TypeRef::MakeOpaque();
        return TypeRef::MakeFunc(std::move(params), std::move(ret));
    }

    TypeRef AssociatedFunctionType(const TypeRef &receiverType, const FuncDecl &method) {
        TypeRef savedSelfType = currentSelfType;
        currentSelfType =
            receiverType.kind == TypeRef::Kind::Pointer ? receiverType : TypeRef::MakePointer(receiverType);
        std::vector<TypeRef> params;
        for (const auto &param : method.params) {
            if (param.isVariadic) {
                continue;
            }
            params.push_back(ResolveType(*param.type));
        }
        TypeRef ret = method.returnType ? ResolveType(*method.returnType->get()) : TypeRef::MakeOpaque();
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

    bool FunctionIsOverloaded(const std::string &name) const {
        const auto it = functionsByName.find(name);
        return it != functionsByName.end() && it->second.size() > 1;
    }

    const std::string &ModulePathOf(const FuncDecl &decl) const {
        static const std::string empty;
        const auto it = funcModulePath.find(&decl);
        return it == funcModulePath.end() ? empty : it->second;
    }

    // Overloading is a property of a single module: two modules that happen to
    // declare the same function name are not overloads of each other, and
    // mangling them as if they were is what makes their symbols collide.
    bool FunctionIsOverloadedInModule(const std::string &name, const FuncDecl &decl) const {
        const auto it = functionsByName.find(name);
        if (it == functionsByName.end()) {
            return false;
        }
        const std::string &modulePath = ModulePathOf(decl);
        std::size_t count = 0;
        for (const auto *candidate : it->second) {
            if (ModulePathOf(*candidate) == modulePath && ++count > 1) {
                return true;
            }
        }
        return false;
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

    std::string MangleWithParams(const std::string &name, const FuncDecl &decl) {
        std::string out = name + "__";
        bool first = true;
        for (const auto &param : decl.params) {
            TypeRef paramType = param.isVariadic ? TypeRef::MakeNamed(SliceTypeName(ResolveType(*param.type)))
                                                 : ResolveType(*param.type);
            if (!first) {
                out += "_";
            }
            out += MangleTypeName(paramType);
            first = false;
        }
        return out;
    }

    // Name within the declaring module: bare unless the module overloads it, in
    // which case the parameter types disambiguate.
    std::string ModuleLocalSymbolName(const std::string &name, const FuncDecl &decl) {
        return FunctionIsOverloadedInModule(name, decl) ? MangleWithParams(name, decl) : name;
    }

    // Assigns every collected free function the name it is emitted under, once
    // all modules have been collected. A module-local name is enough until two
    // modules claim the same one; those get their module path prefixed, so a
    // call can never bind to a same-named function from an unrelated module.
    // Generic templates are excluded: the monomorphizer names each instance
    // after its type arguments.
    void BuildFunctionSymbolNames() {
        std::unordered_map<std::string, std::unordered_set<std::string>> owners;
        for (const auto &[name, decls] : functionsByName) {
            for (const auto *decl : decls) {
                if (decl->typeParams.empty()) {
                    owners[ModuleLocalSymbolName(name, *decl)].insert(ModulePathOf(*decl));
                }
            }
        }
        for (const auto &[name, decls] : functionsByName) {
            for (const auto *decl : decls) {
                if (!decl->typeParams.empty()) {
                    continue;
                }
                std::string local = ModuleLocalSymbolName(name, *decl);
                const std::string &modulePath = ModulePathOf(*decl);
                const bool contested = owners[local].size() > 1 && !modulePath.empty();
                funcSymbolName[decl] = contested ? modulePath + "::" + local : std::move(local);
            }
        }
    }

    std::string FunctionCalleeName(const std::string &name, const FuncDecl &decl) {
        if (const auto it = funcSymbolName.find(&decl); it != funcSymbolName.end()) {
            return it->second;
        }
        // A generic template, which BuildFunctionSymbolNames leaves alone: its
        // instances are named after their type arguments, and the uninstantiated
        // template keeps the original overload mangling.
        return FunctionIsOverloaded(name) ? MangleWithParams(name, decl) : name;
    }

    // Whether a call in the file being lowered may bind to this function: it is
    // declared outside any module, in the same module as the call, or in a
    // module the file imports. Everything else is only reachable through the
    // fallback in LookupFunction.
    bool FunctionIsVisibleHere(const FuncDecl &decl) const {
        const std::string &modulePath = ModulePathOf(decl);
        if (modulePath.empty() || modulePath == declModulePath) {
            return true;
        }
        const auto it = fileImports.find(currentFile);
        if (it == fileImports.end()) {
            return false;
        }
        return std::ranges::find(it->second, modulePath) != it->second.end();
    }

    const FuncDecl *LookupFunction(const std::string &name, const std::vector<TypeRef> &argTypes,
                                   const std::vector<TypeExprPtr> &typeArgs = {}) {
        const auto it = functionsByName.find(name);
        if (it == functionsByName.end() || it->second.empty()) {
            return nullptr;
        }
        // An imported or same-module function always wins over one that merely
        // shares its name from somewhere else in the program. Only if none of
        // them fits do we consider the rest, which keeps existing code that
        // relies on the flat lookup working.
        std::vector<const FuncDecl *> visible;
        for (const auto *decl : it->second) {
            if (FunctionIsVisibleHere(*decl)) {
                visible.push_back(decl);
            }
        }
        if (!visible.empty() && visible.size() < it->second.size()) {
            if (const FuncDecl *decl = ResolveOverload(visible, argTypes, typeArgs)) {
                return decl;
            }
        }
        return ResolveOverload(it->second, argTypes, typeArgs);
    }

    const FuncDecl *ResolveOverload(const std::vector<const FuncDecl *> &candidates,
                                    const std::vector<TypeRef> &argTypes, const std::vector<TypeExprPtr> &typeArgs) {
        if (candidates.size() == 1) {
            // Single-candidate validation. We must still verify arity and
            // assignability to prevent bogus calls from silently bypassing
            // the type-checker.
            const auto *decl = candidates[0];
            std::unordered_map<std::string, TypeRef> substitutions;
            const std::size_t count = std::min(decl->typeParams.size(), typeArgs.size());
            for (std::size_t i = 0; i < count; ++i) {
                substitutions.emplace(decl->typeParams[i], ResolveType(*typeArgs[i]));
            }
            TypeRef ft = MakeFuncTypeWithSubstitution(decl->params, decl->returnType, substitutions, decl->typeParams);
            if (ft.kind != TypeRef::Kind::Func || ft.inner.empty()) {
                return decl;
            }
            const std::size_t paramCount = ft.inner.size() - 1;
            const bool isVariadic = !decl->params.empty() && decl->params.back().isVariadic;
            std::size_t requiredCount = 0;
            for (const auto &p : decl->params) {
                if (!p.isVariadic && !p.defaultValue) {
                    ++requiredCount;
                }
            }
            const bool arityOk = isVariadic ? argTypes.size() >= requiredCount
                                            : (argTypes.size() >= requiredCount && argTypes.size() <= paramCount);
            if (!arityOk) {
                return nullptr;
            }
            for (std::size_t i = 0; i < std::min(argTypes.size(), paramCount); ++i) {
                if (argTypes[i].IsUnknown() || ft.inner[i].IsUnknown()) {
                    continue;
                }
                if (!argTypes[i].IsAssignableTo(ft.inner[i]) && !(argTypes[i].IsInteger() && ft.inner[i].IsInteger())) {
                    return nullptr;
                }
            }
            return decl;
        }
        for (const bool allowVariadic : {false, true}) {
            for (const bool exactOnly : {true, false}) {
                for (const auto *decl : candidates) {
                    std::unordered_map<std::string, TypeRef> substitutions;
                    const std::size_t count = std::min(decl->typeParams.size(), typeArgs.size());
                    for (std::size_t i = 0; i < count; ++i) {
                        substitutions.emplace(decl->typeParams[i], ResolveType(*typeArgs[i]));
                    }
                    TypeRef ft =
                        MakeFuncTypeWithSubstitution(decl->params, decl->returnType, substitutions, decl->typeParams);
                    if (ft.kind != TypeRef::Kind::Func || ft.inner.empty()) {
                        continue;
                    }
                    const std::size_t paramCount = ft.inner.size() - 1;
                    const bool isVariadic = !decl->params.empty() && decl->params.back().isVariadic;
                    if (isVariadic != allowVariadic) {
                        continue;
                    }
                    std::size_t requiredCount = 0;
                    for (const auto &p : decl->params) {
                        if (!p.isVariadic && !p.defaultValue) {
                            ++requiredCount;
                        }
                    }
                    const bool arityOk = isVariadic
                                           ? argTypes.size() >= requiredCount
                                           : (argTypes.size() >= requiredCount && argTypes.size() <= paramCount);
                    if (!arityOk) {
                        continue;
                    }
                    bool match = true;
                    for (std::size_t i = 0; i < std::min(argTypes.size(), paramCount); ++i) {
                        const TypeRef &paramType = ft.inner[i];
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

    const EnumDecl::Variant *LookupEnumVariant(const std::string &enumName, const std::string &variantName) const {
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

    TypeRef EnumVariantConstructorType(const EnumDecl &decl, const EnumDecl::Variant &variant) {
        std::vector<TypeRef> params;
        params.reserve(variant.fields.size() + variant.namedFields.size());
        for (const auto &field : variant.fields) {
            params.push_back(ResolveType(*field));
        }
        for (const auto &field : variant.namedFields) {
            params.push_back(ResolveType(*field.type));
        }
        return TypeRef::MakeFunc(std::move(params), EnumType(decl));
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

    int InterfaceMethodIndex(const std::string &ifaceName, const std::string &methodName) const {
        auto it = interfaceDecls.find(ifaceName);
        if (it == interfaceDecls.end()) {
            return -1;
        }
        const auto &methods = it->second->methods;
        for (int i = 0; i < static_cast<int>(methods.size()); ++i) {
            if (methods[i]->name == methodName) {
                return i;
            }
        }
        return -1;
    }

    TypeRef InterfaceMethodReturnType(const std::string &ifaceName, const std::string &methodName) {
        auto it = interfaceDecls.find(ifaceName);
        if (it == interfaceDecls.end()) {
            return TypeRef::MakeUnknown();
        }
        for (const auto &m : it->second->methods) {
            if (m->name == methodName) {
                return m->returnType ? ResolveType(**m->returnType) : TypeRef::MakeOpaque();
            }
        }
        return TypeRef::MakeUnknown();
    }

    std::vector<TypeRef> InterfaceMethodParamTypes(const std::string &ifaceName, const std::string &methodName) {
        std::vector<TypeRef> params;
        auto it = interfaceDecls.find(ifaceName);
        if (it == interfaceDecls.end()) {
            return params;
        }
        for (const auto &m : it->second->methods) {
            if (m->name != methodName) {
                continue;
            }
            for (const auto &param : m->params) {
                if (param.isVariadic) {
                    continue;
                }
                params.push_back(ResolveType(*param.type));
            }
            return params;
        }
        return params;
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

    std::optional<std::uint64_t> SizeOfTypeRef(const TypeRef &type,
                                               const std::unordered_map<std::string, TypeRef> &substitutions = {}) {
        if (type.kind == TypeRef::Kind::Named) {
            if (type.name.starts_with("Slice<")) {
                return 16;
            }
            if (auto it = substitutions.find(type.name); it != substitutions.end()) {
                return SizeOfTypeRef(it->second, substitutions);
            }
            const std::string baseName = BaseTypeName(type.name);
            std::unordered_map<std::string, TypeRef> localSubs = substitutions;
            const auto structIt = structDecls.find(baseName);
            if (structIt != structDecls.end()) {
                std::vector<TypeRef> typeArgs = ParseTypeArgsFromTypeName(type.name);
                const auto &params = structIt->second->typeParams;
                const std::size_t count = std::min(params.size(), typeArgs.size());
                for (std::size_t i = 0; i < count; ++i) {
                    localSubs[params[i]] = typeArgs[i];
                }
            }

            if (const auto enumIt = enumDecls.find(baseName); enumIt != enumDecls.end()) {
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
            const auto elemSize = SizeOfTypeRef(type.inner[0], substitutions);
            if (!elemSize || *elemSize == 0) {
                return std::nullopt;
            }
            return AlignUp(2 * *elemSize + 1, *elemSize);
        }

        if (type.kind == TypeRef::Kind::Tuple) {
            const auto layout = Layout::FieldsSizeAndAlign(
                type.inner, [&](const TypeRef &elem) { return SizeOfTypeRef(elem, substitutions); });
            if (!layout) {
                return std::nullopt;
            }
            return layout->first;
        }

        return type.SizeInBytes();
    }

    std::optional<std::uint64_t> SizeOfEnum(const EnumDecl &decl,
                                            const std::unordered_map<std::string, TypeRef> &substitutions = {}) {
        const auto tagSize = SizeOfTypeRef(EnumBaseType(decl), substitutions);
        if (!tagSize) {
            return std::nullopt;
        }

        bool hasPayload = false;
        std::uint64_t maxPayloadSize = 0;
        std::uint64_t maxPayloadAlign = 1;

        auto fieldLayout = [&](const auto &fields) {
            return Layout::FieldsSizeAndAlign(
                fields, [&](const auto &field) { return SizeOfTypeExprWithSubstitution(*field, substitutions); });
        };

        auto namedFieldLayout = [&](const auto &fields) {
            return Layout::FieldsSizeAndAlign(
                fields, [&](const auto &field) { return SizeOfTypeExprWithSubstitution(*field.type, substitutions); });
        };

        for (const auto &variant : decl.variants) {
            if (variant.fields.empty() && variant.namedFields.empty()) {
                continue;
            }

            hasPayload = true;
            auto payload =
                !variant.fields.empty() ? fieldLayout(variant.fields) : namedFieldLayout(variant.namedFields);
            if (!payload) {
                return std::nullopt;
            }
            maxPayloadSize = std::max(maxPayloadSize, payload->first);
            maxPayloadAlign = std::max(maxPayloadAlign, payload->second);
        }

        if (!hasPayload) {
            return tagSize;
        }

        const std::uint64_t tagAlign = *tagSize > 0 ? std::min<std::uint64_t>(*tagSize, 8) : 1;
        const std::uint64_t align = std::max(tagAlign, maxPayloadAlign);
        std::uint64_t offset = *tagSize;
        if (maxPayloadAlign > 1) {
            offset = AlignUp(offset, maxPayloadAlign);
        }
        offset += maxPayloadSize;
        return AlignUp(offset, align);
    }

    TypeRef EnumBaseType(const EnumDecl &decl) {
        return decl.baseType ? ResolveType(*decl.baseType) : TypeRef::MakeInt();
    }

    TypeRef EnumType(const EnumDecl &decl) {
        TypeRef type = TypeRef::MakeNamed(decl.name);
        type.inner.push_back(EnumBaseType(decl));
        return type;
    }

    std::optional<std::uint64_t> SizeOfStruct(const std::string &name,
                                              const std::unordered_map<std::string, TypeRef> &substitutions = {}) {
        const auto structIt = structDecls.find(name);
        if (structIt == structDecls.end()) {
            return std::nullopt;
        }

        const auto layout = Layout::FieldsSizeAndAlign(structIt->second->fields, [&](const auto &field) {
            return SizeOfTypeExprWithSubstitution(*field.type, substitutions);
        });
        if (!layout) {
            return std::nullopt;
        }
        return layout->first;
    }

    std::optional<std::uint64_t>
    SizeOfTypeExprWithSubstitution(const TypeExpr &expr,
                                   const std::unordered_map<std::string, TypeRef> &substitutions = {}) {
        if (auto *t = dynamic_cast<const NamedTypeExpr *>(&expr)) {
            const auto structIt = structDecls.find(t->name);
            if (structIt != structDecls.end()) {
                std::unordered_map<std::string, TypeRef> fieldSubstitutions = substitutions;
                const auto &params = structIt->second->typeParams;
                for (std::size_t i = 0; i < params.size() && i < t->typeArgs.size(); ++i) {
                    fieldSubstitutions[params[i]] = ResolveTypeWithSubstitution(*t->typeArgs[i], substitutions);
                }
                return SizeOfStruct(t->name, fieldSubstitutions);
            }
        }

        return SizeOfTypeRef(ResolveTypeWithSubstitution(expr, substitutions), substitutions);
    }

    std::optional<std::uint64_t> SizeOfTypeExpr(const TypeExpr &expr) {
        return SizeOfTypeExprWithSubstitution(expr);
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

    static TypeRef LiteralType(const Token &tok) {
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

    // Derives the Rux module path (e.g. "Std::Io") from a source file path.
    // Finds the "Src" directory component and uses the relative path below
    // it.
    static std::string FilePathToModulePath(const std::string &filePath) {
        const std::string generic = std::filesystem::path(filePath).generic_string();
        std::vector<std::string> parts;
        std::string cur;
        for (const char c : generic) {
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
                    const auto dot = s.rfind('.');
                    if (dot != std::string::npos) {
                        s = s.substr(0, dot);
                    }
                }
                mod.push_back(s);
            }
        }
        else {
            std::string stem = parts.empty() ? filePath : parts.back();
            const auto dot = stem.rfind('.');
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
    HirModule LowerModule(const Module &mod) {
        currentFile = mod.name;
        currentModulePath = FilePathToModulePath(mod.name);
        declModulePath.clear();
        HirModule hmod;
        hmod.name = mod.name;
        for (const auto &decl : mod.items) {
            LowerTopLevelDecl(*decl, hmod);
        }
        std::size_t processed = 0;
        while (processed < monomorphizedFuncs.size()) {
            hmod.funcs.push_back(std::move(monomorphizedFuncs[processed]));
            ++processed;
        }
        monomorphizedFuncs.clear();
        generatedMonomorphizedFuncNames.clear();
        return hmod;
    }

    void LowerTopLevelDecl(const Decl &decl, HirModule &hmod) {
        if (auto *fn = dynamic_cast<const FuncDecl *>(&decl)) {
            HirFunc hf = LowerFunc(*fn);
            hf.name = FunctionCalleeName(fn->name, *fn);
            hmod.funcs.push_back(std::move(hf));
        }
        else if (auto *structDecl = dynamic_cast<const StructDecl *>(&decl)) {
            hmod.structs.push_back(LowerStruct(*structDecl));
        }
        else if (auto *enumDecl = dynamic_cast<const EnumDecl *>(&decl)) {
            hmod.enums.push_back(LowerEnum(*enumDecl));
        }
        else if (auto *unionDecl = dynamic_cast<const UnionDecl *>(&decl)) {
            hmod.unions.push_back(LowerUnion(*unionDecl));
        }
        else if (auto *ifaceDecl = dynamic_cast<const InterfaceDecl *>(&decl)) {
            hmod.interfaces.push_back(LowerInterface(*ifaceDecl));
        }
        else if (auto *implDecl = dynamic_cast<const ImplDecl *>(&decl)) {
            hmod.impls.push_back(LowerImpl(*implDecl));
        }
        else if (auto *constDecl = dynamic_cast<const ConstDecl *>(&decl)) {
            hmod.consts.push_back(LowerConst(*constDecl));
        }
        else if (auto *externFn = dynamic_cast<const ExternFuncDecl *>(&decl)) {
            hmod.externFuncs.push_back(LowerExternFunc(*externFn));
        }
        else if (auto *externVar = dynamic_cast<const ExternVarDecl *>(&decl)) {
            hmod.externVars.push_back(LowerExternVar(*externVar));
        }
        else if (auto *externBlock = dynamic_cast<const ExternBlockDecl *>(&decl)) {
            for (auto &item : externBlock->items) {
                LowerTopLevelDecl(*item, hmod);
            }
        }
        else if (auto *aliasDecl = dynamic_cast<const TypeAliasDecl *>(&decl)) {
            hmod.typeAliases.push_back(LowerTypeAlias(*aliasDecl));
        }
        else if (auto *modDecl = dynamic_cast<const ModuleDecl *>(&decl)) {
            const auto savedModulePath = currentModulePath;
            const auto savedDeclModulePath = declModulePath;
            currentModulePath = currentModulePath.empty() ? modDecl->name : currentModulePath + "::" + modDecl->name;
            declModulePath = declModulePath.empty() ? modDecl->name : declModulePath + "::" + modDecl->name;
            for (auto &item : modDecl->items) {
                LowerTopLevelDecl(*item, hmod);
            }
            currentModulePath = savedModulePath;
            declModulePath = savedDeclModulePath;
        }
        // Import declarations are resolved by sema and have no HIR
        // representation.
    }

    // Declaration lowering

    HirFunc LowerFunc(const FuncDecl &d, bool isMethod = false,
                      const std::unordered_map<std::string, TypeRef> &substitutions = {},
                      const std::string &overrideName = "") {
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

    HirStruct LowerStruct(const StructDecl &d) {
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

    HirEnum LowerEnum(const EnumDecl &d) {
        HirEnum he;
        he.name = d.name;
        he.isPublic = d.isPublic;
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
        return he;
    }

    HirUnion LowerUnion(const UnionDecl &d) {
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

    HirInterface LowerInterface(const InterfaceDecl &d) {
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

    HirImplBlock LowerImpl(const ImplDecl &d) {
        bool savedInImpl = inImpl;
        TypeRef savedSelfType = currentSelfType;
        inImpl = true;
        TypeRef extendedType = d.extendedType ? ResolveType(*d.extendedType) : TypeRef::MakeUnknown();
        const bool isSliceReceiver =
            extendedType.kind == TypeRef::Kind::Slice ||
            (extendedType.kind == TypeRef::Kind::Named && extendedType.name.starts_with("Slice<"));
        if (isSliceReceiver) {
            // `self` is the slice value; the slice ABI passes its address, so
            // slice indexing and iteration inside the method work as usual.
            currentSelfType = extendedType;
        }
        else {
            TypeRef selfBase;
            if (HirSymbol *sym = currentScope->Lookup(d.typeName); sym && !sym->type.IsUnknown()) {
                selfBase = sym->type;
            }
            else {
                selfBase = TypeRef::MakeNamed(d.typeName);
            }
            currentSelfType = TypeRef::MakePointer(selfBase);
        }

        HirImplBlock hib;
        hib.typeName = d.typeName;
        hib.interfaceName = d.interfaceName;
        hib.location = d.location;
        for (const auto &m : d.methods) {
            HirFunc hf = LowerFunc(*m, /*isMethod=*/true);
            if (MethodIsOverloaded(d.typeName, m->name)) {
                TypeRef selfType = TypeRef::MakePointer(TypeRef::MakeNamed(d.typeName));
                hf.name = CalleeName(d.typeName, m->name, selfType, *m).substr(d.typeName.size() + 2);
            }
            hib.methods.push_back(std::move(hf));
        }

        currentSelfType = savedSelfType;
        inImpl = savedInImpl;
        return hib;
    }

    HirConst LowerConst(const ConstDecl &d) {
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

    HirExternFunc LowerExternFunc(const ExternFuncDecl &d) {
        HirExternFunc hef;
        hef.name = d.name;
        hef.dll = d.dll;
        hef.symbolName = d.symbolName;
        hef.isPublic = d.isPublic;
        hef.isNoReturn = d.isNoReturn;
        hef.callConv = d.callConv;
        hef.isVariadic = d.isVariadic;
        hef.returnType = d.returnType ? ResolveType(**d.returnType) : TypeRef::MakeOpaque();
        hef.params = LowerParams(d.params);
        hef.location = d.location;
        return hef;
    }

    HirExternVar LowerExternVar(const ExternVarDecl &d) {
        HirExternVar hev;
        hev.name = d.name;
        hev.isPublic = d.isPublic;
        hev.type = ResolveType(*d.type);
        hev.location = d.location;
        return hev;
    }

    HirTypeAlias LowerTypeAlias(const TypeAliasDecl &d) {
        HirTypeAlias hta;
        hta.name = d.name;
        hta.isPublic = d.isPublic;
        hta.type = ResolveType(*d.type);
        hta.location = d.location;
        return hta;
    }

    // Block & statement lowering

    HirBlock LowerBlock(const Block &block) {
        HirBlock hb;
        hb.location = block.location;
        PushScope();
        for (const auto &stmt : block.stmts) {
            hb.stmts.push_back(LowerStmt(*stmt));
        }
        PopScope();
        return hb;
    }

    HirStmtPtr LowerStmt(const Stmt &stmt) {
        if (auto *s = dynamic_cast<const ExprStmt *>(&stmt)) {
            auto hs = std::make_unique<HirExprStmt>();
            hs->location = s->location;
            hs->expr = LowerExpr(*s->expr);
            return hs;
        }

        if (auto *s = dynamic_cast<const LetStmt *>(&stmt)) {
            auto hs = std::make_unique<HirLetStmt>();
            hs->location = s->location;
            hs->isMut = s->isMut;
            hs->name = s->name;
            const std::optional<TypeRef> explicitType =
                s->type ? std::optional<TypeRef>(ResolveType(**s->type)) : std::nullopt;
            if (s->init) {
                hs->init = explicitType ? LowerExprAs(*s->init, *explicitType) : LowerExpr(*s->init);
            }
            hs->type = explicitType ? *explicitType : (hs->init ? hs->init->type : TypeRef::MakeUnknown());
            if (s->type) {
                if (const auto size = FixedSliceTypeSize(**s->type)) {
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

        if (auto *s = dynamic_cast<const IfStmt *>(&stmt)) {
            auto hs = std::make_unique<HirIfStmt>();
            hs->location = s->location;
            hs->condition = LowerExpr(*s->condition);
            hs->thenBlock = LowerBlock(*s->thenBlock);
            for (const auto &elif : s->elseIfs) {
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

        if (auto *s = dynamic_cast<const WhileStmt *>(&stmt)) {
            auto hs = std::make_unique<HirWhileStmt>();
            hs->location = s->location;
            hs->label = s->label;
            hs->condition = LowerExpr(*s->condition);
            hs->body = LowerBlock(*s->body);
            return hs;
        }

        if (auto *s = dynamic_cast<const DoWhileStmt *>(&stmt)) {
            auto hs = std::make_unique<HirDoWhileStmt>();
            hs->location = s->location;
            hs->label = s->label;
            hs->body = LowerBlock(*s->body);
            hs->condition = LowerExpr(*s->condition);
            return hs;
        }

        if (auto *s = dynamic_cast<const LoopStmt *>(&stmt)) {
            auto hs = std::make_unique<HirLoopStmt>();
            hs->location = s->location;
            hs->label = s->label;
            hs->body = LowerBlock(*s->body);
            return hs;
        }

        if (auto *s = dynamic_cast<const ForStmt *>(&stmt)) {
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
            // If a mutable variable of the same name and type is already in
            // scope, the loop reuses it as the induction variable instead of
            // shadowing it (see HirForStmt::reusesOuterVar). Otherwise the loop
            // introduces a fresh binding scoped to the body.
            HirSymbol *outer = currentScope->Lookup(s->variable);
            hs->reusesOuterVar =
                outer != nullptr && outer->kind == HirSymbol::Kind::Var && outer->isMut && outer->type == elemType;
            PushScope();
            if (!hs->reusesOuterVar) {
                HirSymbol var;
                var.kind = HirSymbol::Kind::Var;
                var.name = s->variable;
                var.type = elemType;
                Define(var);
            }
            hs->body = LowerBlock(*s->body);
            PopScope();
            return hs;
        }

        if (auto *s = dynamic_cast<const MatchStmt *>(&stmt)) {
            auto hs = std::make_unique<HirMatchStmt>();
            hs->location = s->location;
            hs->subject = LowerExpr(*s->subject);
            for (const auto &arm : s->arms) {
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

        if (auto *s = dynamic_cast<const ReturnStmt *>(&stmt)) {
            auto hs = std::make_unique<HirReturnStmt>();
            hs->location = s->location;
            if (s->value) {
                hs->value = LowerExprAs(**s->value, currentReturnType);
            }
            return hs;
        }

        if (auto *s = dynamic_cast<const BreakStmt *>(&stmt)) {
            auto hs = std::make_unique<HirBreakStmt>();
            hs->location = stmt.location;
            hs->label = s->label;
            return hs;
        }

        if (auto *s = dynamic_cast<const ContinueStmt *>(&stmt)) {
            auto hs = std::make_unique<HirContinueStmt>();
            hs->location = stmt.location;
            hs->label = s->label;
            return hs;
        }

        if (auto *s = dynamic_cast<const DeclStmt *>(&stmt)) {
            auto hs = std::make_unique<HirLocalDecl>();
            hs->location = s->location;
            CollectDecl(*s->decl);
            if (auto *fd = dynamic_cast<const FuncDecl *>(s->decl.get())) {
                hs->description = std::format("func {}", fd->name);
            }
            else if (auto *cd = dynamic_cast<const ConstDecl *>(s->decl.get())) {
                hs->description = std::format("const {}", cd->name);
                HirConst constant = LowerConst(*cd);
                hs->hasConstant = true;
                hs->constantName = std::move(constant.name);
                hs->constantType = std::move(constant.type);
                hs->constantValue = std::move(constant.value);
            }
            else if (auto *ta = dynamic_cast<const TypeAliasDecl *>(s->decl.get())) {
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
    HirExprPtr LowerExprAs(const Expr &expr, const TypeRef &targetType) {
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
    // #source.line, #source.column, #source.file, #compiler.version, etc.).
    HirExprPtr LowerDefaultArg(const Expr &defaultExpr, const TypeRef &targetType, const SourceLocation &callSiteLoc) {
        if (const auto *intr = dynamic_cast<const IntrinsicExpr *>(&defaultExpr); intr && intr->args.empty()) {
            IntrinsicExpr tmp;
            tmp.location = callSiteLoc;
            tmp.kind = intr->kind;
            return LowerExprAs(tmp, targetType);
        }
        return LowerExprAs(defaultExpr, targetType);
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
        if (name == "RVV")
            return features.Has(Target::CpuFeature::RVV);
        return false;
    }

    static bool CompilerHasFeature(const std::string_view feature) {
        static constexpr std::array features{
            "conditional-compilation",    "namespaced-intrinsics",    "target-intrinsics",   "build-intrinsics",
            "compiler-feature-detection", "source-location-defaults", "extern-symbol-names", "no-return-attribute"};
        return std::ranges::contains(features, feature);
    }

    std::string LogicalCurrentFilePath() const {
        const std::filesystem::path path(currentFile);
        if (!context.sourceRoot.empty()) {
            const auto relative = path.lexically_relative(context.sourceRoot);
            if (!relative.empty() && relative.begin()->generic_string() != "..") {
                return relative.generic_string();
            }
        }
        return path.generic_string();
    }

    std::string FormatBuildTime(const char *format) const {
        const std::time_t value = static_cast<std::time_t>(context.buildTimestamp);
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

    HirExprPtr LowerExpr(const Expr &expr) {
        if (auto *e = dynamic_cast<const LiteralExpr *>(&expr)) {
            auto he = std::make_unique<HirLiteralExpr>();
            he->location = e->location;
            he->type = LiteralType(e->token);
            if (e->token.kind == TokenKind::CharLiteral) {
                he->value = DecodeCharLiteral(e->token.text);
            }
            else if (e->token.kind == TokenKind::StringLiteral) {
                he->value = DecodeStringLiteral(e->token.text);
            }
            else if (e->token.kind == TokenKind::IntLiteral || e->token.kind == TokenKind::FloatLiteral) {
                he->value = StripNumericLiteralSuffix(e->token.text);
            }
            else {
                he->value = e->token.text;
            }
            return he;
        }
        if (auto *e = dynamic_cast<const IdentExpr *>(&expr)) {
            auto he = std::make_unique<HirVarExpr>();
            he->location = e->location;
            he->name = e->name;
            if (HirSymbol *sym = currentScope->Lookup(e->name)) {
                he->type = sym->type;
            }
            return he;
        }
        if (dynamic_cast<const SelfExpr *>(&expr)) {
            auto he = std::make_unique<HirSelfExpr>();
            he->location = expr.location;
            he->type = currentSelfType.IsUnknown() ? TypeRef::MakeNamed("self") : currentSelfType;
            return he;
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
                        he->name = CalleeName(e->segments[0], e->segments[1], receiverType, *method);
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
            he->type = TypeRef::MakeUInt64();
            he->value = std::to_string(SizeOfTypeExpr(*e->type).value_or(0));
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
                he->value = context.compilerVersion.empty() ? RUX_VERSION : context.compilerVersion;
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
                he->value = context.profileName;
                break;
            case K::DebugAssertions:
                he->type = TypeRef::MakeBool();
                he->value = context.debugAssertions ? "true" : "false";
                break;
            case K::DebugInfo:
                he->type = TypeRef::MakeBool();
                he->value = context.debugInfo ? "true" : "false";
                break;
            case K::IsTest:
                he->type = TypeRef::MakeBool();
                he->value = context.isTest ? "true" : "false";
                break;
            case K::BuildTimestamp:
                he->type = TypeRef::MakeUInt64();
                he->value = std::to_string(context.buildTimestamp);
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
        if (auto *e = dynamic_cast<const UnaryExpr *>(&expr)) {
            auto he = std::make_unique<HirUnaryExpr>();
            he->location = e->location;
            he->op = e->op;
            he->operand = LowerExpr(*e->operand);
            he->type = InferUnaryType(e->op, he->operand->type);
            return he;
        }
        if (auto *e = dynamic_cast<const PostfixExpr *>(&expr)) {
            auto he = std::make_unique<HirPostfixExpr>();
            he->location = e->location;
            he->op = e->op;
            he->operand = LowerExpr(*e->operand);
            he->type = he->operand->type;
            return he;
        }
        if (auto *e = dynamic_cast<const BinaryExpr *>(&expr)) {
            HirExprPtr left = LowerExpr(*e->left);
            HirExprPtr right = LowerExpr(*e->right);
            const std::string opName = std::string(OpStr(e->op));
            if (const FuncDecl *method = LookupMethod(left->type, opName, {right->type})) {
                const std::string receiverBase = NamedBaseTypeName(left->type);
                HirExprPtr selfArg;
                if (left->type.kind == TypeRef::Kind::Pointer) {
                    selfArg = std::move(left);
                }
                else {
                    auto addr = std::make_unique<HirUnaryExpr>();
                    addr->location = left->location;
                    addr->op = TokenKind::At;
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
                call->isNoReturn = method->isNoReturn;
                call->type = callee->type.inner.empty() ? TypeRef::MakeUnknown() : callee->type.inner.back();
                call->callee = std::move(callee);
                call->args.push_back(std::move(selfArg));
                if (call->callee->type.inner.size() > 2) {
                    const TypeRef &expectedType = call->callee->type.inner[1];
                    if (UnsuffixedIntegerLiteralFits(*e->right, expectedType)) {
                        right->type = expectedType;
                    }
                    else if (IsNullLiteral(*e->right) && expectedType.kind == TypeRef::Kind::Pointer) {
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
        if (auto *e = dynamic_cast<const AssignExpr *>(&expr)) {
            auto he = std::make_unique<HirAssignExpr>();
            he->location = e->location;
            he->op = e->op;
            he->target = LowerExpr(*e->target);
            he->value = LowerExprAs(*e->value, he->target->type);
            he->type = he->target->type;
            return he;
        }
        if (auto *e = dynamic_cast<const TernaryExpr *>(&expr)) {
            auto he = std::make_unique<HirTernaryExpr>();
            he->location = e->location;
            he->condition = LowerExpr(*e->condition);
            he->thenExpr = LowerExpr(*e->thenExpr);
            he->elseExpr = LowerExpr(*e->elseExpr);
            he->type = he->thenExpr->type.IsUnknown() ? he->elseExpr->type : he->thenExpr->type;
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
            if (elemType.IsUnknown()) {
                elemType = TypeRef::MakeInt64();
            }
            he->type = TypeRef::MakeRange(elemType);
            return he;
        }
        if (auto *e = dynamic_cast<const CallExpr *>(&expr)) {
            const auto *builtinIdent = dynamic_cast<const IdentExpr *>(e->callee.get());
            HirSymbol *calleeSymbol = builtinIdent ? currentScope->Lookup(builtinIdent->name) : nullptr;
            const bool isAssertion =
                builtinIdent && (builtinIdent->name == "Assert" || builtinIdent->name == "DebugAssert");
            const bool isPanic = builtinIdent && builtinIdent->name == "Panic";
            if ((isAssertion || isPanic) && calleeSymbol && calleeSymbol->kind == HirSymbol::Kind::Const) {
                auto he = std::make_unique<HirCallExpr>();
                he->location = e->location;
                he->type = TypeRef::MakeOpaque();
                he->sourceFile = LogicalCurrentFilePath();
                he->sourceFunction = currentFunctionName;
                he->sourceLine = e->location.line;
                he->sourceColumn = e->location.column;

                auto callee = std::make_unique<HirVarExpr>();
                callee->location = builtinIdent->location;
                callee->type = isPanic ? TypeRef::MakeFunc({TypeRef::MakeNamed(SliceTypeName(TypeRef::MakeChar8()))},
                                                           TypeRef::MakeOpaque())
                                       : TypeRef::MakeFunc({TypeRef::MakeBool(),
                                                            TypeRef::MakeNamed(SliceTypeName(TypeRef::MakeChar8()))},
                                                           TypeRef::MakeOpaque());
                const bool disabled = builtinIdent->name == "DebugAssert" && !context.debugAssertions;
                callee->name = isPanic  ? "__builtin_panic"
                             : disabled ? "__builtin_debug_assert_disabled"
                                        : "__builtin_assert";
                he->callee = std::move(callee);
                he->isNoReturn = isPanic;

                // Disabled debug assertions are still checked by semantic
                // analysis, but their arguments are not evaluated at runtime.
                if (isPanic && e->args.size() == 1) {
                    he->args.push_back(
                        LowerExprAs(*e->args[0], TypeRef::MakeNamed(SliceTypeName(TypeRef::MakeChar8()))));
                }
                else if (!disabled && e->args.size() == 2) {
                    he->args.push_back(LowerExprAs(*e->args[0], TypeRef::MakeBool()));
                    he->args.push_back(
                        LowerExprAs(*e->args[1], TypeRef::MakeNamed(SliceTypeName(TypeRef::MakeChar8()))));
                }
                return he;
            }

            if (auto *path = dynamic_cast<const PathExpr *>(e->callee.get()); path && path->segments.size() == 2) {
                const auto *variant = LookupEnumVariant(path->segments[0], path->segments[1]);
                if (variant && (!variant->fields.empty() || !variant->namedFields.empty())) {
                    const TypeExpr *singlePayloadType = nullptr;
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
                        he->payloads.push_back(LowerExprAs(*e->args[0], ResolveType(*singlePayloadType)));
                        he->discriminant =
                            LookupEnumVariantDiscriminant(path->segments[0], path->segments[1]).value_or("0");
                        return he;
                    }

                    auto he = std::make_unique<HirLiteralExpr>();
                    he->location = e->location;
                    he->type = EnumType(*enumDecls.at(path->segments[0]));
                    he->value = LookupEnumVariantDiscriminant(path->segments[0], path->segments[1]).value_or("0");
                    return he;
                }
            }

            if (auto *path = dynamic_cast<const PathExpr *>(e->callee.get()); path && path->segments.size() == 2) {
                HirSymbol *first = currentScope->Lookup(path->segments[0]);
                if (first && (first->kind == HirSymbol::Kind::Type || first->kind == HirSymbol::Kind::Interface) &&
                    !LookupEnumVariant(path->segments[0], path->segments[1])) {
                    TypeRef receiverType = first->type.IsUnknown() ? TypeRef::MakeNamed(first->name) : first->type;
                    std::vector<HirExprPtr> args;
                    std::vector<TypeRef> argTypes;
                    args.reserve(e->args.size());
                    argTypes.reserve(e->args.size());
                    for (const auto &arg : e->args) {
                        auto lowered = LowerExpr(*arg);
                        argTypes.push_back(lowered->type);
                        args.push_back(std::move(lowered));
                    }
                    if (const FuncDecl *method = LookupMethod(receiverType, path->segments[1], argTypes)) {
                        TypeRef funcType = AssociatedFunctionType(receiverType, *method);
                        auto callee = std::make_unique<HirVarExpr>();
                        callee->location = path->location;
                        callee->name = CalleeName(path->segments[0], path->segments[1], receiverType, *method);
                        callee->type = funcType;
                        auto he = std::make_unique<HirCallExpr>();
                        he->location = e->location;
                        he->isNoReturn = method->isNoReturn;
                        he->callee = std::move(callee);
                        for (std::size_t i = 0; i < args.size(); ++i) {
                            if (i + 1 < funcType.inner.size() &&
                                UnsuffixedIntegerLiteralFits(*e->args[i], funcType.inner[i])) {
                                args[i]->type = funcType.inner[i];
                            }
                            he->args.push_back(std::move(args[i]));
                        }
                        he->type = funcType.inner.empty() ? TypeRef::MakeUnknown() : funcType.inner.back();
                        return he;
                    }
                }
            }

            if (auto *path = dynamic_cast<const PathExpr *>(e->callee.get()); path && path->segments.size() >= 2) {
                std::vector<HirExprPtr> args;
                std::vector<TypeRef> argTypes;
                args.reserve(e->args.size());
                argTypes.reserve(e->args.size());
                for (const auto &arg : e->args) {
                    auto lowered = LowerExpr(*arg);
                    argTypes.push_back(lowered->type);
                    args.push_back(std::move(lowered));
                }
                const std::string &funcName = path->segments.back();
                HirSymbol *sym = currentScope->Lookup(funcName);
                if (sym && sym->kind == HirSymbol::Kind::Func && !sym->funcOverloads.empty()) {
                    if (const FuncDecl *decl = LookupFunction(funcName, argTypes, e->typeArgs)) {
                        std::unordered_map<std::string, TypeRef> substitutions;
                        const std::size_t count = std::min(decl->typeParams.size(), e->typeArgs.size());
                        for (std::size_t i = 0; i < count; ++i) {
                            substitutions.emplace(decl->typeParams[i], ResolveType(*e->typeArgs[i]));
                        }
                        TypeRef funcType = MakeFuncTypeWithSubstitution(decl->params, decl->returnType, substitutions,
                                                                        decl->typeParams);
                        if (funcType.kind == TypeRef::Kind::Func && !funcType.inner.empty()) {
                            const bool isVariadic = !decl->params.empty() && decl->params.back().isVariadic;
                            const std::size_t fixedCount = decl->params.size() - (isVariadic ? 1 : 0);
                            for (std::size_t i = args.size(); i < fixedCount; ++i) {
                                if (decl->params[i].defaultValue) {
                                    TypeRef pt =
                                        (i + 1 < funcType.inner.size()) ? funcType.inner[i] : TypeRef::MakeUnknown();
                                    args.push_back(LowerDefaultArg(**decl->params[i].defaultValue, pt, e->location));
                                }
                            }
                            if (isVariadic) {
                                TypeRef varElemType = ResolveType(*decl->params.back().type);
                                const bool isSingleSpread =
                                    (e->args.size() == fixedCount + 1 &&
                                     dynamic_cast<const SpreadExpr *>(e->args[fixedCount].get()));
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
                                        slice->elements.push_back(LowerExprAs(*e->args[i], varElemType));
                                    }
                                    args.resize(fixedCount);
                                    args.push_back(std::move(slice));
                                }
                            }

                            auto callee = std::make_unique<HirVarExpr>();
                            callee->location = path->location;
                            if (!decl->typeParams.empty()) {
                                std::string specializedName = funcName;
                                for (std::size_t i = 0; i < e->typeArgs.size(); ++i) {
                                    specializedName += "_" + MangleTypeName(ResolveType(*e->typeArgs[i]));
                                }
                                if (generatedMonomorphizedFuncNames.insert(specializedName).second) {
                                    monomorphizedFuncs.push_back(
                                        LowerFunc(*decl, false, substitutions, specializedName));
                                }
                                callee->name = specializedName;
                            }
                            else {
                                callee->name = FunctionCalleeName(funcName, *decl);
                            }
                            callee->type = funcType;

                            auto he = std::make_unique<HirCallExpr>();
                            he->location = e->location;
                            he->isNoReturn = decl->isNoReturn;
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

            if (auto *ident = dynamic_cast<const IdentExpr *>(e->callee.get())) {
                std::vector<HirExprPtr> args;
                std::vector<TypeRef> argTypes;
                args.reserve(e->args.size());
                argTypes.reserve(e->args.size());
                for (const auto &arg : e->args) {
                    auto lowered = LowerExpr(*arg);
                    argTypes.push_back(lowered->type);
                    args.push_back(std::move(lowered));
                }
                HirSymbol *sym = currentScope->Lookup(ident->name);
                if (sym && sym->kind == HirSymbol::Kind::Func && !sym->funcOverloads.empty()) {
                    if (const FuncDecl *decl = LookupFunction(ident->name, argTypes, e->typeArgs)) {
                        std::unordered_map<std::string, TypeRef> substitutions;
                        const std::size_t count = std::min(decl->typeParams.size(), e->typeArgs.size());
                        for (std::size_t i = 0; i < count; ++i) {
                            substitutions.emplace(decl->typeParams[i], ResolveType(*e->typeArgs[i]));
                        }
                        TypeRef funcType = MakeFuncTypeWithSubstitution(decl->params, decl->returnType, substitutions,
                                                                        decl->typeParams);
                        if (funcType.kind == TypeRef::Kind::Func && !funcType.inner.empty()) {
                            const bool isVariadic = !decl->params.empty() && decl->params.back().isVariadic;
                            const std::size_t fixedCount = decl->params.size() - (isVariadic ? 1 : 0);
                            // Inject default arguments for omitted fixed
                            // parameters
                            for (std::size_t i = args.size(); i < fixedCount; ++i) {
                                if (decl->params[i].defaultValue) {
                                    TypeRef pt =
                                        (i + 1 < funcType.inner.size()) ? funcType.inner[i] : TypeRef::MakeUnknown();
                                    args.push_back(LowerDefaultArg(**decl->params[i].defaultValue, pt, e->location));
                                }
                            }
                            if (isVariadic) {
                                TypeRef varElemType = ResolveType(*decl->params.back().type);
                                const bool isSingleSpread =
                                    (e->args.size() == fixedCount + 1 &&
                                     dynamic_cast<const SpreadExpr *>(e->args[fixedCount].get()));
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
                                        slice->elements.push_back(LowerExprAs(*e->args[i], varElemType));
                                    }
                                    args.resize(fixedCount);
                                    args.push_back(std::move(slice));
                                }
                            }

                            auto callee = std::make_unique<HirVarExpr>();
                            callee->location = ident->location;
                            if (!decl->typeParams.empty()) {
                                std::string specializedName = ident->name;
                                for (std::size_t i = 0; i < e->typeArgs.size(); ++i) {
                                    specializedName += "_" + MangleTypeName(ResolveType(*e->typeArgs[i]));
                                }
                                if (generatedMonomorphizedFuncNames.insert(specializedName).second) {
                                    monomorphizedFuncs.push_back(
                                        LowerFunc(*decl, false, substitutions, specializedName));
                                }
                                callee->name = specializedName;
                            }
                            else {
                                callee->name = FunctionCalleeName(ident->name, *decl);
                            }
                            callee->type = funcType;

                            auto he = std::make_unique<HirCallExpr>();
                            he->location = e->location;
                            he->isNoReturn = decl->isNoReturn;
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
            if (auto *field = dynamic_cast<const FieldExpr *>(e->callee.get())) {
                HirExprPtr receiver = LowerExpr(*field->object);
                const std::string receiverBase = NamedBaseTypeName(receiver->type);
                // Pre-lower args when we have overloads so we can pick the
                // right one.
                std::vector<HirExprPtr> preArgs;
                std::vector<TypeRef> argTypes;
                if (MethodIsOverloaded(receiverBase, field->field)) {
                    for (const auto &arg : e->args) {
                        preArgs.push_back(LowerExpr(*arg));
                        argTypes.push_back(preArgs.back()->type);
                    }
                }
                if (const FuncDecl *method = LookupMethod(receiver->type, field->field, argTypes)) {
                    he->isNoReturn = method->isNoReturn;
                    HirExprPtr selfArg;
                    if (receiver->type.kind == TypeRef::Kind::Pointer) {
                        selfArg = std::move(receiver);
                    }
                    else {
                        auto addr = std::make_unique<HirUnaryExpr>();
                        addr->location = receiver->location;
                        addr->op = TokenKind::At;
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
                                const TypeRef &expectedType = he->callee->type.inner[i + 1];
                                if (UnsuffixedIntegerLiteralFits(*e->args[i], expectedType)) {
                                    preArgs[i]->type = expectedType;
                                }
                                else if (IsNullLiteral(*e->args[i]) && expectedType.kind == TypeRef::Kind::Pointer) {
                                    preArgs[i]->type = expectedType;
                                    if (auto *lit = dynamic_cast<HirLiteralExpr *>(preArgs[i].get())) {
                                        lit->value = "0";
                                    }
                                }
                            }
                            he->args.push_back(std::move(preArgs[i]));
                        }
                    }
                    else {
                        for (std::size_t i = 0; i < e->args.size(); ++i) {
                            he->args.push_back(i + 1 < he->callee->type.inner.size()
                                                   ? LowerExprAs(*e->args[i], he->callee->type.inner[i + 1])
                                                   : LowerExpr(*e->args[i]));
                        }
                    }
                    he->type = he->callee->type.inner.back();
                    return he;
                }
                // Interface dispatch: receiver type is a known interface
                if (receiver && receiver->type.kind == TypeRef::Kind::Named) {
                    const std::string receiverName = BaseTypeName(receiver->type.name);
                    if (HirSymbol *sym = currentScope->Lookup(receiverName);
                        sym && sym->kind == HirSymbol::Kind::Interface) {
                        const int idx = InterfaceMethodIndex(receiverName, field->field);
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
                                const std::vector<TypeRef> paramTypes =
                                    InterfaceMethodParamTypes(receiverName, field->field);
                                for (std::size_t i = 0; i < e->args.size(); ++i) {
                                    ic->args.push_back(i < paramTypes.size() ? LowerExprAs(*e->args[i], paramTypes[i])
                                                                             : LowerExpr(*e->args[i]));
                                }
                            }
                            return ic;
                        }
                    }
                }
            }

            he->callee = LowerExpr(*e->callee);
            if (const auto *ident = dynamic_cast<const IdentExpr *>(e->callee.get())) {
                if (const HirSymbol *symbol = currentScope->Lookup(ident->name)) {
                    he->isNoReturn = symbol->isNoReturn;
                }
            }
            else if (const auto *path = dynamic_cast<const PathExpr *>(e->callee.get()); !path->segments.empty()) {
                if (const HirSymbol *symbol = currentScope->Lookup(path->segments.back())) {
                    he->isNoReturn = symbol->isNoReturn;
                }
            }
            const bool hasParamTypes =
                he->callee->type.kind == TypeRef::Kind::Func && he->callee->type.inner.size() == e->args.size() + 1;
            for (std::size_t i = 0; i < e->args.size(); ++i) {
                he->args.push_back(hasParamTypes ? LowerExprAs(*e->args[i], he->callee->type.inner[i])
                                                 : LowerExpr(*e->args[i]));
            }
            // Propagate return type if callee is a known func type
            if (he->callee->type.kind == TypeRef::Kind::Func && !he->callee->type.inner.empty()) {
                he->type = he->callee->type.inner.back();
            }
            return he;
        }
        if (auto *e = dynamic_cast<const IndexExpr *>(&expr)) {
            auto he = std::make_unique<HirIndexExpr>();
            he->location = e->location;
            he->object = LowerExpr(*e->object);
            he->index = LowerExpr(*e->index);
            if (auto elemType = IndexElementType(he->object->type)) {
                he->type = *elemType;
            }
            return he;
        }
        if (auto *e = dynamic_cast<const FieldExpr *>(&expr)) {
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
                TypeRef rangeElemType =
                    he->object->type.inner.empty() ? TypeRef::MakeInt64() : he->object->type.inner[0];
                if (e->field == "lo" || e->field == "hi") {
                    he->type = rangeElemType;
                }
                else if (e->field == "inclusive") {
                    he->type = TypeRef::MakeBool();
                }
            }
            else if (he->object->type.kind == TypeRef::Kind::Tuple) {
                try {
                    const std::size_t idx = std::stoul(e->field);
                    if (idx < he->object->type.inner.size()) {
                        he->type = he->object->type.inner[idx];
                    }
                }
                catch (...) {
                }
            }
            else if (const std::string ifaceName = NamedBaseTypeName(he->object->type);
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
            he->type = TypeRef::MakeNamed(he->typeName);
            for (const auto &f : e->fields) {
                HirStructInitField hf;
                hf.name = f.name;
                hf.value = LowerExprAs(*f.value, StructInitFieldType(*e, f.name));
                he->fields.push_back(std::move(hf));
            }
            return he;
        }
        if (auto *e = dynamic_cast<const SliceExpr *>(&expr)) {
            auto he = std::make_unique<HirSliceExpr>();
            he->location = e->location;
            TypeRef elemType = TypeRef::MakeUnknown();
            for (const auto &el : e->elements) {
                he->elements.push_back(LowerExpr(*el));
                if (elemType.IsUnknown()) {
                    elemType = he->elements.back()->type;
                }
            }
            he->elementType = elemType;
            he->type = TypeRef::MakeNamed(SliceTypeName(elemType));
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
        if (auto *e = dynamic_cast<const CastExpr *>(&expr)) {
            auto he = std::make_unique<HirCastExpr>();
            he->location = e->location;
            he->operand = LowerExpr(*e->operand);
            he->targetType = ResolveType(*e->type);
            he->type = he->targetType;
            return he;
        }
        if (auto *e = dynamic_cast<const IsExpr *>(&expr)) {
            // The answer is statically known for all non-interface types.
            // Interface types are rejected by Sema, so this path never
            // reaches Lir.
            auto he = std::make_unique<HirLiteralExpr>();
            he->value = LowerExpr(*e->operand)->type == ResolveType(*e->type) ? "true" : "false";
            he->type = TypeRef::MakeBool();
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

    static TypeRef InferUnaryType(TokenKind op, const TypeRef &t) {
        switch (op) {
        case TokenKind::Bang:
            return TypeRef::MakeBool();
        case TokenKind::At:
            return TypeRef::MakePointer(t);
        case TokenKind::Star:
            return t.inner.empty() ? TypeRef::MakeUnknown() : t.inner[0];
        default:
            return t;
        }
    }

    static TypeRef InferBinaryType(TokenKind op, const TypeRef &l, const TypeRef &r) {
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

    HirPatternPtr LowerLetPattern(const Pattern &pat, const TypeRef &type, bool isMut) {
        if (dynamic_cast<const WildcardPattern *>(&pat)) {
            auto hp = std::make_unique<HirWildcardPattern>();
            hp->location = pat.location;
            return hp;
        }
        if (auto *p = dynamic_cast<const IdentPattern *>(&pat)) {
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
        if (auto *p = dynamic_cast<const TuplePattern *>(&pat)) {
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
    HirPatternPtr LowerPattern(const Pattern &pat) {
        if (dynamic_cast<const WildcardPattern *>(&pat)) {
            auto hp = std::make_unique<HirWildcardPattern>();
            hp->location = pat.location;
            return hp;
        }
        if (auto *p = dynamic_cast<const LiteralPattern *>(&pat)) {
            auto hp = std::make_unique<HirLiteralPattern>();
            hp->location = p->location;
            hp->type = LiteralType(p->value);
            if (p->value.kind == TokenKind::IntLiteral || p->value.kind == TokenKind::FloatLiteral) {
                hp->value = StripNumericLiteralSuffix(p->value.text);
            }
            else {
                hp->value = p->value.text;
            }
            return hp;
        }
        if (auto *p = dynamic_cast<const IdentPattern *>(&pat)) {
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
        if (auto *p = dynamic_cast<const RangePattern *>(&pat)) {
            auto hp = std::make_unique<HirRangePattern>();
            hp->location = p->location;
            hp->inclusive = p->inclusive;
            hp->lo = LowerPattern(*p->lo);
            hp->hi = LowerPattern(*p->hi);
            return hp;
        }
        if (auto *p = dynamic_cast<const EnumPattern *>(&pat)) {
            auto hp = std::make_unique<HirEnumPattern>();
            hp->location = p->location;
            hp->path = p->path;
            const EnumDecl::Variant *variant = nullptr;
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
                    if (const auto enumIt = enumDecls.find(p->path[0]); enumIt != enumDecls.end()) {
                        for (const auto &unitVariant : enumIt->second->variants) {
                            if (unitVariant.fields.empty() && unitVariant.namedFields.empty()) {
                                if (auto disc = LookupEnumVariantDiscriminant(p->path[0], unitVariant.name)) {
                                    hp->unitDiscriminants.push_back(*disc);
                                }
                            }
                        }
                    }
                }
            }
            std::unordered_map<std::string, const Pattern *> namedArgs;
            for (const auto &arg : p->namedArgs) {
                namedArgs.emplace(arg.name, arg.pattern.get());
            }
            if (variant) {
                for (const auto &field : variant->namedFields) {
                    if (const auto it = namedArgs.find(field.name); it != namedArgs.end()) {
                        hp->argIndices.push_back(&field - variant->namedFields.data());
                        hp->args.push_back(LowerLetPattern(*it->second, ResolveType(*field.type), false));
                    }
                }
            }
            else {
                for (const auto &arg : p->namedArgs) {
                    hp->args.push_back(LowerPattern(*arg.pattern));
                }
            }
            for (std::size_t i = 0; i < p->args.size(); ++i) {
                if (variant && i < variant->fields.size()) {
                    hp->argIndices.push_back(i);
                    hp->args.push_back(LowerLetPattern(*p->args[i], ResolveType(*variant->fields[i]), false));
                }
                else if (variant && i - variant->fields.size() < variant->namedFields.size()) {
                    hp->argIndices.push_back(i);
                    hp->args.push_back(LowerLetPattern(
                        *p->args[i], ResolveType(*variant->namedFields[i - variant->fields.size()].type), false));
                }
                else {
                    hp->args.push_back(LowerPattern(*p->args[i]));
                }
            }
            return hp;
        }
        if (auto *p = dynamic_cast<const StructPattern *>(&pat)) {
            auto hp = std::make_unique<HirStructPattern>();
            hp->location = p->location;
            hp->typeName = p->typeName;
            if (HirSymbol *sym = currentScope->Lookup(p->typeName)) {
                hp->resolvedType = sym->type;
            }
            for (const auto &f : p->fields) {
                HirStructPatternField hf;
                hf.name = f.name;
                hf.pattern = LowerPattern(*f.pattern);
                hp->fields.push_back(std::move(hf));
            }
            return hp;
        }
        if (auto *p = dynamic_cast<const TuplePattern *>(&pat)) {
            auto hp = std::make_unique<HirTuplePattern>();
            hp->location = p->location;
            for (const auto &e : p->elements) {
                hp->elements.push_back(LowerPattern(*e));
            }
            return hp;
        }
        if (auto *p = dynamic_cast<const GuardedPattern *>(&pat)) {
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
AstToHirLowering::AstToHirLowering(const SemanticModel &model)
    : modules_(model.modules)
    , compileTimeContext_(model.compileTimeContext) {
}

HirPackage AstToHirLowering::Generate() {
    Lowering lowering(modules_, compileTimeContext_);
    return lowering.Run();
}
} // namespace Rux
