#include "Frontend/Sema/Sema.h"

#include "Frontend/Lexer.h"
#include "Frontend/Sema/Type.h"
#include "Platform/Target.h"
#include "Support/Layout.h"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace Rux {

using Layout::AlignUp;

// SemaResult

bool SemaResult::HasErrors() const noexcept {
    return std::ranges::any_of(diagnostics,
                               [](const SemaDiagnostic &d) { return d.severity == SemaDiagnostic::Severity::Error; });
}

// Internal: Symbol & Scope
class Scope; // forward declaration — Scope is defined below

struct Symbol {
    enum class Kind {
        Var,
        Func,
        Type,
        Const,
        Module,
        Interface,
    };

    Kind kind = Kind::Var;
    std::string name;
    SourceLocation location;
    TypeRef type;
    bool isMut = false;
    std::vector<const FuncDecl *> funcOverloads;
    std::vector<std::string> interfaceMethods; // for Interface kind
    Scope *moduleScope = nullptr;              // for Module kind: the imported module's scope
};

class Scope {
public:
    explicit Scope(Scope *parent = nullptr)
        : parent(parent) {
    }

    // Returns false and emits a diagnostic if the name is already defined.
    bool Define(Symbol sym, std::vector<SemaDiagnostic> &diags, const std::string &sourceName) {
        if (auto it = table.find(sym.name); it != table.end()) {
            if (it->second.kind == Symbol::Kind::Func && sym.kind == Symbol::Kind::Func) {
                it->second.funcOverloads.insert(it->second.funcOverloads.end(), sym.funcOverloads.begin(),
                                                sym.funcOverloads.end());
                if (it->second.type.IsUnknown() && !sym.type.IsUnknown()) {
                    it->second.type = std::move(sym.type);
                }
                return true;
            }
            diags.push_back({SemaDiagnostic::Severity::Error, sourceName, sym.location,
                             std::format("'{}' is already defined (first defined at {}:{})", sym.name,
                                         it->second.location.line, it->second.location.column)});
            return false;
        }
        table.emplace(sym.name, std::move(sym));
        return true;
    }

    Symbol *Lookup(const std::string &name) {
        auto it = table.find(name);
        if (it != table.end()) {
            return &it->second;
        }
        if (parent) {
            return parent->Lookup(name);
        }
        return nullptr;
    }

    Symbol *LookupLocal(const std::string &name) {
        auto it = table.find(name);
        return it == table.end() ? nullptr : &it->second;
    }

    [[nodiscard]] Scope *Parent() const {
        return parent;
    }

    [[nodiscard]] const std::unordered_map<std::string, Symbol> &Table() const {
        return table;
    }

private:
    Scope *parent;
    std::unordered_map<std::string, Symbol> table;
};

// Internal: Analyzer
class Analyzer {
public:
    Analyzer(std::vector<const Module *> &modules, std::vector<DepPackage> &deps, const std::string &packageName,
             std::vector<SemaDiagnostic> &diags, std::vector<SemaSymbol> &symbols, const std::string &targetOs)
        : modules(modules)
        , deps(deps)
        , packageName(packageName)
        , diags(diags)
        , symbols(symbols)
        , targetOs(targetOs)
        , currentScope(&globalScope) {
    }

    void Run() {
        RegisterBuiltins();
        // Collect dependency package symbols into package-level scopes.
        // Logical module declarations inside any source file populate
        // nested module scopes.
        for (auto &pkg : deps) {
            auto rootScope = std::make_unique<Scope>(&globalScope);
            Scope *rootScopePtr = rootScope.get();
            for (auto &entry : pkg.modules) {
                currentFile = entry.module->name;
                for (const auto &decl : entry.module->items) {
                    CollectDecl(*decl, *rootScope, &pkg.name, "");
                }
            }
            packageModuleScopes[pkg.name][""] = rootScopePtr;
            packageRootScopes.push_back(std::move(rootScope));
        }
        for (auto &pkg : deps) {
            for (auto &entry : pkg.modules) {
                ApplyModuleImportsInScope(*entry.module, *packageModuleScopes[pkg.name][""]);
            }
        }
        // Resolve dep function signatures in their per-module scopes.
        for (auto &pkg : deps) {
            for (auto &entry : pkg.modules) {
                ResolveModuleSignaturesInScope(*entry.module, *packageModuleScopes[pkg.name][""]);
            }
        }
        for (auto &pkg : deps) {
            for (auto &entry : pkg.modules) {
                CheckModuleInScope(*entry.module, *packageModuleScopes[pkg.name][""]);
            }
        }
        // User modules go into globalScope as before. When a package
        // imports itself by name, expose the global/module scopes through
        // the same package import table used for dependencies.
        if (!packageName.empty()) {
            packageModuleScopes[packageName][""] = &globalScope;
        }
        for (auto *mod : modules) {
            CollectModule(*mod);
        }
        for (auto *mod : modules) {
            ApplyModuleImports(*mod);
        }
        for (auto *mod : modules) {
            ResolveModuleSignatures(*mod);
        }
        for (auto *mod : modules) {
            CheckModule(*mod);
        }
    }

private:
    std::vector<const Module *> &modules;
    std::vector<DepPackage> &deps;
    const std::string &packageName;
    std::vector<SemaDiagnostic> &diags;
    std::vector<SemaSymbol> &symbols;
    const std::string &targetOs;
    Scope globalScope{nullptr};
    Scope *currentScope;
    // packageModuleScopes[pkgName][modulePath] = logical module scope.
    // The empty modulePath is the package root.
    std::unordered_map<std::string, std::unordered_map<std::string, Scope *>> packageModuleScopes;
    std::vector<std::unique_ptr<Scope>> packageRootScopes;
    std::vector<std::unique_ptr<Scope>> ownedScopes;
    std::string currentFile;
    TypeRef currentReturnType = TypeRef::MakeOpaque();
    int loopDepth = 0;
    std::unordered_set<std::string> activeLabels;
    bool inImpl = false;
    TypeRef currentSelfType = TypeRef::MakeUnknown();
    std::vector<std::string> currentTypeParams;
    std::unordered_map<std::string, const StructDecl *> structDecls;
    std::unordered_map<std::string, const EnumDecl *> enumDecls;
    std::unordered_map<std::string, const InterfaceDecl *> interfaceDecls;
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<const FuncDecl *>>> methodsByType;
    std::unordered_map<std::string, std::unordered_set<std::string>> typeImplementsInterfaces;

    // Diagnostics

    void EmitError(SourceLocation loc, std::string msg) const {
        diags.push_back({SemaDiagnostic::Severity::Error, currentFile, loc, std::move(msg)});
    }

    void EmitWarning(SourceLocation loc, std::string msg) const {
        diags.push_back({SemaDiagnostic::Severity::Warning, currentFile, loc, std::move(msg)});
    }

    // Scope management
    void PushScope() {
        ownedScopes.push_back(std::make_unique<Scope>(currentScope));
        currentScope = ownedScopes.back().get();
    }

    void PopScope() {
        assert(currentScope->Parent() != nullptr && "cannot pop global scope");
        currentScope = currentScope->Parent();
    }

    bool Define(Symbol sym) const {
        return currentScope->Define(std::move(sym), diags, currentFile);
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

    // Builtins
    void RegisterBuiltins() {
        auto add = [&](const char *name, TypeRef t) {
            Symbol sym;
            sym.kind = Symbol::Kind::Type;
            sym.name = name;
            sym.type = std::move(t);
            globalScope.Define(sym, diags, "<builtin>");
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

    // First pass: collect global declaration names
    void CollectModule(const Module &mod) {
        currentFile = mod.name;
        const std::string *selfPackageName = packageName.empty() ? nullptr : &packageName;
        for (const auto &decl : mod.items) {
            CollectDecl(*decl, globalScope, selfPackageName, "");
        }
    }

    void ResolveModuleSignatures(const Module &mod) {
        currentFile = mod.name;
        for (const auto &decl : mod.items) {
            ResolveDeclSignature(*decl);
        }
    }

    void ResolveDeclSignature(const Decl &decl) {
        if (!DeclMatchesTarget(decl)) {
            return;
        }
        if (auto *fn = dynamic_cast<const FuncDecl *>(&decl)) {
            if (Symbol *sym = globalScope.Lookup(fn->name)) {
                sym->type = MakeFuncType(fn->params, fn->returnType, fn->typeParams);
            }
        }
        else if (auto *externFn = dynamic_cast<const ExternFuncDecl *>(&decl)) {
            if (Symbol *sym = globalScope.Lookup(externFn->name)) {
                sym->type = MakeFuncType(externFn->params, externFn->returnType);
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

    void ResolveModuleSignaturesInScope(const Module &mod, Scope &scope) {
        Scope *savedScope = currentScope;
        currentScope = &scope;
        currentFile = mod.name;
        for (const auto &decl : mod.items) {
            ResolveDeclSignatureInScope(*decl, scope);
        }
        currentScope = savedScope;
    }

    void ResolveDeclSignatureInScope(const Decl &decl, Scope &scope) {
        if (!DeclMatchesTarget(decl)) {
            return;
        }
        if (auto *fn = dynamic_cast<const FuncDecl *>(&decl)) {
            if (Symbol *sym = scope.Lookup(fn->name)) {
                sym->type = MakeFuncType(fn->params, fn->returnType, fn->typeParams);
            }
        }
        else if (auto *externFn = dynamic_cast<const ExternFuncDecl *>(&decl)) {
            if (Symbol *sym = scope.Lookup(externFn->name)) {
                sym->type = MakeFuncType(externFn->params, externFn->returnType);
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

    void ApplyModuleImports(const Module &mod) {
        currentFile = mod.name;
        for (const auto &decl : mod.items) {
            ApplyDeclImports(*decl);
        }
    }

    void ApplyModuleImportsInScope(const Module &mod, Scope &scope) {
        Scope *savedScope = currentScope;
        currentScope = &scope;
        ApplyModuleImports(mod);
        currentScope = savedScope;
    }

    void ApplyDeclImports(const Decl &decl) {
        if (!DeclMatchesTarget(decl)) {
            return;
        }
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

    static std::string JoinModulePath(const std::string &prefix, const std::string &name) {
        if (prefix.empty()) {
            return name;
        }
        return prefix + "::" + name;
    }

    Scope &ModuleScopeFor(const std::string &name, Scope &parent) {
        if (Symbol *sym = parent.Lookup(name); sym && sym->kind == Symbol::Kind::Module && sym->moduleScope) {
            return *sym->moduleScope;
        }
        return parent;
    }

    void CollectDecl(const Decl &decl, Scope &scope, const std::string *depPackageName = nullptr,
                     const std::string &modulePath = "") {
        if (!DeclMatchesTarget(decl)) {
            return;
        }
        // Records the symbol in `scope` and, for top-level (global) scope,
        // also appends a SemaSymbol to `symbols_` for the dump.
        bool isGlobal = (&scope == &globalScope);

        auto simple = [&](Symbol::Kind kind, const std::string &name, SemaSymbol::Kind pubKind,
                          std::string resolvedType = {}, bool isMut = false) {
            Symbol sym;
            sym.kind = kind;
            sym.name = name;
            sym.location = decl.location;
            sym.isMut = isMut;
            if (scope.Define(sym, diags, currentFile) && isGlobal) {
                symbols.push_back({pubKind, name, currentFile, decl.location, std::move(resolvedType), isMut});
            }
        };

        if (auto *fn = dynamic_cast<const FuncDecl *>(&decl)) {
            Symbol sym;
            sym.kind = Symbol::Kind::Func;
            sym.name = fn->name;
            sym.location = fn->location;
            sym.funcOverloads.push_back(fn);
            if (scope.Define(sym, diags, currentFile) && isGlobal) {
                symbols.push_back({SemaSymbol::Kind::Func, fn->name, currentFile, fn->location, {}, false});
            }
        }
        else if (auto *structDecl = dynamic_cast<const StructDecl *>(&decl)) {
            structDecls[structDecl->name] = structDecl;
            simple(Symbol::Kind::Type, structDecl->name, SemaSymbol::Kind::Type, "struct");
        }
        else if (auto *enumDecl = dynamic_cast<const EnumDecl *>(&decl)) {
            enumDecls[enumDecl->name] = enumDecl;
            simple(Symbol::Kind::Type, enumDecl->name, SemaSymbol::Kind::Type, "enum");
        }
        else if (auto *unionDecl = dynamic_cast<const UnionDecl *>(&decl)) {
            simple(Symbol::Kind::Type, unionDecl->name, SemaSymbol::Kind::Type, "union");
        }
        else if (auto *ifaceDecl = dynamic_cast<const InterfaceDecl *>(&decl)) {
            interfaceDecls[ifaceDecl->name] = ifaceDecl;
            Symbol sym;
            sym.kind = Symbol::Kind::Interface;
            sym.name = ifaceDecl->name;
            sym.location = ifaceDecl->location;
            for (auto &m : ifaceDecl->methods) {
                sym.interfaceMethods.push_back(m->name);
            }
            if (scope.Define(sym, diags, currentFile) && isGlobal) {
                symbols.push_back(
                    {SemaSymbol::Kind::Interface, ifaceDecl->name, currentFile, ifaceDecl->location, "interface"});
            }
        }
        else if (auto *constDecl = dynamic_cast<const ConstDecl *>(&decl)) {
            Symbol sym;
            sym.kind = Symbol::Kind::Const;
            sym.name = constDecl->name;
            sym.location = constDecl->location;
            if (constDecl->type) {
                sym.type = ResolveType(*constDecl->type->get());
            }
            if (scope.Define(sym, diags, currentFile) && isGlobal) {
                symbols.push_back({SemaSymbol::Kind::Const, constDecl->name, currentFile, constDecl->location,
                                   sym.type.IsUnknown() ? "" : sym.type.ToString(), false});
            }
        }
        else if (auto *aliasDecl = dynamic_cast<const TypeAliasDecl *>(&decl)) {
            Symbol sym;
            sym.kind = Symbol::Kind::Type;
            sym.name = aliasDecl->name;
            sym.location = aliasDecl->location;
            sym.type = ResolveType(*aliasDecl->type);
            if (scope.Define(sym, diags, currentFile) && isGlobal) {
                symbols.push_back({SemaSymbol::Kind::Type, aliasDecl->name, currentFile, aliasDecl->location,
                                   sym.type.IsUnknown() ? "" : sym.type.ToString(), false});
            }
        }
        else if (auto *externFn = dynamic_cast<const ExternFuncDecl *>(&decl)) {
            simple(Symbol::Kind::Func, externFn->name, SemaSymbol::Kind::Func, "extern");
        }
        else if (auto *externVar = dynamic_cast<const ExternVarDecl *>(&decl)) {
            Symbol sym;
            sym.kind = Symbol::Kind::Var;
            sym.name = externVar->name;
            sym.location = externVar->location;
            sym.isMut = true;
            if (scope.Define(sym, diags, currentFile) && isGlobal) {
                symbols.push_back(
                    {SemaSymbol::Kind::Var, externVar->name, currentFile, externVar->location, "extern", true});
            }
        }
        else if (auto *externBlock = dynamic_cast<const ExternBlockDecl *>(&decl)) {
            for (auto &item : externBlock->items) {
                CollectDecl(*item, scope, depPackageName, modulePath);
            }
        }
        else if (auto *modDecl = dynamic_cast<const ModuleDecl *>(&decl)) {
            Scope *moduleScopePtr = nullptr;
            if (Symbol *existing = scope.Lookup(modDecl->name);
                existing && existing->kind == Symbol::Kind::Module && existing->moduleScope) {
                moduleScopePtr = existing->moduleScope;
            }
            else {
                auto moduleScope = std::make_unique<Scope>(&scope);
                moduleScopePtr = moduleScope.get();
                ownedScopes.push_back(std::move(moduleScope));

                Symbol sym;
                sym.kind = Symbol::Kind::Module;
                sym.name = modDecl->name;
                sym.location = decl.location;
                sym.moduleScope = moduleScopePtr;
                if (scope.Define(sym, diags, currentFile) && isGlobal) {
                    symbols.push_back({SemaSymbol::Kind::Module, modDecl->name, currentFile, decl.location, {}, false});
                }
            }

            const std::string childPath = JoinModulePath(modulePath, modDecl->name);
            if (depPackageName) {
                packageModuleScopes[*depPackageName][childPath] = moduleScopePtr;
            }
            for (auto &item : modDecl->items) {
                CollectDecl(*item, *moduleScopePtr, depPackageName, childPath);
            }
        }
        else if (auto *implDecl = dynamic_cast<const ImplDecl *>(&decl)) {
            for (const auto &method : implDecl->methods) {
                methodsByType[implDecl->typeName][method->name].push_back(method.get());
            }
            if (implDecl->interfaceName) {
                typeImplementsInterfaces[implDecl->typeName].insert(*implDecl->interfaceName);
            }
        }
        // Import declarations don't add names in the first pass.
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

    // Picks the diagnostic for a rejected assignment/conversion. An
    // unsuffixed integer literal that does not fit the target gets a
    // dedicated "out of range" message; everything else uses `fallback`.
    // Keeps the wording consistent across let, return, assignment, const,
    // and field positions.
    static std::string AssignmentErrorMessage(const Expr &expr, const TypeRef &targetType, std::string fallback) {
        if (IsIntegerLiteralOutOfRangeFor(expr, targetType)) {
            return std::format("integer literal is out of range for type '{}'", targetType.ToString());
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

    bool CanAssignExprTo(const Expr &expr, const TypeRef &exprType, const TypeRef &targetType) const {
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

    static std::string NamedBaseTypeName(const TypeRef &type) {
        const TypeRef *named = &type;
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

    TypeRef ResolveType(const TypeExpr &expr) {
        // Helper to resolve enums from the global declaration table
        auto ResolveEnumType = [&](const std::string &name) -> TypeRef {
            if (const auto it = enumDecls.find(name); it != enumDecls.end()) {
                return EnumType(*it->second);
            }
            return TypeRef::MakeUnknown();
        };

        if (const auto *t = dynamic_cast<const NamedTypeExpr *>(&expr)) {
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

            TypeRef enumType = ResolveEnumType(t->name);
            if (!enumType.IsUnknown()) {
                if (!t->typeArgs.empty()) {
                    EmitError(expr.location, std::format("Enum '{}' cannot take type arguments", t->name));
                    return TypeRef::MakeUnknown();
                }
                return enumType;
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
            return TypeRef::MakePointer(pointeeType);
        }

        if (const auto *t = dynamic_cast<const SliceTypeExpr *>(&expr)) {
            TypeRef elemType = ResolveType(*t->element);
            if (elemType.IsUnknown()) {
                return TypeRef::MakeUnknown();
            }
            return TypeRef::MakeNamed(SliceTypeName(elemType));
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

        return TypeRef::MakeUnknown();
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

    [[nodiscard]] const FuncDecl *LookupMethod(const TypeRef &receiverType, const std::string &methodName,
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

    TypeRef ResolveMethodReturnType(const TypeRef &receiverType, const FuncDecl &method) {
        TypeRef savedSelfType = currentSelfType;
        currentSelfType =
            receiverType.kind == TypeRef::Kind::Pointer ? receiverType : TypeRef::MakePointer(receiverType);
        TypeRef ret = method.returnType ? ResolveType(*method.returnType->get()) : TypeRef::MakeOpaque();
        currentSelfType = savedSelfType;
        return ret;
    }

    std::vector<TypeRef> ResolveMethodParamTypes(const TypeRef &receiverType, const FuncDecl &method) {
        TypeRef savedSelfType = currentSelfType;
        currentSelfType =
            receiverType.kind == TypeRef::Kind::Pointer ? receiverType : TypeRef::MakePointer(receiverType);
        std::vector<TypeRef> params;
        for (const auto &param : method.params) {
            if (param.isVariadic || param.name == "self") {
                continue;
            }
            params.push_back(ResolveType(*param.type));
        }
        currentSelfType = savedSelfType;
        return params;
    }

    TypeRef AssociatedFunctionType(const TypeRef &receiverType, const FuncDecl &method) {
        TypeRef savedSelfType = currentSelfType;
        currentSelfType =
            receiverType.kind == TypeRef::Kind::Pointer ? receiverType : TypeRef::MakePointer(receiverType);
        TypeRef type = MakeFuncType(method.params, method.returnType, method.typeParams);
        currentSelfType = savedSelfType;
        return type;
    }

    [[nodiscard]] const FuncDecl *LookupInterfaceMethod(const TypeRef &receiverType,
                                                        const std::string &methodName) const {
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

    TypeRef ResolveInterfaceMethodReturnType(const FuncDecl &method) {
        return method.returnType ? ResolveType(*method.returnType->get()) : TypeRef::MakeOpaque();
    }

    std::vector<TypeRef> ResolveInterfaceMethodParamTypes(const FuncDecl &method) {
        std::vector<TypeRef> params;
        for (const auto &param : method.params) {
            if (param.isVariadic) {
                continue;
            }
            params.push_back(ResolveType(*param.type));
        }
        return params;
    }

    const FuncDecl *LookupFunctionOverload(const Symbol &sym, const std::vector<TypeRef> &argTypes) {
        if (sym.kind != Symbol::Kind::Func || sym.funcOverloads.empty()) {
            return nullptr;
        }
        if (sym.funcOverloads.size() == 1) {
            // Single overload: still validate arity and assignability so
            // that Bar(wrongType) against a lone Bar(int32) returns null
            // and lets the caller emit a proper diagnostic.
            const auto *decl = sym.funcOverloads[0];
            TypeRef funcType = MakeFuncType(decl->params, decl->returnType, decl->typeParams);
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
                    TypeRef funcType = MakeFuncType(decl->params, decl->returnType, decl->typeParams);
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
            // Interface fat pointers are {data: *opaque, vtable: *opaque} =
            // 16 bytes
            if (Symbol *sym = currentScope->Lookup(baseName); sym) {
                if (sym->kind == Symbol::Kind::Interface) {
                    return 16;
                }
                if (sym->kind == Symbol::Kind::Type && !sym->type.IsUnknown()) {
                    return SizeOfTypeRef(sym->type, localSubs);
                }
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

    // Second pass: check declarations
    void CheckModule(const Module &mod) {
        currentFile = mod.name;
        for (const auto &decl : mod.items) {
            CheckDecl(*decl);
        }
    }

    void CheckModuleInScope(const Module &mod, Scope &scope) {
        Scope *savedScope = currentScope;
        currentScope = &scope;
        CheckModule(mod);
        currentScope = savedScope;
    }

    void CheckDecl(const Decl &decl) {
        if (!DeclMatchesTarget(decl)) {
            return;
        }
        if (auto *fn = dynamic_cast<const FuncDecl *>(&decl)) {
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
            ResolveType(*aliasDecl->type); // triggers unknown-type errors
        }
        else if (auto *externFn = dynamic_cast<const ExternFuncDecl *>(&decl)) {
            if (externFn->dll.empty()) {
                EmitError(externFn->location, std::format("extern function '{}' must specify a "
                                                          "source DLL via "
                                                          "@[Import(lib: \"dll.dll\")]",
                                                          externFn->name));
            }
            if (externFn->returnType) {
                ResolveType(*externFn->returnType->get());
            }
            for (auto &p : externFn->params) {
                if (!p.isVariadic) {
                    ResolveType(*p.type);
                }
            }
        }
        else if (auto *externVar = dynamic_cast<const ExternVarDecl *>(&decl)) {
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
        currentTypeParams = d.typeParams;

        TypeRef retType = d.returnType ? ResolveType(*d.returnType->get()) : TypeRef::MakeOpaque();

        auto savedRet = currentReturnType;
        currentReturnType = retType;

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
                continue;
            }
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
            sym.isMut = false;
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

        if (!d.body) {
            EmitError(d.location, std::format("function '{}' has no body", d.name));
        }
        else {
            CheckBlock(*d.body);
        }

        PopScope();
        currentReturnType = savedRet;
        currentTypeParams = savedTypeParams;
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
        for (const auto &field : d.fields) {
            if (!seen.insert(field.name).second) {
                EmitError(field.location, std::format("duplicate field '{}' in struct '{}'", field.name, d.name));
            }
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
                ResolveType(*f);
            }
            std::unordered_set<std::string> namedFields;
            for (const auto &f : variant.namedFields) {
                if (!namedFields.insert(f.name).second) {
                    EmitError(f.location, std::format("duplicate field '{}' in enum variant '{}::{}'", f.name, d.name,
                                                      variant.name));
                }
                ResolveType(*f.type);
            }
        }
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

    void CheckUnionDecl(const UnionDecl &d) {
        std::unordered_set<std::string> seen;
        for (const auto &field : d.fields) {
            if (!seen.insert(field.name).second) {
                EmitError(field.location, std::format("duplicate field '{}' in union '{}'", field.name, d.name));
            }
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
        if (!currentScope->Lookup(d.typeName)) {
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
        TypeRef selfBase;
        if (Symbol *sym = currentScope->Lookup(d.typeName); sym && !sym->type.IsUnknown()) {
            selfBase = sym->type;
        }
        else {
            selfBase = TypeRef::MakeNamed(d.typeName);
        }
        currentSelfType = TypeRef::MakePointer(selfBase);
        for (const auto &m : d.methods) {
            CheckFuncDecl(*m, /*isMethod=*/true);
        }
        currentSelfType = savedSelfType;
        inImpl = savedInImpl;
    }

    void CheckModuleDecl(const ModuleDecl &d) {
        Scope *savedScope = currentScope;
        currentScope = &ModuleScopeFor(d.name, *currentScope);
        for (const auto &item : d.items) {
            CheckDecl(*item);
        }
        currentScope = savedScope;
    }

    void CheckConstDecl(const ConstDecl &d) {
        TypeRef valueType = CheckExpr(*d.value);
        TypeRef constType = d.type ? ResolveType(*d.type->get()) : valueType;
        if (d.type && !valueType.IsUnknown() && !constType.IsUnknown() &&
            !CanAssignExprTo(*d.value, valueType, constType)) {
            EmitError(d.value->location,
                      AssignmentErrorMessage(*d.value, constType,
                                             std::format("cannot assign '{}' to constant of type '{}'",
                                                         valueType.ToString(), constType.ToString())));
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
                if (modulePath.empty() && !logicalModulePath.empty()) {
                    if (auto logicalIt = pkgIt->second.find(logicalModulePath); logicalIt != pkgIt->second.end()) {
                        return {&logicalIt->second->Table(), ImportScopeDisplayName(pkgName, logicalModulePath)};
                    }
                }
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
            EmitError(d.location, std::format("'{}' not found in {}", name, scope.displayName));
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
            else if (const auto *slice = dynamic_cast<const SliceTypeExpr *>(&type)) {
                self(*slice->element);
            }
            else if (const auto *tuple = dynamic_cast<const TupleTypeExpr *>(&type)) {
                for (const auto &elem : tuple->elements) {
                    self(*elem);
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

    static std::string_view HostOs() noexcept {
        return ToString(Platform::HostOS);
    }

    [[nodiscard]] std::string_view EffectiveOs() const {
        return targetOs.empty() ? HostOs() : std::string_view(targetOs);
    }

    [[nodiscard]] bool DeclMatchesTarget(const Decl &d) const {
        return d.targetOs.empty() || d.targetOs == EffectiveOs();
    }

    void CheckUseDecl(const UseDecl &d) {
        if (!DeclMatchesTarget(d)) {
            return;
        }
        if (d.path.empty()) {
            EmitError(d.location, "empty import path");
            return;
        }
        const std::string &pkgName = d.path[0];

        if (d.kind == UseDecl::Kind::Single) {
            if (d.path.size() < 2) {
                EmitError(d.location, std::format("import '{}' must name at least one "
                                                  "item (e.g. import {}::Name)",
                                                  pkgName, pkgName));
                return;
            }
            const std::string &name = d.path.back();
            // If path.size()==2 and name matches a logical module, create a
            // module alias.
            if (d.path.size() == 2) {
                auto pkgIt = packageModuleScopes.find(pkgName);
                if (pkgIt != packageModuleScopes.end()) {
                    auto modIt = pkgIt->second.find(name);
                    if (modIt != pkgIt->second.end()) {
                        Symbol sym;
                        sym.kind = Symbol::Kind::Module;
                        sym.name = name;
                        sym.location = d.location;
                        sym.moduleScope = modIt->second;
                        DefineImportedSymbol(sym);
                        return;
                    }
                }
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

    // Block & statements
    void CheckBlock(const Block &block) {
        PushScope();
        for (const auto &stmt : block.stmts) {
            CheckStmt(*stmt);
        }
        PopScope();
    }

    void CheckStmt(const Stmt &stmt) {
        if (auto *exprStmt = dynamic_cast<const ExprStmt *>(&stmt)) {
            CheckExpr(*exprStmt->expr);
        }
        else if (auto *letStmt = dynamic_cast<const LetStmt *>(&stmt)) {
            TypeRef initType = letStmt->init ? CheckExpr(*letStmt->init) : TypeRef::MakeUnknown();
            TypeRef declType = letStmt->type ? ResolveType(*letStmt->type->get()) : initType;

            if (!letStmt->init && !letStmt->type) {
                EmitError(letStmt->location, "uninitialized variable requires an explicit type");
            }

            if (!letStmt->init && !letStmt->isMut) {
                EmitError(letStmt->location, "immutable variable requires an initializer");
            }

            if (!letStmt->init && letStmt->pattern) {
                EmitError(letStmt->location, "destructuring declaration requires an initializer");
            }

            if (!letStmt->type && declType.IsUnknown() && !letStmt->pattern) {
                EmitWarning(letStmt->location, std::format("cannot infer type of '{}'", letStmt->name));
            }

            if (letStmt->init && letStmt->type && !initType.IsUnknown() && !declType.IsUnknown() &&
                !CanAssignExprTo(*letStmt->init, initType, declType)) {
                EmitError(letStmt->location,
                          AssignmentErrorMessage(
                              *letStmt->init, declType,
                              std::format("cannot assign '{}' to '{}'", initType.ToString(), declType.ToString())));
            }

            if (letStmt->pattern) {
                CheckLetPattern(*letStmt->pattern, declType, letStmt->isMut);
                return;
            }

            Symbol sym;
            sym.kind = Symbol::Kind::Var;
            sym.name = letStmt->name;
            sym.location = letStmt->location;
            sym.type = declType;
            sym.isMut = letStmt->isMut;
            Define(sym);
        }
        else if (const auto *ifStmt = dynamic_cast<const IfStmt *>(&stmt)) {
            TypeRef cond = CheckExpr(*ifStmt->condition);
            if (!cond.IsUnknown() && !cond.IsBool()) {
                EmitError(ifStmt->condition->location, "if condition must be 'bool'");
            }
            CheckBlock(*ifStmt->thenBlock);
            for (const auto &elif : ifStmt->elseIfs) {
                TypeRef elifCond = CheckExpr(*elif.condition);
                if (!elifCond.IsUnknown() && !elifCond.IsBool()) {
                    EmitError(elif.condition->location, "if condition must be 'bool'");
                }
                CheckBlock(*elif.block);
            }
            if (ifStmt->elseBlock) {
                CheckBlock(*ifStmt->elseBlock);
            }
        }
        else if (const auto *whileStmt = dynamic_cast<const WhileStmt *>(&stmt)) {
            if (!whileStmt->label.empty()) {
                activeLabels.insert(whileStmt->label);
            }

            // FIX: Capture and validate the condition type
            TypeRef cond = CheckExpr(*whileStmt->condition);
            if (!cond.IsUnknown() && !cond.IsBool()) {
                EmitError(whileStmt->condition->location, "while condition must be 'bool'");
            }

            ++loopDepth;
            CheckBlock(*whileStmt->body);
            --loopDepth;
            if (!whileStmt->label.empty()) {
                activeLabels.erase(whileStmt->label);
            }
        }

        else if (const auto *doWhileStmt = dynamic_cast<const DoWhileStmt *>(&stmt)) {
            if (!doWhileStmt->label.empty()) {
                activeLabels.insert(doWhileStmt->label);
            }

            ++loopDepth;
            CheckBlock(*doWhileStmt->body);
            --loopDepth;

            TypeRef cond = CheckExpr(*doWhileStmt->condition);
            if (!cond.IsUnknown() && !cond.IsBool()) {
                EmitError(doWhileStmt->condition->location, "do-while condition must be 'bool'");
            }

            if (!doWhileStmt->label.empty()) {
                activeLabels.erase(doWhileStmt->label);
            }
        }
        else if (auto *loopStmt = dynamic_cast<const LoopStmt *>(&stmt)) {
            if (!loopStmt->label.empty()) {
                activeLabels.insert(loopStmt->label);
            }
            ++loopDepth;
            CheckBlock(*loopStmt->body);
            --loopDepth;
            if (!loopStmt->label.empty()) {
                activeLabels.erase(loopStmt->label);
            }
        }
        else if (auto *forStmt = dynamic_cast<const ForStmt *>(&stmt)) {
            TypeRef iterType = CheckExpr(*forStmt->iterable);
            PushScope(); // scope for the loop variable
            Symbol var;
            var.kind = Symbol::Kind::Var;
            var.name = forStmt->variable;
            var.location = forStmt->location;
            if (iterType.IsRange() && !iterType.inner.empty()) {
                var.type = iterType.inner[0];
            }
            else if (auto elemType = SliceElementType(iterType)) {
                var.type = *elemType;
            }
            else {
                var.type = TypeRef::MakeUnknown();
            }
            var.isMut = false;
            Define(var);
            if (!forStmt->label.empty()) {
                activeLabels.insert(forStmt->label);
            }
            ++loopDepth;
            CheckBlock(*forStmt->body); // CheckBlock pushes its own nested scope
            --loopDepth;
            if (!forStmt->label.empty()) {
                activeLabels.erase(forStmt->label);
            }
            PopScope();
        }
        else if (auto *matchStmt = dynamic_cast<const MatchStmt *>(&stmt)) {
            CheckExpr(*matchStmt->subject);
            for (const auto &arm : matchStmt->arms) {
                PushScope(); // each arm has its own binding scope
                CheckPattern(*arm.pattern);
                CheckExpr(*arm.body);
                PopScope();
            }
        }
        else if (auto *retStmt = dynamic_cast<const ReturnStmt *>(&stmt)) {
            if (retStmt->value) {
                if (TypeRef valType = CheckExpr(**retStmt->value);
                    !valType.IsUnknown() && !currentReturnType.IsUnknown() && !currentReturnType.IsOpaque() &&
                    !CanAssignExprTo(**retStmt->value, valType, currentReturnType)) {
                    EmitError(retStmt->location,
                              AssignmentErrorMessage(**retStmt->value, currentReturnType,
                                                     std::format("return type mismatch: "
                                                                 "expected '{}', found '{}'",
                                                                 currentReturnType.ToString(), valType.ToString())));
                }
            }
            else if (!currentReturnType.IsOpaque() && !currentReturnType.IsUnknown()) {
                EmitError(retStmt->location,
                          std::format("missing return value; expected '{}'", currentReturnType.ToString()));
            }
        }
        else if (auto *breakStmt = dynamic_cast<const BreakStmt *>(&stmt)) {
            if (loopDepth == 0) {
                EmitError(stmt.location, "'break' outside of a loop");
            }
            else if (!breakStmt->label.empty() && !activeLabels.count(breakStmt->label)) {
                EmitError(stmt.location, std::format("unknown loop label '{}'", breakStmt->label));
            }
        }
        else if (auto *contStmt = dynamic_cast<const ContinueStmt *>(&stmt)) {
            if (loopDepth == 0) {
                EmitError(stmt.location, "'continue' outside of a loop");
            }
            else if (!contStmt->label.empty() && !activeLabels.count(contStmt->label)) {
                EmitError(stmt.location, std::format("unknown loop label '{}'", contStmt->label));
            }
        }
        else if (auto *declStmt = dynamic_cast<const DeclStmt *>(&stmt)) {
            CollectDecl(*declStmt->decl, *currentScope);
            CheckDecl(*declStmt->decl);
        }
    }

    void CheckLetPattern(const Pattern &pat, const TypeRef &type, bool isMut) {
        if (auto *identPat = dynamic_cast<const IdentPattern *>(&pat)) {
            Symbol sym;
            sym.kind = Symbol::Kind::Var;
            sym.name = identPat->name;
            sym.location = identPat->location;
            sym.type = type;
            sym.isMut = isMut;
            Define(sym);
        }
        else if (dynamic_cast<const WildcardPattern *>(&pat)) {}
        else if (auto *tuplePat = dynamic_cast<const TuplePattern *>(&pat)) {
            if (type.kind != TypeRef::Kind::Tuple) {
                if (!type.IsUnknown()) {
                    EmitError(tuplePat->location,
                              std::format("cannot destructure non-tuple type '{}'", type.ToString()));
                }
                for (const auto &elem : tuplePat->elements) {
                    CheckLetPattern(*elem, TypeRef::MakeUnknown(), isMut);
                }
                return;
            }

            if (tuplePat->elements.size() != type.inner.size()) {
                EmitError(tuplePat->location,
                          std::format("tuple pattern has {} elements but "
                                      "type '{}' has {}",
                                      tuplePat->elements.size(), type.ToString(), type.inner.size()));
            }

            const std::size_t n = std::min(tuplePat->elements.size(), type.inner.size());
            for (std::size_t i = 0; i < n; ++i) {
                CheckLetPattern(*tuplePat->elements[i], type.inner[i], isMut);
            }
            for (std::size_t i = n; i < tuplePat->elements.size(); ++i) {
                CheckLetPattern(*tuplePat->elements[i], TypeRef::MakeUnknown(), isMut);
            }
        }
        else {
            EmitError(pat.location, "unsupported pattern in let binding");
            CheckPattern(pat);
        }
    }

    void CheckPattern(const Pattern &pat) {
        if (auto *identPat = dynamic_cast<const IdentPattern *>(&pat)) {
            Symbol sym;
            sym.kind = Symbol::Kind::Var;
            sym.name = identPat->name;
            sym.location = identPat->location;
            sym.type = TypeRef::MakeUnknown();
            sym.isMut = false;
            Define(sym);
        }
        else if (auto *guardPat = dynamic_cast<const GuardedPattern *>(&pat)) {
            CheckPattern(*guardPat->inner);
            CheckExpr(*guardPat->guard);
        }
        else if (auto *rangePat = dynamic_cast<const RangePattern *>(&pat)) {
            CheckPattern(*rangePat->lo);
            CheckPattern(*rangePat->hi);
        }
        else if (auto *tuplePat = dynamic_cast<const TuplePattern *>(&pat)) {
            for (const auto &e : tuplePat->elements) {
                CheckPattern(*e);
            }
        }
        else if (auto *structPat = dynamic_cast<const StructPattern *>(&pat)) {
            if (!currentScope->Lookup(structPat->typeName)) {
                EmitError(structPat->location, std::format("unknown type '{}' in struct pattern", structPat->typeName));
            }
            for (const auto &f : structPat->fields) {
                CheckPattern(*f.pattern);
            }
        }
        else if (auto *enumPat = dynamic_cast<const EnumPattern *>(&pat)) {
            if (!enumPat->path.empty() && !currentScope->Lookup(enumPat->path[0])) {
                EmitError(enumPat->location, std::format("unknown name '{}' in enum pattern", enumPat->path[0]));
            }
            const EnumDecl::Variant *variant =
                enumPat->path.size() >= 2 ? LookupEnumVariant(enumPat->path[0], enumPat->path[1]) : nullptr;
            std::unordered_set<std::string> named;
            for (const auto &arg : enumPat->namedArgs) {
                if (!named.insert(arg.name).second) {
                    EmitError(arg.location, std::format("duplicate field '{}' in enum pattern", arg.name));
                    continue;
                }

                const EnumDecl::Variant::NamedField *field = nullptr;
                if (variant) {
                    for (const auto &candidate : variant->namedFields) {
                        if (candidate.name == arg.name) {
                            field = &candidate;
                            break;
                        }
                    }
                }

                if (field) {
                    CheckLetPattern(*arg.pattern, ResolveType(*field->type), false);
                }
                else {
                    if (variant) {
                        EmitError(arg.location, std::format("unknown field '{}' in enum pattern", arg.name));
                    }
                    CheckPattern(*arg.pattern);
                }
            }
            for (std::size_t i = 0; i < enumPat->args.size(); ++i) {
                if (variant && i < variant->fields.size()) {
                    CheckLetPattern(*enumPat->args[i], ResolveType(*variant->fields[i]), false);
                }
                else if (variant && i - variant->fields.size() < variant->namedFields.size()) {
                    CheckLetPattern(*enumPat->args[i],
                                    ResolveType(*variant->namedFields[i - variant->fields.size()].type), false);
                }
                else {
                    CheckPattern(*enumPat->args[i]);
                }
            }
        }
        // WildcardPattern, LiteralPattern: nothing to resolve
    }

    // Expressions
    TypeRef CheckExpr(const Expr &expr) {
        if (auto *e = dynamic_cast<const LiteralExpr *>(&expr)) {
            return LiteralType(e->token);
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
            TypeRef t = ResolveType(*e->type);
            if (!t.IsUnknown() && !SizeOfTypeExpr(*e->type)) {
                EmitError(e->location, std::format("cannot determine size of type '{}'", t.ToString()));
            }
            return TypeRef::MakeUInt64();
        }

        if (dynamic_cast<const IntrinsicExpr *>(&expr)) {
            const auto *e = static_cast<const IntrinsicExpr *>(&expr);
            using K = IntrinsicExpr::Kind;
            if (e->kind == K::Line || e->kind == K::Column) {
                return TypeRef::MakeUInt();
            }
            return TypeRef::MakeNamed(SliceTypeName(TypeRef::MakeChar8()));
        }

        if (auto *e = dynamic_cast<const UnaryExpr *>(&expr)) {
            if (e->op == TokenKind::PlusPlus || e->op == TokenKind::MinusMinus) {
                CheckMutability(*e->operand);
            }
            TypeRef t = CheckExpr(*e->operand);
            return CheckUnary(e->op, t, e->location);
        }

        if (auto *e = dynamic_cast<const PostfixExpr *>(&expr)) {
            CheckMutability(*e->operand);
            TypeRef t = CheckExpr(*e->operand);
            if (!t.IsUnknown() && !t.IsNumeric()) {
                EmitError(e->location, std::format("'{}' applied to non-numeric type '{}'",
                                                   e->op == TokenKind::PlusPlus ? "++" : "--", t.ToString()));
            }
            return t;
        }

        if (auto *e = dynamic_cast<const BinaryExpr *>(&expr)) {
            TypeRef l = CheckExpr(*e->left);
            TypeRef r = CheckExpr(*e->right);
            return CheckBinary(e->op, l, r, *e->left, *e->right, e->location);
        }

        if (auto *e = dynamic_cast<const AssignExpr *>(&expr)) {
            CheckMutability(*e->target);
            TypeRef tgt = CheckExpr(*e->target);
            TypeRef val = CheckExpr(*e->value);
            if (!tgt.IsUnknown() && !val.IsUnknown() && !CanAssignExprTo(*e->value, val, tgt)) {
                EmitError(e->location, AssignmentErrorMessage(
                                           *e->value, tgt,
                                           std::format("cannot assign '{}' to '{}'", val.ToString(), tgt.ToString())));
            }
            return TypeRef::MakeOpaque();
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
            if (!loType.IsUnknown() && !hiType.IsUnknown() && !loType.IsNumeric() && !hiType.IsNumeric()) {
                EmitError(e->location, "range operands must be numeric");
            }
            TypeRef elemType = loType.IsUnknown() ? hiType : loType;
            if (elemType.IsUnknown()) {
                elemType = TypeRef::MakeInt64();
            }
            return TypeRef::MakeRange(elemType);
        }

        if (auto *e = dynamic_cast<const CallExpr *>(&expr)) {
            if (auto *ident = dynamic_cast<const IdentExpr *>(e->callee.get())) {
                std::vector<TypeRef> argTypes;
                argTypes.reserve(e->args.size());
                for (const auto &arg : e->args) {
                    argTypes.push_back(CheckExpr(*arg));
                }

                if (Symbol *sym = currentScope->Lookup(ident->name);
                    sym && sym->kind == Symbol::Kind::Func && !sym->funcOverloads.empty()) {
                    const FuncDecl *decl = LookupFunctionOverload(*sym, argTypes);
                    if (!decl) {
                        std::string argList;
                        for (std::size_t i = 0; i < argTypes.size(); ++i) {
                            if (i > 0) {
                                argList += ", ";
                            }
                            argList += argTypes[i].ToString();
                        }
                        EmitError(e->location, std::format("no matching overload for '{}' "
                                                           "with argument types ({})",
                                                           ident->name, argList));
                        return TypeRef::MakeUnknown();
                    }
                    if (!decl->warnMessage.empty()) {
                        EmitWarning(e->location, decl->warnMessage);
                    }
                    if (!decl->errorMessage.empty()) {
                        EmitError(e->location, decl->errorMessage);
                    }
                    TypeRef funcType = FunctionType(*decl);
                    const std::size_t paramCount =
                        funcType.kind == TypeRef::Kind::Func && !funcType.inner.empty() ? funcType.inner.size() - 1 : 0;
                    const bool isVariadic = !decl->params.empty() && decl->params.back().isVariadic;
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
                        EmitError(e->location,
                                  std::format("function expects {} argument(s), got {}", paramCount, argTypes.size()));
                    }
                    else {
                        for (std::size_t i = 0; i < argTypes.size() && i < paramCount; ++i) {
                            const TypeRef &paramType = funcType.inner[i];
                            if (!argTypes[i].IsUnknown() && !paramType.IsUnknown() &&
                                !CanAssignExprTo(*e->args[i], argTypes[i], paramType)) {
                                EmitError(e->args[i]->location,
                                          std::format("cannot pass '{}' to "
                                                      "parameter of type '{}'",
                                                      argTypes[i].ToString(), paramType.ToString()));
                            }
                        }
                        if (isVariadic) {
                            const TypeRef varElemType = ResolveType(*decl->params.back().type);
                            const TypeRef sliceType = TypeRef::MakeNamed(SliceTypeName(varElemType));
                            const bool isSingleSpread = (argTypes.size() == paramCount + 1 &&
                                                         dynamic_cast<const SpreadExpr *>(e->args[paramCount].get()));
                            if (isSingleSpread) {
                                if (!argTypes[paramCount].IsUnknown() && !sliceType.IsUnknown() &&
                                    argTypes[paramCount] != sliceType) {
                                    EmitError(e->args[paramCount]->location,
                                              std::format("cannot spread '{}' to "
                                                          "variadic "
                                                          "parameter of type '{}'",
                                                          argTypes[paramCount].ToString(), varElemType.ToString()));
                                }
                            }
                            else {
                                for (std::size_t i = paramCount; i < argTypes.size(); ++i) {
                                    if (dynamic_cast<const SpreadExpr *>(e->args[i].get())) {
                                        EmitError(e->args[i]->location, "spread argument must be "
                                                                        "the only variadic "
                                                                        "argument");
                                    }
                                    else if (!argTypes[i].IsUnknown() && !varElemType.IsUnknown() &&
                                             !CanAssignExprTo(*e->args[i], argTypes[i], varElemType)) {
                                        EmitError(e->args[i]->location,
                                                  std::format("cannot pass '{}' to "
                                                              "variadic "
                                                              "parameter of type '{}'",
                                                              argTypes[i].ToString(), varElemType.ToString()));
                                    }
                                }
                            }
                        }
                    }
                    return funcType.inner.empty() ? TypeRef::MakeUnknown() : funcType.inner.back();
                }
            }

            if (auto *field = dynamic_cast<const FieldExpr *>(e->callee.get())) {
                TypeRef receiverType = CheckExpr(*field->object);
                std::vector<TypeRef> argTypes;
                argTypes.reserve(e->args.size());
                for (const auto &arg : e->args) {
                    argTypes.push_back(CheckExpr(*arg));
                }
                if (const FuncDecl *method = LookupMethod(receiverType, field->field, argTypes)) {
                    if (!method->warnMessage.empty()) {
                        EmitWarning(e->location, method->warnMessage);
                    }
                    if (!method->errorMessage.empty()) {
                        EmitError(e->location, method->errorMessage);
                    }
                    std::vector<TypeRef> paramTypes = ResolveMethodParamTypes(receiverType, *method);

                    if (argTypes.size() != paramTypes.size()) {
                        EmitError(e->location, std::format("function expects {} argument(s), got {}", paramTypes.size(),
                                                           argTypes.size()));
                    }
                    else {
                        for (std::size_t i = 0; i < argTypes.size(); ++i) {
                            const TypeRef &argType = argTypes[i];
                            const TypeRef &paramType = paramTypes[i];
                            if (!argType.IsUnknown() && !paramType.IsUnknown() &&
                                !CanAssignExprTo(*e->args[i], argType, paramType)) {
                                EmitError(e->args[i]->location, std::format("cannot pass '{}' to "
                                                                            "parameter of type '{}'",
                                                                            argType.ToString(), paramType.ToString()));
                            }
                        }
                    }

                    return ResolveMethodReturnType(receiverType, *method);
                }

                if (const FuncDecl *method = LookupInterfaceMethod(receiverType, field->field)) {
                    std::vector<TypeRef> paramTypes = ResolveInterfaceMethodParamTypes(*method);
                    const bool isVariadic = !method->params.empty() && method->params.back().isVariadic;
                    const bool arityOk =
                        isVariadic ? argTypes.size() >= paramTypes.size() : argTypes.size() == paramTypes.size();

                    if (!arityOk) {
                        EmitError(e->location, std::format("function expects {} argument(s), got {}", paramTypes.size(),
                                                           argTypes.size()));
                    }
                    else {
                        for (std::size_t i = 0; i < paramTypes.size(); ++i) {
                            const TypeRef &argType = argTypes[i];
                            const TypeRef &paramType = paramTypes[i];
                            if (!argType.IsUnknown() && !paramType.IsUnknown() &&
                                !CanAssignExprTo(*e->args[i], argType, paramType)) {
                                EmitError(e->args[i]->location, std::format("cannot pass '{}' to "
                                                                            "parameter of type '{}'",
                                                                            argType.ToString(), paramType.ToString()));
                            }
                        }

                        if (isVariadic) {
                            const TypeRef varElemType = ResolveType(*method->params.back().type);
                            for (std::size_t i = paramTypes.size(); i < argTypes.size(); ++i) {
                                if (!argTypes[i].IsUnknown() && !varElemType.IsUnknown() &&
                                    !CanAssignExprTo(*e->args[i], argTypes[i], varElemType)) {
                                    EmitError(e->args[i]->location,
                                              std::format("cannot pass '{}' to variadic "
                                                          "parameter of type '{}'",
                                                          argTypes[i].ToString(), varElemType.ToString()));
                                }
                            }
                        }
                    }

                    return ResolveInterfaceMethodReturnType(*method);
                }
            }

            if (auto *path = dynamic_cast<const PathExpr *>(e->callee.get())) {
                if (path->segments.size() == 2) {
                    Symbol *first = currentScope->Lookup(path->segments[0]);
                    if (first && (first->kind == Symbol::Kind::Type || first->kind == Symbol::Kind::Interface)) {
                        TypeRef receiverType = first->type.IsUnknown() ? TypeRef::MakeNamed(first->name) : first->type;
                        const std::string &methodName = path->segments[1];
                        std::vector<TypeRef> argTypes;
                        argTypes.reserve(e->args.size());
                        for (const auto &arg : e->args) {
                            argTypes.push_back(CheckExpr(*arg));
                        }
                        if (const FuncDecl *method = LookupMethod(receiverType, methodName, argTypes)) {
                            std::vector<TypeRef> paramTypes = ResolveMethodParamTypes(receiverType, *method);
                            if (argTypes.size() != paramTypes.size()) {
                                EmitError(e->location, std::format("function expects {} "
                                                                   "argument(s), got {}",
                                                                   paramTypes.size(), argTypes.size()));
                            }
                            else {
                                for (std::size_t i = 0; i < argTypes.size(); ++i) {
                                    const TypeRef &argType = argTypes[i];
                                    const TypeRef &paramType = paramTypes[i];
                                    if (!argType.IsUnknown() && !paramType.IsUnknown() &&
                                        !CanAssignExprTo(*e->args[i], argType, paramType)) {
                                        EmitError(e->args[i]->location,
                                                  std::format("cannot pass '{}' to "
                                                              "parameter of type '{}'",
                                                              argType.ToString(), paramType.ToString()));
                                    }
                                }
                            }
                            return ResolveMethodReturnType(receiverType, *method);
                        }
                    }
                }
            }

            TypeRef calleeType = CheckExpr(*e->callee);
            std::vector<TypeRef> argTypes;
            argTypes.reserve(e->args.size());
            for (const auto &arg : e->args) {
                argTypes.push_back(CheckExpr(*arg));
            }

            if (calleeType.kind == TypeRef::Kind::Func && !calleeType.inner.empty()) {
                const std::size_t paramCount = calleeType.inner.size() - 1;
                if (argTypes.size() != paramCount) {
                    EmitError(e->location,
                              std::format("function expects {} argument(s), got {}", paramCount, argTypes.size()));
                }
                else {
                    for (std::size_t i = 0; i < argTypes.size(); ++i) {
                        const TypeRef &argType = argTypes[i];
                        const TypeRef &paramType = calleeType.inner[i];
                        if (!argType.IsUnknown() && !paramType.IsUnknown() &&
                            !CanAssignExprTo(*e->args[i], argType, paramType)) {
                            EmitError(e->args[i]->location, std::format("cannot pass '{}' to "
                                                                        "parameter of type '{}'",
                                                                        argType.ToString(), paramType.ToString()));
                        }
                    }
                }
                return calleeType.inner.back();
            }
            return TypeRef::MakeUnknown();
        }

        if (auto *e = dynamic_cast<const IndexExpr *>(&expr)) {
            TypeRef obj = CheckExpr(*e->object);
            CheckExpr(*e->index);
            if (auto elemType = IndexElementType(obj)) {
                return *elemType;
            }
            return TypeRef::MakeUnknown();
        }

        if (auto *e = dynamic_cast<const FieldExpr *>(&expr)) {
            TypeRef obj = CheckExpr(*e->object);
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
                if (e->field == "lo" || e->field == "hi") {
                    return elemType;
                }
                if (e->field == "inclusive") {
                    return TypeRef::MakeBool();
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
            return TypeRef::MakeNamed(GenericStructInitName(*e));
        }

        if (auto *e = dynamic_cast<const SliceExpr *>(&expr)) {
            TypeRef elemType = TypeRef::MakeUnknown();
            for (const auto &el : e->elements) {
                TypeRef t = CheckExpr(*el);
                if (elemType.IsUnknown()) {
                    elemType = t;
                }
            }
            return TypeRef::MakeNamed(SliceTypeName(elemType));
        }

        if (auto *e = dynamic_cast<const TupleExpr *>(&expr)) {
            std::vector<TypeRef> elemTypes;
            for (const auto &el : e->elements) {
                elemTypes.push_back(CheckExpr(*el));
            }
            return TypeRef::MakeTuple(std::move(elemTypes));
        }

        if (auto *e = dynamic_cast<const CastExpr *>(&expr)) {
            TypeRef operandType = CheckExpr(*e->operand);
            TypeRef targetType = ResolveType(*e->type);
            if (const auto maxCodePoint = CharTypeMaxCodePoint(targetType);
                maxCodePoint && (operandType.IsInteger() || IsCharType(operandType))) {
                if (const auto value = EvalConstInt(*e->operand); value && *value < 0) {
                    EmitError(e->location,
                              std::format("constant value is out of range for type '{}'", targetType.ToString()));
                }
                else if (const auto charValue = EvalConstCharCastValue(*e->operand)) {
                    if (*charValue > *maxCodePoint) {
                        EmitError(e->location,
                                  std::format("constant value is out of range for type '{}'", targetType.ToString()));
                    }
                    else if (IsSurrogateCodePoint(*charValue)) {
                        EmitError(e->location, std::format("surrogate code point U+{:04X} cannot be converted to '{}'",
                                                           *charValue, targetType.ToString()));
                    }
                }
            }
            return targetType;
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
            CheckExpr(*e->subject);
            TypeRef resultType = TypeRef::MakeUnknown();
            for (const auto &arm : e->arms) {
                PushScope();
                CheckPattern(*arm.pattern);
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

    TypeRef CheckUnary(TokenKind op, const TypeRef &t, SourceLocation loc) {
        if (t.IsUnknown()) {
            return TypeRef::MakeUnknown();
        }
        switch (op) {
        case TokenKind::Bang:
            if (!t.IsBool()) {
                EmitError(loc, std::format("'!' applied to non-bool type '{}'", t.ToString()));
            }
            return TypeRef::MakeBool();
        case TokenKind::Minus:
            if (!t.IsNumeric()) {
                EmitError(loc, std::format("unary '-' applied to non-numeric type '{}'", t.ToString()));
            }
            return t;
        case TokenKind::Tilde:
            if (!t.IsInteger() && !t.IsBool()) {
                EmitError(loc, std::format("'~' applied to non-integer type '{}'", t.ToString()));
            }
            return t;
        case TokenKind::Star:
            if (t.kind != TypeRef::Kind::Pointer) {
                EmitError(loc, std::format("'*' (dereference) applied to "
                                           "non-pointer type '{}'",
                                           t.ToString()));
            }
            return t.inner.empty() ? TypeRef::MakeUnknown() : t.inner[0];
        case TokenKind::Amp:
            return TypeRef::MakePointer(t);
        case TokenKind::PlusPlus:
        case TokenKind::MinusMinus:
            if (!t.IsNumeric()) {
                EmitError(loc, std::format("'{}' applied to non-numeric type '{}'",
                                           op == TokenKind::PlusPlus ? "++" : "--", t.ToString()));
            }
            return t;
        default:
            return TypeRef::MakeUnknown();
        }
    }

    static std::string_view BinaryOperatorName(TokenKind op) noexcept {
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
        case TK::Amp:
            return "&";
        case TK::Pipe:
            return "|";
        case TK::Caret:
            return "^";
        case TK::LessLess:
            return "<<";
        case TK::GreaterGreater:
            return ">>";
        case TK::AmpAmp:
            return "&&";
        case TK::PipePipe:
            return "||";
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
        default:
            return {};
        }
    }

    TypeRef CheckBinary(TokenKind op, const TypeRef &l, const TypeRef &r, const Expr &leftExpr, const Expr &rightExpr,
                        SourceLocation loc) {
        if (l.IsUnknown() || r.IsUnknown()) {
            return TypeRef::MakeUnknown();
        }

        const std::string_view opName = BinaryOperatorName(op);
        if (!opName.empty()) {
            if (const FuncDecl *method = LookupMethod(l, std::string(opName), {r})) {
                std::vector<TypeRef> paramTypes = ResolveMethodParamTypes(l, *method);
                TypeRef ret = ResolveMethodReturnType(l, *method);
                if (paramTypes.size() != 1) {
                    EmitError(loc, std::format("operator '{}' expects 1 argument, got {}", opName, paramTypes.size()));
                }
                else if (!paramTypes[0].IsUnknown() && !CanAssignExprTo(rightExpr, r, paramTypes[0])) {
                    EmitError(rightExpr.location, std::format("cannot pass '{}' to parameter of type '{}'",
                                                              r.ToString(), paramTypes[0].ToString()));
                }
                return ret;
            }
        }

        auto isNumericOrChar = [](const TypeRef &t) {
            return t.IsNumeric() || t.kind == TypeRef::Kind::Char8 || t.kind == TypeRef::Kind::Char16 ||
                   t.kind == TypeRef::Kind::Char32;
        };
        auto isIntegerOrChar = [](const TypeRef &t) {
            return t.IsInteger() || t.kind == TypeRef::Kind::Char8 || t.kind == TypeRef::Kind::Char16 ||
                   t.kind == TypeRef::Kind::Char32;
        };
        auto isChar = [](TypeRef::Kind k) {
            return k == TypeRef::Kind::Char8 || k == TypeRef::Kind::Char16 || k == TypeRef::Kind::Char32;
        };
        auto getCompatibleType = [&](const Expr &left, const TypeRef &lt, const Expr &right,
                                     const TypeRef &rt) -> std::optional<TypeRef> {
            if ((lt.IsInteger() && isChar(rt.kind)) || (rt.IsInteger() && isChar(lt.kind))) {
                return lt.IsInteger() ? lt : rt;
            }
            if (isChar(lt.kind) && isChar(rt.kind)) {
                return lt;
            }
            if (lt.IsInteger() && rt.IsInteger()) {
                return lt;
            }
            if (CanAssignExprTo(right, rt, lt)) {
                return lt;
            }
            if (CanAssignExprTo(left, lt, rt)) {
                return rt;
            }
            return std::nullopt;
        };

        using TK = TokenKind;
        switch (op) {
        case TK::Plus: {
            if (l.kind == TypeRef::Kind::Pointer && isIntegerOrChar(r)) {
                return l;
            }
            if (isIntegerOrChar(l) && r.kind == TypeRef::Kind::Pointer) {
                return r;
            }
            if (!isNumericOrChar(l)) {
                EmitError(loc, std::format("'+' applied to non-numeric type '{}'", l.ToString()));
            }
            else if (!isNumericOrChar(r)) {
                EmitError(loc, std::format("'+' right operand must be numeric, got '{}'", r.ToString()));
            }
            else {
                auto res = getCompatibleType(leftExpr, l, rightExpr, r);
                if (!res.has_value()) {
                    EmitError(loc,
                              std::format("mismatched types in addition: '{}' and '{}'", l.ToString(), r.ToString()));
                }
                else {
                    return *res;
                }
            }
            return l;
        }

        case TK::Minus: {
            if (l.kind == TypeRef::Kind::Pointer && isIntegerOrChar(r)) {
                return l;
            }
            if (!isNumericOrChar(l)) {
                EmitError(loc, std::format("'-' applied to non-numeric type '{}'", l.ToString()));
            }
            else if (!isNumericOrChar(r)) {
                EmitError(loc, std::format("'-' right operand must be numeric, got '{}'", r.ToString()));
            }
            else {
                auto res = getCompatibleType(leftExpr, l, rightExpr, r);
                if (!res.has_value()) {
                    EmitError(loc, std::format("mismatched types in "
                                               "subtraction: '{}' and '{}'",
                                               l.ToString(), r.ToString()));
                }
                else {
                    return *res;
                }
            }
            return l;
        }

        case TK::Star:
        case TK::Slash:
        case TK::Percent:
        case TK::StarStar: {
            std::string opStr = op == TK::Star ? "*" : op == TK::Slash ? "/" : op == TK::Percent ? "%" : "**";
            if (!isNumericOrChar(l)) {
                EmitError(loc, std::format("'{}' applied to non-numeric type '{}'", opStr, l.ToString()));
            }
            else if (!isNumericOrChar(r)) {
                EmitError(loc, std::format("'{}' right operand must be numeric, got '{}'", opStr, r.ToString()));
            }
            else {
                auto res = getCompatibleType(leftExpr, l, rightExpr, r);
                if (!res.has_value()) {
                    EmitError(loc, std::format("mismatched types in binary "
                                               "operation: '{}' and '{}'",
                                               l.ToString(), r.ToString()));
                }
                else {
                    return *res;
                }
            }
            return l;
        }

        case TK::Amp:
        case TK::Pipe:
        case TK::Caret:
        case TK::LessLess:
        case TK::GreaterGreater: {
            auto isBitwiseOperand = [](const TypeRef &t) {
                return t.IsInteger() || t.IsBool() || t.kind == TypeRef::Kind::Char8 ||
                       t.kind == TypeRef::Kind::Char16 || t.kind == TypeRef::Kind::Char32;
            };
            if (!isBitwiseOperand(l)) {
                EmitError(loc, std::format("bitwise operator applied to non-integer type '{}'", l.ToString()));
            }
            else if (!isBitwiseOperand(r)) {
                EmitError(loc, std::format("bitwise operator right operand must "
                                           "be integer, got '{}'",
                                           r.ToString()));
            }
            else {
                auto res = getCompatibleType(leftExpr, l, rightExpr, r);
                if (!res.has_value()) {
                    EmitError(loc, std::format("mismatched types in bitwise "
                                               "operation: '{}' and '{}'",
                                               l.ToString(), r.ToString()));
                }
                else {
                    return *res;
                }
            }
            return l;
        }

        case TK::AmpAmp:
        case TK::PipePipe:
            if (!l.IsBool()) {
                EmitError(loc, std::format("'{}' applied to non-bool type '{}'", op == TK::AmpAmp ? "&&" : "||",
                                           l.ToString()));
            }
            if (!r.IsBool()) {
                EmitError(loc, std::format("'{}' applied to non-bool type '{}'", op == TK::AmpAmp ? "&&" : "||",
                                           r.ToString()));
            }
            return TypeRef::MakeBool();

        case TK::Equal:
        case TK::BangEqual:
        case TK::Less:
        case TK::LessEqual:
        case TK::Greater:
        case TK::GreaterEqual: {
            bool compat = false;
            if ((op == TK::Equal || op == TK::BangEqual) &&
                ((l.IsBool() && r.IsInteger()) || (l.IsInteger() && r.IsBool()))) {
                compat = true;
            }
            else if (getCompatibleType(leftExpr, l, rightExpr, r).has_value()) {
                compat = true;
            }
            if (!compat) {
                EmitError(loc,
                          std::format("cannot compare mismatched types '{}' and '{}'", l.ToString(), r.ToString()));
            }
            return TypeRef::MakeBool();
        }

        default:
            return TypeRef::MakeUnknown();
        }
    }

    // Check that an assignment target is mutable.
    void CheckMutability(const Expr &target) {
        if (auto *e = dynamic_cast<const IdentExpr *>(&target)) {
            Symbol *sym = currentScope->Lookup(e->name);
            if (!sym) {
                return;
            }
            if (sym->kind == Symbol::Kind::Const) {
                EmitError(target.location, std::format("cannot assign to constant '{}'", e->name));
                return;
            }
            if (sym->kind == Symbol::Kind::Var && !sym->isMut) {
                EmitError(target.location, std::format("cannot assign to immutable variable '{}'", e->name));
            }
        }
    }
};

// Sema public API
Sema::Sema(std::vector<const Module *> userModules, std::vector<DepPackage> deps, std::string packageName,
           std::string targetOs)
    : modules(std::move(userModules))
    , deps(std::move(deps))
    , packageName(std::move(packageName))
    , targetOs(std::move(targetOs)) {
}

SemaResult Sema::Analyze() {
    Analyzer analyzer(modules, deps, packageName, diags, symbols, targetOs);
    analyzer.Run();
    return SemaResult{std::move(diags), std::move(symbols)};
}
} // namespace Rux
