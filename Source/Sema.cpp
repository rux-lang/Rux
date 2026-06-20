// Copyright (c) Rux contributors.
// SPDX-License-Identifier: MIT

#include "Rux/Sema.h"

#include "Rux/Lexer.h"
#include "Rux/Platform/Host.h"
#include "Rux/Type.h"

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

bool TypeRef::IsAssignableTo(TypeRef const &other) const noexcept {
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
    if (other.kind == Kind::Int &&
        (kind == Kind::Int8 || kind == Kind::Int16 || kind == Kind::Int32)) {
        return true;
    }
    if (other.kind == Kind::UInt &&
        (kind == Kind::UInt8 || kind == Kind::UInt16 || kind == Kind::UInt32)) {
        return true;
    }
    // smaller fixed-width signed integers widen to larger signed integers
    if (other.kind == Kind::Int64 &&
        (kind == Kind::Int8 || kind == Kind::Int16 || kind == Kind::Int32)) {
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
    if (other.kind == Kind::UInt64 &&
        (kind == Kind::UInt8 || kind == Kind::UInt16 || kind == Kind::UInt32)) {
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
    if (kind == Kind::Pointer && other.kind == Kind::Pointer && !other.inner.empty() &&
        other.inner[0].IsOpaque()) {
        return true;
    }
    return false;
}

std::optional<std::uint64_t> TypeRef::SizeInBytes() const noexcept {
    auto alignUp = [](std::uint64_t const value, std::uint64_t const align) {
        return (value + align - 1) & ~(align - 1);
    };

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
        auto const elemSize = inner[0].SizeInBytes();
        if (!elemSize || *elemSize == 0) {
            return std::nullopt;
        }
        return alignUp(2 * *elemSize + 1, *elemSize);
    }
    case Kind::Tuple: {
        std::uint64_t offset = 0;
        std::uint64_t maxAlign = 1;
        for (auto const &elem : inner) {
            auto const elemSize = elem.SizeInBytes();
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
    case Kind::Named:
        if (!inner.empty()) {
            return inner[0].SizeInBytes();
        }
        if (name.starts_with("Slice<")) {
            return 16;
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

bool TypeRef::operator==(TypeRef const &o) const noexcept {
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

// SemaResult

bool SemaResult::HasErrors() const noexcept {
    return std::ranges::any_of(diagnostics, [](SemaDiagnostic const &d) {
        return d.severity == SemaDiagnostic::Severity::Error;
    });
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
    std::vector<FuncDecl const *> funcOverloads;
    std::vector<std::string> interfaceMethods; // for Interface kind
    Scope *moduleScope = nullptr;              // for Module kind: the imported module's scope
};

class Scope {
public:
    explicit Scope(Scope *parent = nullptr)
        : parent(parent) {
    }

    // Returns false and emits a diagnostic if the name is already defined.
    bool Define(Symbol sym, std::vector<SemaDiagnostic> &diags, std::string const &sourceName) {
        if (auto it = table.find(sym.name); it != table.end()) {
            if (it->second.kind == Symbol::Kind::Func && sym.kind == Symbol::Kind::Func) {
                it->second.funcOverloads.insert(it->second.funcOverloads.end(),
                                                sym.funcOverloads.begin(), sym.funcOverloads.end());
                if (it->second.type.IsUnknown() && !sym.type.IsUnknown()) {
                    it->second.type = std::move(sym.type);
                }
                return true;
            }
            diags.push_back(
                {SemaDiagnostic::Severity::Error, sourceName, sym.location,
                 std::format("'{}' is already defined (first defined at {}:{})", sym.name,
                             it->second.location.line, it->second.location.column)});
            return false;
        }
        table.emplace(sym.name, std::move(sym));
        return true;
    }

    Symbol *Lookup(std::string const &name) {
        auto it = table.find(name);
        if (it != table.end()) {
            return &it->second;
        }
        if (parent) {
            return parent->Lookup(name);
        }
        return nullptr;
    }

    Symbol *LookupLocal(std::string const &name) {
        auto it = table.find(name);
        return it == table.end() ? nullptr : &it->second;
    }

    [[nodiscard]] Scope *Parent() const {
        return parent;
    }

    [[nodiscard]] std::unordered_map<std::string, Symbol> const &Table() const {
        return table;
    }

private:
    Scope *parent;
    std::unordered_map<std::string, Symbol> table;
};

// Internal: Analyzer
class Analyzer {
public:
    Analyzer(std::vector<Module const *> &modules, std::vector<DepPackage> &deps,
             std::string const &packageName, std::vector<SemaDiagnostic> &diags,
             std::vector<SemaSymbol> &symbols, std::string const &targetOs)
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
                for (auto const &decl : entry.module->items) {
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
    std::vector<Module const *> &modules;
    std::vector<DepPackage> &deps;
    std::string const &packageName;
    std::vector<SemaDiagnostic> &diags;
    std::vector<SemaSymbol> &symbols;
    std::string const &targetOs;
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
    std::unordered_map<std::string, StructDecl const *> structDecls;
    std::unordered_map<std::string, EnumDecl const *> enumDecls;
    std::unordered_map<std::string, InterfaceDecl const *> interfaceDecls;
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<FuncDecl const *>>>
        methodsByType;
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

    // Builtins
    void RegisterBuiltins() {
        auto add = [&](char const *name, TypeRef t) {
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
    void CollectModule(Module const &mod) {
        currentFile = mod.name;
        std::string const *selfPackageName = packageName.empty() ? nullptr : &packageName;
        for (auto const &decl : mod.items) {
            CollectDecl(*decl, globalScope, selfPackageName, "");
        }
    }

    void ResolveModuleSignatures(Module const &mod) {
        currentFile = mod.name;
        for (auto const &decl : mod.items) {
            ResolveDeclSignature(*decl);
        }
    }

    void ResolveDeclSignature(Decl const &decl) {
        if (!DeclMatchesTarget(decl)) {
            return;
        }
        if (auto *d = dynamic_cast<FuncDecl const *>(&decl)) {
            if (Symbol *sym = globalScope.Lookup(d->name)) {
                sym->type = MakeFuncType(d->params, d->returnType, d->typeParams);
            }
        }
        else if (auto *d = dynamic_cast<ExternFuncDecl const *>(&decl)) {
            if (Symbol *sym = globalScope.Lookup(d->name)) {
                sym->type = MakeFuncType(d->params, d->returnType);
            }
        }
        else if (auto *d = dynamic_cast<ExternBlockDecl const *>(&decl)) {
            for (auto const &item : d->items) {
                ResolveDeclSignature(*item);
            }
        }
        else if (auto *d = dynamic_cast<ModuleDecl const *>(&decl)) {
            Scope &moduleScope = ModuleScopeFor(d->name, globalScope);
            for (auto const &item : d->items) {
                ResolveDeclSignatureInScope(*item, moduleScope);
            }
        }
    }

    void ResolveModuleSignaturesInScope(Module const &mod, Scope &scope) {
        Scope *savedScope = currentScope;
        currentScope = &scope;
        currentFile = mod.name;
        for (auto const &decl : mod.items) {
            ResolveDeclSignatureInScope(*decl, scope);
        }
        currentScope = savedScope;
    }

    void ResolveDeclSignatureInScope(Decl const &decl, Scope &scope) {
        if (!DeclMatchesTarget(decl)) {
            return;
        }
        if (auto *d = dynamic_cast<FuncDecl const *>(&decl)) {
            if (Symbol *sym = scope.Lookup(d->name)) {
                sym->type = MakeFuncType(d->params, d->returnType, d->typeParams);
            }
        }
        else if (auto *d = dynamic_cast<ExternFuncDecl const *>(&decl)) {
            if (Symbol *sym = scope.Lookup(d->name)) {
                sym->type = MakeFuncType(d->params, d->returnType);
            }
        }
        else if (auto *d = dynamic_cast<ExternBlockDecl const *>(&decl)) {
            for (auto const &item : d->items) {
                ResolveDeclSignatureInScope(*item, scope);
            }
        }
        else if (auto *d = dynamic_cast<ModuleDecl const *>(&decl)) {
            Scope &moduleScope = ModuleScopeFor(d->name, scope);
            for (auto const &item : d->items) {
                ResolveDeclSignatureInScope(*item, moduleScope);
            }
        }
    }

    void ApplyModuleImports(Module const &mod) {
        currentFile = mod.name;
        for (auto const &decl : mod.items) {
            ApplyDeclImports(*decl);
        }
    }

    void ApplyModuleImportsInScope(Module const &mod, Scope &scope) {
        Scope *savedScope = currentScope;
        currentScope = &scope;
        ApplyModuleImports(mod);
        currentScope = savedScope;
    }

    void ApplyDeclImports(Decl const &decl) {
        if (!DeclMatchesTarget(decl)) {
            return;
        }
        if (auto *d = dynamic_cast<UseDecl const *>(&decl)) {
            CheckUseDecl(*d);
        }
        else if (auto *d = dynamic_cast<ModuleDecl const *>(&decl)) {
            Scope *savedScope = currentScope;
            currentScope = &ModuleScopeFor(d->name, *currentScope);
            for (auto const &item : d->items) {
                ApplyDeclImports(*item);
            }
            currentScope = savedScope;
        }
    }

    static std::string JoinModulePath(std::string const &prefix, std::string const &name) {
        if (prefix.empty()) {
            return name;
        }
        return prefix + "::" + name;
    }

    Scope &ModuleScopeFor(std::string const &name, Scope &parent) {
        if (Symbol *sym = parent.Lookup(name);
            sym && sym->kind == Symbol::Kind::Module && sym->moduleScope) {
            return *sym->moduleScope;
        }
        return parent;
    }

    void CollectDecl(Decl const &decl, Scope &scope, std::string const *packageName = nullptr,
                     std::string const &modulePath = "") {
        if (!DeclMatchesTarget(decl)) {
            return;
        }
        // Records the symbol in `scope` and, for top-level (global) scope,
        // also appends a SemaSymbol to `symbols_` for the dump.
        bool isGlobal = (&scope == &globalScope);

        auto simple = [&](Symbol::Kind kind, std::string const &name, SemaSymbol::Kind pubKind,
                          std::string resolvedType = {}, bool isMut = false) {
            Symbol sym;
            sym.kind = kind;
            sym.name = name;
            sym.location = decl.location;
            sym.isMut = isMut;
            if (scope.Define(sym, diags, currentFile) && isGlobal) {
                symbols.push_back(
                    {pubKind, name, currentFile, decl.location, std::move(resolvedType), isMut});
            }
        };

        if (auto *d = dynamic_cast<FuncDecl const *>(&decl)) {
            Symbol sym;
            sym.kind = Symbol::Kind::Func;
            sym.name = d->name;
            sym.location = d->location;
            sym.funcOverloads.push_back(d);
            if (scope.Define(sym, diags, currentFile) && isGlobal) {
                symbols.push_back(
                    {SemaSymbol::Kind::Func, d->name, currentFile, d->location, {}, false});
            }
        }
        else if (auto *d = dynamic_cast<StructDecl const *>(&decl)) {
            structDecls[d->name] = d;
            simple(Symbol::Kind::Type, d->name, SemaSymbol::Kind::Type, "struct");
        }
        else if (auto *d = dynamic_cast<EnumDecl const *>(&decl)) {
            enumDecls[d->name] = d;
            simple(Symbol::Kind::Type, d->name, SemaSymbol::Kind::Type, "enum");
        }
        else if (auto *d = dynamic_cast<UnionDecl const *>(&decl)) {
            simple(Symbol::Kind::Type, d->name, SemaSymbol::Kind::Type, "union");
        }
        else if (auto *d = dynamic_cast<InterfaceDecl const *>(&decl)) {
            interfaceDecls[d->name] = d;
            Symbol sym;
            sym.kind = Symbol::Kind::Interface;
            sym.name = d->name;
            sym.location = d->location;
            for (auto &m : d->methods) {
                sym.interfaceMethods.push_back(m->name);
            }
            if (scope.Define(sym, diags, currentFile) && isGlobal) {
                symbols.push_back(
                    {SemaSymbol::Kind::Interface, d->name, currentFile, d->location, "interface"});
            }
        }
        else if (auto *d = dynamic_cast<ConstDecl const *>(&decl)) {
            Symbol sym;
            sym.kind = Symbol::Kind::Const;
            sym.name = d->name;
            sym.location = d->location;
            if (d->type) {
                sym.type = ResolveType(*d->type->get());
            }
            if (scope.Define(sym, diags, currentFile) && isGlobal) {
                symbols.push_back({SemaSymbol::Kind::Const, d->name, currentFile, d->location,
                                   sym.type.IsUnknown() ? "" : sym.type.ToString(), false});
            }
        }
        else if (auto *d = dynamic_cast<TypeAliasDecl const *>(&decl)) {
            Symbol sym;
            sym.kind = Symbol::Kind::Type;
            sym.name = d->name;
            sym.location = d->location;
            sym.type = ResolveType(*d->type);
            if (scope.Define(sym, diags, currentFile) && isGlobal) {
                symbols.push_back({SemaSymbol::Kind::Type, d->name, currentFile, d->location,
                                   sym.type.IsUnknown() ? "" : sym.type.ToString(), false});
            }
        }
        else if (auto *d = dynamic_cast<ExternFuncDecl const *>(&decl)) {
            simple(Symbol::Kind::Func, d->name, SemaSymbol::Kind::Func, "extern");
        }
        else if (auto *d = dynamic_cast<ExternVarDecl const *>(&decl)) {
            Symbol sym;
            sym.kind = Symbol::Kind::Var;
            sym.name = d->name;
            sym.location = d->location;
            sym.isMut = true;
            if (scope.Define(sym, diags, currentFile) && isGlobal) {
                symbols.push_back(
                    {SemaSymbol::Kind::Var, d->name, currentFile, d->location, "extern", true});
            }
        }
        else if (auto *d = dynamic_cast<ExternBlockDecl const *>(&decl)) {
            for (auto &item : d->items) {
                CollectDecl(*item, scope, packageName, modulePath);
            }
        }
        else if (auto *d = dynamic_cast<ModuleDecl const *>(&decl)) {
            Scope *moduleScopePtr = nullptr;
            if (Symbol *existing = scope.Lookup(d->name);
                existing && existing->kind == Symbol::Kind::Module && existing->moduleScope) {
                moduleScopePtr = existing->moduleScope;
            }
            else {
                auto moduleScope = std::make_unique<Scope>(&scope);
                moduleScopePtr = moduleScope.get();
                ownedScopes.push_back(std::move(moduleScope));

                Symbol sym;
                sym.kind = Symbol::Kind::Module;
                sym.name = d->name;
                sym.location = decl.location;
                sym.moduleScope = moduleScopePtr;
                if (scope.Define(sym, diags, currentFile) && isGlobal) {
                    symbols.push_back(
                        {SemaSymbol::Kind::Module, d->name, currentFile, decl.location, {}, false});
                }
            }

            std::string const childPath = JoinModulePath(modulePath, d->name);
            if (packageName) {
                packageModuleScopes[*packageName][childPath] = moduleScopePtr;
            }
            for (auto &item : d->items) {
                CollectDecl(*item, *moduleScopePtr, packageName, childPath);
            }
        }
        else if (auto *d = dynamic_cast<ImplDecl const *>(&decl)) {
            for (auto const &method : d->methods) {
                methodsByType[d->typeName][method->name].push_back(method.get());
            }
            if (d->interfaceName) {
                typeImplementsInterfaces[d->typeName].insert(*d->interfaceName);
            }
        }
        // Import declarations don't add names in the first pass.
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

    static std::optional<std::uint64_t> ParseIntegerLiteralValue(Token const &tok) {
        if (tok.kind != TokenKind::IntLiteral) {
            return std::nullopt;
        }

        std::string text;
        text.reserve(tok.text.size());
        for (char const c : tok.text) {
            if (c != '_') {
                text.push_back(c);
            }
        }

        std::string const suffix = NumericLiteralSuffix(text);
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
        auto const *first = digits.data();
        auto const *last = first + digits.size();
        auto const [ptr, ec] = std::from_chars(first, last, value, base);
        if (ec != std::errc{} || ptr != last) {
            return std::nullopt;
        }
        return value;
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

    static bool IsUnsuffixedIntegerLiteral(Expr const &expr) {
        LiteralExpr const *literal = dynamic_cast<LiteralExpr const *>(&expr);
        if (!literal) {
            auto const *unary = dynamic_cast<UnaryExpr const *>(&expr);
            if (!unary || unary->op != TokenKind::Minus) {
                return false;
            }
            literal = dynamic_cast<LiteralExpr const *>(unary->operand.get());
        }
        return literal && literal->token.kind == TokenKind::IntLiteral &&
               NumericLiteralSuffix(literal->token.text).empty();
    }

    static bool IsIntegerLiteralOutOfRangeFor(Expr const &expr, TypeRef const &targetType) {
        return targetType.IsInteger() && IsUnsuffixedIntegerLiteral(expr) &&
               !UnsuffixedIntegerLiteralFits(expr, targetType);
    }

    // Picks the diagnostic for a rejected assignment/conversion. An
    // unsuffixed integer literal that does not fit the target gets a
    // dedicated "out of range" message; everything else uses `fallback`.
    // Keeps the wording consistent across let, return, assignment, const,
    // and field positions.
    static std::string AssignmentErrorMessage(Expr const &expr, TypeRef const &targetType,
                                              std::string fallback) {
        if (IsIntegerLiteralOutOfRangeFor(expr, targetType)) {
            return std::format("integer literal is out of range for type '{}'",
                               targetType.ToString());
        }
        return fallback;
    }

    bool TypeImplementsInterface(TypeRef const &exprType, TypeRef const &targetType) const {
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
        auto implements = [&](TypeRef const &type) {
            std::string const typeName = type.ToString();
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
    static std::optional<std::int64_t> EvalConstInt(Expr const &expr) {
        using I = std::int64_t;
        using U = std::uint64_t;

        if (auto const *lit = dynamic_cast<LiteralExpr const *>(&expr)) {
            if (lit->token.kind != TokenKind::IntLiteral ||
                !NumericLiteralSuffix(lit->token.text).empty()) {
                return std::nullopt;
            }
            auto const v = ParseUnsuffixedIntegerLiteral(lit->token);
            if (!v || *v > static_cast<U>(std::numeric_limits<I>::max())) {
                return std::nullopt;
            }
            return static_cast<I>(*v);
        }

        if (auto const *un = dynamic_cast<UnaryExpr const *>(&expr)) {
            auto const v = EvalConstInt(*un->operand);
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

        if (auto const *bin = dynamic_cast<BinaryExpr const *>(&expr)) {
            auto const l = EvalConstInt(*bin->left);
            auto const r = EvalConstInt(*bin->right);
            if (!l || !r) {
                return std::nullopt;
            }
            U const lu = static_cast<U>(*l);
            U const ru = static_cast<U>(*r);
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

    static bool ConstantFitsTarget(std::int64_t value, TypeRef const &target) {
        if (auto const max = UnsignedIntegerMax(target)) {
            return value >= 0 && static_cast<std::uint64_t>(value) <= *max;
        }
        if (auto const range = SignedIntegerRange(target)) {
            return value >= range->first && value <= range->second;
        }
        return false;
    }

    static std::optional<std::uint32_t> CharTypeMaxCodePoint(TypeRef const &type) {
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

    static bool IsCharType(TypeRef const &type) noexcept {
        switch (type.kind) {
        case TypeRef::Kind::Char8:
        case TypeRef::Kind::Char16:
        case TypeRef::Kind::Char32:
            return true;
        default:
            return false;
        }
    }

    static bool IsSurrogateCodePoint(std::uint64_t const value) noexcept {
        return value >= 0xD800 && value <= 0xDFFF;
    }

    static std::optional<std::uint64_t> EvalConstCharCastValue(Expr const &expr) {
        if (auto const *literal = dynamic_cast<LiteralExpr const *>(&expr)) {
            if (literal->token.kind == TokenKind::CharLiteral) {
                if (auto const codePoint = Lexer::DecodeCharLiteralCodePoint(literal->token.text)) {
                    return static_cast<std::uint64_t>(*codePoint);
                }
            }
            if (literal->token.kind == TokenKind::IntLiteral) {
                if (auto const value = ParseIntegerLiteralValue(literal->token)) {
                    return *value;
                }
            }
        }

        if (auto const value = EvalConstInt(expr)) {
            if (*value >= 0) {
                return static_cast<std::uint64_t>(*value);
            }
        }

        return std::nullopt;
    }

    bool CanAssignExprTo(Expr const &expr, TypeRef const &exprType,
                         TypeRef const &targetType) const {
        if (targetType.IsInteger() && IsUnsuffixedIntegerLiteral(expr)) {
            return UnsuffixedIntegerLiteralFits(expr, targetType);
        }

        // A constant integer expression (e.g. 10 + 2 * (5 - 3)) coerces to
        // any integer type it fits in, the same way a bare literal does.
        if (targetType.IsInteger()) {
            if (auto const folded = EvalConstInt(expr);
                folded && ConstantFitsTarget(*folded, targetType)) {
                return true;
            }
        }

        return exprType.IsAssignableTo(targetType) ||
               (IsNullLiteral(expr) && targetType.kind == TypeRef::Kind::Pointer) ||
               UnsuffixedIntegerLiteralFits(expr, targetType) ||
               TypeImplementsInterface(exprType, targetType);
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
        // Helper to resolve enums from the global declaration table
        auto ResolveEnumType = [&](std::string const &name) -> TypeRef {
            if (auto const it = enumDecls.find(name); it != enumDecls.end()) {
                return EnumType(*it->second);
            }
            return TypeRef::MakeUnknown();
        };

        if (auto const *t = dynamic_cast<NamedTypeExpr const *>(&expr)) {
            for (auto const &tp : currentTypeParams) {
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
            for (auto const &argExpr : t->typeArgs) {
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

            TypeRef enumType = ResolveEnumType(t->name);
            if (!enumType.IsUnknown()) {
                if (!t->typeArgs.empty()) {
                    EmitError(expr.location,
                              std::format("Enum '{}' cannot take type arguments", t->name));
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

        if (auto const *t = dynamic_cast<PathTypeExpr const *>(&expr)) {
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

        if (auto const *t = dynamic_cast<PointerTypeExpr const *>(&expr)) {
            TypeRef pointeeType = ResolveType(*t->pointee);
            if (pointeeType.IsUnknown()) {
                return TypeRef::MakeUnknown();
            }
            return TypeRef::MakePointer(pointeeType);
        }

        if (auto const *t = dynamic_cast<SliceTypeExpr const *>(&expr)) {
            TypeRef elemType = ResolveType(*t->element);
            if (elemType.IsUnknown()) {
                return TypeRef::MakeUnknown();
            }
            return TypeRef::MakeNamed(SliceTypeName(elemType));
        }

        if (auto const *t = dynamic_cast<TupleTypeExpr const *>(&expr)) {
            std::vector<TypeRef> elems;
            elems.reserve(t->elements.size());

            for (auto const &e : t->elements) {
                TypeRef elem = ResolveType(*e);
                if (elem.IsUnknown()) {
                    return TypeRef::MakeUnknown();
                }
                elems.push_back(elem);
            }

            return TypeRef::MakeTuple(std::move(elems));
        }

        if (dynamic_cast<SelfTypeExpr const *>(&expr)) {
            return currentSelfType.IsUnknown() ? TypeRef::MakeNamed("self") : currentSelfType;
        }

        return TypeRef::MakeUnknown();
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

    [[nodiscard]] FuncDecl const *LookupMethod(TypeRef const &receiverType,
                                               std::string const &methodName,
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
            // Single overload: validate arity and assignability before
            // returning.
            auto const *decl = overloads[0];
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
        for (auto const *decl : overloads) {
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

    TypeRef ResolveMethodReturnType(TypeRef const &receiverType, FuncDecl const &method) {
        TypeRef savedSelfType = currentSelfType;
        currentSelfType = receiverType.kind == TypeRef::Kind::Pointer
                            ? receiverType
                            : TypeRef::MakePointer(receiverType);
        TypeRef ret =
            method.returnType ? ResolveType(*method.returnType->get()) : TypeRef::MakeOpaque();
        currentSelfType = savedSelfType;
        return ret;
    }

    std::vector<TypeRef> ResolveMethodParamTypes(TypeRef const &receiverType,
                                                 FuncDecl const &method) {
        TypeRef savedSelfType = currentSelfType;
        currentSelfType = receiverType.kind == TypeRef::Kind::Pointer
                            ? receiverType
                            : TypeRef::MakePointer(receiverType);
        std::vector<TypeRef> params;
        for (auto const &param : method.params) {
            if (param.isVariadic || param.name == "self") {
                continue;
            }
            params.push_back(ResolveType(*param.type));
        }
        currentSelfType = savedSelfType;
        return params;
    }

    TypeRef AssociatedFunctionType(TypeRef const &receiverType, FuncDecl const &method) {
        TypeRef savedSelfType = currentSelfType;
        currentSelfType = receiverType.kind == TypeRef::Kind::Pointer
                            ? receiverType
                            : TypeRef::MakePointer(receiverType);
        TypeRef type = MakeFuncType(method.params, method.returnType, method.typeParams);
        currentSelfType = savedSelfType;
        return type;
    }

    [[nodiscard]] FuncDecl const *LookupInterfaceMethod(TypeRef const &receiverType,
                                                        std::string const &methodName) const {
        std::string const ifaceName = NamedBaseTypeName(receiverType);
        if (ifaceName.empty()) {
            return nullptr;
        }
        auto const ifaceIt = interfaceDecls.find(ifaceName);
        if (ifaceIt == interfaceDecls.end()) {
            return nullptr;
        }
        for (auto const &method : ifaceIt->second->methods) {
            if (method->name == methodName) {
                return method.get();
            }
        }
        return nullptr;
    }

    TypeRef ResolveInterfaceMethodReturnType(FuncDecl const &method) {
        return method.returnType ? ResolveType(*method.returnType->get()) : TypeRef::MakeOpaque();
    }

    std::vector<TypeRef> ResolveInterfaceMethodParamTypes(FuncDecl const &method) {
        std::vector<TypeRef> params;
        for (auto const &param : method.params) {
            if (param.isVariadic) {
                continue;
            }
            params.push_back(ResolveType(*param.type));
        }
        return params;
    }

    FuncDecl const *LookupFunctionOverload(Symbol const &sym,
                                           std::vector<TypeRef> const &argTypes) {
        if (sym.kind != Symbol::Kind::Func || sym.funcOverloads.empty()) {
            return nullptr;
        }
        if (sym.funcOverloads.size() == 1) {
            // Single overload: still validate arity and assignability so
            // that Bar(wrongType) against a lone Bar(int32) returns null
            // and lets the caller emit a proper diagnostic.
            auto const *decl = sym.funcOverloads[0];
            TypeRef funcType = MakeFuncType(decl->params, decl->returnType, decl->typeParams);
            if (funcType.kind != TypeRef::Kind::Func || funcType.inner.empty()) {
                return decl;
            }
            std::size_t const paramCount = funcType.inner.size() - 1;
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
        for (bool const allowVariadic : {false, true}) {
            for (bool const exactOnly : {true, false}) {
                for (auto const *decl : sym.funcOverloads) {
                    TypeRef funcType =
                        MakeFuncType(decl->params, decl->returnType, decl->typeParams);
                    if (funcType.kind != TypeRef::Kind::Func || funcType.inner.empty()) {
                        continue;
                    }
                    std::size_t const paramCount = funcType.inner.size() - 1;
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
                        TypeRef const &paramType = funcType.inner[i];
                        if (argTypes[i].IsUnknown() || paramType.IsUnknown()) {
                            continue;
                        }
                        if (exactOnly ? !(argTypes[i] == paramType)
                                      : !argTypes[i].IsAssignableTo(paramType)) {
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

    TypeRef FunctionType(FuncDecl const &decl) {
        return MakeFuncType(decl.params, decl.returnType, decl.typeParams);
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

    // Second pass: check declarations
    void CheckModule(Module const &mod) {
        currentFile = mod.name;
        for (auto const &decl : mod.items) {
            CheckDecl(*decl);
        }
    }

    void CheckModuleInScope(Module const &mod, Scope &scope) {
        Scope *savedScope = currentScope;
        currentScope = &scope;
        CheckModule(mod);
        currentScope = savedScope;
    }

    void CheckDecl(Decl const &decl) {
        if (!DeclMatchesTarget(decl)) {
            return;
        }
        if (auto *d = dynamic_cast<FuncDecl const *>(&decl)) {
            CheckFuncDecl(*d);
        }
        else if (auto *d = dynamic_cast<StructDecl const *>(&decl)) {
            CheckStructDecl(*d);
        }
        else if (auto *d = dynamic_cast<EnumDecl const *>(&decl)) {
            CheckEnumDecl(*d);
        }
        else if (auto *d = dynamic_cast<UnionDecl const *>(&decl)) {
            CheckUnionDecl(*d);
        }
        else if (auto *d = dynamic_cast<InterfaceDecl const *>(&decl)) {
            CheckInterfaceDecl(*d);
        }
        else if (auto *d = dynamic_cast<ImplDecl const *>(&decl)) {
            CheckImplDecl(*d);
        }
        else if (auto *d = dynamic_cast<ModuleDecl const *>(&decl)) {
            CheckModuleDecl(*d);
        }
        else if (auto *d = dynamic_cast<ConstDecl const *>(&decl)) {
            CheckConstDecl(*d);
        }
        else if (auto *d = dynamic_cast<TypeAliasDecl const *>(&decl)) {
            ResolveType(*d->type); // triggers unknown-type errors
        }
        else if (auto *d = dynamic_cast<ExternFuncDecl const *>(&decl)) {
            if (d->dll.empty()) {
                EmitError(d->location, std::format("extern function '{}' must specify a "
                                                   "source DLL via "
                                                   "@[Import(lib: \"dll.dll\")]",
                                                   d->name));
            }
            if (d->returnType) {
                ResolveType(*d->returnType->get());
            }
            for (auto &p : d->params) {
                if (!p.isVariadic) {
                    ResolveType(*p.type);
                }
            }
        }
        else if (auto *d = dynamic_cast<ExternVarDecl const *>(&decl)) {
            ResolveType(*d->type);
        }
        else if (auto *d = dynamic_cast<ExternBlockDecl const *>(&decl)) {
            for (auto &item : d->items) {
                CheckDecl(*item);
            }
        }
        else if (auto *d = dynamic_cast<UseDecl const *>(&decl)) {
            CheckUseDecl(*d);
        }
    }

    void CheckFuncDecl(FuncDecl const &d, bool isMethod = false) {
        auto savedTypeParams = currentTypeParams;
        currentTypeParams = d.typeParams;

        TypeRef retType = d.returnType ? ResolveType(*d.returnType->get()) : TypeRef::MakeOpaque();

        auto savedRet = currentReturnType;
        currentReturnType = retType;

        PushScope();

        for (auto const &tp : d.typeParams) {
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
        for (auto const &param : d.params) {
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
            sym.type = param.isVariadic
                         ? TypeRef::MakeNamed(SliceTypeName(ResolveType(*param.type)))
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
                                                                 defaultType.ToString(),
                                                                 paramType.ToString())));
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

    void CheckStructDecl(StructDecl const &d) {
        auto savedTypeParams = currentTypeParams;
        currentTypeParams = d.typeParams;

        PushScope();
        for (auto const &tp : d.typeParams) {
            Symbol sym;
            sym.kind = Symbol::Kind::Type;
            sym.name = tp;
            sym.type = TypeRef::MakeTypeParam(tp);
            Define(sym);
        }

        std::unordered_set<std::string> seen;
        for (auto const &field : d.fields) {
            if (!seen.insert(field.name).second) {
                EmitError(field.location,
                          std::format("duplicate field '{}' in struct '{}'", field.name, d.name));
            }
            ResolveType(*field.type);
        }

        PopScope();
        currentTypeParams = savedTypeParams;
    }

    void CheckStructInitExpr(StructInitExpr const &e) {
        auto structIt = structDecls.find(e.typeName);
        if (structIt == structDecls.end()) {
            if (auto const [enumDecl, variant] = LookupEnumVariantInitializer(e.typeName);
                enumDecl) {
                if (!variant) {
                    EmitError(e.location,
                              std::format("unknown enum variant '{}' in initializer", e.typeName));
                    for (auto const &f : e.fields) {
                        CheckExpr(*f.value);
                    }
                    return;
                }
                if (variant->namedFields.empty()) {
                    EmitError(e.location,
                              std::format("enum variant '{}' has no named fields", e.typeName));
                    for (auto const &f : e.fields) {
                        CheckExpr(*f.value);
                    }
                    return;
                }

                std::unordered_map<std::string, EnumDecl::Variant::NamedField const *> fieldMap;
                for (auto const &field : variant->namedFields) {
                    fieldMap.emplace(field.name, &field);
                }

                std::unordered_set<std::string> initialized;
                for (auto const &f : e.fields) {
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
                        EmitError(f.location,
                                  AssignmentErrorMessage(*f.value, fieldType,
                                                         std::format("cannot assign '{}' to "
                                                                     "field '{}' of type '{}'",
                                                                     valueType.ToString(), f.name,
                                                                     fieldType.ToString())));
                    }
                }

                for (auto const &field : variant->namedFields) {
                    if (!initialized.contains(field.name)) {
                        EmitError(e.location, std::format("missing field '{}' in "
                                                          "initializer for '{}'",
                                                          field.name, e.typeName));
                    }
                }
                return;
            }

            EmitError(e.location,
                      std::format("unknown type '{}' in struct initializer", e.typeName));
            for (auto const &f : e.fields) {
                CheckExpr(*f.value);
            }
            return;
        }

        StructDecl const &decl = *structIt->second;
        if (e.typeArgs.size() != decl.typeParams.size()) {
            EmitError(e.location,
                      std::format("struct '{}' expects {} type argument(s), got {}", e.typeName,
                                  decl.typeParams.size(), e.typeArgs.size()));
        }

        auto const substitutions = StructTypeSubstitutions(decl, e.typeArgs);
        std::unordered_map<std::string, StructDecl::Field const *> fieldMap;
        for (auto const &field : decl.fields) {
            fieldMap.emplace(field.name, &field);
        }

        std::unordered_set<std::string> initialized;
        for (auto const &f : e.fields) {
            TypeRef valueType = CheckExpr(*f.value);
            if (!initialized.insert(f.name).second) {
                EmitError(f.location, std::format("duplicate field '{}' in initializer for '{}'",
                                                  f.name, e.typeName));
                continue;
            }

            auto fieldIt = fieldMap.find(f.name);
            if (fieldIt == fieldMap.end()) {
                EmitError(f.location, std::format("unknown field '{}' in initializer for '{}'",
                                                  f.name, e.typeName));
                continue;
            }

            TypeRef fieldType = ResolveTypeWithSubstitution(*fieldIt->second->type, substitutions);
            if (!valueType.IsUnknown() && !fieldType.IsUnknown() &&
                !CanAssignExprTo(*f.value, valueType, fieldType)) {
                EmitError(f.location,
                          AssignmentErrorMessage(
                              *f.value, fieldType,
                              std::format("cannot assign '{}' to field '{}' of type '{}'",
                                          valueType.ToString(), f.name, fieldType.ToString())));
            }
        }

        for (auto const &field : decl.fields) {
            if (!initialized.contains(field.name)) {
                EmitError(e.location, std::format("missing field '{}' in initializer for '{}'",
                                                  field.name, e.typeName));
            }
        }
    }

    void CheckEnumDecl(EnumDecl const &d) {
        TypeRef const baseType = EnumBaseType(d);
        if (!baseType.IsUnknown() && !baseType.IsInteger()) {
            EmitError(d.location,
                      std::format("enum '{}' base type must be an integer type", d.name));
        }
        std::unordered_set<std::string> seen;
        for (auto const &variant : d.variants) {
            if (!seen.insert(variant.name).second) {
                EmitError(variant.location,
                          std::format("duplicate variant '{}' in enum '{}'", variant.name, d.name));
            }
            if (variant.discriminant && (!variant.fields.empty() || !variant.namedFields.empty())) {
                EmitError(variant.location, std::format("enum variant '{}::{}' cannot have "
                                                        "both fields and a discriminant",
                                                        d.name, variant.name));
            }
            for (auto const &f : variant.fields) {
                ResolveType(*f);
            }
            std::unordered_set<std::string> namedFields;
            for (auto const &f : variant.namedFields) {
                if (!namedFields.insert(f.name).second) {
                    EmitError(f.location,
                              std::format("duplicate field '{}' in enum variant '{}::{}'", f.name,
                                          d.name, variant.name));
                }
                ResolveType(*f.type);
            }
        }
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

    void CheckUnionDecl(UnionDecl const &d) {
        std::unordered_set<std::string> seen;
        for (auto const &field : d.fields) {
            if (!seen.insert(field.name).second) {
                EmitError(field.location,
                          std::format("duplicate field '{}' in union '{}'", field.name, d.name));
            }
            ResolveType(*field.type);
        }
    }

    void CheckInterfaceDecl(InterfaceDecl const &d) {
        std::unordered_set<std::string> seen;
        for (auto const &method : d.methods) {
            if (!seen.insert(method->name).second) {
                EmitError(method->location, std::format("duplicate method '{}' in interface '{}'",
                                                        method->name, d.name));
            }
            if (method->returnType) {
                ResolveType(**method->returnType);
            }
            for (auto const &p : method->params) {
                if (!p.isVariadic) {
                    ResolveType(*p.type);
                }
            }
        }
    }

    void CheckImplDecl(ImplDecl const &d) {
        if (!currentScope->Lookup(d.typeName)) {
            EmitError(d.location, std::format("extend for unknown type '{}'", d.typeName));
        }

        if (d.interfaceName) {
            Symbol *ifaceSym = currentScope->Lookup(*d.interfaceName);
            if (!ifaceSym || ifaceSym->kind != Symbol::Kind::Interface) {
                EmitError(d.location,
                          std::format("'{}' is not a known interface", *d.interfaceName));
            }
            else {
                std::unordered_set<std::string> implNames;
                for (auto const &m : d.methods) {
                    implNames.insert(m->name);
                }
                for (auto const &required : ifaceSym->interfaceMethods) {
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
        for (auto const &m : d.methods) {
            CheckFuncDecl(*m, /*isMethod=*/true);
        }
        currentSelfType = savedSelfType;
        inImpl = savedInImpl;
    }

    void CheckModuleDecl(ModuleDecl const &d) {
        Scope *savedScope = currentScope;
        currentScope = &ModuleScopeFor(d.name, *currentScope);
        for (auto const &item : d.items) {
            CheckDecl(*item);
        }
        currentScope = savedScope;
    }

    void CheckConstDecl(ConstDecl const &d) {
        TypeRef valueType = CheckExpr(*d.value);
        TypeRef constType = d.type ? ResolveType(*d.type->get()) : valueType;
        if (d.type && !valueType.IsUnknown() && !constType.IsUnknown() &&
            !CanAssignExprTo(*d.value, valueType, constType)) {
            EmitError(
                d.value->location,
                AssignmentErrorMessage(*d.value, constType,
                                       std::format("cannot assign '{}' to constant of type '{}'",
                                                   valueType.ToString(), constType.ToString())));
        }
        if (Symbol *sym = currentScope->Lookup(d.name)) {
            sym->type = constType;
        }
    }

    static std::string JoinPathSegments(std::vector<std::string> const &path, std::size_t first,
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

    static std::string ModulePathForImport(UseDecl const &d) {
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

    static std::string LogicalModulePathForImport(UseDecl const &d) {
        if (d.kind == UseDecl::Kind::Single) {
            if (d.path.size() <= 1) {
                return "";
            }
            return JoinPathSegments(d.path, 0, d.path.size() - 1);
        }
        return JoinPathSegments(d.path, 0, d.path.size());
    }

    struct ImportScope {
        std::unordered_map<std::string, Symbol> const *table = nullptr;
        std::string displayName;
    };

    static std::string ImportScopeDisplayName(std::string const &pkgName,
                                              std::string const &modulePath) {
        if (modulePath.empty()) {
            return std::format("package '{}'", pkgName);
        }
        return std::format("module '{}'", modulePath);
    }

    ImportScope ResolveImportScope(UseDecl const &d, std::string const &pkgName,
                                   std::string const &modulePath) {
        std::string const logicalModulePath = LogicalModulePathForImport(d);
        if (auto pkgIt = packageModuleScopes.find(pkgName); pkgIt != packageModuleScopes.end()) {
            if (auto modIt = pkgIt->second.find(modulePath); modIt != pkgIt->second.end()) {
                if (modulePath.empty() && !logicalModulePath.empty()) {
                    if (auto logicalIt = pkgIt->second.find(logicalModulePath);
                        logicalIt != pkgIt->second.end()) {
                        return {&logicalIt->second->Table(),
                                ImportScopeDisplayName(pkgName, logicalModulePath)};
                    }
                }
                return {&modIt->second->Table(), ImportScopeDisplayName(pkgName, modulePath)};
            }
        }

        Scope *matchedScope = nullptr;
        std::string matchedPackage;
        for (auto const &[candidatePackage, modules] : packageModuleScopes) {
            auto modIt = modules.find(logicalModulePath);
            if (modIt == modules.end()) {
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
            return {&matchedScope->Table(),
                    ImportScopeDisplayName(matchedPackage, logicalModulePath)};
        }

        if (!packageModuleScopes.contains(pkgName)) {
            EmitError(d.location, std::format("unknown package or module '{}'", pkgName));
        }
        else {
            EmitError(d.location,
                      std::format("module '{}' not found in package '{}'", modulePath, pkgName));
        }
        return {};
    }

    void PromoteFromPackage(UseDecl const &d, std::string const &pkgName, std::string const &name) {
        std::string const modulePath = ModulePathForImport(d);
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

    void DefineImportedSymbol(Symbol const &sym) {
        if (Symbol *existing = currentScope->LookupLocal(sym.name)) {
            if (existing->kind == sym.kind && existing->location.line == sym.location.line &&
                existing->location.column == sym.location.column) {
                *existing = sym;
                return;
            }
        }
        currentScope->Define(sym, diags, currentFile);
    }

    void ImportSignatureDependencies(Symbol const &sym,
                                     std::unordered_map<std::string, Symbol> const &sourceTable) {
        if (sym.kind != Symbol::Kind::Func) {
            return;
        }

        auto importNamedType = [&](std::string const &name) {
            if (currentScope->Lookup(name)) {
                return;
            }
            auto depIt = sourceTable.find(name);
            if (depIt == sourceTable.end()) {
                return;
            }
            if (depIt->second.kind == Symbol::Kind::Type ||
                depIt->second.kind == Symbol::Kind::Interface) {
                DefineImportedSymbol(depIt->second);
            }
        };

        auto visitType = [&](this auto &&self, TypeExpr const &type) -> void {
            if (auto const *named = dynamic_cast<NamedTypeExpr const *>(&type)) {
                importNamedType(named->name);
                for (auto const &arg : named->typeArgs) {
                    self(*arg);
                }
            }
            else if (auto const *ptr = dynamic_cast<PointerTypeExpr const *>(&type)) {
                self(*ptr->pointee);
            }
            else if (auto const *slice = dynamic_cast<SliceTypeExpr const *>(&type)) {
                self(*slice->element);
            }
            else if (auto const *tuple = dynamic_cast<TupleTypeExpr const *>(&type)) {
                for (auto const &elem : tuple->elements) {
                    self(*elem);
                }
            }
        };

        for (auto const *overload : sym.funcOverloads) {
            for (auto const &param : overload->params) {
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

    [[nodiscard]] bool DeclMatchesTarget(Decl const &d) const {
        return d.targetOs.empty() || d.targetOs == EffectiveOs();
    }

    void CheckUseDecl(UseDecl const &d) {
        if (!DeclMatchesTarget(d)) {
            return;
        }
        if (d.path.empty()) {
            EmitError(d.location, "empty import path");
            return;
        }
        std::string const &pkgName = d.path[0];

        if (d.kind == UseDecl::Kind::Single) {
            if (d.path.size() < 2) {
                EmitError(d.location, std::format("import '{}' must name at least one "
                                                  "item (e.g. import {}::Name)",
                                                  pkgName, pkgName));
                return;
            }
            std::string const &name = d.path.back();
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
            for (auto const &name : d.names) {
                PromoteFromPackage(d, pkgName, name);
            }
        }
        else // Glob: promote all from the specific module (or all modules
             // if Pkg::*)
        {
            std::string const modulePath = ModulePathForImport(d);
            ImportScope scope = ResolveImportScope(d, pkgName, modulePath);
            if (!scope.table) {
                return;
            }
            for (auto const &[name, sym] : *scope.table) {
                DefineImportedSymbol(sym);
            }
        }
    }

    // Block & statements
    void CheckBlock(Block const &block) {
        PushScope();
        for (auto const &stmt : block.stmts) {
            CheckStmt(*stmt);
        }
        PopScope();
    }

    void CheckStmt(Stmt const &stmt) {
        if (auto *s = dynamic_cast<ExprStmt const *>(&stmt)) {
            CheckExpr(*s->expr);
        }
        else if (auto *s = dynamic_cast<LetStmt const *>(&stmt)) {
            TypeRef initType = s->init ? CheckExpr(*s->init) : TypeRef::MakeUnknown();
            TypeRef declType = s->type ? ResolveType(*s->type->get()) : initType;

            if (!s->init && !s->type) {
                EmitError(s->location, "uninitialized variable requires an explicit type");
            }

            if (!s->init && !s->isMut) {
                EmitError(s->location, "immutable variable requires an initializer");
            }

            if (!s->init && s->pattern) {
                EmitError(s->location, "destructuring declaration requires an initializer");
            }

            if (!s->type && declType.IsUnknown() && !s->pattern) {
                EmitWarning(s->location, std::format("cannot infer type of '{}'", s->name));
            }

            if (s->init && s->type && !initType.IsUnknown() && !declType.IsUnknown() &&
                !CanAssignExprTo(*s->init, initType, declType)) {
                EmitError(s->location, AssignmentErrorMessage(
                                           *s->init, declType,
                                           std::format("cannot assign '{}' to '{}'",
                                                       initType.ToString(), declType.ToString())));
            }

            if (s->pattern) {
                CheckLetPattern(*s->pattern, declType, s->isMut);
                return;
            }

                Symbol sym;
                sym.kind = Symbol::Kind::Var;
                sym.name = s->name;
                sym.location = s->location;
                sym.type = declType;
                sym.isMut = s->isMut;
                Define(sym);
            }
            else if (auto const *s = dynamic_cast<IfStmt const *>(&stmt)) {
                TypeRef cond = CheckExpr(*s->condition);
                if (!cond.IsUnknown() && !cond.IsBool()) {
                    EmitError(s->condition->location,
                              "if condition must be 'bool'");
                }
                CheckBlock(*s->thenBlock);
                for (auto const &elif : s->elseIfs) {
                    TypeRef elifCond = CheckExpr(*elif.condition);
                    if (!elifCond.IsUnknown() && !elifCond.IsBool()) {
                        EmitError(elif.condition->location,
                                  "if condition must be 'bool'");
                    }
                    CheckBlock(*elif.block);
                }
                if (s->elseBlock) {
                    CheckBlock(*s->elseBlock);
                }
            }
            else if (auto const *s = dynamic_cast<WhileStmt const *>(&stmt)) {
                if (!s->label.empty()) {
                    activeLabels.insert(s->label);
                }
                
                // FIX: Capture and validate the condition type
                TypeRef cond = CheckExpr(*s->condition);
                if (!cond.IsUnknown() && !cond.IsBool()) {
                    EmitError(s->condition->location, "while condition must be 'bool'");
                }
            
                ++loopDepth;
                CheckBlock(*s->body);
                --loopDepth;
                if (!s->label.empty()) {
                    activeLabels.erase(s->label);
                }
            }

        else if (auto const *s = dynamic_cast<DoWhileStmt const *>(&stmt)) {
            if (!s->label.empty()) {
                activeLabels.insert(s->label);
            }
            
            ++loopDepth;
            CheckBlock(*s->body);
            --loopDepth;

            TypeRef cond = CheckExpr(*s->condition);
            if (!cond.IsUnknown() && !cond.IsBool()) {
                EmitError(s->condition->location, "do-while condition must be 'bool'");
            }
        
            if (!s->label.empty()) {
                activeLabels.erase(s->label);
            }
        }
        else if (auto *s = dynamic_cast<LoopStmt const *>(&stmt)) {
            if (!s->label.empty()) {
                activeLabels.insert(s->label);
            }
            ++loopDepth;
            CheckBlock(*s->body);
            --loopDepth;
            if (!s->label.empty()) {
                activeLabels.erase(s->label);
            }
        }
        else if (auto *s = dynamic_cast<ForStmt const *>(&stmt)) {
            TypeRef iterType = CheckExpr(*s->iterable);
            PushScope(); // scope for the loop variable
            Symbol var;
            var.kind = Symbol::Kind::Var;
            var.name = s->variable;
            var.location = s->location;
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
            if (!s->label.empty()) {
                activeLabels.insert(s->label);
            }
            ++loopDepth;
            CheckBlock(*s->body); // CheckBlock pushes its own nested scope
            --loopDepth;
            if (!s->label.empty()) {
                activeLabels.erase(s->label);
            }
            PopScope();
        }
        else if (auto *s = dynamic_cast<MatchStmt const *>(&stmt)) {
            CheckExpr(*s->subject);
            for (auto const &arm : s->arms) {
                PushScope(); // each arm has its own binding scope
                CheckPattern(*arm.pattern);
                CheckExpr(*arm.body);
                PopScope();
            }
        }
        else if (auto *s = dynamic_cast<ReturnStmt const *>(&stmt)) {
            if (s->value) {
                if (TypeRef valType = CheckExpr(**s->value);
                    !valType.IsUnknown() && !currentReturnType.IsUnknown() &&
                    !currentReturnType.IsOpaque() &&
                    !CanAssignExprTo(**s->value, valType, currentReturnType)) {
                    EmitError(s->location,
                              AssignmentErrorMessage(**s->value, currentReturnType,
                                                     std::format("return type mismatch: "
                                                                 "expected '{}', found '{}'",
                                                                 currentReturnType.ToString(),
                                                                 valType.ToString())));
                }
            }
            else if (!currentReturnType.IsOpaque() && !currentReturnType.IsUnknown()) {
                EmitError(s->location, std::format("missing return value; expected '{}'",
                                                   currentReturnType.ToString()));
            }
        }
        else if (auto *s = dynamic_cast<BreakStmt const *>(&stmt)) {
            if (loopDepth == 0) {
                EmitError(stmt.location, "'break' outside of a loop");
            }
            else if (!s->label.empty() && !activeLabels.count(s->label)) {
                EmitError(stmt.location, std::format("unknown loop label '{}'", s->label));
            }
        }
        else if (auto *s = dynamic_cast<ContinueStmt const *>(&stmt)) {
            if (loopDepth == 0) {
                EmitError(stmt.location, "'continue' outside of a loop");
            }
            else if (!s->label.empty() && !activeLabels.count(s->label)) {
                EmitError(stmt.location, std::format("unknown loop label '{}'", s->label));
            }
        }
        else if (auto *s = dynamic_cast<DeclStmt const *>(&stmt)) {
            CollectDecl(*s->decl, *currentScope);
            CheckDecl(*s->decl);
        }
    }

    void CheckLetPattern(Pattern const &pat, TypeRef const &type, bool isMut) {
        if (auto *p = dynamic_cast<IdentPattern const *>(&pat)) {
            Symbol sym;
            sym.kind = Symbol::Kind::Var;
            sym.name = p->name;
            sym.location = p->location;
            sym.type = type;
            sym.isMut = isMut;
            Define(sym);
        }
        else if (dynamic_cast<WildcardPattern const *>(&pat)) {}
        else if (auto *p = dynamic_cast<TuplePattern const *>(&pat)) {
            if (type.kind != TypeRef::Kind::Tuple) {
                if (!type.IsUnknown()) {
                    EmitError(p->location, std::format("cannot destructure non-tuple type '{}'",
                                                       type.ToString()));
                }
                for (auto const &elem : p->elements) {
                    CheckLetPattern(*elem, TypeRef::MakeUnknown(), isMut);
                }
                return;
            }

            if (p->elements.size() != type.inner.size()) {
                EmitError(p->location,
                          std::format("tuple pattern has {} elements but "
                                      "type '{}' has {}",
                                      p->elements.size(), type.ToString(), type.inner.size()));
            }

            std::size_t const n = std::min(p->elements.size(), type.inner.size());
            for (std::size_t i = 0; i < n; ++i) {
                CheckLetPattern(*p->elements[i], type.inner[i], isMut);
            }
            for (std::size_t i = n; i < p->elements.size(); ++i) {
                CheckLetPattern(*p->elements[i], TypeRef::MakeUnknown(), isMut);
            }
        }
        else {
            EmitError(pat.location, "unsupported pattern in let binding");
            CheckPattern(pat);
        }
    }

    void CheckPattern(Pattern const &pat) {
        if (auto *p = dynamic_cast<IdentPattern const *>(&pat)) {
            Symbol sym;
            sym.kind = Symbol::Kind::Var;
            sym.name = p->name;
            sym.location = p->location;
            sym.type = TypeRef::MakeUnknown();
            sym.isMut = false;
            Define(sym);
        }
        else if (auto *p = dynamic_cast<GuardedPattern const *>(&pat)) {
            CheckPattern(*p->inner);
            CheckExpr(*p->guard);
        }
        else if (auto *p = dynamic_cast<RangePattern const *>(&pat)) {
            CheckPattern(*p->lo);
            CheckPattern(*p->hi);
        }
        else if (auto *p = dynamic_cast<TuplePattern const *>(&pat)) {
            for (auto const &e : p->elements) {
                CheckPattern(*e);
            }
        }
        else if (auto *p = dynamic_cast<StructPattern const *>(&pat)) {
            if (!currentScope->Lookup(p->typeName)) {
                EmitError(p->location,
                          std::format("unknown type '{}' in struct pattern", p->typeName));
            }
            for (auto const &f : p->fields) {
                CheckPattern(*f.pattern);
            }
        }
        else if (auto *p = dynamic_cast<EnumPattern const *>(&pat)) {
            if (!p->path.empty() && !currentScope->Lookup(p->path[0])) {
                EmitError(p->location,
                          std::format("unknown name '{}' in enum pattern", p->path[0]));
            }
            EnumDecl::Variant const *variant =
                p->path.size() >= 2 ? LookupEnumVariant(p->path[0], p->path[1]) : nullptr;
            std::unordered_set<std::string> named;
            for (auto const &arg : p->namedArgs) {
                if (!named.insert(arg.name).second) {
                    EmitError(arg.location,
                              std::format("duplicate field '{}' in enum pattern", arg.name));
                    continue;
                }

                EnumDecl::Variant::NamedField const *field = nullptr;
                if (variant) {
                    for (auto const &candidate : variant->namedFields) {
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
                        EmitError(arg.location,
                                  std::format("unknown field '{}' in enum pattern", arg.name));
                    }
                    CheckPattern(*arg.pattern);
                }
            }
            for (std::size_t i = 0; i < p->args.size(); ++i) {
                if (variant && i < variant->fields.size()) {
                    CheckLetPattern(*p->args[i], ResolveType(*variant->fields[i]), false);
                }
                else if (variant && i - variant->fields.size() < variant->namedFields.size()) {
                    CheckLetPattern(
                        *p->args[i],
                        ResolveType(*variant->namedFields[i - variant->fields.size()].type), false);
                }
                else {
                    CheckPattern(*p->args[i]);
                }
            }
        }
        // WildcardPattern, LiteralPattern: nothing to resolve
    }

    // Expressions
    TypeRef CheckExpr(Expr const &expr) {
        if (auto *e = dynamic_cast<LiteralExpr const *>(&expr)) {
            return LiteralType(e->token);
        }

        if (auto *e = dynamic_cast<IdentExpr const *>(&expr)) {
            Symbol *sym = currentScope->Lookup(e->name);
            if (sym) {
                return sym->type;
            }
            EmitError(e->location, std::format("undefined name '{}'", e->name));
            return TypeRef::MakeUnknown();
        }

        if (dynamic_cast<SelfExpr const *>(&expr)) {
            if (!inImpl) {
                EmitError(expr.location, "'self' used outside of an extend block");
            }
            return currentSelfType.IsUnknown() ? TypeRef::MakeNamed("self") : currentSelfType;
        }

        if (auto *e = dynamic_cast<PathExpr const *>(&expr)) {
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
                    std::string const &variantName = e->segments[1];
                    if (EnumDecl::Variant const *variant =
                            LookupEnumVariant(first->name, variantName)) {
                        if (e->segments.size() > 2) {
                            EmitError(
                                e->location,
                                std::format("'{}' is an enum variant, not a module", variantName));
                            return TypeRef::MakeUnknown();
                        }
                        if (!variant->fields.empty() || !variant->namedFields.empty()) {
                            return EnumVariantConstructorType(*enumDecls.at(first->name), *variant);
                        }
                        return EnumType(*enumDecls.at(first->name));
                    }
                }
                TypeRef receiverType =
                    first->type.IsUnknown() ? TypeRef::MakeNamed(first->name) : first->type;
                std::string const &methodName = e->segments[1];
                FuncDecl const *method = LookupMethod(receiverType, methodName);
                if (!method) {
                    EmitError(e->location, std::format("'{}' not found in extend for type '{}'",
                                                       methodName, first->name));
                    return TypeRef::MakeUnknown();
                }
                if (e->segments.size() > 2) {
                    EmitError(e->location,
                              std::format("'{}' is a function, not a module", methodName));
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
                    EmitError(e->location, std::format("'{}' not found in module '{}'",
                                                       e->segments[i], e->segments[i - 1]));
                    return TypeRef::MakeUnknown();
                }
                current = item;
            }
            return current->type;
        }

        if (auto *e = dynamic_cast<SizeOfExpr const *>(&expr)) {
            TypeRef t = ResolveType(*e->type);
            if (!t.IsUnknown() && !SizeOfTypeExpr(*e->type)) {
                EmitError(e->location,
                          std::format("cannot determine size of type '{}'", t.ToString()));
            }
            return TypeRef::MakeUInt64();
        }

        if (dynamic_cast<IntrinsicExpr const *>(&expr)) {
            auto const *e = static_cast<IntrinsicExpr const *>(&expr);
            using K = IntrinsicExpr::Kind;
            if (e->kind == K::Line || e->kind == K::Column) {
                return TypeRef::MakeUInt();
            }
            return TypeRef::MakeNamed(SliceTypeName(TypeRef::MakeChar8()));
        }

        if (auto *e = dynamic_cast<UnaryExpr const *>(&expr)) {
            if (e->op == TokenKind::PlusPlus || e->op == TokenKind::MinusMinus) {
                CheckMutability(*e->operand);
            }
            TypeRef t = CheckExpr(*e->operand);
            return CheckUnary(e->op, t, e->location);
        }

        if (auto *e = dynamic_cast<PostfixExpr const *>(&expr)) {
            CheckMutability(*e->operand);
            TypeRef t = CheckExpr(*e->operand);
            if (!t.IsUnknown() && !t.IsNumeric()) {
                EmitError(e->location,
                          std::format("'{}' applied to non-numeric type '{}'",
                                      e->op == TokenKind::PlusPlus ? "++" : "--", t.ToString()));
            }
            return t;
        }

        if (auto *e = dynamic_cast<BinaryExpr const *>(&expr)) {
            TypeRef l = CheckExpr(*e->left);
            TypeRef r = CheckExpr(*e->right);
            return CheckBinary(e->op, l, r, *e->left, *e->right, e->location);
        }

        if (auto *e = dynamic_cast<AssignExpr const *>(&expr)) {
            CheckMutability(*e->target);
            TypeRef tgt = CheckExpr(*e->target);
            TypeRef val = CheckExpr(*e->value);
            if (!tgt.IsUnknown() && !val.IsUnknown() && !CanAssignExprTo(*e->value, val, tgt)) {
                EmitError(e->location,
                          AssignmentErrorMessage(*e->value, tgt,
                                                 std::format("cannot assign '{}' to '{}'",
                                                             val.ToString(), tgt.ToString())));
            }
            return TypeRef::MakeOpaque();
        }

        if (auto *e = dynamic_cast<TernaryExpr const *>(&expr)) {
            TypeRef cond = CheckExpr(*e->condition);
            if (!cond.IsUnknown() && !cond.IsBool()) {
                EmitError(e->condition->location, "ternary condition must be 'bool'");
            }
            TypeRef thenT = CheckExpr(*e->thenExpr);
            TypeRef elseT = CheckExpr(*e->elseExpr);
            return thenT.IsUnknown() ? elseT : thenT;
        }

        if (auto *e = dynamic_cast<RangeExpr const *>(&expr)) {
            TypeRef loType = e->lo ? CheckExpr(*e->lo) : TypeRef::MakeUnknown();
            TypeRef hiType = e->hi ? CheckExpr(*e->hi) : TypeRef::MakeUnknown();
            if (!loType.IsUnknown() && !hiType.IsUnknown() && !loType.IsNumeric() &&
                !hiType.IsNumeric()) {
                EmitError(e->location, "range operands must be numeric");
            }
            TypeRef elemType = loType.IsUnknown() ? hiType : loType;
            if (elemType.IsUnknown()) {
                elemType = TypeRef::MakeInt64();
            }
            return TypeRef::MakeRange(elemType);
        }

        if (auto *e = dynamic_cast<CallExpr const *>(&expr)) {
            if (auto *ident = dynamic_cast<IdentExpr const *>(e->callee.get())) {
                std::vector<TypeRef> argTypes;
                argTypes.reserve(e->args.size());
                for (auto const &arg : e->args) {
                    argTypes.push_back(CheckExpr(*arg));
                }

                if (Symbol *sym = currentScope->Lookup(ident->name);
                    sym && sym->kind == Symbol::Kind::Func && !sym->funcOverloads.empty()) {
                    FuncDecl const *decl = LookupFunctionOverload(*sym, argTypes);
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
                    std::size_t const paramCount =
                        funcType.kind == TypeRef::Kind::Func && !funcType.inner.empty()
                            ? funcType.inner.size() - 1
                            : 0;
                    bool const isVariadic = !decl->params.empty() && decl->params.back().isVariadic;
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
                        EmitError(e->location,
                                  std::format("function expects {} argument(s), got {}", paramCount,
                                              argTypes.size()));
                    }
                    else {
                        for (std::size_t i = 0; i < argTypes.size() && i < paramCount; ++i) {
                            TypeRef const &paramType = funcType.inner[i];
                            if (!argTypes[i].IsUnknown() && !paramType.IsUnknown() &&
                                !CanAssignExprTo(*e->args[i], argTypes[i], paramType)) {
                                EmitError(e->args[i]->location,
                                          std::format("cannot pass '{}' to "
                                                      "parameter of type '{}'",
                                                      argTypes[i].ToString(),
                                                      paramType.ToString()));
                            }
                        }
                        if (isVariadic) {
                            TypeRef const varElemType = ResolveType(*decl->params.back().type);
                            TypeRef const sliceType =
                                TypeRef::MakeNamed(SliceTypeName(varElemType));
                            bool const isSingleSpread =
                                (argTypes.size() == paramCount + 1 &&
                                 dynamic_cast<SpreadExpr const *>(e->args[paramCount].get()));
                            if (isSingleSpread) {
                                if (!argTypes[paramCount].IsUnknown() && !sliceType.IsUnknown() &&
                                    argTypes[paramCount] != sliceType) {
                                    EmitError(e->args[paramCount]->location,
                                              std::format("cannot spread '{}' to "
                                                          "variadic "
                                                          "parameter of type '{}'",
                                                          argTypes[paramCount].ToString(),
                                                          varElemType.ToString()));
                                }
                            }
                            else {
                                for (std::size_t i = paramCount; i < argTypes.size(); ++i) {
                                    if (dynamic_cast<SpreadExpr const *>(e->args[i].get())) {
                                        EmitError(e->args[i]->location, "spread argument must be "
                                                                        "the only variadic "
                                                                        "argument");
                                    }
                                    else if (!argTypes[i].IsUnknown() && !varElemType.IsUnknown() &&
                                             !CanAssignExprTo(*e->args[i], argTypes[i],
                                                              varElemType)) {
                                        EmitError(e->args[i]->location,
                                                  std::format("cannot pass '{}' to "
                                                              "variadic "
                                                              "parameter of type '{}'",
                                                              argTypes[i].ToString(),
                                                              varElemType.ToString()));
                                    }
                                }
                            }
                        }
                    }
                    return funcType.inner.empty() ? TypeRef::MakeUnknown() : funcType.inner.back();
                }
            }

            if (auto *field = dynamic_cast<FieldExpr const *>(e->callee.get())) {
                TypeRef receiverType = CheckExpr(*field->object);
                std::vector<TypeRef> argTypes;
                argTypes.reserve(e->args.size());
                for (auto const &arg : e->args) {
                    argTypes.push_back(CheckExpr(*arg));
                }
                if (FuncDecl const *method = LookupMethod(receiverType, field->field, argTypes)) {
                    if (!method->warnMessage.empty()) {
                        EmitWarning(e->location, method->warnMessage);
                    }
                    if (!method->errorMessage.empty()) {
                        EmitError(e->location, method->errorMessage);
                    }
                    std::vector<TypeRef> paramTypes =
                        ResolveMethodParamTypes(receiverType, *method);

                    if (argTypes.size() != paramTypes.size()) {
                        EmitError(e->location,
                                  std::format("function expects {} argument(s), got {}",
                                              paramTypes.size(), argTypes.size()));
                    }
                    else {
                        for (std::size_t i = 0; i < argTypes.size(); ++i) {
                            TypeRef const &argType = argTypes[i];
                            TypeRef const &paramType = paramTypes[i];
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

                if (FuncDecl const *method = LookupInterfaceMethod(receiverType, field->field)) {
                    std::vector<TypeRef> paramTypes = ResolveInterfaceMethodParamTypes(*method);
                    bool const isVariadic =
                        !method->params.empty() && method->params.back().isVariadic;
                    bool const arityOk = isVariadic ? argTypes.size() >= paramTypes.size()
                                                    : argTypes.size() == paramTypes.size();

                    if (!arityOk) {
                        EmitError(e->location,
                                  std::format("function expects {} argument(s), got {}",
                                              paramTypes.size(), argTypes.size()));
                    }
                    else {
                        for (std::size_t i = 0; i < paramTypes.size(); ++i) {
                            TypeRef const &argType = argTypes[i];
                            TypeRef const &paramType = paramTypes[i];
                            if (!argType.IsUnknown() && !paramType.IsUnknown() &&
                                !CanAssignExprTo(*e->args[i], argType, paramType)) {
                                EmitError(e->args[i]->location,
                                          std::format("cannot pass '{}' to "
                                                      "parameter of type '{}'",
                                                      argType.ToString(), paramType.ToString()));
                            }
                        }

                        if (isVariadic) {
                            TypeRef const varElemType = ResolveType(*method->params.back().type);
                            for (std::size_t i = paramTypes.size(); i < argTypes.size(); ++i) {
                                if (!argTypes[i].IsUnknown() && !varElemType.IsUnknown() &&
                                    !CanAssignExprTo(*e->args[i], argTypes[i], varElemType)) {
                                    EmitError(e->args[i]->location,
                                              std::format("cannot pass '{}' to variadic "
                                                          "parameter of type '{}'",
                                                          argTypes[i].ToString(),
                                                          varElemType.ToString()));
                                }
                            }
                        }
                    }

                    return ResolveInterfaceMethodReturnType(*method);
                }
            }

            if (auto *path = dynamic_cast<PathExpr const *>(e->callee.get())) {
                if (path->segments.size() == 2) {
                    Symbol *first = currentScope->Lookup(path->segments[0]);
                    if (first && (first->kind == Symbol::Kind::Type ||
                                  first->kind == Symbol::Kind::Interface)) {
                        TypeRef receiverType =
                            first->type.IsUnknown() ? TypeRef::MakeNamed(first->name) : first->type;
                        std::string const &methodName = path->segments[1];
                        std::vector<TypeRef> argTypes;
                        argTypes.reserve(e->args.size());
                        for (auto const &arg : e->args) {
                            argTypes.push_back(CheckExpr(*arg));
                        }
                        if (FuncDecl const *method =
                                LookupMethod(receiverType, methodName, argTypes)) {
                            std::vector<TypeRef> paramTypes =
                                ResolveMethodParamTypes(receiverType, *method);
                            if (argTypes.size() != paramTypes.size()) {
                                EmitError(e->location,
                                          std::format("function expects {} "
                                                      "argument(s), got {}",
                                                      paramTypes.size(), argTypes.size()));
                            }
                            else {
                                for (std::size_t i = 0; i < argTypes.size(); ++i) {
                                    TypeRef const &argType = argTypes[i];
                                    TypeRef const &paramType = paramTypes[i];
                                    if (!argType.IsUnknown() && !paramType.IsUnknown() &&
                                        !CanAssignExprTo(*e->args[i], argType, paramType)) {
                                        EmitError(e->args[i]->location,
                                                  std::format("cannot pass '{}' to "
                                                              "parameter of type '{}'",
                                                              argType.ToString(),
                                                              paramType.ToString()));
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
            for (auto const &arg : e->args) {
                argTypes.push_back(CheckExpr(*arg));
            }

            if (calleeType.kind == TypeRef::Kind::Func && !calleeType.inner.empty()) {
                std::size_t const paramCount = calleeType.inner.size() - 1;
                if (argTypes.size() != paramCount) {
                    EmitError(e->location, std::format("function expects {} argument(s), got {}",
                                                       paramCount, argTypes.size()));
                }
                else {
                    for (std::size_t i = 0; i < argTypes.size(); ++i) {
                        TypeRef const &argType = argTypes[i];
                        TypeRef const &paramType = calleeType.inner[i];
                        if (!argType.IsUnknown() && !paramType.IsUnknown() &&
                            !CanAssignExprTo(*e->args[i], argType, paramType)) {
                            EmitError(e->args[i]->location,
                                      std::format("cannot pass '{}' to "
                                                  "parameter of type '{}'",
                                                  argType.ToString(), paramType.ToString()));
                        }
                    }
                }
                return calleeType.inner.back();
            }
            return TypeRef::MakeUnknown();
        }

        if (auto *e = dynamic_cast<IndexExpr const *>(&expr)) {
            TypeRef obj = CheckExpr(*e->object);
            CheckExpr(*e->index);
            if (auto elemType = IndexElementType(obj)) {
                return *elemType;
            }
            return TypeRef::MakeUnknown();
        }

        if (auto *e = dynamic_cast<FieldExpr const *>(&expr)) {
            TypeRef obj = CheckExpr(*e->object);
            if (auto elemType = SliceElementType(obj)) {
                if (e->field == "data") {
                    return TypeRef::MakePointer(*elemType);
                }
                if (e->field == "length") {
                    return TypeRef::MakeUInt64();
                }
                EmitError(e->location,
                          std::format("unknown field '{}' on type '{}'", e->field, obj.ToString()));
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
                EmitError(e->location,
                          std::format("unknown field '{}' on type '{}'", e->field, obj.ToString()));
                return TypeRef::MakeUnknown();
            }
            if (obj.kind == TypeRef::Kind::Tuple) {
                try {
                    std::size_t const idx = std::stoul(e->field);
                    if (idx < obj.inner.size()) {
                        return obj.inner[idx];
                    }
                }
                catch (...) {
                }
                EmitError(e->location, std::format("tuple index '{}' out of range for type '{}'",
                                                   e->field, obj.ToString()));
                return TypeRef::MakeUnknown();
            }

            // Interface fat-pointer fields: data → *opaque, vtable →
            // *opaque
            if (std::string const ifaceName = NamedBaseTypeName(obj);
                !ifaceName.empty() && currentScope->Lookup(ifaceName) &&
                currentScope->Lookup(ifaceName)->kind == Symbol::Kind::Interface) {
                TypeRef const ptrOpaque = TypeRef::MakePointer(TypeRef::MakeOpaque());
                if (e->field == "data" || e->field == "vtable") {
                    return ptrOpaque;
                }
                EmitError(e->location, std::format("unknown field '{}' on interface type '{}'",
                                                   e->field, obj.ToString()));
                return TypeRef::MakeUnknown();
            }

            std::string const structName = NamedBaseTypeName(obj);
            if (!structName.empty() && structDecls.contains(structName)) {
                if (TypeRef fieldType = StructFieldType(obj, e->field); !fieldType.IsUnknown()) {
                    return fieldType;
                }
                EmitError(e->location,
                          std::format("unknown field '{}' on type '{}'", e->field, obj.ToString()));
                return TypeRef::MakeUnknown();
            }

            if (TypeRef fieldType = StructFieldType(obj, e->field); !fieldType.IsUnknown()) {
                return fieldType;
            }
            if (!obj.IsUnknown()) {
                EmitError(e->location,
                          std::format("type '{}' has no field '{}'", obj.ToString(), e->field));
            }
            return TypeRef::MakeUnknown(); // field type lookup needs full
                                           // type info
        }

        if (auto *e = dynamic_cast<StructInitExpr const *>(&expr)) {
            CheckStructInitExpr(*e);
            if (auto const [enumDecl, variant] = LookupEnumVariantInitializer(e->typeName);
                enumDecl && variant) {
                return EnumType(*enumDecl);
            }
            return TypeRef::MakeNamed(GenericStructInitName(*e));
        }

        if (auto *e = dynamic_cast<SliceExpr const *>(&expr)) {
            TypeRef elemType = TypeRef::MakeUnknown();
            for (auto const &el : e->elements) {
                TypeRef t = CheckExpr(*el);
                if (elemType.IsUnknown()) {
                    elemType = t;
                }
            }
            return TypeRef::MakeNamed(SliceTypeName(elemType));
        }

        if (auto *e = dynamic_cast<TupleExpr const *>(&expr)) {
            std::vector<TypeRef> elemTypes;
            for (auto const &el : e->elements) {
                elemTypes.push_back(CheckExpr(*el));
            }
            return TypeRef::MakeTuple(std::move(elemTypes));
        }

        if (auto *e = dynamic_cast<CastExpr const *>(&expr)) {
            TypeRef operandType = CheckExpr(*e->operand);
            TypeRef targetType = ResolveType(*e->type);
            if (auto const maxCodePoint = CharTypeMaxCodePoint(targetType);
                maxCodePoint && (operandType.IsInteger() || IsCharType(operandType))) {
                if (auto const value = EvalConstInt(*e->operand); value && *value < 0) {
                    EmitError(e->location,
                              std::format("constant value is out of range for type '{}'",
                                          targetType.ToString()));
                }
                else if (auto const value = EvalConstCharCastValue(*e->operand)) {
                    if (*value > *maxCodePoint) {
                        EmitError(e->location,
                                  std::format("constant value is out of range for type '{}'",
                                              targetType.ToString()));
                    }
                    else if (IsSurrogateCodePoint(*value)) {
                        EmitError(
                            e->location,
                            std::format("surrogate code point U+{:04X} cannot be converted to '{}'",
                                        *value, targetType.ToString()));
                    }
                }
            }
            return targetType;
        }

        if (auto *e = dynamic_cast<IsExpr const *>(&expr)) {
            TypeRef operandType = CheckExpr(*e->operand);
            ResolveType(*e->type);
            std::string const ifaceName = NamedBaseTypeName(operandType);
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

        if (auto *e = dynamic_cast<MatchExpr const *>(&expr)) {
            CheckExpr(*e->subject);
            TypeRef resultType = TypeRef::MakeUnknown();
            for (auto const &arm : e->arms) {
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
                                                                 resultType.ToString(),
                                                                 armType.ToString())));
                }
            }
            return resultType;
        }

        if (auto *e = dynamic_cast<BlockExpr const *>(&expr)) {
            CheckBlock(*e->block);
            return TypeRef::MakeUnknown();
        }

        if (auto *e = dynamic_cast<SpreadExpr const *>(&expr)) {
            return CheckExpr(*e->operand);
        }

        return TypeRef::MakeUnknown();
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

    TypeRef CheckUnary(TokenKind op, TypeRef const &t, SourceLocation loc) {
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
                EmitError(loc,
                          std::format("unary '-' applied to non-numeric type '{}'", t.ToString()));
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

    TypeRef CheckBinary(TokenKind op, TypeRef const &l, TypeRef const &r, Expr const &leftExpr,
                        Expr const &rightExpr, SourceLocation loc) {
        if (l.IsUnknown() || r.IsUnknown()) {
            return TypeRef::MakeUnknown();
        }

        std::string_view const opName = BinaryOperatorName(op);
        if (!opName.empty()) {
            if (FuncDecl const *method = LookupMethod(l, std::string(opName), {r})) {
                std::vector<TypeRef> paramTypes = ResolveMethodParamTypes(l, *method);
                TypeRef ret = ResolveMethodReturnType(l, *method);
                if (paramTypes.size() != 1) {
                    EmitError(loc, std::format("operator '{}' expects 1 argument, got {}", opName,
                                               paramTypes.size()));
                }
                else if (!paramTypes[0].IsUnknown() &&
                         !CanAssignExprTo(rightExpr, r, paramTypes[0])) {
                    EmitError(rightExpr.location,
                              std::format("cannot pass '{}' to parameter of type '{}'",
                                          r.ToString(), paramTypes[0].ToString()));
                }
                return ret;
            }
        }

        auto isNumericOrChar = [](TypeRef const &t) {
            return t.IsNumeric() || t.kind == TypeRef::Kind::Char8 ||
                   t.kind == TypeRef::Kind::Char16 || t.kind == TypeRef::Kind::Char32;
        };
        auto isIntegerOrChar = [](TypeRef const &t) {
            return t.IsInteger() || t.kind == TypeRef::Kind::Char8 ||
                   t.kind == TypeRef::Kind::Char16 || t.kind == TypeRef::Kind::Char32;
        };
        auto isChar = [](TypeRef::Kind k) {
            return k == TypeRef::Kind::Char8 || k == TypeRef::Kind::Char16 ||
                   k == TypeRef::Kind::Char32;
        };
        auto getCompatibleType = [&](Expr const &left, TypeRef const &lt, Expr const &right,
                                     TypeRef const &rt) -> std::optional<TypeRef> {
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
                EmitError(loc,
                          std::format("'+' right operand must be numeric, got '{}'", r.ToString()));
            }
            else {
                auto res = getCompatibleType(leftExpr, l, rightExpr, r);
                if (!res.has_value()) {
                    EmitError(loc, std::format("mismatched types in addition: '{}' and '{}'",
                                               l.ToString(), r.ToString()));
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
                EmitError(loc,
                          std::format("'-' right operand must be numeric, got '{}'", r.ToString()));
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
            std::string opStr = op == TK::Star    ? "*"
                              : op == TK::Slash   ? "/"
                              : op == TK::Percent ? "%"
                                                  : "**";
            if (!isNumericOrChar(l)) {
                EmitError(
                    loc, std::format("'{}' applied to non-numeric type '{}'", opStr, l.ToString()));
            }
            else if (!isNumericOrChar(r)) {
                EmitError(loc, std::format("'{}' right operand must be numeric, got '{}'", opStr,
                                           r.ToString()));
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
            auto isBitwiseOperand = [](TypeRef const &t) {
                return t.IsInteger() || t.IsBool() || t.kind == TypeRef::Kind::Char8 ||
                       t.kind == TypeRef::Kind::Char16 || t.kind == TypeRef::Kind::Char32;
            };
            if (!isBitwiseOperand(l)) {
                EmitError(loc, std::format("bitwise operator applied to non-integer type '{}'",
                                           l.ToString()));
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
                EmitError(loc, std::format("'{}' applied to non-bool type '{}'",
                                           op == TK::AmpAmp ? "&&" : "||", l.ToString()));
            }
            if (!r.IsBool()) {
                EmitError(loc, std::format("'{}' applied to non-bool type '{}'",
                                           op == TK::AmpAmp ? "&&" : "||", r.ToString()));
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
                EmitError(loc, std::format("cannot compare mismatched types '{}' and '{}'",
                                           l.ToString(), r.ToString()));
            }
            return TypeRef::MakeBool();
        }

        default:
            return TypeRef::MakeUnknown();
        }
    }

    // Check that an assignment target is mutable.
    void CheckMutability(Expr const &target) {
        if (auto *e = dynamic_cast<IdentExpr const *>(&target)) {
            Symbol *sym = currentScope->Lookup(e->name);
            if (!sym) {
                return;
            }
            if (sym->kind == Symbol::Kind::Const) {
                EmitError(target.location, std::format("cannot assign to constant '{}'", e->name));
                return;
            }
            if (sym->kind == Symbol::Kind::Var && !sym->isMut) {
                EmitError(target.location,
                          std::format("cannot assign to immutable variable '{}'", e->name));
            }
        }
    }
};

// Sema public API
Sema::Sema(std::vector<Module const *> userModules, std::vector<DepPackage> deps,
           std::string packageName, std::string targetOs)
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

bool Sema::DumpResult(SemaResult const &result, std::filesystem::path const &path) {
    std::ofstream out(path);
    if (!out) {
        return false;
    }

    static constexpr auto kindName = [](SemaSymbol::Kind k) -> std::string_view {
        switch (k) {
        case SemaSymbol::Kind::Var:
            return "var";
        case SemaSymbol::Kind::Func:
            return "func";
        case SemaSymbol::Kind::Type:
            return "type";
        case SemaSymbol::Kind::Const:
            return "const";
        case SemaSymbol::Kind::Module:
            return "module";
        case SemaSymbol::Kind::Interface:
            return "interface";
        }
        return "?";
    };

    out << "=== Semantic Analysis Results ===\n\n";

    // Symbols
    out << std::format("Symbols ({} total)\n", result.symbols.size());
    out << std::string(40, '-') << '\n';

    if (result.symbols.empty()) {
        out << "(none)\n";
    }
    else {
        for (auto const &sym : result.symbols) {
            std::string tag = std::format("{:<10}", kindName(sym.kind));
            std::string qname = sym.name;
            if (sym.isMut) {
                qname += " (var)";
            }
            std::string typeStr = sym.resolvedType.empty() ? "" : "  " + sym.resolvedType;
            out << std::format("{}  {:<28}{}  [{}:{}:{}]\n", tag, qname, typeStr, sym.sourceName,
                               sym.location.line, sym.location.column);
        }
    }

    out << '\n';

    // Diagnostics
    out << std::format("Diagnostics ({} total)\n", result.diagnostics.size());
    out << std::string(40, '-') << '\n';

    if (result.diagnostics.empty()) {
        out << "(none)\n";
    }
    else {
        for (auto const &diag : result.diagnostics) {
            char const *sev =
                diag.severity == SemaDiagnostic::Severity::Error ? "error" : "warning";
            out << std::format("{}:{}:{}: {}: {}\n", diag.sourceName, diag.location.line,
                               diag.location.column, sev, diag.message);
        }
    }

    return out.good();
}
} // namespace Rux
