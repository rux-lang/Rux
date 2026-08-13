#include "Semantic/SemanticAnalyzer.h"

#include "Lexer/Lexer.h"
#include "Semantic/ConditionalCompilation.h"
#include "Semantic/Detail/SemanticAnalyzerContext.h"
#include "Semantic/PrimitiveConstants.h"
#include "Semantic/Type.h"
#include "Target/Layout.h"
#include "Target/Target.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <charconv>
#include <format>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace Rux {
using Layout::AlignUp;

using SemanticDetail::Scope;
using SemanticDetail::SemanticAnalyzerContext;
using SemanticDetail::Symbol;

class SemanticAnalyzerImplementation final : public SemanticAnalyzerContext {
public:
    SemanticAnalyzerImplementation(std::vector<const Module *> &inputModules,
                                   std::vector<DepPackage> &inputDependencies, const std::string &inputPackageName,
                                   std::vector<SemanticDiagnostic> &inputDiagnostics,
                                   std::vector<SemanticSymbol> &inputSymbols, const CompileTimeContext &inputContext,
                                   std::unordered_map<const Expr *, TypeRef> &inputExpressionTypes,
                                   std::unordered_map<const TypeExpr *, TypeRef> &inputTypeNodeTypes,
                                   std::unordered_map<const Pattern *, TypeRef> &inputPatternTypes,
                                   std::unordered_map<const CallExpr *, ResolvedCallableBinding> &inputCallableBindings,
                                   std::unordered_map<const Decl *, ResolvedSymbolIdentity> &inputSymbolIdentities,
                                   std::unordered_map<const ImplDecl *, ResolvedVtableIdentity> &inputVtableIdentities,
                                   std::unordered_map<std::string, ResolvedTypeLayout> &inputTypeLayouts,
                                   std::unordered_map<const SizeOfExpr *, std::uint64_t> &inputSizeOfValues)
        : SemanticAnalyzerContext(inputModules, inputDependencies, inputPackageName, inputDiagnostics, inputSymbols,
                                  inputContext, inputExpressionTypes, inputTypeNodeTypes, inputPatternTypes,
                                  inputCallableBindings, inputSymbolIdentities, inputVtableIdentities, inputTypeLayouts,
                                  inputSizeOfValues) {
    }

private:
    struct DeferredGenericCall {
        const FuncDecl *callee;
        std::unordered_map<std::string, TypeRef> substitutions;
    };

    struct PendingGenericInstantiation {
        const FuncDecl *decl;
        std::unordered_map<std::string, TypeRef> substitutions;
    };

    std::unordered_map<const FuncDecl *, std::vector<DeferredGenericCall>> deferredGenericCalls;
    std::vector<PendingGenericInstantiation> pendingGenericInstantiations;
    std::unordered_map<const FuncDecl *, std::unordered_set<std::string>> validatedGenericInstantiations;
    std::unordered_set<std::string> activeLayoutTypes;

    struct FunctionSignature {
        std::size_t typeParamCount = 0;
        std::vector<TypeRef> paramTypes;
        std::vector<bool> variadicParams;
    };

    TypeRef MakeFuncType(const std::vector<Param> &params, const std::optional<TypeExprPtr> &returnType,
                         const std::vector<std::string> &typeParams = {}, bool cVariadic = false) {
        auto savedTypeParams = currentTypeParams;
        currentTypeParams = typeParams;

        std::vector<TypeRef> paramTypes;
        bool variadic = cVariadic;
        for (const auto &param : params) {
            if (!param.isVariadic) {
                paramTypes.push_back(ResolveType(*param.type));
            }
            else {
                variadic = true;
            }
        }
        TypeRef ret = returnType ? ResolveType(*returnType->get()) : TypeRef::MakeOpaque();

        currentTypeParams = savedTypeParams;
        TypeRef funcType = TypeRef::MakeFunc(std::move(paramTypes), std::move(ret));
        funcType.isVariadic = variadic;
        return funcType;
    }

    TypeRef MakeFuncTypeWithSubstitution(const std::vector<Param> &params, const std::optional<TypeExprPtr> &returnType,
                                         const std::unordered_map<std::string, TypeRef> &substitutions,
                                         const std::vector<std::string> &typeParams = {},
                                         bool cVariadic = false) override {
        auto savedTypeParams = currentTypeParams;
        currentTypeParams = typeParams;

        std::vector<TypeRef> paramTypes;
        bool variadic = cVariadic;
        for (const auto &param : params) {
            if (!param.isVariadic) {
                paramTypes.push_back(ResolveTypeWithSubstitution(*param.type, substitutions));
            }
            else {
                variadic = true;
            }
        }
        TypeRef ret = returnType ? ResolveTypeWithSubstitution(**returnType, substitutions) : TypeRef::MakeOpaque();

        currentTypeParams = savedTypeParams;
        TypeRef funcType = TypeRef::MakeFunc(std::move(paramTypes), std::move(ret));
        funcType.isVariadic = variadic;
        return funcType;
    }

    void ResolveModuleSignatures(const Module &mod) override {
        currentFile = mod.name;
        for (const auto &decl : mod.items) {
            ResolveDeclSignature(*decl);
        }
    }

    void ResolveDeclSignature(const Decl &decl) {
        if (auto *fn = dynamic_cast<const FuncDecl *>(&decl)) {
            if (Symbol *sym = globalScope.Lookup(fn->name)) {
                sym->type = MakeFuncType(fn->params, fn->returnType, fn->typeParams);
            }
        }
        else if (auto *enumDecl = dynamic_cast<const EnumDecl *>(&decl)) {
            if (Symbol *sym = globalScope.Lookup(enumDecl->name)) {
                sym->type = EnumType(*enumDecl);
            }
        }
        else if (auto *externFn = dynamic_cast<const ExternFuncDecl *>(&decl)) {
            if (Symbol *sym = globalScope.Lookup(externFn->name)) {
                sym->type = MakeFuncType(externFn->params, externFn->returnType, {}, externFn->isVariadic);
            }
        }
        else if (auto *externBlock = dynamic_cast<const ExternBlockDecl *>(&decl)) {
            for (const auto &item : externBlock->items) {
                ResolveDeclSignature(*item);
            }
        }
        else if (auto *modDecl = dynamic_cast<const ModuleDecl *>(&decl)) {
            Scope &moduleScope = ModuleScopeFor(modDecl->name, globalScope);
            for (const auto &item : modDecl->items) {
                ResolveDeclSignatureInScope(*item, moduleScope);
            }
        }
    }

    void ResolveModuleSignaturesInScope(const Module &mod, Scope &scope) override {
        Scope *savedScope = currentScope;
        currentScope = &scope;
        currentFile = mod.name;
        for (const auto &decl : mod.items) {
            ResolveDeclSignatureInScope(*decl, scope);
        }
        currentScope = savedScope;
    }

    void ResolveDeclSignatureInScope(const Decl &decl, Scope &scope) {
        if (auto *fn = dynamic_cast<const FuncDecl *>(&decl)) {
            if (Symbol *sym = scope.Lookup(fn->name)) {
                sym->type = MakeFuncType(fn->params, fn->returnType, fn->typeParams);
            }
        }
        else if (auto *enumDecl = dynamic_cast<const EnumDecl *>(&decl)) {
            if (Symbol *sym = scope.Lookup(enumDecl->name)) {
                sym->type = EnumType(*enumDecl);
            }
        }
        else if (auto *externFn = dynamic_cast<const ExternFuncDecl *>(&decl)) {
            if (Symbol *sym = scope.Lookup(externFn->name)) {
                sym->type = MakeFuncType(externFn->params, externFn->returnType, {}, externFn->isVariadic);
            }
        }
        else if (auto *externBlock = dynamic_cast<const ExternBlockDecl *>(&decl)) {
            for (const auto &item : externBlock->items) {
                ResolveDeclSignatureInScope(*item, scope);
            }
        }
        else if (auto *modDecl = dynamic_cast<const ModuleDecl *>(&decl)) {
            Scope &moduleScope = ModuleScopeFor(modDecl->name, scope);
            for (const auto &item : modDecl->items) {
                ResolveDeclSignatureInScope(*item, moduleScope);
            }
        }
    }

    void ApplyModuleImports(const Module &mod) override {
        currentFile = mod.name;
        for (const auto &decl : mod.items) {
            ApplyDeclImports(*decl);
        }
    }

    void ApplyModuleImportsInScope(const Module &mod, Scope &scope) override {
        Scope *savedScope = currentScope;
        currentScope = &scope;
        ApplyModuleImports(mod);
        currentScope = savedScope;
    }

    void ApplyDeclImports(const Decl &decl) {
        if (auto *useDecl = dynamic_cast<const UseDecl *>(&decl)) {
            CheckUseDecl(*useDecl);
        }
        else if (auto *modDecl = dynamic_cast<const ModuleDecl *>(&decl)) {
            Scope *savedScope = currentScope;
            currentScope = &ModuleScopeFor(modDecl->name, *currentScope);
            for (const auto &item : modDecl->items) {
                ApplyDeclImports(*item);
            }
            currentScope = savedScope;
        }
    }

    Scope &ModuleScopeFor(const std::string &name, Scope &parent) {
        return programIndex.ModuleScopeFor(name, parent);
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

    std::string BaseTypeName(const std::string &name) const override {
        const std::size_t pos = name.find('<');
        return pos == std::string::npos ? name : name.substr(0, pos);
    }

    std::vector<std::string> ImplTypeParams(const ImplDecl &decl) const {
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

    // The text of a string-literal token, with the surrounding quotes and any
    // encoding prefix removed and the common escapes decoded, for use as a
    // human-readable diagnostic message.
    static std::string DecodeStringMessage(const std::string &text) {
        const std::size_t open = text.find('"');
        if (open == std::string::npos || text.size() < open + 2 || text.back() != '"') {
            return {};
        }
        const std::string_view body(text.data() + open + 1, text.size() - open - 2);
        std::string out;
        out.reserve(body.size());
        for (std::size_t i = 0; i < body.size(); ++i) {
            if (body[i] != '\\' || i + 1 == body.size()) {
                out.push_back(body[i]);
                continue;
            }
            switch (body[++i]) {
            case 'n':
                out.push_back('\n');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case '0':
                out.push_back('\0');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '"':
                out.push_back('"');
                break;
            default:
                out.push_back('\\');
                out.push_back(body[i]);
                break;
            }
        }
        return out;
    }

    // `#Error(message)` and `#Warn(message)` are compile-time directives: at each
    // live call site they emit a diagnostic with their message and produce no
    // runtime code. A live call is one the `when` fold kept, so a directive in a
    // branch that is not taken never fires. The message must be a string literal.
    void EmitDiagnosticIntrinsic(const std::string &intrinsicName, const CallExpr &call) override {
        const bool isError = intrinsicName == "#Error";
        if (call.args.size() != 1 || !call.args[0]) {
            EmitError(call.location, std::format("'{}' expects exactly one string argument", intrinsicName));
            return;
        }
        const auto *literal = dynamic_cast<const LiteralExpr *>(call.args[0].get());
        if (!literal || literal->token.kind != TokenKind::StringLiteral) {
            EmitError(call.args[0]->location, std::format("'{}' message must be a string literal", intrinsicName));
            return;
        }
        std::string message = DecodeStringMessage(literal->token.text);
        if (isError) {
            EmitError(call.location, std::move(message));
        }
        else {
            EmitWarning(call.location, std::move(message));
        }
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

    static std::optional<std::uint64_t> ParseIntegerLiteralValue(const Token &tok) {
        if (tok.kind != TokenKind::IntLiteral) {
            return std::nullopt;
        }

        std::string text;
        text.reserve(tok.text.size());
        for (const char c : tok.text) {
            if (c != '_') {
                text.push_back(c);
            }
        }

        const std::string suffix = NumericLiteralSuffix(text);
        if (!suffix.empty()) {
            text.resize(text.size() - suffix.size());
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

    static bool IsUnsuffixedIntegerLiteral(const Expr &expr) {
        const LiteralExpr *literal = dynamic_cast<const LiteralExpr *>(&expr);
        if (!literal) {
            const auto *unary = dynamic_cast<const UnaryExpr *>(&expr);
            if (!unary || unary->op != TokenKind::Minus) {
                return false;
            }
            literal = dynamic_cast<const LiteralExpr *>(unary->operand.get());
        }
        return literal && literal->token.kind == TokenKind::IntLiteral &&
               NumericLiteralSuffix(literal->token.text).empty();
    }

    static bool IsIntegerLiteralOutOfRangeFor(const Expr &expr, const TypeRef &targetType) {
        return targetType.IsInteger() && IsUnsuffixedIntegerLiteral(expr) &&
               !UnsuffixedIntegerLiteralFits(expr, targetType);
    }

    // Explains why the address of an immutable place cannot initialize a
    // writable pointer. The types alone do not point at the required binding
    // change, so name it when possible.
    std::string ImmutableAddressOfHint(const Expr &expr, const TypeRef &targetType) {
        // Only a '*var T' target can reject a read-only '*T' source this way.
        if (targetType.kind != TypeRef::Kind::Pointer || targetType.inner.empty() || !targetType.inner[0].isMut) {
            return {};
        }
        const auto *addressOf = dynamic_cast<const UnaryExpr *>(&expr);
        if (!addressOf || addressOf->op != TokenKind::At) {
            return {};
        }
        const auto *ident = dynamic_cast<const IdentExpr *>(addressOf->operand.get());
        if (!ident) {
            return ": the addressed place is immutable and yields a read-only '*T'";
        }
        const Symbol *sym = currentScope->Lookup(ident->name);
        if (sym && sym->kind == Symbol::Kind::Const) {
            return std::format(": '{}' is a constant; a mutable pointer to it is not allowed", ident->name);
        }
        if (PlaceIsImmutable(*addressOf->operand)) {
            return std::format(": '@{0}' yields a read-only '*T'; declare '{0}' with 'var' for a '*var T'",
                               ident->name);
        }
        return {};
    }

    // Picks the diagnostic for a rejected assignment/conversion. An
    // unsuffixed integer literal that does not fit the target gets a
    // dedicated "out of range" message; taking the address of an immutable
    // place gets the reason appended; everything else uses `fallback`.
    // Keeps the wording consistent across let, return, assignment, const,
    // and field positions.
    std::string AssignmentErrorMessage(const Expr &expr, const TypeRef &targetType, std::string fallback) override {
        if (IsIntegerLiteralOutOfRangeFor(expr, targetType)) {
            return std::format("integer literal is out of range for type '{}'", targetType.ToString());
        }
        if (const std::string hint = ImmutableAddressOfHint(expr, targetType); !hint.empty()) {
            return fallback + hint;
        }
        return fallback;
    }

    bool TypeImplementsInterface(const TypeRef &exprType, const TypeRef &targetType) const {
        if (targetType.kind != TypeRef::Kind::Named) {
            return false;
        }
        Symbol *sym = currentScope->Lookup(targetType.name);
        if (!sym || sym->kind != Symbol::Kind::Interface) {
            return false;
        }
        // An empty interface is trivially satisfied by every type.
        if (sym->interfaceMethods.empty()) {
            return true;
        }
        auto implements = [&](const TypeRef &type) {
            const std::string typeName = type.ToString();
            auto it = typeImplementsInterfaces.find(typeName);
            return it != typeImplementsInterfaces.end() && it->second.count(targetType.name);
        };
        if (implements(exprType)) {
            return true;
        }
        if (exprType.kind == TypeRef::Kind::Int) {
            return implements(TypeRef::MakeInt64());
        }
        if (exprType.kind == TypeRef::Kind::Int64) {
            return implements(TypeRef::MakeInt());
        }
        if (exprType.kind == TypeRef::Kind::UInt) {
            return implements(TypeRef::MakeUInt64());
        }
        if (exprType.kind == TypeRef::Kind::UInt64) {
            return implements(TypeRef::MakeUInt());
        }
        return false;
    }

    // Folds a compile-time-constant integer expression (unsuffixed integer
    // literals combined with the integer operators) to its int64 value,
    // using the same two's-complement wrapping the generated code produces
    // at run time, so the folded value always matches what the program
    // computes. Returns nullopt when the expression is not such a constant,
    // so callers fall back to ordinary type checking. Division/modulo by
    // zero and the INT64_MIN / -1 overflow are left unfolded and keep their
    // runtime behavior; '**' is not folded (it lowers to a runtime helper
    // call).
    static std::optional<std::int64_t> EvalConstInt(const Expr &expr) {
        using I = std::int64_t;
        using U = std::uint64_t;

        if (const auto *lit = dynamic_cast<const LiteralExpr *>(&expr)) {
            if (lit->token.kind != TokenKind::IntLiteral || !NumericLiteralSuffix(lit->token.text).empty()) {
                return std::nullopt;
            }
            const auto v = ParseUnsuffixedIntegerLiteral(lit->token);
            if (!v || *v > static_cast<U>(std::numeric_limits<I>::max())) {
                return std::nullopt;
            }
            return static_cast<I>(*v);
        }

        if (const auto *un = dynamic_cast<const UnaryExpr *>(&expr)) {
            const auto v = EvalConstInt(*un->operand);
            if (!v) {
                return std::nullopt;
            }
            switch (un->op) {
            case TokenKind::Plus:
                return *v;
            case TokenKind::Minus:
                return static_cast<I>(0u - static_cast<U>(*v));
            case TokenKind::Tilde:
                return ~*v;
            default:
                return std::nullopt;
            }
        }

        if (const auto *bin = dynamic_cast<const BinaryExpr *>(&expr)) {
            const auto l = EvalConstInt(*bin->left);
            const auto r = EvalConstInt(*bin->right);
            if (!l || !r) {
                return std::nullopt;
            }
            const U lu = static_cast<U>(*l);
            const U ru = static_cast<U>(*r);
            switch (bin->op) {
            case TokenKind::Plus:
                return static_cast<I>(lu + ru);
            case TokenKind::Minus:
                return static_cast<I>(lu - ru);
            case TokenKind::Star:
                return static_cast<I>(lu * ru);
            case TokenKind::Slash:
                if (*r == 0 || (*l == std::numeric_limits<I>::min() && *r == -1)) {
                    return std::nullopt;
                }
                return *l / *r;
            case TokenKind::Percent:
                if (*r == 0 || (*l == std::numeric_limits<I>::min() && *r == -1)) {
                    return std::nullopt;
                }
                return *l % *r;
            case TokenKind::Amp:
                return *l & *r;
            case TokenKind::Pipe:
                return *l | *r;
            case TokenKind::Caret:
                return *l ^ *r;
            case TokenKind::LessLess:
                if (*r < 0 || *r >= 64) {
                    return std::nullopt;
                }
                return static_cast<I>(lu << static_cast<U>(*r));
            case TokenKind::GreaterGreater:
                if (*r < 0 || *r >= 64) {
                    return std::nullopt;
                }
                return *l >> *r;
            case TokenKind::GreaterGreaterGreater:
                if (*r < 0 || *r >= 64) {
                    return std::nullopt;
                }
                return static_cast<I>(lu >> static_cast<U>(*r));
            default:
                return std::nullopt;
            }
        }

        return std::nullopt;
    }

    static bool ConstantFitsTarget(std::int64_t value, const TypeRef &target) {
        if (const auto max = UnsignedIntegerMax(target)) {
            return value >= 0 && static_cast<std::uint64_t>(value) <= *max;
        }
        if (const auto range = SignedIntegerRange(target)) {
            return value >= range->first && value <= range->second;
        }
        return false;
    }

    static std::optional<std::uint32_t> CharTypeMaxCodePoint(const TypeRef &type) {
        switch (type.kind) {
        case TypeRef::Kind::Char8:
            return 0xFF;
        case TypeRef::Kind::Char16:
            return 0xFFFF;
        case TypeRef::Kind::Char32:
            return 0x10FFFF;
        default:
            return std::nullopt;
        }
    }

    static bool IsCharType(const TypeRef &type) noexcept {
        switch (type.kind) {
        case TypeRef::Kind::Char8:
        case TypeRef::Kind::Char16:
        case TypeRef::Kind::Char32:
            return true;
        default:
            return false;
        }
    }

    static bool IsSurrogateCodePoint(const std::uint64_t value) noexcept {
        return value >= 0xD800 && value <= 0xDFFF;
    }

    static std::optional<std::uint64_t> EvalConstCharCastValue(const Expr &expr) {
        if (const auto *literal = dynamic_cast<const LiteralExpr *>(&expr)) {
            if (literal->token.kind == TokenKind::CharLiteral) {
                if (const auto codePoint = Lexer::DecodeCharLiteralCodePoint(literal->token.text)) {
                    return static_cast<std::uint64_t>(*codePoint);
                }
            }
            if (literal->token.kind == TokenKind::IntLiteral) {
                if (const auto value = ParseIntegerLiteralValue(literal->token)) {
                    return *value;
                }
            }
        }

        if (const auto value = EvalConstInt(expr)) {
            if (*value >= 0) {
                return static_cast<std::uint64_t>(*value);
            }
        }

        return std::nullopt;
    }

    bool CanAssignExprTo(const Expr &expr, const TypeRef &exprType, const TypeRef &targetType) override {
        if (exprType.kind == TypeRef::Kind::Array && exprType.arrayLength && !exprType.inner.empty()) {
            if (const auto sliceElement = SliceElementType(targetType)) {
                if (const auto *array = dynamic_cast<const ArrayExpr *>(&expr)) {
                    for (const auto &element : array->elements) {
                        const TypeRef elementType = CheckExpr(*element);
                        if (!CanAssignExprTo(*element, elementType, *sliceElement)) {
                            return false;
                        }
                    }
                    return true;
                }
                return exprType.inner[0].IsAssignableTo(*sliceElement);
            }
        }

        if (const auto *array = dynamic_cast<const ArrayExpr *>(&expr);
            array && targetType.kind == TypeRef::Kind::Array && targetType.arrayLength && !targetType.inner.empty()) {
            if (array->elements.size() != *targetType.arrayLength) {
                return false;
            }
            for (const auto &element : array->elements) {
                const TypeRef elementType = CheckExpr(*element);
                if (!CanAssignExprTo(*element, elementType, targetType.inner[0])) {
                    return false;
                }
            }
            return true;
        }

        // Tuple literals are contextually typed element-by-element. This lets
        // each element use the same assignment rules as a scalar expression
        // (notably range-checked unsuffixed integer literals), and naturally
        // handles nested tuple literals as well.
        if (const auto *tuple = dynamic_cast<const TupleExpr *>(&expr);
            tuple && exprType.kind == TypeRef::Kind::Tuple && targetType.kind == TypeRef::Kind::Tuple) {
            if (tuple->elements.size() != targetType.inner.size() || exprType.inner.size() != targetType.inner.size()) {
                return false;
            }
            for (std::size_t i = 0; i < tuple->elements.size(); ++i) {
                if (!CanAssignExprTo(*tuple->elements[i], exprType.inner[i], targetType.inner[i])) {
                    return false;
                }
            }
            return true;
        }

        if (targetType.IsInteger() && IsUnsuffixedIntegerLiteral(expr)) {
            return UnsuffixedIntegerLiteralFits(expr, targetType);
        }

        // A constant integer expression (e.g. 10 + 2 * (5 - 3)) coerces to
        // any integer type it fits in, the same way a bare literal does.
        if (targetType.IsInteger()) {
            if (const auto folded = EvalConstInt(expr); folded && ConstantFitsTarget(*folded, targetType)) {
                return true;
            }
        }

        return exprType.IsAssignableTo(targetType) ||
               (IsNullLiteral(expr) && targetType.kind == TypeRef::Kind::Pointer) ||
               UnsuffixedIntegerLiteralFits(expr, targetType) || TypeImplementsInterface(exprType, targetType);
    }

    std::string NamedBaseTypeName(const TypeRef &type) const {
        const TypeRef *named = &type;
        if (type.kind == TypeRef::Kind::Pointer && !type.inner.empty()) {
            named = &type.inner[0];
        }
        if (named->kind == TypeRef::Kind::Named) {
            // Slice extension methods are keyed on the full element-specific
            // name (e.g. `Slice<int>`) so `extend int[]` stays distinct from
            // `extend str[]`; other named types collapse to their base name so
            // generic instantiations share one method set.
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

    std::optional<TypeRef> IndexElementType(const TypeRef &type) override {
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

    std::optional<std::uint64_t> EvalArrayLength(const Expr &expr) const {
        const auto value = EvalConstInt(expr);
        if (!value || *value < 0) {
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(*value);
    }

    // Validate array extents and reject flexible arrays everywhere except the
    // final, top-level field of a struct. A nested T[] is never a tail field.
    void ValidateArrayType(const TypeExpr &type, bool allowFlexibleTail = false) override {
        if (const auto *array = dynamic_cast<const ArrayTypeExpr *>(&type)) {
            if (!array->size) {
                if (!allowFlexibleTail) {
                    EmitError(array->location, "flexible array type is only allowed as the final field of a struct");
                }
            }
            else if (!EvalArrayLength(*array->size)) {
                EmitError(array->size->location, "array length must be a non-negative compile-time integer");
            }
            ValidateArrayType(*array->element);
            return;
        }
        if (const auto *pointer = dynamic_cast<const PointerTypeExpr *>(&type)) {
            ValidateArrayType(*pointer->pointee);
            return;
        }
        if (const auto *tuple = dynamic_cast<const TupleTypeExpr *>(&type)) {
            for (const auto &element : tuple->elements) {
                ValidateArrayType(*element);
            }
            return;
        }
        if (const auto *function = dynamic_cast<const FunctionTypeExpr *>(&type)) {
            for (const auto &param : function->params) {
                ValidateArrayType(*param);
            }
            if (function->returnType) {
                ValidateArrayType(**function->returnType);
            }
            return;
        }
        if (const auto *named = dynamic_cast<const NamedTypeExpr *>(&type)) {
            for (const auto &arg : named->typeArgs) {
                ValidateArrayType(*arg);
            }
        }
    }

    Symbol *FindUniquePackageType(const std::string &name) const {
        auto sameSymbol = [](const Symbol &lhs, const Symbol &rhs) {
            return lhs.kind == rhs.kind && lhs.name == rhs.name && lhs.location.line == rhs.location.line &&
                   lhs.location.column == rhs.location.column;
        };

        Symbol *matched = nullptr;
        for (const auto &[_, moduleScopes] : packageModuleScopes) {
            for (const auto &[__, scope] : moduleScopes) {
                auto *sym = const_cast<Scope *>(scope)->LookupLocal(name);
                if (!sym || (sym->kind != Symbol::Kind::Type && sym->kind != Symbol::Kind::Interface)) {
                    continue;
                }
                if (matched && !sameSymbol(*matched, *sym)) {
                    return nullptr;
                }
                matched = sym;
            }
        }
        return matched;
    }

    TypeRef ResolveType(const TypeExpr &expr) override {
        TypeRef type = ResolveTypeImpl(expr);
        if (!type.IsUnknown()) {
            typeNodeTypes.insert_or_assign(&expr, type);
        }
        return type;
    }

    TypeRef ResolveTypeImpl(const TypeExpr &expr) {
        if (const auto *t = dynamic_cast<const NamedTypeExpr *>(&expr)) {
            if (IsUnimplementedPrimitiveType(t->name)) {
                EmitError(expr.location, std::format("primitive type '{}' is reserved but is not implemented in this "
                                                     "compiler version",
                                                     t->name));
                return TypeRef::MakeUnknown();
            }

            for (const auto &tp : currentTypeParams) {
                if (tp == t->name) {
                    if (!t->typeArgs.empty()) {
                        EmitError(expr.location, std::format("Type parameter '{}' cannot "
                                                             "take type arguments",
                                                             t->name));
                        return TypeRef::MakeUnknown();
                    }
                    return TypeRef::MakeTypeParam(t->name);
                }
            }

            std::vector<TypeRef> resolvedArgs;
            bool hasUnknownArgs = false;
            for (const auto &argExpr : t->typeArgs) {
                TypeRef argType = ResolveType(*argExpr);
                if (argType.IsUnknown()) {
                    hasUnknownArgs = true;
                }
                resolvedArgs.push_back(argType);
            }

            if (hasUnknownArgs) {
                return TypeRef::MakeUnknown();
            }

            if (t->name == "RangeFull" && resolvedArgs.empty()) {
                return TypeRef::MakeRangeFull();
            }
            if (resolvedArgs.size() == 1) {
                if (t->name == "Range") {
                    return TypeRef::MakeRange(resolvedArgs[0]);
                }
                if (t->name == "RangeInclusive") {
                    return TypeRef::MakeRange(resolvedArgs[0], true, true, true);
                }
                if (t->name == "RangeFrom") {
                    return TypeRef::MakeRange(resolvedArgs[0], true, false);
                }
                if (t->name == "RangeTo") {
                    return TypeRef::MakeRange(resolvedArgs[0], false, true);
                }
                if (t->name == "RangeToInclusive") {
                    return TypeRef::MakeRange(resolvedArgs[0], false, true, true);
                }
            }

            if (const auto enumIt = enumDecls.find(t->name); enumIt != enumDecls.end()) {
                const auto &decl = *enumIt->second;
                if (resolvedArgs.size() != decl.typeParams.size()) {
                    EmitError(expr.location, std::format("enum '{}' expects {} type argument(s), got {}", t->name,
                                                         decl.typeParams.size(), resolvedArgs.size()));
                    return TypeRef::MakeUnknown();
                }
                return EnumType(decl, resolvedArgs);
            }

            Symbol *sym = currentScope ? currentScope->Lookup(t->name) : nullptr;
            if (sym && (sym->kind == Symbol::Kind::Type || sym->kind == Symbol::Kind::Interface)) {
                // Return base type if no generic arguments are provided
                if (t->typeArgs.empty() && !sym->type.IsUnknown()) {
                    return sym->type;
                }

                return TypeRef::MakeNamed(GenericTypeName(*t));
            }
            if (!sym) {
                sym = FindUniquePackageType(t->name);
                if (sym && (sym->kind == Symbol::Kind::Type || sym->kind == Symbol::Kind::Interface)) {
                    if (t->typeArgs.empty() && !sym->type.IsUnknown()) {
                        return sym->type;
                    }
                    return TypeRef::MakeNamed(GenericTypeName(*t));
                }
            }

            if (structDecls.contains(t->name)) {
                return TypeRef::MakeNamed(GenericTypeName(*t));
            }

            EmitError(expr.location, std::format("unknown type '{}'", t->name));
            return TypeRef::MakeUnknown();
        }

        if (const auto *t = dynamic_cast<const PathTypeExpr *>(&expr)) {
            if (t->segments.empty()) {
                EmitError(expr.location, "empty type path");
                return TypeRef::MakeUnknown();
            }

            std::string fullPath = t->segments.front();
            for (size_t i = 1; i < t->segments.size(); ++i) {
                fullPath += "::" + t->segments[i];
            }
            return TypeRef::MakeNamed(fullPath);
        }

        if (const auto *t = dynamic_cast<const PointerTypeExpr *>(&expr)) {
            TypeRef pointeeType = ResolveType(*t->pointee);
            if (pointeeType.IsUnknown()) {
                return TypeRef::MakeUnknown();
            }
            pointeeType.isMut = pointeeType.isMut || t->pointeeMut;
            return TypeRef::MakePointer(std::move(pointeeType));
        }

        if (const auto *t = dynamic_cast<const ArrayTypeExpr *>(&expr)) {
            TypeRef elemType = ResolveType(*t->element);
            if (elemType.IsUnknown()) {
                return TypeRef::MakeUnknown();
            }
            return TypeRef::MakeArray(std::move(elemType), t->size ? EvalArrayLength(*t->size) : std::nullopt);
        }

        if (const auto *t = dynamic_cast<const TupleTypeExpr *>(&expr)) {
            std::vector<TypeRef> elems;
            elems.reserve(t->elements.size());

            for (const auto &e : t->elements) {
                TypeRef elem = ResolveType(*e);
                if (elem.IsUnknown()) {
                    return TypeRef::MakeUnknown();
                }
                elems.push_back(elem);
            }

            return TypeRef::MakeTuple(std::move(elems));
        }

        if (dynamic_cast<const SelfTypeExpr *>(&expr)) {
            return currentSelfType.IsUnknown() ? TypeRef::MakeNamed("self") : currentSelfType;
        }

        if (const auto *t = dynamic_cast<const FunctionTypeExpr *>(&expr)) {
            std::vector<TypeRef> paramTypes;
            paramTypes.reserve(t->params.size());
            for (const auto &p : t->params) {
                TypeRef pt = ResolveType(*p);
                if (pt.IsUnknown()) {
                    return TypeRef::MakeUnknown();
                }
                paramTypes.push_back(std::move(pt));
            }
            TypeRef ret = t->returnType ? ResolveType(*t->returnType->get()) : TypeRef::MakeOpaque();
            if (ret.IsUnknown()) {
                return TypeRef::MakeUnknown();
            }
            TypeRef funcType = TypeRef::MakeFunc(std::move(paramTypes), std::move(ret));
            funcType.isVariadic = t->isVariadic;
            return funcType;
        }

        return TypeRef::MakeUnknown();
    }

    TypeRef ResolveTypeWithSubstitution(const TypeExpr &expr,
                                        const std::unordered_map<std::string, TypeRef> &substitutions) override {
        if (auto *t = dynamic_cast<const NamedTypeExpr *>(&expr)) {
            if (t->typeArgs.empty()) {
                if (auto it = substitutions.find(t->name); it != substitutions.end()) {
                    return it->second;
                }
                return ResolveType(expr);
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
            TypeRef pointeeType = ResolveTypeWithSubstitution(*t->pointee, substitutions);
            pointeeType.isMut = pointeeType.isMut || t->pointeeMut;
            return TypeRef::MakePointer(std::move(pointeeType));
        }
        if (auto *t = dynamic_cast<const ArrayTypeExpr *>(&expr)) {
            return TypeRef::MakeArray(ResolveTypeWithSubstitution(*t->element, substitutions),
                                      t->size ? EvalArrayLength(*t->size) : std::nullopt);
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

    [[nodiscard]] const FuncDecl *LookupMethod(const TypeRef &receiverType, const std::string &methodName,
                                               const std::vector<TypeRef> &argTypes = {}) override {
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
            // Single overload: validate arity and assignability before
            // returning.
            const auto *decl = overloads[0];
            std::vector<TypeRef> paramTypes = ResolveMethodParamTypes(receiverType, *decl);
            if (paramTypes.size() != argTypes.size()) {
                return nullptr;
            }
            for (std::size_t i = 0; i < argTypes.size(); ++i) {
                if (argTypes[i].IsUnknown() || paramTypes[i].IsUnknown()) {
                    continue;
                }
                if (!argTypes[i].IsAssignableTo(paramTypes[i]) &&
                    !(argTypes[i].IsInteger() && paramTypes[i].IsInteger())) {
                    return nullptr;
                }
            }
            return decl;
        }
        for (const auto *decl : overloads) {
            std::vector<TypeRef> paramTypes = ResolveMethodParamTypes(receiverType, *decl);
            if (paramTypes.size() != argTypes.size()) {
                continue;
            }
            bool match = true;
            for (std::size_t i = 0; i < argTypes.size(); ++i) {
                if (!argTypes[i].IsUnknown() && !paramTypes[i].IsUnknown() &&
                    !argTypes[i].IsAssignableTo(paramTypes[i]) &&
                    !(argTypes[i].IsInteger() && paramTypes[i].IsInteger())) {
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

    std::unordered_map<std::string, TypeRef> MethodTypeSubstitutions(const TypeRef &receiverType) const override {
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

    TypeRef InstantiateAssociatedReceiver(TypeRef receiverType, const std::vector<TypeExprPtr> &typeArgs) override {
        const std::string typeName = NamedBaseTypeName(receiverType);
        const auto structIt = structDecls.find(typeName);
        if (structIt == structDecls.end() || structIt->second->typeParams.empty() || typeArgs.empty()) {
            return receiverType;
        }

        std::string name = typeName + "<";
        for (std::size_t i = 0; i < typeArgs.size(); ++i) {
            if (i) {
                name += ", ";
            }
            name += ResolveType(*typeArgs[i]).ToString();
        }
        name += ">";
        return TypeRef::MakeNamed(std::move(name));
    }

    TypeRef ResolveMethodReturnType(const TypeRef &receiverType, const FuncDecl &method) override {
        TypeRef savedSelfType = currentSelfType;
        currentSelfType =
            receiverType.kind == TypeRef::Kind::Pointer ? receiverType : TypeRef::MakePointer(receiverType);
        const auto substitutions = MethodTypeSubstitutions(receiverType);
        TypeRef ret = method.returnType ? ResolveTypeWithSubstitution(*method.returnType->get(), substitutions)
                                        : TypeRef::MakeOpaque();
        currentSelfType = savedSelfType;
        return ret;
    }

    std::vector<TypeRef> ResolveMethodParamTypes(const TypeRef &receiverType, const FuncDecl &method) override {
        TypeRef savedSelfType = currentSelfType;
        currentSelfType =
            receiverType.kind == TypeRef::Kind::Pointer ? receiverType : TypeRef::MakePointer(receiverType);
        std::vector<TypeRef> params;
        for (const auto &param : method.params) {
            if (param.isVariadic || param.name == "self") {
                continue;
            }
            params.push_back(ResolveTypeWithSubstitution(*param.type, MethodTypeSubstitutions(receiverType)));
        }
        currentSelfType = savedSelfType;
        return params;
    }

    const FuncDecl *LookupOperatorMethod(const TypeRef &receiverType, const std::string &operatorName,
                                         const std::vector<TypeRef> &argumentTypes) override {
        return LookupMethod(receiverType, operatorName, argumentTypes);
    }

    std::vector<TypeRef> ResolveOperatorParameterTypes(const TypeRef &receiverType, const FuncDecl &method) override {
        return ResolveMethodParamTypes(receiverType, method);
    }

    TypeRef ResolveOperatorReturnType(const TypeRef &receiverType, const FuncDecl &method) override {
        return ResolveMethodReturnType(receiverType, method);
    }

    TypeRef AssociatedFunctionType(const TypeRef &receiverType, const FuncDecl &method) {
        TypeRef savedSelfType = currentSelfType;
        currentSelfType =
            receiverType.kind == TypeRef::Kind::Pointer ? receiverType : TypeRef::MakePointer(receiverType);
        TypeRef type = MakeFuncTypeWithSubstitution(method.params, method.returnType,
                                                    MethodTypeSubstitutions(receiverType), method.typeParams);
        currentSelfType = savedSelfType;
        return type;
    }

    [[nodiscard]] const FuncDecl *LookupInterfaceMethod(const TypeRef &receiverType,
                                                        const std::string &methodName) const override {
        const std::string ifaceName = NamedBaseTypeName(receiverType);
        if (ifaceName.empty()) {
            return nullptr;
        }
        const auto ifaceIt = interfaceDecls.find(ifaceName);
        if (ifaceIt == interfaceDecls.end()) {
            return nullptr;
        }
        for (const auto &method : ifaceIt->second->methods) {
            if (method->name == methodName) {
                return method.get();
            }
        }
        return nullptr;
    }

    TypeRef ResolveInterfaceMethodReturnType(const FuncDecl &method) override {
        return method.returnType ? ResolveType(*method.returnType->get()) : TypeRef::MakeOpaque();
    }

    std::vector<TypeRef> ResolveInterfaceMethodParamTypes(const FuncDecl &method) override {
        std::vector<TypeRef> params;
        for (const auto &param : method.params) {
            if (param.isVariadic) {
                continue;
            }
            params.push_back(ResolveType(*param.type));
        }
        return params;
    }

    const FuncDecl *LookupFunctionOverload(const Symbol &sym, const std::vector<TypeRef> &argTypes,
                                           const std::vector<TypeExprPtr> &typeArgs = {}) override {
        if (sym.kind != Symbol::Kind::Func || sym.funcOverloads.empty()) {
            return nullptr;
        }
        if (sym.funcOverloads.size() == 1) {
            // Single overload: still validate arity and assignability so
            // that Bar(wrongType) against a lone Bar(int32) returns null
            // and lets the caller emit a proper diagnostic.
            const auto *decl = sym.funcOverloads[0];
            std::unordered_map<std::string, TypeRef> substitutions;
            const std::size_t count = std::min(decl->typeParams.size(), typeArgs.size());
            for (std::size_t i = 0; i < count; ++i) {
                substitutions.emplace(decl->typeParams[i], ResolveType(*typeArgs[i]));
            }
            TypeRef funcType =
                MakeFuncTypeWithSubstitution(decl->params, decl->returnType, substitutions, decl->typeParams);
            if (funcType.kind != TypeRef::Kind::Func || funcType.inner.empty()) {
                return decl;
            }
            const std::size_t paramCount = funcType.inner.size() - 1;
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
                if (argTypes[i].IsUnknown() || funcType.inner[i].IsUnknown()) {
                    continue;
                }
                if (!argTypes[i].IsAssignableTo(funcType.inner[i]) &&
                    !(argTypes[i].IsInteger() && funcType.inner[i].IsInteger())) {
                    return nullptr;
                }
            }
            return decl;
        }
        for (const bool allowVariadic : {false, true}) {
            for (const bool exactOnly : {true, false}) {
                for (const auto *decl : sym.funcOverloads) {
                    std::unordered_map<std::string, TypeRef> substitutions;
                    const std::size_t count = std::min(decl->typeParams.size(), typeArgs.size());
                    for (std::size_t i = 0; i < count; ++i) {
                        substitutions.emplace(decl->typeParams[i], ResolveType(*typeArgs[i]));
                    }
                    TypeRef funcType =
                        MakeFuncTypeWithSubstitution(decl->params, decl->returnType, substitutions, decl->typeParams);
                    if (funcType.kind != TypeRef::Kind::Func || funcType.inner.empty()) {
                        continue;
                    }
                    const std::size_t paramCount = funcType.inner.size() - 1;
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
                        const TypeRef &paramType = funcType.inner[i];
                        if (argTypes[i].IsUnknown() || paramType.IsUnknown()) {
                            continue;
                        }
                        if (exactOnly ? !(argTypes[i] == paramType) : !argTypes[i].IsAssignableTo(paramType)) {
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

    TypeRef FunctionType(const FuncDecl &decl) {
        return MakeFuncType(decl.params, decl.returnType, decl.typeParams);
    }

    static std::optional<std::uint64_t> CheckedAlignUp(const std::uint64_t value, const std::uint64_t alignment) {
        if (alignment == 0 || value > std::numeric_limits<std::uint64_t>::max() - (alignment - 1)) {
            return std::nullopt;
        }
        return AlignUp(value, alignment);
    }

    std::optional<ResolvedTypeLayout>
    LayoutOfTypeRef(const TypeRef &inputType, const std::unordered_map<std::string, TypeRef> &substitutions = {}) {
        if ((inputType.kind == TypeRef::Kind::Named || inputType.kind == TypeRef::Kind::TypeParam) &&
            substitutions.contains(inputType.name) &&
            substitutions.at(inputType.name).ToString() != inputType.ToString()) {
            return LayoutOfTypeRef(substitutions.at(inputType.name), substitutions);
        }

        const std::string key = inputType.ToString();
        if (const auto known = typeLayouts.find(key); known != typeLayouts.end()) {
            return known->second;
        }
        if (!activeLayoutTypes.insert(key).second) {
            return std::nullopt;
        }

        const auto finish = [&](std::optional<ResolvedTypeLayout> result) {
            activeLayoutTypes.erase(key);
            if (result) {
                typeLayouts.insert_or_assign(key, *result);
            }
            return result;
        };
        const auto checkedAdd = [](const std::uint64_t left,
                                   const std::uint64_t right) -> std::optional<std::uint64_t> {
            if (right > std::numeric_limits<std::uint64_t>::max() - left) {
                return std::nullopt;
            }
            return left + right;
        };
        const auto checkedMultiply = [](const std::uint64_t left,
                                        const std::uint64_t right) -> std::optional<std::uint64_t> {
            if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
                return std::nullopt;
            }
            return left * right;
        };

        if (inputType.kind == TypeRef::Kind::Named) {
            if (inputType.name.starts_with("Slice<") || inputType.name == "Slice") {
                return finish(ResolvedTypeLayout{16, 8});
            }

            const std::string baseName = BaseTypeName(inputType.name);
            std::unordered_map<std::string, TypeRef> localSubs = substitutions;
            const std::vector<TypeRef> typeArgs = ParseTypeArgsFromTypeName(inputType.name);
            if (const auto structure = structDecls.find(baseName); structure != structDecls.end()) {
                const std::size_t count = std::min(structure->second->typeParams.size(), typeArgs.size());
                for (std::size_t i = 0; i < count; ++i) {
                    localSubs[structure->second->typeParams[i]] = typeArgs[i];
                }
                return finish(LayoutOfStruct(*structure->second, localSubs));
            }
            if (const auto enumeration = enumDecls.find(baseName); enumeration != enumDecls.end()) {
                const std::size_t count = std::min(enumeration->second->typeParams.size(), typeArgs.size());
                for (std::size_t i = 0; i < count; ++i) {
                    localSubs[enumeration->second->typeParams[i]] = typeArgs[i];
                }
                return finish(LayoutOfEnum(*enumeration->second, localSubs));
            }
            if (const auto unionType = unionDecls.find(baseName); unionType != unionDecls.end()) {
                return finish(LayoutOfUnion(*unionType->second, localSubs));
            }

            // Interface values are fat pointers: {data, vtable}.
            if (Symbol *sym = currentScope->Lookup(baseName); sym) {
                if (sym->kind == Symbol::Kind::Interface) {
                    return finish(ResolvedTypeLayout{16, 8});
                }
                if (sym->kind == Symbol::Kind::Type && !sym->type.IsUnknown() && !(sym->type == inputType)) {
                    return finish(LayoutOfTypeRef(sym->type, localSubs));
                }
            }
            if (!inputType.inner.empty()) {
                return finish(LayoutOfTypeRef(inputType.inner[0], localSubs));
            }
            return finish(std::nullopt);
        }

        if (inputType.kind == TypeRef::Kind::Tuple) {
            std::uint64_t offset = 0;
            std::uint64_t alignment = 1;
            for (const TypeRef &element : inputType.inner) {
                const auto elementLayout = LayoutOfTypeRef(element, substitutions);
                if (!elementLayout) {
                    return finish(std::nullopt);
                }
                const auto alignedOffset = CheckedAlignUp(offset, elementLayout->alignment);
                if (!alignedOffset) {
                    return finish(std::nullopt);
                }
                offset = *alignedOffset;
                const auto end = checkedAdd(offset, elementLayout->size > 0 ? elementLayout->size : 8);
                if (!end) {
                    return finish(std::nullopt);
                }
                offset = *end;
                alignment = std::max(alignment, elementLayout->alignment);
            }
            const auto size = CheckedAlignUp(offset, alignment);
            return finish(size ? std::optional(ResolvedTypeLayout{*size, alignment}) : std::nullopt);
        }

        if (inputType.kind == TypeRef::Kind::Array) {
            if (inputType.inner.empty() || !inputType.arrayLength) {
                return finish(std::nullopt);
            }
            const auto elementLayout = LayoutOfTypeRef(inputType.inner[0], substitutions);
            if (!elementLayout) {
                return finish(std::nullopt);
            }
            const auto size = checkedMultiply(elementLayout->size, *inputType.arrayLength);
            return finish(size ? std::optional(ResolvedTypeLayout{*size, elementLayout->alignment}) : std::nullopt);
        }

        if (inputType.IsRange()) {
            if (inputType.kind == TypeRef::Kind::RangeFull) {
                return finish(ResolvedTypeLayout{0, 1});
            }
            if (inputType.inner.empty()) {
                return finish(std::nullopt);
            }
            const auto elementLayout = LayoutOfTypeRef(inputType.inner[0], substitutions);
            if (!elementLayout || elementLayout->size == 0) {
                return finish(std::nullopt);
            }
            const std::uint64_t count =
                inputType.kind == TypeRef::Kind::Range || inputType.kind == TypeRef::Kind::RangeInclusive ? 2 : 1;
            const auto size = checkedMultiply(elementLayout->size, count);
            return finish(size ? std::optional(ResolvedTypeLayout{*size, elementLayout->alignment}) : std::nullopt);
        }

        const auto size = inputType.SizeInBytes();
        return finish(size ? std::optional(ResolvedTypeLayout{*size, Layout::FieldAlign(*size)}) : std::nullopt);
    }

    std::optional<ResolvedTypeLayout>
    LayoutOfFields(const std::vector<TypeRef> &fields,
                   const std::unordered_map<std::string, TypeRef> &substitutions = {}) {
        std::uint64_t offset = 0;
        std::uint64_t alignment = 1;
        for (const TypeRef &field : fields) {
            const auto fieldLayout = LayoutOfTypeRef(field, substitutions);
            if (!fieldLayout) {
                return std::nullopt;
            }
            const auto alignedOffset = CheckedAlignUp(offset, fieldLayout->alignment);
            if (!alignedOffset) {
                return std::nullopt;
            }
            offset = *alignedOffset;
            if (fieldLayout->size > std::numeric_limits<std::uint64_t>::max() - offset) {
                return std::nullopt;
            }
            offset += fieldLayout->size > 0 ? fieldLayout->size : 8;
            alignment = std::max(alignment, fieldLayout->alignment);
        }
        const auto size = CheckedAlignUp(offset, alignment);
        return size ? std::optional(ResolvedTypeLayout{*size, alignment}) : std::nullopt;
    }

    std::optional<ResolvedTypeLayout> LayoutOfEnum(const EnumDecl &decl,
                                                   const std::unordered_map<std::string, TypeRef> &substitutions = {}) {
        const auto tagLayout = LayoutOfTypeRef(EnumBaseType(decl), substitutions);
        if (!tagLayout) {
            return std::nullopt;
        }

        bool hasPayload = false;
        ResolvedTypeLayout maximumPayload;
        for (const auto &variant : decl.variants) {
            std::vector<TypeRef> fields;
            fields.reserve(variant.fields.size() + variant.namedFields.size());
            for (const auto &field : variant.fields) {
                fields.push_back(ResolveTypeWithSubstitution(*field, substitutions));
            }
            for (const auto &field : variant.namedFields) {
                fields.push_back(ResolveTypeWithSubstitution(*field.type, substitutions));
            }
            if (fields.empty()) {
                continue;
            }
            hasPayload = true;
            const auto payload = LayoutOfFields(fields, substitutions);
            if (!payload) {
                return std::nullopt;
            }
            maximumPayload.size = std::max(maximumPayload.size, payload->size);
            maximumPayload.alignment = std::max(maximumPayload.alignment, payload->alignment);
        }

        if (!hasPayload) {
            return tagLayout;
        }
        const std::uint64_t alignment = std::max(tagLayout->alignment, maximumPayload.alignment);
        const auto payloadOffset = CheckedAlignUp(tagLayout->size, maximumPayload.alignment);
        if (!payloadOffset || maximumPayload.size > std::numeric_limits<std::uint64_t>::max() - *payloadOffset) {
            return std::nullopt;
        }
        const auto size = CheckedAlignUp(*payloadOffset + maximumPayload.size, alignment);
        return size ? std::optional(ResolvedTypeLayout{*size, alignment}) : std::nullopt;
    }

    TypeRef EnumBaseType(const EnumDecl &decl) {
        return decl.baseType ? ResolveType(*decl.baseType) : TypeRef::MakeInt();
    }

    TypeRef EnumType(const EnumDecl &decl, const std::vector<TypeRef> &typeArgs = {}) {
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
            std::unordered_map<std::string, TypeRef> substitutions;
            for (std::size_t i = 0; i < typeArgs.size(); ++i) {
                substitutions.emplace(decl.typeParams[i], typeArgs[i]);
            }
            if (const auto layout = LayoutOfEnum(decl, substitutions)) {
                type.inner.push_back(TypeRef::MakeArray(TypeRef::MakeChar8(), layout->size));
            }
        }
        return type;
    }

    std::optional<ResolvedTypeLayout>
    LayoutOfStruct(const StructDecl &decl, const std::unordered_map<std::string, TypeRef> &substitutions = {}) {
        std::uint64_t offset = 0;
        std::uint64_t maxAlign = 1;
        for (const auto &field : decl.fields) {
            const TypeRef fieldType = ResolveTypeWithSubstitution(*field.type, substitutions);
            std::optional<ResolvedTypeLayout> fieldLayout;
            if (fieldType.kind == TypeRef::Kind::Array && !fieldType.arrayLength && !fieldType.inner.empty()) {
                const auto elementLayout = LayoutOfTypeRef(fieldType.inner[0], substitutions);
                if (elementLayout) {
                    fieldLayout = ResolvedTypeLayout{0, elementLayout->alignment};
                }
            }
            else {
                fieldLayout = LayoutOfTypeRef(fieldType, substitutions);
            }
            if (!fieldLayout) {
                return std::nullopt;
            }
            const auto alignedOffset = CheckedAlignUp(offset, fieldLayout->alignment);
            if (!alignedOffset) {
                return std::nullopt;
            }
            offset = *alignedOffset;
            if (fieldLayout->size > std::numeric_limits<std::uint64_t>::max() - offset) {
                return std::nullopt;
            }
            offset += fieldLayout->size;
            maxAlign = std::max(maxAlign, fieldLayout->alignment);
        }
        const auto size = CheckedAlignUp(offset, maxAlign);
        return size ? std::optional(ResolvedTypeLayout{*size, maxAlign}) : std::nullopt;
    }

    std::optional<ResolvedTypeLayout>
    LayoutOfUnion(const UnionDecl &decl, const std::unordered_map<std::string, TypeRef> &substitutions = {}) {
        std::uint64_t size = 0;
        std::uint64_t alignment = 1;
        for (const auto &field : decl.fields) {
            const TypeRef fieldType = ResolveTypeWithSubstitution(*field.type, substitutions);
            const auto fieldLayout = LayoutOfTypeRef(fieldType, substitutions);
            if (!fieldLayout) {
                return std::nullopt;
            }
            size = std::max(size, fieldLayout->size);
            alignment = std::max(alignment, fieldLayout->alignment);
        }
        const auto alignedSize = CheckedAlignUp(size, alignment);
        return alignedSize ? std::optional(ResolvedTypeLayout{*alignedSize, alignment}) : std::nullopt;
    }

    std::optional<ResolvedTypeLayout>
    LayoutOfTypeExpr(const TypeExpr &expr, const std::unordered_map<std::string, TypeRef> &substitutions = {}) {
        return LayoutOfTypeRef(ResolveTypeWithSubstitution(expr, substitutions), substitutions);
    }

    void RecordResolvedTypeLayouts() override {
        std::vector<TypeRef> resolvedTypes;
        resolvedTypes.reserve(typeNodeTypes.size() + expressionTypes.size() + patternTypes.size());
        for (const auto &[_, type] : typeNodeTypes) {
            resolvedTypes.push_back(type);
        }
        for (const auto &[_, type] : expressionTypes) {
            resolvedTypes.push_back(type);
        }
        for (const auto &[_, type] : patternTypes) {
            resolvedTypes.push_back(type);
        }
        for (const auto &[_, binding] : callableBindings) {
            for (const auto &[__, type] : binding.substitutions) {
                resolvedTypes.push_back(type);
            }
            if (binding.receiverType) {
                resolvedTypes.push_back(*binding.receiverType);
            }
        }
        for (const TypeRef &type : resolvedTypes) {
            LayoutOfTypeRef(type);
        }
    }

    std::optional<FunctionSignature> ResolveFunctionSignature(const FuncDecl &decl, const bool isMethod) {
        auto savedTypeParams = currentTypeParams;
        if (!isMethod) {
            currentTypeParams.clear();
        }
        currentTypeParams.insert(currentTypeParams.end(), decl.typeParams.begin(), decl.typeParams.end());

        std::unordered_map<std::string, TypeRef> substitutions;
        for (std::size_t i = 0; i < decl.typeParams.size(); ++i) {
            substitutions.emplace(decl.typeParams[i], TypeRef::MakeTypeParam(std::format("${}", i)));
        }

        FunctionSignature signature;
        signature.typeParamCount = decl.typeParams.size();
        for (const auto &param : decl.params) {
            if (isMethod && param.name == "self") {
                continue;
            }
            TypeRef type = ResolveTypeWithSubstitution(*param.type, substitutions);
            if (type.IsUnknown()) {
                currentTypeParams = savedTypeParams;
                return std::nullopt;
            }
            signature.paramTypes.push_back(std::move(type));
            signature.variadicParams.push_back(param.isVariadic);
        }

        currentTypeParams = savedTypeParams;
        return signature;
    }

    static bool SameFunctionSignature(const FunctionSignature &lhs, const FunctionSignature &rhs) {
        return lhs.typeParamCount == rhs.typeParamCount && lhs.paramTypes == rhs.paramTypes &&
               lhs.variadicParams == rhs.variadicParams;
    }

    void ValidateFunctionSignature(const FuncDecl &decl, const std::vector<const FuncDecl *> &overloads,
                                   const bool isMethod = false) {
        const auto signature = ResolveFunctionSignature(decl, isMethod);
        if (!signature) {
            return;
        }

        for (const FuncDecl *previous : overloads) {
            if (previous == &decl) {
                break;
            }
            const auto previousSignature = ResolveFunctionSignature(*previous, isMethod);
            if (previousSignature && SameFunctionSignature(*signature, *previousSignature)) {
                EmitError(decl.location,
                          std::format("function '{}' has the same parameter signature as a previous declaration at "
                                      "{}:{}",
                                      decl.name, previous->location.line, previous->location.column));
                return;
            }
        }
    }

    // Second pass: check declarations
    void CheckModule(const Module &mod) override {
        currentFile = mod.name;
        for (const auto &decl : mod.items) {
            CheckDecl(*decl);
        }
    }

    void CheckModuleInScope(const Module &mod, Scope &scope) override {
        Scope *savedScope = currentScope;
        currentScope = &scope;
        CheckModule(mod);
        currentScope = savedScope;
    }

    void CheckDecl(const Decl &decl) override {
        if (auto *fn = dynamic_cast<const FuncDecl *>(&decl)) {
            if (const Symbol *symbol = currentScope->LookupLocal(fn->name);
                symbol && symbol->kind == Symbol::Kind::Func) {
                ValidateFunctionSignature(*fn, symbol->funcOverloads);
            }
            CheckFuncDecl(*fn);
        }
        else if (auto *structDecl = dynamic_cast<const StructDecl *>(&decl)) {
            CheckStructDecl(*structDecl);
        }
        else if (auto *enumDecl = dynamic_cast<const EnumDecl *>(&decl)) {
            CheckEnumDecl(*enumDecl);
        }
        else if (auto *unionDecl = dynamic_cast<const UnionDecl *>(&decl)) {
            CheckUnionDecl(*unionDecl);
        }
        else if (auto *ifaceDecl = dynamic_cast<const InterfaceDecl *>(&decl)) {
            CheckInterfaceDecl(*ifaceDecl);
        }
        else if (auto *implDecl = dynamic_cast<const ImplDecl *>(&decl)) {
            CheckImplDecl(*implDecl);
        }
        else if (auto *modDecl = dynamic_cast<const ModuleDecl *>(&decl)) {
            CheckModuleDecl(*modDecl);
        }
        else if (auto *constDecl = dynamic_cast<const ConstDecl *>(&decl)) {
            CheckConstDecl(*constDecl);
        }
        else if (auto *aliasDecl = dynamic_cast<const TypeAliasDecl *>(&decl)) {
            ValidateArrayType(*aliasDecl->type);
            ResolveType(*aliasDecl->type); // triggers unknown-type errors
        }
        else if (auto *externFn = dynamic_cast<const ExternFuncDecl *>(&decl)) {
            if (externFn->dll.empty()) {
                EmitError(externFn->location, std::format("extern function '{}' must specify a "
                                                          "source DLL via "
                                                          "#Link(\"dll.dll\")",
                                                          externFn->name));
            }
            if (externFn->returnType) {
                ValidateArrayType(*externFn->returnType->get());
                ResolveType(*externFn->returnType->get());
            }
            for (auto &p : externFn->params) {
                if (!p.isVariadic) {
                    ValidateArrayType(*p.type);
                    ResolveType(*p.type);
                }
            }
        }
        else if (auto *externVar = dynamic_cast<const ExternVarDecl *>(&decl)) {
            ValidateArrayType(*externVar->type);
            ResolveType(*externVar->type);
        }
        else if (auto *externBlock = dynamic_cast<const ExternBlockDecl *>(&decl)) {
            for (auto &item : externBlock->items) {
                CheckDecl(*item);
            }
        }
        else if (auto *useDecl = dynamic_cast<const UseDecl *>(&decl)) {
            CheckUseDecl(*useDecl);
        }
    }

    void CheckFuncDecl(const FuncDecl &d, bool isMethod = false) {
        auto savedTypeParams = currentTypeParams;
        const FuncDecl *savedFunctionDecl = currentFunctionDecl;
        currentFunctionDecl = &d;
        if (!isMethod) {
            currentTypeParams.clear();
        }
        currentTypeParams.insert(currentTypeParams.end(), d.typeParams.begin(), d.typeParams.end());

        if (d.returnType) {
            ValidateArrayType(*d.returnType->get());
        }
        TypeRef retType = d.returnType ? ResolveType(*d.returnType->get()) : TypeRef::MakeOpaque();

        auto savedRet = currentReturnType;
        currentReturnType = retType;
        const bool savedNoReturn = currentFunctionNoReturn;
        currentFunctionNoReturn = d.isNoReturn;

        PushScope();

        for (const auto &tp : d.typeParams) {
            Symbol sym;
            sym.kind = Symbol::Kind::Type;
            sym.name = tp;
            sym.type = TypeRef::MakeTypeParam(tp);
            Define(sym);
        }

        if (isMethod) {
            Symbol self;
            self.kind = Symbol::Kind::Var;
            self.name = "self";
            self.type = currentSelfType.IsUnknown() ? TypeRef::MakeNamed("self") : currentSelfType;
            self.isMut = true;
            Define(self);
        }

        bool seenDefault = false;
        for (const auto &param : d.params) {
            if (param.name == "self") {
                ResolveType(*param.type);
                continue;
            }
            ValidateArrayType(*param.type);
            if (param.isVariadic) {
                seenDefault = false; // variadic ends fixed params; reset
            }
            else if (param.defaultValue) {
                seenDefault = true;
            }
            else if (seenDefault) {
                EmitError(param.location, std::format("parameter '{}' without a default "
                                                      "value cannot follow a "
                                                      "parameter with a default value",
                                                      param.name));
            }
            Symbol sym;
            sym.kind = Symbol::Kind::Var;
            sym.name = param.name;
            sym.location = param.location;
            sym.type = param.isVariadic ? TypeRef::MakeNamed(SliceTypeName(ResolveType(*param.type)))
                                        : ResolveType(*param.type);
            sym.isMut = param.isMut;
            Define(sym);
            if (param.defaultValue) {
                TypeRef paramType = ResolveType(*param.type);
                TypeRef defaultType = CheckExpr(**param.defaultValue);
                if (!defaultType.IsUnknown() && !paramType.IsUnknown() &&
                    !CanAssignExprTo(**param.defaultValue, defaultType, paramType)) {
                    EmitError(param.location,
                              AssignmentErrorMessage(**param.defaultValue, paramType,
                                                     std::format("default value type '{}' does not "
                                                                 "match parameter type '{}'",
                                                                 defaultType.ToString(), paramType.ToString())));
                }
            }
        }

        if (d.isAsm) {
            // An asm function's body is raw machine instructions, not Rux
            // statements, so it is validated when the assembler encodes it —
            // except for the one thing the assembler for the target cannot
            // say, which is that the body was written for the other one.
            CheckAsmBodyArchitecture(d);
        }
        else if (!d.body) {
            if (d.intrinsicName.empty()) {
                EmitError(d.location, std::format("function '{}' has no body", d.name));
            }
        }
        else {
            CheckBlock(*d.body);
        }

        PopScope();
        currentReturnType = savedRet;
        currentFunctionNoReturn = savedNoReturn;
        currentTypeParams = savedTypeParams;
        currentFunctionDecl = savedFunctionDecl;
    }

    // An `asm func` body is written for one architecture, and nothing in the
    // syntax says which: the mnemonics do. Report the first instruction that
    // names an instruction of the architecture the compilation is not for,
    // which is the whole body's mistake rather than that one line's.
    //
    // This runs after `when` folding, so a body a build never reaches is never
    // reported, and it stops at the first offender so a body written for the
    // wrong machine costs one diagnostic rather than one per line. A body that
    // reaches this far is one the build needs and no assembler can encode, so
    // it is an error: `when #target.arch` is how a function written twice
    // reaches both machines, and every first-party body uses it.
    void CheckAsmBodyArchitecture(const FuncDecl &d) const {
        const Target::Arch target = context.target.arch;
        for (const auto &instr : d.asmBody) {
            if (instr.mnemonic.empty()) {
                continue; // a label definition
            }
            const Target::Arch mnemonicArch = AsmMnemonicArch(instr.mnemonic);
            if (mnemonicArch == Target::Arch::Unknown || mnemonicArch == target) {
                continue;
            }
            EmitError(instr.location,
                      std::format("'{}' is an {} instruction, but asm func '{}' is compiled for {}", instr.mnemonic,
                                  Target::ToDisplayString(mnemonicArch), d.name, Target::ToDisplayString(target)));
            return;
        }
    }

    void CheckStructDecl(const StructDecl &d) {
        auto savedTypeParams = currentTypeParams;
        currentTypeParams = d.typeParams;

        PushScope();
        for (const auto &tp : d.typeParams) {
            Symbol sym;
            sym.kind = Symbol::Kind::Type;
            sym.name = tp;
            sym.type = TypeRef::MakeTypeParam(tp);
            Define(sym);
        }

        std::unordered_set<std::string> seen;
        for (std::size_t i = 0; i < d.fields.size(); ++i) {
            const auto &field = d.fields[i];
            if (!seen.insert(field.name).second) {
                EmitError(field.location, std::format("duplicate field '{}' in struct '{}'", field.name, d.name));
            }
            const auto *array = dynamic_cast<const ArrayTypeExpr *>(field.type.get());
            const bool isFlexibleTail = array && !array->size && i + 1 == d.fields.size();
            ValidateArrayType(*field.type, isFlexibleTail);
            ResolveType(*field.type);
        }

        PopScope();
        currentTypeParams = savedTypeParams;
    }

    void CheckStructInitExpr(const StructInitExpr &e) {
        auto structIt = structDecls.find(e.typeName);
        if (structIt == structDecls.end()) {
            if (const auto [enumDecl, variant] = LookupEnumVariantInitializer(e.typeName); enumDecl) {
                if (!variant) {
                    EmitError(e.location, std::format("unknown enum variant '{}' in initializer", e.typeName));
                    for (const auto &f : e.fields) {
                        CheckExpr(*f.value);
                    }
                    return;
                }
                if (variant->namedFields.empty()) {
                    EmitError(e.location, std::format("enum variant '{}' has no named fields", e.typeName));
                    for (const auto &f : e.fields) {
                        CheckExpr(*f.value);
                    }
                    return;
                }

                std::unordered_map<std::string, const EnumDecl::Variant::NamedField *> fieldMap;
                for (const auto &field : variant->namedFields) {
                    fieldMap.emplace(field.name, &field);
                }

                std::unordered_set<std::string> initialized;
                for (const auto &f : e.fields) {
                    TypeRef valueType = CheckExpr(*f.value);
                    if (!initialized.insert(f.name).second) {
                        EmitError(f.location, std::format("duplicate field '{}' in "
                                                          "initializer for '{}'",
                                                          f.name, e.typeName));
                        continue;
                    }

                    auto fieldIt = fieldMap.find(f.name);
                    if (fieldIt == fieldMap.end()) {
                        EmitError(f.location, std::format("unknown field '{}' in "
                                                          "initializer for '{}'",
                                                          f.name, e.typeName));
                        continue;
                    }

                    TypeRef fieldType = ResolveType(*fieldIt->second->type);
                    if (!valueType.IsUnknown() && !fieldType.IsUnknown() &&
                        !CanAssignExprTo(*f.value, valueType, fieldType)) {
                        EmitError(f.location, AssignmentErrorMessage(*f.value, fieldType,
                                                                     std::format("cannot assign '{}' to "
                                                                                 "field '{}' of type '{}'",
                                                                                 valueType.ToString(), f.name,
                                                                                 fieldType.ToString())));
                    }
                }

                for (const auto &field : variant->namedFields) {
                    if (!initialized.contains(field.name)) {
                        EmitError(e.location, std::format("missing field '{}' in "
                                                          "initializer for '{}'",
                                                          field.name, e.typeName));
                    }
                }
                return;
            }

            EmitError(e.location, std::format("unknown type '{}' in struct initializer", e.typeName));
            for (const auto &f : e.fields) {
                CheckExpr(*f.value);
            }
            return;
        }

        const StructDecl &decl = *structIt->second;
        if (e.typeArgs.size() != decl.typeParams.size()) {
            EmitError(e.location, std::format("struct '{}' expects {} type argument(s), got {}", e.typeName,
                                              decl.typeParams.size(), e.typeArgs.size()));
        }

        const auto substitutions = StructTypeSubstitutions(decl, e.typeArgs);
        std::unordered_map<std::string, const StructDecl::Field *> fieldMap;
        for (const auto &field : decl.fields) {
            fieldMap.emplace(field.name, &field);
        }

        std::unordered_set<std::string> initialized;
        for (const auto &f : e.fields) {
            TypeRef valueType = CheckExpr(*f.value);
            if (!initialized.insert(f.name).second) {
                EmitError(f.location, std::format("duplicate field '{}' in initializer for '{}'", f.name, e.typeName));
                continue;
            }

            auto fieldIt = fieldMap.find(f.name);
            if (fieldIt == fieldMap.end()) {
                EmitError(f.location, std::format("unknown field '{}' in initializer for '{}'", f.name, e.typeName));
                continue;
            }

            TypeRef fieldType = ResolveTypeWithSubstitution(*fieldIt->second->type, substitutions);
            if (!valueType.IsUnknown() && !fieldType.IsUnknown() && !CanAssignExprTo(*f.value, valueType, fieldType)) {
                EmitError(f.location,
                          AssignmentErrorMessage(*f.value, fieldType,
                                                 std::format("cannot assign '{}' to field '{}' of type '{}'",
                                                             valueType.ToString(), f.name, fieldType.ToString())));
            }
        }

        for (const auto &field : decl.fields) {
            if (!initialized.contains(field.name)) {
                EmitError(e.location,
                          std::format("missing field '{}' in initializer for '{}'", field.name, e.typeName));
            }
        }
    }

    void CheckEnumDecl(const EnumDecl &d) {
        const auto savedTypeParams = currentTypeParams;
        currentTypeParams.insert(currentTypeParams.end(), d.typeParams.begin(), d.typeParams.end());
        const TypeRef baseType = EnumBaseType(d);
        if (!baseType.IsUnknown() && !baseType.IsInteger()) {
            EmitError(d.location, std::format("enum '{}' base type must be an integer type", d.name));
        }
        std::unordered_set<std::string> seen;
        for (const auto &variant : d.variants) {
            if (!seen.insert(variant.name).second) {
                EmitError(variant.location, std::format("duplicate variant '{}' in enum '{}'", variant.name, d.name));
            }
            if (variant.discriminant && (!variant.fields.empty() || !variant.namedFields.empty())) {
                EmitError(variant.location, std::format("enum variant '{}::{}' cannot have "
                                                        "both fields and a discriminant",
                                                        d.name, variant.name));
            }
            for (const auto &f : variant.fields) {
                ValidateArrayType(*f);
                ResolveType(*f);
            }
            std::unordered_set<std::string> namedFields;
            for (const auto &f : variant.namedFields) {
                if (!namedFields.insert(f.name).second) {
                    EmitError(f.location, std::format("duplicate field '{}' in enum variant '{}::{}'", f.name, d.name,
                                                      variant.name));
                }
                ValidateArrayType(*f.type);
                ResolveType(*f.type);
            }
        }
        currentTypeParams = savedTypeParams;
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

    void CheckUnionDecl(const UnionDecl &d) {
        std::unordered_set<std::string> seen;
        for (const auto &field : d.fields) {
            if (!seen.insert(field.name).second) {
                EmitError(field.location, std::format("duplicate field '{}' in union '{}'", field.name, d.name));
            }
            ValidateArrayType(*field.type);
            ResolveType(*field.type);
        }
    }

    void CheckInterfaceDecl(const InterfaceDecl &d) {
        std::unordered_set<std::string> seen;
        for (const auto &method : d.methods) {
            if (!seen.insert(method->name).second) {
                EmitError(method->location,
                          std::format("duplicate method '{}' in interface '{}'", method->name, d.name));
            }
            if (method->returnType) {
                ResolveType(**method->returnType);
            }
            for (const auto &p : method->params) {
                if (!p.isVariadic) {
                    ResolveType(*p.type);
                }
            }
        }
    }

    void CheckImplDecl(const ImplDecl &d) {
        const auto savedTypeParams = currentTypeParams;
        currentTypeParams = ImplTypeParams(d);

        // A compound receiver (e.g. `int[]`) resolves through the type
        // expression rather than a named symbol.
        if (d.extendedType) {
            ValidateArrayType(*d.extendedType);
        }
        TypeRef extendedType = d.extendedType ? ResolveType(*d.extendedType) : TypeRef::MakeUnknown();
        const bool isSliceReceiver =
            extendedType.kind == TypeRef::Kind::Array ||
            (extendedType.kind == TypeRef::Kind::Named && extendedType.name.starts_with("Slice<"));
        const std::string typeName = d.typeName.starts_with("Slice<") ? d.typeName : BaseTypeName(d.typeName);
        if (!isSliceReceiver && !currentScope->Lookup(typeName)) {
            EmitError(d.location, std::format("extend for unknown type '{}'", d.typeName));
        }

        if (d.interfaceName) {
            Symbol *ifaceSym = currentScope->Lookup(*d.interfaceName);
            if (!ifaceSym || ifaceSym->kind != Symbol::Kind::Interface) {
                EmitError(d.location, std::format("'{}' is not a known interface", *d.interfaceName));
            }
            else {
                std::unordered_set<std::string> implNames;
                for (const auto &m : d.methods) {
                    implNames.insert(m->name);
                }
                for (const auto &required : ifaceSym->interfaceMethods) {
                    if (!implNames.count(required)) {
                        EmitError(d.location, std::format("extend of '{}' for '{}' is "
                                                          "missing method '{}'",
                                                          *d.interfaceName, d.typeName, required));
                    }
                }
            }
        }

        bool savedInImpl = inImpl;
        TypeRef savedSelfType = currentSelfType;
        inImpl = true;
        if (isSliceReceiver) {
            // A slice is a fat pointer already; `self` is the slice value, so
            // `for x in self` and `self[i]` work directly.
            currentSelfType = extendedType;
        }
        else {
            TypeRef selfBase = extendedType.IsUnknown() ? TypeRef::MakeNamed(d.typeName) : extendedType;
            currentSelfType = TypeRef::MakePointer(selfBase);
        }
        for (const auto &m : d.methods) {
            if (const auto typeIt = methodsByType.find(typeName); typeIt != methodsByType.end()) {
                if (const auto methodIt = typeIt->second.find(m->name); methodIt != typeIt->second.end()) {
                    ValidateFunctionSignature(*m, methodIt->second, /*isMethod=*/true);
                }
            }
            CheckFuncDecl(*m, /*isMethod=*/true);
        }
        currentSelfType = savedSelfType;
        inImpl = savedInImpl;
        currentTypeParams = savedTypeParams;
    }

    void CheckModuleDecl(const ModuleDecl &d) {
        Scope *savedScope = currentScope;
        currentScope = &ModuleScopeFor(d.name, *currentScope);
        for (const auto &item : d.items) {
            CheckDecl(*item);
        }
        currentScope = savedScope;
    }

    static bool IsSliceTypeRef(const TypeRef &type) {
        return type.kind == TypeRef::Kind::Named && type.name.starts_with("Slice<");
    }

    // An element of a constant array must reduce to a literal, since the array
    // is laid out in read-only data rather than evaluated at each use.
    bool IsConstArrayElement(const Expr &e) const {
        if (dynamic_cast<const LiteralExpr *>(&e)) {
            return true;
        }
        if (const auto *u = dynamic_cast<const UnaryExpr *>(&e)) {
            return u->op == TokenKind::Minus && IsConstArrayElement(*u->operand);
        }
        if (const auto *ident = dynamic_cast<const IdentExpr *>(&e)) {
            const Symbol *sym = currentScope->Lookup(ident->name);
            return sym && sym->kind == Symbol::Kind::Const;
        }
        return false;
    }

    void CheckConstDecl(const ConstDecl &d) {
        if (!d.intrinsicName.empty()) {
            if (!d.type) {
                EmitError(d.location, std::format("'intrinsic' constant '{}' requires a type", d.name));
                return;
            }
            const TypeRef constType = ResolveType(**d.type);
            if (Symbol *sym = currentScope->Lookup(d.name)) {
                sym->type = constType;
                sym->intrinsicName = d.intrinsicName;
            }
            return;
        }
        if (!d.value) {
            EmitError(d.location, std::format("constant '{}' requires an initializer", d.name));
            return;
        }
        if (d.type) {
            ValidateArrayType(**d.type);
        }
        TypeRef valueType = CheckExpr(*d.value);
        TypeRef constType = d.type ? ResolveType(*d.type->get()) : valueType;
        if (d.type && !valueType.IsUnknown() && !constType.IsUnknown() &&
            !CanAssignExprTo(*d.value, valueType, constType)) {
            EmitError(d.value->location,
                      AssignmentErrorMessage(*d.value, constType,
                                             std::format("cannot assign '{}' to constant of type '{}'",
                                                         valueType.ToString(), constType.ToString())));
        }
        if (IsSliceTypeRef(constType) || constType.kind == TypeRef::Kind::Array) {
            const auto *array = dynamic_cast<const ArrayExpr *>(d.value.get());
            const bool isText = dynamic_cast<const LiteralExpr *>(d.value.get()) != nullptr;
            if (!isText && !array) {
                EmitError(d.value->location,
                          "a constant sequence must be initialized with an array literal or a string literal");
            }
            else if (array) {
                for (const auto &element : array->elements) {
                    if (!IsConstArrayElement(*element)) {
                        EmitError(element->location, "element of a constant array must be a literal or a "
                                                     "named constant");
                        break;
                    }
                }
            }
        }
        if (Symbol *sym = currentScope->Lookup(d.name)) {
            sym->type = constType;
        }
    }

    static std::string JoinPathSegments(const std::vector<std::string> &path, std::size_t first,
                                        std::size_t lastExclusive) {
        std::string result;
        for (std::size_t i = first; i < lastExclusive; ++i) {
            if (!result.empty()) {
                result += "::";
            }
            result += path[i];
        }
        return result;
    }

    static std::string ModulePathForImport(const UseDecl &d) {
        if (d.path.size() <= 1) {
            return "";
        }
        if (d.kind == UseDecl::Kind::Single) {
            if (d.path.size() <= 2) {
                return "";
            }
            return JoinPathSegments(d.path, 1, d.path.size() - 1);
        }
        return JoinPathSegments(d.path, 1, d.path.size());
    }

    static std::string LogicalModulePathForImport(const UseDecl &d) {
        if (d.kind == UseDecl::Kind::Single) {
            if (d.path.size() <= 1) {
                return "";
            }
            return JoinPathSegments(d.path, 0, d.path.size() - 1);
        }
        return JoinPathSegments(d.path, 0, d.path.size());
    }

    struct ImportScope {
        const std::unordered_map<std::string, Symbol> *table = nullptr;
        std::string displayName;
    };

    static std::string ImportScopeDisplayName(const std::string &pkgName, const std::string &modulePath) {
        if (modulePath.empty()) {
            return std::format("package '{}'", pkgName);
        }
        return std::format("module '{}'", modulePath);
    }

    ImportScope ResolveImportScope(const UseDecl &d, const std::string &pkgName, const std::string &modulePath) {
        const std::string logicalModulePath = LogicalModulePathForImport(d);
        if (auto pkgIt = packageModuleScopes.find(pkgName); pkgIt != packageModuleScopes.end()) {
            if (auto modIt = pkgIt->second.find(modulePath); modIt != pkgIt->second.end()) {
                return {&modIt->second->Table(), ImportScopeDisplayName(pkgName, modulePath)};
            }
        }

        Scope *matchedScope = nullptr;
        std::string matchedPackage;
        for (const auto &[candidatePackage, moduleScopes] : packageModuleScopes) {
            auto modIt = moduleScopes.find(logicalModulePath);
            if (modIt == moduleScopes.end()) {
                continue;
            }
            if (matchedScope && matchedScope != modIt->second) {
                EmitError(d.location, std::format("ambiguous module '{}'", logicalModulePath));
                return {};
            }
            matchedScope = modIt->second;
            matchedPackage = candidatePackage;
        }

        if (matchedScope) {
            return {&matchedScope->Table(), ImportScopeDisplayName(matchedPackage, logicalModulePath)};
        }

        if (!packageModuleScopes.contains(pkgName)) {
            EmitError(d.location, std::format("unknown package or module '{}'", pkgName));
        }
        else {
            EmitError(d.location, std::format("module '{}' not found in package '{}'", modulePath, pkgName));
        }
        return {};
    }

    void PromoteFromPackage(const UseDecl &d, const std::string &pkgName, const std::string &name) {
        const std::string modulePath = ModulePathForImport(d);
        ImportScope scope = ResolveImportScope(d, pkgName, modulePath);
        if (!scope.table) {
            return;
        }
        auto sym_it = scope.table->find(name);
        if (sym_it == scope.table->end()) {
            std::string message = std::format("'{}' not found in {}", name, scope.displayName);
            // The item is not at this path, but if one of the package's modules
            // holds it, point at the fully-qualified import.
            if (auto pkgIt = packageModuleScopes.find(pkgName); pkgIt != packageModuleScopes.end()) {
                for (const auto &[candidateModule, candidateScope] : pkgIt->second) {
                    if (!candidateModule.empty() && candidateScope->Table().contains(name)) {
                        message += std::format("; did you mean 'import {}::{}::{}'?", pkgName, candidateModule, name);
                        break;
                    }
                }
            }
            EmitError(d.location, std::move(message));
            return;
        }
        DefineImportedSymbol(sym_it->second);
        ImportSignatureDependencies(sym_it->second, *scope.table);
    }

    void DefineImportedSymbol(const Symbol &sym) {
        if (Symbol *existing = currentScope->LookupLocal(sym.name)) {
            if (existing->kind == sym.kind && existing->location.line == sym.location.line &&
                existing->location.column == sym.location.column) {
                *existing = sym;
                return;
            }
        }
        currentScope->Define(sym, diags, currentFile);
    }

    void ImportSignatureDependencies(const Symbol &sym, const std::unordered_map<std::string, Symbol> &sourceTable) {
        if (sym.kind != Symbol::Kind::Func) {
            return;
        }

        auto findPackageType = [&](const std::string &name) -> const Symbol * {
            auto sameSymbol = [](const Symbol &lhs, const Symbol &rhs) {
                return lhs.kind == rhs.kind && lhs.name == rhs.name && lhs.location.line == rhs.location.line &&
                       lhs.location.column == rhs.location.column;
            };

            const Symbol *matched = nullptr;
            for (const auto &[_, moduleScopes] : packageModuleScopes) {
                for (const auto &[__, scope] : moduleScopes) {
                    const auto &table = scope->Table();
                    auto it = table.find(name);
                    if (it == table.end()) {
                        continue;
                    }
                    if (it->second.kind != Symbol::Kind::Type && it->second.kind != Symbol::Kind::Interface) {
                        continue;
                    }
                    if (matched && !sameSymbol(*matched, it->second)) {
                        return nullptr;
                    }
                    matched = &it->second;
                }
            }
            return matched;
        };

        auto importNamedType = [&](const std::string &name) {
            if (currentScope->Lookup(name)) {
                return;
            }
            auto depIt = sourceTable.find(name);
            const Symbol *dep = depIt == sourceTable.end() ? findPackageType(name) : &depIt->second;
            if (!dep) {
                return;
            }
            if (dep->kind == Symbol::Kind::Type || dep->kind == Symbol::Kind::Interface) {
                DefineImportedSymbol(*dep);
            }
        };

        auto visitType = [&](this auto &&self, const TypeExpr &type) -> void {
            if (const auto *named = dynamic_cast<const NamedTypeExpr *>(&type)) {
                importNamedType(named->name);
                for (const auto &arg : named->typeArgs) {
                    self(*arg);
                }
            }
            else if (const auto *ptr = dynamic_cast<const PointerTypeExpr *>(&type)) {
                self(*ptr->pointee);
            }
            else if (const auto *slice = dynamic_cast<const ArrayTypeExpr *>(&type)) {
                self(*slice->element);
            }
            else if (const auto *tuple = dynamic_cast<const TupleTypeExpr *>(&type)) {
                for (const auto &elem : tuple->elements) {
                    self(*elem);
                }
            }
            else if (const auto *fn = dynamic_cast<const FunctionTypeExpr *>(&type)) {
                for (const auto &param : fn->params) {
                    self(*param);
                }
                if (fn->returnType) {
                    self(**fn->returnType);
                }
            }
        };

        for (const auto *overload : sym.funcOverloads) {
            for (const auto &param : overload->params) {
                visitType(*param.type);
            }
            if (overload->returnType) {
                visitType(**overload->returnType);
            }
        }
    }

    void CheckUseDecl(const UseDecl &d) {
        if (d.path.empty()) {
            EmitError(d.location, "empty import path");
            return;
        }
        const std::string &pkgName = d.path[0];

        if (d.kind == UseDecl::Kind::Single) {
            // Bind `packageModuleScopes[pkgName][moduleName]` as a module alias
            // usable through `::`. Returns true when the module exists.
            auto bindModuleAlias = [&](const std::string &moduleName) -> bool {
                auto pkgIt = packageModuleScopes.find(pkgName);
                if (pkgIt == packageModuleScopes.end()) {
                    return false;
                }
                auto modIt = pkgIt->second.find(moduleName);
                if (modIt == pkgIt->second.end()) {
                    return false;
                }
                Symbol sym;
                sym.kind = Symbol::Kind::Module;
                sym.name = moduleName;
                sym.location = d.location;
                sym.moduleScope = modIt->second;
                DefineImportedSymbol(sym);
                return true;
            };

            // Bare `import Pkg;` binds the package's eponymous module as a
            // namespace, so its members are reached through `Pkg::Name`.
            if (d.path.size() < 2) {
                if (bindModuleAlias(pkgName)) {
                    return;
                }
                EmitError(d.location, std::format("import '{}' does not name a module; name an "
                                                  "item instead (e.g. import {}::Name)",
                                                  pkgName, pkgName));
                return;
            }
            const std::string &name = d.path.back();
            // If path.size()==2 and name matches a logical module, create a
            // module alias.
            if (d.path.size() == 2 && bindModuleAlias(name)) {
                return;
            }
            PromoteFromPackage(d, pkgName, name);
        }
        else if (d.kind == UseDecl::Kind::Multi) {
            for (const auto &name : d.names) {
                PromoteFromPackage(d, pkgName, name);
            }
        }
        else // Glob: promote all from the specific module (or all modules
        // if Pkg::*)
        {
            const std::string modulePath = ModulePathForImport(d);
            ImportScope scope = ResolveImportScope(d, pkgName, modulePath);
            if (!scope.table) {
                return;
            }
            for (const auto &[name, sym] : *scope.table) {
                DefineImportedSymbol(sym);
            }
        }
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

    TypeRef SubstituteIdentityType(TypeRef type, const std::unordered_map<std::string, TypeRef> &substitutions) const {
        if (type.kind == TypeRef::Kind::TypeParam) {
            if (const auto substitution = substitutions.find(type.name); substitution != substitutions.end()) {
                return substitution->second;
            }
        }
        for (auto &inner : type.inner) {
            inner = SubstituteIdentityType(std::move(inner), substitutions);
        }
        return type;
    }

    TypeRef IdentityParameterType(const Param &parameter,
                                  const std::unordered_map<std::string, TypeRef> &substitutions = {}) const {
        TypeRef type = TypeRef::MakeUnknown();
        if (const auto resolved = typeNodeTypes.find(parameter.type.get()); resolved != typeNodeTypes.end()) {
            type = SubstituteIdentityType(resolved->second, substitutions);
        }
        if (parameter.isVariadic) {
            type = TypeRef::MakeNamed(SliceTypeName(type));
        }
        return type;
    }

    std::string MangleFunctionWithParams(const FuncDecl &declaration) const {
        std::string name = declaration.name + "__";
        for (std::size_t i = 0; i < declaration.params.size(); ++i) {
            if (i != 0) {
                name += "_";
            }
            name += MangleTypeName(IdentityParameterType(declaration.params[i]));
        }
        return name;
    }

    bool FunctionIsOverloadedInModule(const FuncDecl &declaration) const {
        const auto functions = functionsByName.find(declaration.name);
        if (functions == functionsByName.end()) {
            return false;
        }
        const std::string &modulePath = functionModulePaths.at(&declaration);
        std::size_t count = 0;
        for (const auto *candidate : functions->second) {
            if (functionModulePaths.at(candidate) == modulePath && ++count > 1) {
                return true;
            }
        }
        return false;
    }

    bool MethodIsOverloadedForIdentity(const std::string &typeName, const std::string &methodName) const {
        const auto type = methodsByType.find(typeName);
        if (type == methodsByType.end()) {
            return false;
        }
        const auto method = type->second.find(methodName);
        return method != type->second.end() && method->second.size() > 1;
    }

    std::string MethodLinkerName(const FuncDecl &method, const TypeRef &receiverType,
                                 const std::unordered_map<std::string, TypeRef> &substitutions) const {
        const std::string typeName = NamedBaseTypeName(receiverType);
        std::string name = typeName + "::" + method.name;
        if (MethodIsOverloadedForIdentity(typeName, method.name)) {
            name += "__";
            bool first = true;
            for (const auto &parameter : method.params) {
                if (parameter.name == "self" || parameter.isVariadic) {
                    continue;
                }
                if (!first) {
                    name += "_";
                }
                name += MangleTypeName(IdentityParameterType(parameter, substitutions));
                first = false;
            }
        }

        const auto implementation = methodImpls.find(&method);
        if (implementation == methodImpls.end() || ImplTypeParams(*implementation->second).empty()) {
            return name;
        }
        const auto structure = structDecls.find(typeName);
        if (structure != structDecls.end()) {
            for (const auto &parameter : structure->second->typeParams) {
                if (const auto substitution = substitutions.find(parameter); substitution != substitutions.end()) {
                    name += "_" + MangleTypeName(substitution->second);
                }
            }
        }
        return name;
    }

    void RecordMethodIdentityRecipe(ResolvedCallableBinding &binding, const FuncDecl &method) const {
        assert(binding.receiverType && "method binding is missing its receiver type");
        const std::string typeName = NamedBaseTypeName(*binding.receiverType);
        binding.linkerNameBase = typeName + "::" + method.name;
        binding.linkerNameHasOverloadSignature = MethodIsOverloadedForIdentity(typeName, method.name);
        if (binding.linkerNameHasOverloadSignature) {
            for (const auto &parameter : method.params) {
                if (parameter.name != "self" && !parameter.isVariadic) {
                    binding.linkerOverloadTypes.push_back(IdentityParameterType(parameter, binding.substitutions));
                }
            }
        }

        const auto implementation = methodImpls.find(&method);
        if (implementation == methodImpls.end() || ImplTypeParams(*implementation->second).empty()) {
            return;
        }
        if (const auto structure = structDecls.find(typeName); structure != structDecls.end()) {
            binding.linkerSpecializationParameters = structure->second->typeParams;
        }
    }

    void BuildFinalSymbolIdentities() override {
        std::unordered_map<std::string, std::unordered_set<std::string>> owners;
        for (const auto &[name, declarations] : functionsByName) {
            for (const auto *declaration : declarations) {
                if (declaration->typeParams.empty()) {
                    const std::string local =
                        FunctionIsOverloadedInModule(*declaration) ? MangleFunctionWithParams(*declaration) : name;
                    owners[local].insert(functionModulePaths.at(declaration));
                }
            }
        }
        for (const auto &[name, declarations] : functionsByName) {
            for (const auto *declaration : declarations) {
                if (!declaration->typeParams.empty()) {
                    const bool overloaded = declarations.size() > 1;
                    symbolIdentities.insert_or_assign(
                        declaration,
                        ResolvedSymbolIdentity{overloaded ? MangleFunctionWithParams(*declaration) : name});
                    continue;
                }
                std::string local =
                    FunctionIsOverloadedInModule(*declaration) ? MangleFunctionWithParams(*declaration) : name;
                const std::string &modulePath = functionModulePaths.at(declaration);
                if (owners[local].size() > 1 && !modulePath.empty()) {
                    local = modulePath + "::" + local;
                }
                symbolIdentities.insert_or_assign(declaration, ResolvedSymbolIdentity{std::move(local)});
            }
        }

        for (const auto *declaration : externFuncDecls) {
            symbolIdentities.insert_or_assign(
                declaration,
                ResolvedSymbolIdentity{declaration->symbolName.empty() ? declaration->name : declaration->symbolName});
        }

        for (const auto *implementation : implDecls) {
            const std::string typeName = implementation->typeName.starts_with("Slice<")
                                           ? implementation->typeName
                                           : BaseTypeName(implementation->typeName);
            if (ImplTypeParams(*implementation).empty()) {
                const TypeRef receiverType = TypeRef::MakeNamed(typeName);
                for (const auto &method : implementation->methods) {
                    symbolIdentities.insert_or_assign(
                        method.get(), ResolvedSymbolIdentity{MethodLinkerName(*method, receiverType, {})});
                }
            }
            if (!implementation->interfaceName || !ImplTypeParams(*implementation).empty()) {
                continue;
            }
            const auto interface = interfaceDecls.find(*implementation->interfaceName);
            if (interface == interfaceDecls.end() || interface->second->methods.empty()) {
                continue;
            }
            ResolvedVtableIdentity identity;
            identity.linkerName = "__vtable__" + typeName + "__" + *implementation->interfaceName;
            for (const auto &method : interface->second->methods) {
                identity.entries.push_back(typeName + "::" + method->name);
            }
            vtableIdentities.insert_or_assign(implementation, std::move(identity));
        }

        for (auto &[call, binding] : callableBindings) {
            (void)call;
            if (!binding.selectedDeclaration || binding.dispatch == ResolvedCallableBinding::DispatchKind::Interface ||
                binding.dispatch == ResolvedCallableBinding::DispatchKind::Indirect ||
                binding.dispatch == ResolvedCallableBinding::DispatchKind::EnumVariant) {
                continue;
            }
            const auto *function = dynamic_cast<const FuncDecl *>(binding.selectedDeclaration);
            if (function && binding.dispatch == ResolvedCallableBinding::DispatchKind::Method && binding.receiverType) {
                binding.linkerName = MethodLinkerName(*function, *binding.receiverType, binding.substitutions);
                RecordMethodIdentityRecipe(binding, *function);
                continue;
            }
            if (function && !function->typeParams.empty()) {
                binding.linkerName = function->name;
                binding.linkerNameBase = function->name;
                binding.linkerSpecializationParameters = function->typeParams;
                for (const auto &parameter : function->typeParams) {
                    if (const auto substitution = binding.substitutions.find(parameter);
                        substitution != binding.substitutions.end()) {
                        binding.linkerName += "_" + MangleTypeName(substitution->second);
                    }
                }
                continue;
            }
            if (const auto identity = symbolIdentities.find(binding.selectedDeclaration);
                identity != symbolIdentities.end()) {
                binding.linkerName = identity->second.linkerName;
            }
        }
    }

    // Expressions
    TypeRef CheckExpr(const Expr &expr) override {
        const std::size_t diagnosticStart = diags.size();
        TypeRef type = CheckExprImpl(expr);
        if (const auto *call = dynamic_cast<const CallExpr *>(&expr)) {
            const bool hasNewError =
                std::ranges::any_of(diags.begin() + static_cast<std::ptrdiff_t>(diagnosticStart), diags.end(),
                                    [](const SemanticDiagnostic &diagnostic) {
                                        return diagnostic.severity == SemanticDiagnostic::Severity::Error;
                                    });
            if (type.IsUnknown() || hasNewError) {
                callableBindings.erase(call);
            }
        }
        if (!type.IsUnknown()) {
            expressionTypes.insert_or_assign(&expr, type);
        }
        return type;
    }

    TypeRef CheckExprImpl(const Expr &expr) {
        if (const std::optional<TypeRef> basicType = CheckBasicExpression(expr)) {
            return *basicType;
        }

        if (auto *e = dynamic_cast<const IdentExpr *>(&expr)) {
            Symbol *sym = currentScope->Lookup(e->name);
            if (sym) {
                return sym->type;
            }
            EmitError(e->location, std::format("undefined name '{}'", e->name));
            return TypeRef::MakeUnknown();
        }

        if (dynamic_cast<const SelfExpr *>(&expr)) {
            if (!inImpl) {
                EmitError(expr.location, "'self' used outside of an extend block");
            }
            return currentSelfType.IsUnknown() ? TypeRef::MakeNamed("self") : currentSelfType;
        }

        if (auto *e = dynamic_cast<const PathExpr *>(&expr)) {
            if (e->segments.empty()) {
                return TypeRef::MakeUnknown();
            }
            Symbol *first = currentScope->Lookup(e->segments[0]);
            if (!first) {
                EmitError(e->location, std::format("undefined name '{}'", e->segments[0]));
                return TypeRef::MakeUnknown();
            }
            if (e->segments.size() >= 2 &&
                (first->kind == Symbol::Kind::Type || first->kind == Symbol::Kind::Interface)) {
                if (first->kind == Symbol::Kind::Type) {
                    if (e->segments.size() == 2) {
                        if (const auto constant = LookupPrimitiveConstant(first->type, e->segments[1], context)) {
                            return constant->type;
                        }
                    }
                    const std::string &variantName = e->segments[1];
                    if (const EnumDecl::Variant *variant = LookupEnumVariant(first->name, variantName)) {
                        if (e->segments.size() > 2) {
                            EmitError(e->location, std::format("'{}' is an enum variant, not a module", variantName));
                            return TypeRef::MakeUnknown();
                        }
                        if (!variant->fields.empty() || !variant->namedFields.empty()) {
                            return EnumVariantConstructorType(*enumDecls.at(first->name), *variant);
                        }
                        return EnumType(*enumDecls.at(first->name));
                    }
                }
                TypeRef receiverType = first->type.IsUnknown() ? TypeRef::MakeNamed(first->name) : first->type;
                const std::string &methodName = e->segments[1];
                const FuncDecl *method = LookupMethod(receiverType, methodName);
                if (!method) {
                    EmitError(e->location,
                              std::format("'{}' not found in extend for type '{}'", methodName, first->name));
                    return TypeRef::MakeUnknown();
                }
                if (e->segments.size() > 2) {
                    EmitError(e->location, std::format("'{}' is a function, not a module", methodName));
                    return TypeRef::MakeUnknown();
                }
                return AssociatedFunctionType(receiverType, *method);
            }
            Symbol *current = first;
            Scope *moduleScope = nullptr;
            for (std::size_t i = 1; i < e->segments.size(); ++i) {
                if (current->kind != Symbol::Kind::Module || !current->moduleScope) {
                    return current->type;
                }
                moduleScope = current->moduleScope;
                Symbol *item = moduleScope->Lookup(e->segments[i]);
                if (!item) {
                    EmitError(e->location,
                              std::format("'{}' not found in module '{}'", e->segments[i], e->segments[i - 1]));
                    return TypeRef::MakeUnknown();
                }
                current = item;
            }
            return current->type;
        }

        if (auto *e = dynamic_cast<const SizeOfExpr *>(&expr)) {
            ValidateArrayType(*e->type);
            TypeRef t = ResolveType(*e->type);
            if (!t.IsUnknown() && t.kind != TypeRef::Kind::TypeParam) {
                if (const auto layout = LayoutOfTypeExpr(*e->type)) {
                    sizeOfValues.insert_or_assign(e, layout->size);
                }
                else {
                    EmitError(e->location, std::format("cannot determine size of type '{}'", t.ToString()));
                }
            }
            return TypeRef::MakeUInt64();
        }

        if (dynamic_cast<const IntrinsicExpr *>(&expr)) {
            const auto *e = static_cast<const IntrinsicExpr *>(&expr);
            using K = IntrinsicExpr::Kind;
            const bool takesArgument = e->kind == K::TargetFeature || e->kind == K::CompilerHasFeature ||
                                       e->kind == K::Config || e->kind == K::HasConfig;
            if (takesArgument) {
                if (e->args.size() != 1 || !e->args[0]) {
                    EmitError(e->location, "compile-time intrinsic expects exactly one argument");
                }
                else if (e->kind == K::TargetFeature && dynamic_cast<const EnumShorthandExpr *>(e->args[0].get())) {
                    // `.AVX2` is given its meaning by #target.HasFeature.
                }
                else {
                    const TypeRef argType = CheckExpr(*e->args[0]);
                    const std::string stringType = SliceTypeName(TypeRef::MakeChar8());
                    if (!argType.IsUnknown() && !(argType.kind == TypeRef::Kind::Named && argType.name == stringType)) {
                        EmitError(e->args[0]->location, "compile-time intrinsic argument must be a string");
                    }
                }
            }

            if (e->kind == K::Line || e->kind == K::Column || e->kind == K::PointerBits) {
                return TypeRef::MakeUInt();
            }
            if (e->kind == K::BuildTimestamp) {
                return TypeRef::MakeUInt64();
            }
            if (e->kind == K::TargetFeature || e->kind == K::CompilerHasFeature || e->kind == K::HasConfig ||
                e->kind == K::DebugAssertions || e->kind == K::DebugInfo || e->kind == K::IsTest) {
                return TypeRef::MakeBool();
            }
            if (e->kind == K::Os || e->kind == K::Arch || e->kind == K::Abi || e->kind == K::Endian ||
                e->kind == K::DataModel || e->kind == K::ObjectFormat || e->kind == K::BuildMode ||
                e->kind == K::Optimization || e->kind == K::OutputKind) {
                const char *name = e->kind == K::Os           ? "#target.os"
                                 : e->kind == K::Arch         ? "#target.arch"
                                 : e->kind == K::Abi          ? "#target.abi"
                                 : e->kind == K::Endian       ? "#target.endian"
                                 : e->kind == K::DataModel    ? "#target.dataModel"
                                 : e->kind == K::ObjectFormat ? "#target.objectFormat"
                                 : e->kind == K::BuildMode    ? "#build.mode"
                                 : e->kind == K::Optimization ? "#build.optimization"
                                                              : "#build.outputKind";
                EmitError(e->location, std::string("'") + name + "' can only be used in a 'when' condition");
                return TypeRef::MakeUnknown();
            }
            return TypeRef::MakeNamed(SliceTypeName(TypeRef::MakeChar8()));
        }

        if (const auto *e = dynamic_cast<const EnumShorthandExpr *>(&expr)) {
            // The bare `.Variant` shorthand is not part of the language; the
            // variant must always be written out in full.
            EmitError(e->location,
                      std::format("'.{}' must be written in full, as in 'Enum::{}'", e->variant, e->variant));
            return TypeRef::MakeUnknown();
        }

        if (auto *e = dynamic_cast<const TernaryExpr *>(&expr)) {
            TypeRef cond = CheckExpr(*e->condition);
            if (!cond.IsUnknown() && !cond.IsBool()) {
                EmitError(e->condition->location, "ternary condition must be 'bool'");
            }
            TypeRef thenT = CheckExpr(*e->thenExpr);
            TypeRef elseT = CheckExpr(*e->elseExpr);
            return thenT.IsUnknown() ? elseT : thenT;
        }

        if (auto *e = dynamic_cast<const RangeExpr *>(&expr)) {
            TypeRef loType = e->lo ? CheckExpr(*e->lo) : TypeRef::MakeUnknown();
            TypeRef hiType = e->hi ? CheckExpr(*e->hi) : TypeRef::MakeUnknown();
            if ((!loType.IsUnknown() && !loType.IsNumeric()) || (!hiType.IsUnknown() && !hiType.IsNumeric())) {
                EmitError(e->location, "range bounds must be numeric");
            }
            if (e->lo && e->hi) {
                const auto start = EvalConstInt(*e->lo);
                const auto end = EvalConstInt(*e->hi);
                if (start && end && *start > *end) {
                    EmitError(e->location, "range start cannot be greater than its end");
                }
            }
            TypeRef elemType = loType.IsUnknown() ? hiType : loType;
            if (e->lo && e->hi && hiType.IsInteger() && UnsuffixedIntegerLiteralFits(*e->lo, hiType)) {
                elemType = hiType;
            }
            else if (e->lo && e->hi && loType.IsInteger() && UnsuffixedIntegerLiteralFits(*e->hi, loType)) {
                elemType = loType;
            }
            if (elemType.IsUnknown()) {
                return TypeRef::MakeRangeFull();
            }
            return TypeRef::MakeRange(elemType, e->lo != nullptr, e->hi != nullptr, e->inclusive);
        }

        if (const auto *e = dynamic_cast<const CallExpr *>(&expr)) {
            return CheckCallExpression(*e);
        }

        if (auto *e = dynamic_cast<const IndexExpr *>(&expr)) {
            TypeRef obj = CheckExpr(*e->object);
            TypeRef index = CheckExpr(*e->index);
            if (index.IsRange()) {
                std::optional<TypeRef> elemType;
                if (obj.kind == TypeRef::Kind::Array && !obj.inner.empty()) {
                    elemType = obj.inner[0];
                }
                else {
                    elemType = SliceElementType(obj);
                }
                if (elemType) {
                    return TypeRef::MakeNamed(SliceTypeName(*elemType));
                }
                EmitError(e->location, std::format("cannot slice value of type '{}'", obj.ToString()));
                return TypeRef::MakeUnknown();
            }
            if (auto elemType = IndexElementType(obj)) {
                return *elemType;
            }
            return TypeRef::MakeUnknown();
        }

        if (auto *e = dynamic_cast<const FieldExpr *>(&expr)) {
            TypeRef obj = CheckExpr(*e->object);
            if (obj.kind == TypeRef::Kind::Array && obj.arrayLength && e->field == "length") {
                return TypeRef::MakeUInt();
            }
            if (auto elemType = SliceElementType(obj)) {
                if (e->field == "data") {
                    return TypeRef::MakePointer(*elemType);
                }
                if (e->field == "length") {
                    return TypeRef::MakeUInt64();
                }
                EmitError(e->location, std::format("unknown field '{}' on type '{}'", e->field, obj.ToString()));
                return TypeRef::MakeUnknown();
            }
            if (obj.IsRange()) {
                TypeRef elemType = obj.inner.empty() ? TypeRef::MakeInt64() : obj.inner[0];
                if (e->field == "start" && obj.RangeHasStart()) {
                    return elemType;
                }
                if (e->field == "end" && obj.RangeHasEnd()) {
                    return elemType;
                }
                EmitError(e->location, std::format("unknown field '{}' on type '{}'", e->field, obj.ToString()));
                return TypeRef::MakeUnknown();
            }
            if (obj.kind == TypeRef::Kind::Tuple) {
                try {
                    const std::size_t idx = std::stoul(e->field);
                    if (idx < obj.inner.size()) {
                        return obj.inner[idx];
                    }
                }
                catch (...) {
                }
                EmitError(e->location,
                          std::format("tuple index '{}' out of range for type '{}'", e->field, obj.ToString()));
                return TypeRef::MakeUnknown();
            }

            // Interface fat-pointer fields: data → *opaque, vtable →
            // *opaque
            if (const std::string ifaceName = NamedBaseTypeName(obj);
                !ifaceName.empty() && currentScope->Lookup(ifaceName) &&
                currentScope->Lookup(ifaceName)->kind == Symbol::Kind::Interface) {
                const TypeRef ptrOpaque = TypeRef::MakePointer(TypeRef::MakeOpaque());
                if (e->field == "data" || e->field == "vtable") {
                    return ptrOpaque;
                }
                EmitError(e->location,
                          std::format("unknown field '{}' on interface type '{}'", e->field, obj.ToString()));
                return TypeRef::MakeUnknown();
            }

            const std::string structName = NamedBaseTypeName(obj);
            if (!structName.empty() && structDecls.contains(structName)) {
                if (TypeRef fieldType = StructFieldType(obj, e->field); !fieldType.IsUnknown()) {
                    return fieldType;
                }
                EmitError(e->location, std::format("unknown field '{}' on type '{}'", e->field, obj.ToString()));
                return TypeRef::MakeUnknown();
            }

            if (TypeRef fieldType = StructFieldType(obj, e->field); !fieldType.IsUnknown()) {
                return fieldType;
            }
            if (!obj.IsUnknown()) {
                EmitError(e->location, std::format("type '{}' has no field '{}'", obj.ToString(), e->field));
            }
            return TypeRef::MakeUnknown(); // field type lookup needs full
            // type info
        }

        if (auto *e = dynamic_cast<const StructInitExpr *>(&expr)) {
            CheckStructInitExpr(*e);
            if (const auto [enumDecl, variant] = LookupEnumVariantInitializer(e->typeName); enumDecl && variant) {
                return EnumType(*enumDecl);
            }
            const std::string typeName = GenericStructInitName(*e);
            TypeRef type = ParseTypeRefFromString(typeName);
            return type.IsRange() ? type : TypeRef::MakeNamed(typeName);
        }

        if (auto *e = dynamic_cast<const ArrayExpr *>(&expr)) {
            TypeRef elemType = TypeRef::MakeUnknown();
            for (const auto &el : e->elements) {
                TypeRef t = CheckExpr(*el);
                if (elemType.IsUnknown()) {
                    elemType = t;
                }
            }
            return TypeRef::MakeArray(elemType, e->elements.size());
        }

        if (auto *e = dynamic_cast<const TupleExpr *>(&expr)) {
            std::vector<TypeRef> elemTypes;
            for (const auto &el : e->elements) {
                elemTypes.push_back(CheckExpr(*el));
            }
            return TypeRef::MakeTuple(std::move(elemTypes));
        }

        if (auto *e = dynamic_cast<const IsExpr *>(&expr)) {
            TypeRef operandType = CheckExpr(*e->operand);
            ResolveType(*e->type);
            const std::string ifaceName = NamedBaseTypeName(operandType);
            if (!ifaceName.empty()) {
                Symbol *sym = currentScope->Lookup(ifaceName);
                if (sym && sym->kind == Symbol::Kind::Interface) {
                    EmitError(e->location, "runtime type checking with 'is' on "
                                           "interface types is not yet "
                                           "implemented");
                }
            }
            return TypeRef::MakeBool();
        }

        if (auto *e = dynamic_cast<const MatchExpr *>(&expr)) {
            const TypeRef subjectType = CheckExpr(*e->subject);
            TypeRef resultType = TypeRef::MakeUnknown();
            for (const auto &arm : e->arms) {
                PushScope();
                CheckPattern(*arm.pattern, subjectType);
                TypeRef armType = CheckExpr(*arm.body);
                PopScope();

                if (resultType.IsUnknown()) {
                    resultType = armType;
                }
                else if (!armType.IsUnknown() && !CanAssignExprTo(*arm.body, armType, resultType)) {
                    EmitError(arm.location,
                              AssignmentErrorMessage(*arm.body, resultType,
                                                     std::format("match arm type mismatch: "
                                                                 "expected '{}', found '{}'",
                                                                 resultType.ToString(), armType.ToString())));
                }
            }
            return resultType;
        }

        if (auto *e = dynamic_cast<const BlockExpr *>(&expr)) {
            CheckBlock(*e->block);
            return TypeRef::MakeUnknown();
        }

        if (auto *e = dynamic_cast<const SpreadExpr *>(&expr)) {
            return CheckExpr(*e->operand);
        }

        return TypeRef::MakeUnknown();
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

    void ValidateCastConstant(const CastExpr &expression, const TypeRef &operandType,
                              const TypeRef &targetType) const override {
        if (const auto maxCodePoint = CharTypeMaxCodePoint(targetType);
            maxCodePoint && (operandType.IsInteger() || IsCharType(operandType))) {
            if (const auto value = EvalConstInt(*expression.operand); value && *value < 0) {
                EmitError(expression.location,
                          std::format("constant value is out of range for type '{}'", targetType.ToString()));
            }
            else if (const auto charValue = EvalConstCharCastValue(*expression.operand)) {
                if (*charValue > *maxCodePoint) {
                    EmitError(expression.location,
                              std::format("constant value is out of range for type '{}'", targetType.ToString()));
                }
                else if (IsSurrogateCodePoint(*charValue)) {
                    EmitError(expression.location,
                              std::format("surrogate code point U+{:04X} cannot be converted to '{}'", *charValue,
                                          targetType.ToString()));
                }
            }
        }
    }

    static bool ContainsTypeParam(const TypeRef &type) {
        if (type.kind == TypeRef::Kind::TypeParam) {
            return true;
        }
        return std::ranges::any_of(type.inner, [](const TypeRef &inner) { return ContainsTypeParam(inner); });
    }

    static TypeRef SubstituteTypeParams(TypeRef type, const std::unordered_map<std::string, TypeRef> &substitutions) {
        if (type.kind == TypeRef::Kind::TypeParam) {
            if (const auto it = substitutions.find(type.name); it != substitutions.end()) {
                return it->second;
            }
            return type;
        }
        for (TypeRef &inner : type.inner) {
            inner = SubstituteTypeParams(std::move(inner), substitutions);
        }
        return type;
    }

    void QueueGenericInstantiation(const FuncDecl &decl,
                                   const std::unordered_map<std::string, TypeRef> &substitutions) override {
        if (decl.typeParams.empty() || substitutions.size() != decl.typeParams.size()) {
            return;
        }

        const bool isConcrete = std::ranges::all_of(decl.typeParams, [&](const std::string &param) {
            const auto it = substitutions.find(param);
            return it != substitutions.end() && !it->second.IsUnknown() && !ContainsTypeParam(it->second);
        });
        if (!isConcrete) {
            if (currentFunctionDecl) {
                deferredGenericCalls[currentFunctionDecl].push_back({&decl, substitutions});
            }
            return;
        }
        pendingGenericInstantiations.push_back({&decl, substitutions});
    }

    void ValidatePendingGenericInstantiations() override {
        std::size_t processed = 0;
        while (processed < pendingGenericInstantiations.size()) {
            PendingGenericInstantiation instantiation = std::move(pendingGenericInstantiations[processed++]);

            std::string key;
            for (const std::string &param : instantiation.decl->typeParams) {
                if (!key.empty()) {
                    key += ";";
                }
                key += instantiation.substitutions.at(param).ToString();
            }
            if (!validatedGenericInstantiations[instantiation.decl].insert(std::move(key)).second) {
                continue;
            }

            Scope *savedScope = currentScope;
            const std::string savedFile = currentFile;
            const FuncDecl *savedFunctionDecl = currentFunctionDecl;
            if (const auto it = functionDeclScopes.find(instantiation.decl); it != functionDeclScopes.end()) {
                currentScope = it->second;
            }
            if (const auto it = functionDeclFiles.find(instantiation.decl); it != functionDeclFiles.end()) {
                currentFile = it->second;
            }
            currentFunctionDecl = nullptr;

            ValidateDeferredBasicExpressionChecks(*instantiation.decl, instantiation.substitutions);
            if (const auto it = deferredGenericCalls.find(instantiation.decl); it != deferredGenericCalls.end()) {
                for (const DeferredGenericCall &call : it->second) {
                    std::unordered_map<std::string, TypeRef> substitutions;
                    for (const auto &[param, type] : call.substitutions) {
                        substitutions.emplace(param, SubstituteTypeParams(type, instantiation.substitutions));
                    }
                    QueueGenericInstantiation(*call.callee, substitutions);
                }
            }

            currentFunctionDecl = savedFunctionDecl;
            currentFile = savedFile;
            currentScope = savedScope;
        }
    }
};

// Sema public API
SemanticAnalyzer::SemanticAnalyzer(std::vector<Module *> userModules, std::vector<DepPackage> inputDeps,
                                   std::string inputPackageName, CompileTimeContext inputContext)
    : modules(std::move(userModules))
    , deps(std::move(inputDeps))
    , packageName(std::move(inputPackageName))
    , compileTimeContext(std::move(inputContext)) {
}

SemanticAnalyzer::SemanticAnalyzer(std::vector<Module *> userModules, std::vector<DepPackage> inputDeps,
                                   std::string inputPackageName, std::string inputTargetSystem)
    : SemanticAnalyzer(std::move(userModules), std::move(inputDeps), std::move(inputPackageName),
                       CompileTimeContext{}) {
    if (inputTargetSystem == "FreeBSD")
        compileTimeContext.target.os = Target::OS::FreeBSD;
    else if (inputTargetSystem == "Linux")
        compileTimeContext.target.os = Target::OS::Linux;
    else if (inputTargetSystem == "macOS" || inputTargetSystem == "MacOS")
        compileTimeContext.target.os = Target::OS::MacOS;
    else if (inputTargetSystem == "Windows")
        compileTimeContext.target.os = Target::OS::Windows;
    compileTimeContext.target.object_format = Target::GetObjectFormat(compileTimeContext.target.os);
}

SemanticModel SemanticAnalyzer::Analyze() {
    // Fold `when` first: the branches that were not taken are dropped here, so
    // nothing below ever sees — or type-checks — them. Each package resolves its
    // own conditionals against its own constants.
    for (auto &dep : deps) {
        std::vector<Module *> depModules;
        depModules.reserve(dep.modules.size());
        for (const auto &entry : dep.modules) {
            depModules.push_back(entry.module);
        }
        ResolveConditionalCompilation(depModules, compileTimeContext, diags);
    }
    ResolveConditionalCompilation(modules, compileTimeContext, diags);

    std::vector<const Module *> constModules(modules.begin(), modules.end());
    std::unordered_map<const Expr *, TypeRef> expressionTypes;
    std::unordered_map<const TypeExpr *, TypeRef> typeNodeTypes;
    std::unordered_map<const Pattern *, TypeRef> patternTypes;
    std::unordered_map<const CallExpr *, ResolvedCallableBinding> callableBindings;
    std::unordered_map<const Decl *, ResolvedSymbolIdentity> symbolIdentities;
    std::unordered_map<const ImplDecl *, ResolvedVtableIdentity> vtableIdentities;
    std::unordered_map<std::string, ResolvedTypeLayout> typeLayouts;
    std::unordered_map<const SizeOfExpr *, std::uint64_t> sizeOfValues;
    SemanticAnalyzerImplementation analyzer(constModules, deps, packageName, diags, symbols, compileTimeContext,
                                            expressionTypes, typeNodeTypes, patternTypes, callableBindings,
                                            symbolIdentities, vtableIdentities, typeLayouts, sizeOfValues);
    analyzer.Run();
    std::vector<const Module *> orderedModules;
    for (const auto &dep : deps) {
        for (const auto &entry : dep.modules) {
            orderedModules.push_back(entry.module);
        }
    }
    orderedModules.insert(orderedModules.end(), modules.begin(), modules.end());
    return SemanticModel{std::move(diags),
                         std::move(symbols),
                         std::move(orderedModules),
                         std::move(compileTimeContext),
                         std::move(expressionTypes),
                         std::move(typeNodeTypes),
                         std::move(patternTypes),
                         std::move(callableBindings),
                         std::move(symbolIdentities),
                         std::move(vtableIdentities),
                         std::move(typeLayouts),
                         std::move(sizeOfValues)};
}
} // namespace Rux
