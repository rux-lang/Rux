// The scope and symbol tables name resolution reads: how declarations are
// indexed, and how a name that resolves to nothing is explained.

#include "Semantic/SemanticProgramIndex.h"

#include <algorithm>
#include <format>
#include <limits>
#include <utility>

namespace Rux::SemanticDetail {
namespace {
/// Join a module prefix to a name, skipping the separator at the root so a top-level module is not spelled `::Name`.
std::string JoinModulePath(const std::string &prefix, const std::string &name) {
    return prefix.empty() ? name : prefix + "::" + name;
}

/// A generic type's name without its argument list, so `List<int32>` and `List<char8>` resolve to the same declaration.
std::string BaseTypeName(const std::string &name) {
    const std::size_t position = name.find('<');
    return position == std::string::npos ? name : name.substr(0, position);
}

/// Levenshtein distance, used to suggest the name an unresolved identifier probably meant.
std::size_t EditDistance(const std::string_view left, const std::string_view right) {
    std::vector<std::size_t> previous(right.size() + 1);
    std::vector<std::size_t> current(right.size() + 1);
    for (std::size_t i = 0; i <= right.size(); ++i) {
        previous[i] = i;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        current[0] = i + 1;
        for (std::size_t j = 0; j < right.size(); ++j) {
            current[j + 1] =
                std::min({current[j] + 1, previous[j + 1] + 1, previous[j] + (left[i] == right[j] ? 0U : 1U)});
        }
        previous.swap(current);
    }
    return previous.back();
}
} // namespace

std::string_view SymbolKindName(const Symbol::Kind kind) {
    switch (kind) {
    case Symbol::Kind::Var:
        return "variable";
    case Symbol::Kind::Func:
        return "function";
    case Symbol::Kind::Type:
        return "type";
    case Symbol::Kind::Const:
        return "constant";
    case Symbol::Kind::Module:
        return "module";
    case Symbol::Kind::Interface:
        return "interface";
    }
    return "symbol";
}

std::string DeclarationNote(const Symbol &symbol) {
    if (symbol.sourceName.empty() || symbol.location.line == 0) {
        return std::format("'{}' is declared as a {}", symbol.name, SymbolKindName(symbol.kind));
    }
    return std::format("'{}' was declared as a {} at '{}':{}:{}", symbol.name, SymbolKindName(symbol.kind),
                       symbol.sourceName, symbol.location.line, symbol.location.column);
}

Scope::Scope(Scope *parentScope)
    : parent(parentScope) {
}

bool Scope::Define(Symbol symbol, std::vector<SemanticDiagnostic> &diagnostics, const std::string &sourceName) {
    if (symbol.sourceName.empty()) {
        symbol.sourceName = sourceName;
    }
    if (auto iterator = table.find(symbol.name); iterator != table.end()) {
        if (iterator->second.kind == Symbol::Kind::Func && symbol.kind == Symbol::Kind::Func) {
            iterator->second.funcOverloads.insert(iterator->second.funcOverloads.end(), symbol.funcOverloads.begin(),
                                                  symbol.funcOverloads.end());
            if (iterator->second.type.IsUnknown() && !symbol.type.IsUnknown()) {
                iterator->second.type = std::move(symbol.type);
            }
            if (!iterator->second.externDecl && symbol.externDecl) {
                iterator->second.externDecl = symbol.externDecl;
            }
            iterator->second.isPublic = iterator->second.isPublic || symbol.isPublic;
            iterator->second.isEffectivelyPublic = iterator->second.isEffectivelyPublic || symbol.isEffectivelyPublic;
            return true;
        }
        const Symbol &previous = iterator->second;
        const std::string message =
            previous.kind == symbol.kind
                ? std::format("{} '{}' is already declared in this scope", SymbolKindName(symbol.kind), symbol.name)
                : std::format("name '{}' cannot be declared as a {} because it is already a {} "
                              "in this scope",
                              symbol.name, SymbolKindName(symbol.kind), SymbolKindName(previous.kind));
        diagnostics.push_back({SemanticDiagnostic::Severity::Error,
                               sourceName,
                               symbol.location,
                               message,
                               {DeclarationNote(previous)},
                               {},
                               {}});
        return false;
    }
    table.emplace(symbol.name, std::move(symbol));
    return true;
}

Symbol *Scope::Lookup(const std::string &name) {
    auto iterator = table.find(name);
    if (iterator != table.end()) {
        return &iterator->second;
    }
    return parent ? parent->Lookup(name) : nullptr;
}

Symbol *Scope::LookupLocal(const std::string &name) {
    auto iterator = table.find(name);
    return iterator == table.end() ? nullptr : &iterator->second;
}

const Symbol *Scope::Suggest(const std::string &name) const {
    const Symbol *best = nullptr;
    std::size_t bestDistance = std::numeric_limits<std::size_t>::max();
    for (const Scope *scope = this; scope != nullptr; scope = scope->parent) {
        for (const auto &[candidate, symbol] : scope->table) {
            const std::size_t distance = EditDistance(name, candidate);
            if (distance < bestDistance || (distance == bestDistance && best != nullptr && candidate < best->name)) {
                best = &symbol;
                bestDistance = distance;
            }
        }
    }
    const std::size_t threshold = name.size() < 4 ? 1 : std::min<std::size_t>(3, (name.size() + 2) / 3);
    return bestDistance <= threshold ? best : nullptr;
}

Scope *Scope::Parent() const {
    return parent;
}

const std::unordered_map<std::string, Symbol> &Scope::Table() const {
    return table;
}

SemanticProgramIndex::SemanticProgramIndex(std::vector<SemanticDiagnostic> &inputDiagnostics,
                                           std::vector<SemanticSymbol> &inputPublicSymbols)
    : diagnostics(inputDiagnostics)
    , publicSymbols(inputPublicSymbols) {
}

Scope &SemanticProgramIndex::GlobalScope() {
    return globalScope;
}

Scope &SemanticProgramIndex::CreateScope(Scope &parent) {
    scopes.push_back(std::make_unique<Scope>(&parent));
    return *scopes.back();
}

Scope &SemanticProgramIndex::CreatePackageRoot(const std::string &packageName) {
    Scope &root = CreateScope(globalScope);
    RegisterPackageRoot(packageName, root);
    return root;
}

void SemanticProgramIndex::RegisterPackageRoot(const std::string &packageName, Scope &scope) {
    packageScopes[packageName][""] = &scope;
}

Scope &SemanticProgramIndex::ModuleScopeFor(const std::string &name, Scope &parent) const {
    if (Symbol *symbol = parent.Lookup(name); symbol && symbol->kind == Symbol::Kind::Module && symbol->moduleScope) {
        return *symbol->moduleScope;
    }
    return parent;
}

void SemanticProgramIndex::CollectModule(const Module &module, const std::string *packageName,
                                         const ResolveType &resolveType) {
    for (const auto &declaration : module.items) {
        CollectDeclaration(*declaration, globalScope, module.name, resolveType, packageName);
    }
}

void SemanticProgramIndex::CollectDeclaration(const Decl &declaration, Scope &scope, const std::string &sourceName,
                                              const ResolveType &resolveType, const std::string *packageName,
                                              const std::string &modulePath, const bool containingModulesPublic) {
    const bool isGlobal = &scope == &globalScope;
    const std::string ownerPackage = packageName ? *packageName : std::string{};
    const bool isEffectivelyPublic = containingModulesPublic && declaration.isPublic;
    declarationInfos.insert_or_assign(&declaration,
                                      DeclarationInfo{ownerPackage, modulePath, sourceName, isEffectivelyPublic});
    auto defineSimple = [&](Symbol::Kind kind, const std::string &name, SemanticSymbol::Kind publicKind,
                            std::string resolvedType = {}, bool isMut = false) {
        Symbol symbol;
        symbol.kind = kind;
        symbol.name = name;
        symbol.location = declaration.location;
        symbol.isMut = isMut;
        symbol.isPublic = declaration.isPublic;
        symbol.isEffectivelyPublic = isEffectivelyPublic;
        symbol.ownerPackage = ownerPackage;
        symbol.modulePath = modulePath;
        if (scope.Define(symbol, diagnostics, sourceName) && isGlobal) {
            publicSymbols.push_back(
                {publicKind, name, sourceName, declaration.location, std::move(resolvedType), isMut});
        }
    };

    if (const auto *function = dynamic_cast<const FuncDecl *>(&declaration)) {
        functionScopes[function] = &scope;
        functionSources[function] = sourceName;
        functions[function->name].push_back(function);
        // The package is part of where a function lives, not only the `module` blocks around it. Two packages both
        // declaring `Compare` are two functions in two places, and recording only the module path -- empty for a
        // file with no `module` block, which is most of them -- said they were one, so nothing distinguished their
        // symbols and one of them was linked for both. Nothing changes for a name only one package declares: this
        // is read to decide whether two owners exist at all, and a name with one owner is never qualified.
        functionModulePaths[function] =
            packageName && !packageName->empty() ? JoinModulePath(*packageName, modulePath) : modulePath;
        Symbol symbol;
        symbol.kind = Symbol::Kind::Func;
        symbol.name = function->name;
        symbol.location = function->location;
        symbol.isPublic = function->isPublic;
        symbol.isEffectivelyPublic = isEffectivelyPublic;
        symbol.ownerPackage = ownerPackage;
        symbol.modulePath = modulePath;
        symbol.intrinsicName = function->intrinsicName;
        symbol.funcOverloads.push_back(function);
        if (scope.Define(symbol, diagnostics, sourceName) && isGlobal) {
            publicSymbols.push_back(
                {SemanticSymbol::Kind::Func, function->name, sourceName, function->location, {}, false});
        }
    }
    else if (const auto *structure = dynamic_cast<const StructDecl *>(&declaration)) {
        structs[structure->name] = structure;
        structsBySource[sourceName][structure->name] = structure;
        defineSimple(Symbol::Kind::Type, structure->name, SemanticSymbol::Kind::Type, "struct");
    }
    else if (const auto *enumeration = dynamic_cast<const EnumDecl *>(&declaration)) {
        enums[enumeration->name] = enumeration;
        enumsBySource[sourceName][enumeration->name] = enumeration;
        defineSimple(Symbol::Kind::Type, enumeration->name, SemanticSymbol::Kind::Type,
                     enumeration->IsVariant() ? "variant" : "enum");
    }
    else if (const auto *unionType = dynamic_cast<const UnionDecl *>(&declaration)) {
        unions[unionType->name] = unionType;
        defineSimple(Symbol::Kind::Type, unionType->name, SemanticSymbol::Kind::Type, "union");
    }
    else if (const auto *interface = dynamic_cast<const InterfaceDecl *>(&declaration)) {
        interfaces[interface->name] = interface;
        Symbol symbol;
        symbol.kind = Symbol::Kind::Interface;
        symbol.name = interface->name;
        symbol.location = interface->location;
        symbol.isPublic = interface->isPublic;
        symbol.isEffectivelyPublic = isEffectivelyPublic;
        symbol.ownerPackage = ownerPackage;
        symbol.modulePath = modulePath;
        for (const auto &method : interface->methods) {
            symbol.interfaceMethods.push_back(method->name);
        }
        if (scope.Define(symbol, diagnostics, sourceName) && isGlobal) {
            publicSymbols.push_back({SemanticSymbol::Kind::Interface, interface->name, sourceName, interface->location,
                                     "interface", false});
        }
    }
    else if (const auto *constant = dynamic_cast<const ConstDecl *>(&declaration)) {
        Symbol symbol;
        symbol.kind = Symbol::Kind::Const;
        symbol.name = constant->name;
        symbol.location = constant->location;
        symbol.isPublic = constant->isPublic;
        symbol.isEffectivelyPublic = isEffectivelyPublic;
        symbol.ownerPackage = ownerPackage;
        symbol.modulePath = modulePath;
        symbol.intrinsicName = constant->intrinsicName;
        if (constant->type) {
            symbol.type = resolveType(**constant->type);
        }
        if (scope.Define(symbol, diagnostics, sourceName) && isGlobal) {
            publicSymbols.push_back({SemanticSymbol::Kind::Const, constant->name, sourceName, constant->location,
                                     symbol.type.IsUnknown() ? "" : symbol.type.ToString(), false});
        }
    }
    else if (const auto *alias = dynamic_cast<const TypeAliasDecl *>(&declaration)) {
        Symbol symbol;
        symbol.kind = Symbol::Kind::Type;
        symbol.name = alias->name;
        symbol.location = alias->location;
        symbol.isPublic = alias->isPublic;
        symbol.isEffectivelyPublic = isEffectivelyPublic;
        symbol.ownerPackage = ownerPackage;
        symbol.modulePath = modulePath;
        symbol.type = resolveType(*alias->type);
        if (scope.Define(symbol, diagnostics, sourceName) && isGlobal) {
            publicSymbols.push_back({SemanticSymbol::Kind::Type, alias->name, sourceName, alias->location,
                                     symbol.type.IsUnknown() ? "" : symbol.type.ToString(), false});
        }
    }
    else if (const auto *externFunction = dynamic_cast<const ExternFuncDecl *>(&declaration)) {
        externFunctions.push_back(externFunction);
        Symbol symbol;
        symbol.kind = Symbol::Kind::Func;
        symbol.name = externFunction->name;
        symbol.location = externFunction->location;
        symbol.isPublic = externFunction->isPublic;
        symbol.isEffectivelyPublic = isEffectivelyPublic;
        symbol.ownerPackage = ownerPackage;
        symbol.modulePath = modulePath;
        symbol.externDecl = externFunction;
        if (scope.Define(symbol, diagnostics, sourceName) && isGlobal) {
            publicSymbols.push_back({SemanticSymbol::Kind::Func, externFunction->name, sourceName,
                                     externFunction->location, "extern", false});
        }
    }
    else if (const auto *externVariable = dynamic_cast<const ExternVarDecl *>(&declaration)) {
        defineSimple(Symbol::Kind::Var, externVariable->name, SemanticSymbol::Kind::Var, "extern", true);
    }
    else if (const auto *externBlock = dynamic_cast<const ExternBlockDecl *>(&declaration)) {
        for (const auto &item : externBlock->items) {
            CollectDeclaration(*item, scope, sourceName, resolveType, packageName, modulePath, containingModulesPublic);
        }
    }
    else if (const auto *module = dynamic_cast<const ModuleDecl *>(&declaration)) {
        const std::string childPath = JoinModulePath(modulePath, module->name);
        Scope *moduleScope = nullptr;
        if (Symbol *existing = scope.Lookup(module->name);
            existing && existing->kind == Symbol::Kind::Module && existing->moduleScope) {
            moduleScope = existing->moduleScope;
            existing->isPublic = existing->isPublic || module->isPublic;
            existing->isEffectivelyPublic = existing->isEffectivelyPublic || isEffectivelyPublic;
        }
        else {
            moduleScope = &CreateScope(scope);
            Symbol symbol;
            symbol.kind = Symbol::Kind::Module;
            symbol.name = module->name;
            symbol.location = declaration.location;
            symbol.isPublic = module->isPublic;
            symbol.isEffectivelyPublic = isEffectivelyPublic;
            symbol.ownerPackage = ownerPackage;
            symbol.modulePath = childPath;
            symbol.moduleScope = moduleScope;
            if (scope.Define(symbol, diagnostics, sourceName) && isGlobal) {
                publicSymbols.push_back(
                    {SemanticSymbol::Kind::Module, module->name, sourceName, declaration.location, {}, false});
            }
        }
        if (packageName) {
            packageScopes[*packageName][childPath] = moduleScope;
        }
        for (const auto &item : module->items) {
            CollectDeclaration(*item, *moduleScope, sourceName, resolveType, packageName, childPath,
                               isEffectivelyPublic);
        }
    }
    else if (const auto *implementation = dynamic_cast<const ImplDecl *>(&declaration)) {
        implementations.push_back(implementation);
        const std::string typeName = implementation->typeName.starts_with("Slice<")
                                       ? implementation->typeName
                                       : BaseTypeName(implementation->typeName);
        const Symbol *extendedSymbol = scope.Lookup(typeName);
        const bool extendedTypePublic = !extendedSymbol || extendedSymbol->isEffectivelyPublic;
        for (const auto &method : implementation->methods) {
            methods[typeName][method->name].push_back(method.get());
            methodImplementations[method.get()] = implementation;
            functionScopes[method.get()] = &scope;
            functionSources[method.get()] = sourceName;
            functionModulePaths[method.get()] =
                packageName && !packageName->empty() ? JoinModulePath(*packageName, modulePath) : modulePath;
            declarationInfos.insert_or_assign(
                method.get(), DeclarationInfo{ownerPackage, modulePath, sourceName,
                                              containingModulesPublic && extendedTypePublic && method->isPublic});
        }
        if (implementation->interfaceName) {
            implementedInterfaces[typeName].insert(*implementation->interfaceName);
        }
    }
}

const SemanticProgramIndex::DeclarationInfo *SemanticProgramIndex::InfoFor(const Decl &declaration) const {
    const auto found = declarationInfos.find(&declaration);
    return found == declarationInfos.end() ? nullptr : &found->second;
}

void SemanticProgramIndex::FinalizeVisibility() {
    const auto finalizeScope = [&](auto &&self, Scope &scope, const bool containingModulesPublic) -> void {
        for (const auto &[name, unused] : scope.Table()) {
            (void)unused;
            Symbol *symbol = scope.LookupLocal(name);
            if (!symbol || symbol->ownerPackage == "<builtin>") {
                continue;
            }
            symbol->isEffectivelyPublic = containingModulesPublic && symbol->isPublic;
            if (symbol->kind == Symbol::Kind::Module && symbol->moduleScope) {
                self(self, *symbol->moduleScope, symbol->isEffectivelyPublic);
            }
        }
    };
    for (const auto &[package, scopesByPath] : packageScopes) {
        (void)package;
        if (const auto root = scopesByPath.find(""); root != scopesByPath.end()) {
            finalizeScope(finalizeScope, *root->second, true);
        }
    }

    const auto containingModulesArePublic = [&](const DeclarationInfo &info) {
        if (info.modulePath.empty()) {
            return true;
        }
        const auto package = packageScopes.find(info.ownerPackage);
        if (package == packageScopes.end()) {
            return false;
        }
        const auto root = package->second.find("");
        if (root == package->second.end()) {
            return false;
        }
        Scope *scope = root->second;
        std::size_t start = 0;
        while (start < info.modulePath.size()) {
            const std::size_t separator = info.modulePath.find("::", start);
            const std::string name = info.modulePath.substr(start, separator - start);
            const Symbol *module = scope->LookupLocal(name);
            if (!module || module->kind != Symbol::Kind::Module || !module->moduleScope ||
                !module->isEffectivelyPublic) {
                return false;
            }
            scope = module->moduleScope;
            if (separator == std::string::npos) {
                break;
            }
            start = separator + 2;
        }
        return true;
    };
    for (auto &[declaration, info] : declarationInfos) {
        info.isEffectivelyPublic = declaration->isPublic && containingModulesArePublic(info);
    }

    for (const auto &[method, implementation] : methodImplementations) {
        const auto info = declarationInfos.find(method);
        const auto scope = functionScopes.find(method);
        if (info == declarationInfos.end() || scope == functionScopes.end()) {
            continue;
        }
        const std::string typeName = implementation->typeName.starts_with("Slice<")
                                       ? implementation->typeName
                                       : BaseTypeName(implementation->typeName);
        const Symbol *extendedType = scope->second->Lookup(typeName);
        info->second.isEffectivelyPublic =
            info->second.isEffectivelyPublic && extendedType && extendedType->isEffectivelyPublic;
    }
}

namespace {
/// Look `name` up in the per-source index `bySource`.
template <typename Map>
[[nodiscard]] auto DeclaredIn(const Map &bySource, const std::string &sourceName, const std::string &name) {
    using Declaration = typename Map::mapped_type::mapped_type;
    const auto source = bySource.find(sourceName);
    if (source == bySource.end()) {
        return Declaration{nullptr};
    }
    const auto declared = source->second.find(name);
    return declared == source->second.end() ? Declaration{nullptr} : declared->second;
}
} // namespace

const StructDecl *SemanticProgramIndex::StructIn(const std::string &sourceName, const std::string &name) const {
    return DeclaredIn(structsBySource, sourceName, name);
}

const EnumDecl *SemanticProgramIndex::EnumIn(const std::string &sourceName, const std::string &name) const {
    return DeclaredIn(enumsBySource, sourceName, name);
}

const std::vector<TypeParameter> *SemanticProgramIndex::TypeParamsIn(const std::string &sourceName,
                                                                     const std::string &name) const {
    if (const StructDecl *structure = StructIn(sourceName, name)) {
        return &structure->typeParams;
    }
    if (const EnumDecl *enumeration = EnumIn(sourceName, name)) {
        return &enumeration->typeParams;
    }
    return nullptr;
}
} // namespace Rux::SemanticDetail
