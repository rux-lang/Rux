#pragma once

#include "Semantic/SemanticModel.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Rux::SemanticDetail {
class Scope;

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
    std::string sourceName;
    SourceLocation location;
    TypeRef type;
    bool isMut = false;
    bool isPublic = false;
    bool isEffectivelyPublic = false;
    std::string ownerPackage;
    std::string modulePath;
    std::string intrinsicName;
    std::vector<const FuncDecl *> funcOverloads;
    const ExternFuncDecl *externDecl = nullptr;
    std::vector<std::string> interfaceMethods;
    Scope *moduleScope = nullptr;
};

class Scope {
public:
    explicit Scope(Scope *parentScope = nullptr);

    bool Define(Symbol symbol, std::vector<SemanticDiagnostic> &diagnostics, const std::string &sourceName);
    [[nodiscard]] Symbol *Lookup(const std::string &name);
    [[nodiscard]] Symbol *LookupLocal(const std::string &name);
    [[nodiscard]] const Symbol *Suggest(const std::string &name) const;
    [[nodiscard]] Scope *Parent() const;
    [[nodiscard]] const std::unordered_map<std::string, Symbol> &Table() const;

private:
    Scope *parent;
    std::unordered_map<std::string, Symbol> table;
};

[[nodiscard]] std::string_view SymbolKindName(Symbol::Kind kind);
[[nodiscard]] std::string DeclarationNote(const Symbol &symbol);

/// Owns the package/module declaration topology built before semantic checking. The analyzer supplies type resolution
/// because aliases and typed constants still use its in-progress type context.
class SemanticProgramIndex {
public:
    struct DeclarationInfo {
        std::string ownerPackage;
        std::string modulePath;
        std::string sourceName;
        bool isEffectivelyPublic = false;
    };

    using PackageScopes = std::unordered_map<std::string, std::unordered_map<std::string, Scope *>>;
    using ResolveType = std::function<TypeRef(const TypeExpr &)>;

    SemanticProgramIndex(std::vector<SemanticDiagnostic> &diagnostics, std::vector<SemanticSymbol> &publicSymbols);

    [[nodiscard]] Scope &GlobalScope();
    [[nodiscard]] Scope &CreateScope(Scope &parent);
    [[nodiscard]] Scope &CreatePackageRoot(const std::string &packageName);
    void RegisterPackageRoot(const std::string &packageName, Scope &scope);
    [[nodiscard]] Scope &ModuleScopeFor(const std::string &name, Scope &parent) const;

    void CollectModule(const Module &module, const std::string *packageName, const ResolveType &resolveType);
    void CollectDeclaration(const Decl &declaration, Scope &scope, const std::string &sourceName,
                            const ResolveType &resolveType, const std::string *packageName = nullptr,
                            const std::string &modulePath = {}, bool containingModulesPublic = true);
    void FinalizeVisibility();

    [[nodiscard]] const DeclarationInfo *InfoFor(const Decl &declaration) const;

    [[nodiscard]] const auto &Packages() const {
        return packageScopes;
    }

    [[nodiscard]] const auto &Structs() const {
        return structs;
    }

    [[nodiscard]] const auto &Enums() const {
        return enums;
    }

    /// The struct and enum type parameters declared in `sourceName`.
    ///
    /// `Structs()` and `Enums()` are keyed by bare name across every package, so the last declaration of a name wins
    /// and a program that declares its own `Option` displaces the one in `Core`. An `extend` block has to resolve the
    /// type it is written beside rather than whichever one was indexed last, so it asks here first.
    ///
    /// @return the declared parameters, or nullptr when `sourceName` declares no type of that name
    [[nodiscard]] const std::vector<TypeParameter> *TypeParamsIn(const std::string &sourceName,
                                                                 const std::string &name) const;

    /// The enum `name` declared in `sourceName`, for the same reason `TypeParamsIn` exists.
    ///
    /// @return nullptr when `sourceName` declares no enum of that name
    [[nodiscard]] const EnumDecl *EnumIn(const std::string &sourceName, const std::string &name) const;

    /// The struct `name` declared in `sourceName`.
    ///
    /// @return nullptr when `sourceName` declares no struct of that name
    [[nodiscard]] const StructDecl *StructIn(const std::string &sourceName, const std::string &name) const;

    [[nodiscard]] const auto &Unions() const {
        return unions;
    }

    [[nodiscard]] const auto &Interfaces() const {
        return interfaces;
    }

    [[nodiscard]] const auto &Methods() const {
        return methods;
    }

    [[nodiscard]] const auto &Functions() const {
        return functions;
    }

    [[nodiscard]] const auto &FunctionModulePaths() const {
        return functionModulePaths;
    }

    [[nodiscard]] const auto &MethodImplementations() const {
        return methodImplementations;
    }

    [[nodiscard]] const auto &Implementations() const {
        return implementations;
    }

    [[nodiscard]] const auto &ExternFunctions() const {
        return externFunctions;
    }

    [[nodiscard]] const auto &ImplementedInterfaces() const {
        return implementedInterfaces;
    }

    [[nodiscard]] const auto &FunctionScopes() const {
        return functionScopes;
    }

    [[nodiscard]] const auto &FunctionSources() const {
        return functionSources;
    }

    [[nodiscard]] const auto &DeclarationInfos() const {
        return declarationInfos;
    }

private:
    std::vector<SemanticDiagnostic> &diagnostics;
    std::vector<SemanticSymbol> &publicSymbols;
    Scope globalScope;
    PackageScopes packageScopes;
    std::vector<std::unique_ptr<Scope>> scopes;
    std::unordered_map<std::string, const StructDecl *> structs;
    std::unordered_map<std::string, const EnumDecl *> enums;
    /// Declarations by the source that made them, which is what tells two same-named types apart.
    std::unordered_map<std::string, std::unordered_map<std::string, const StructDecl *>> structsBySource;
    std::unordered_map<std::string, std::unordered_map<std::string, const EnumDecl *>> enumsBySource;
    std::unordered_map<std::string, const UnionDecl *> unions;
    std::unordered_map<std::string, const InterfaceDecl *> interfaces;
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<const FuncDecl *>>> methods;
    std::unordered_map<std::string, std::vector<const FuncDecl *>> functions;
    std::unordered_map<const FuncDecl *, std::string> functionModulePaths;
    std::unordered_map<const FuncDecl *, const ImplDecl *> methodImplementations;
    std::vector<const ImplDecl *> implementations;
    std::vector<const ExternFuncDecl *> externFunctions;
    std::unordered_map<std::string, std::unordered_set<std::string>> implementedInterfaces;
    std::unordered_map<const FuncDecl *, Scope *> functionScopes;
    std::unordered_map<const FuncDecl *, std::string> functionSources;
    std::unordered_map<const Decl *, DeclarationInfo> declarationInfos;
};
} // namespace Rux::SemanticDetail
