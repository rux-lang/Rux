#include "Semantic/SemanticProgramIndex.h"

#include <format>
#include <utility>

namespace Rux::SemanticDetail {
namespace {
std::string JoinModulePath(const std::string &prefix, const std::string &name) {
    return prefix.empty() ? name : prefix + "::" + name;
}

std::string BaseTypeName(const std::string &name) {
    const std::size_t position = name.find('<');
    return position == std::string::npos ? name : name.substr(0, position);
}
} // namespace

Scope::Scope(Scope *parentScope)
    : parent(parentScope) {
}

bool Scope::Define(Symbol symbol, std::vector<SemanticDiagnostic> &diagnostics, const std::string &sourceName) {
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
            return true;
        }
        diagnostics.push_back({SemanticDiagnostic::Severity::Error, sourceName, symbol.location,
                               std::format("'{}' is already defined (first defined at {}:{})", symbol.name,
                                           iterator->second.location.line, iterator->second.location.column)});
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
                                              const std::string &modulePath) {
    const bool isGlobal = &scope == &globalScope;
    auto defineSimple = [&](Symbol::Kind kind, const std::string &name, SemanticSymbol::Kind publicKind,
                            std::string resolvedType = {}, bool isMut = false) {
        Symbol symbol;
        symbol.kind = kind;
        symbol.name = name;
        symbol.location = declaration.location;
        symbol.isMut = isMut;
        if (scope.Define(symbol, diagnostics, sourceName) && isGlobal) {
            publicSymbols.push_back(
                {publicKind, name, sourceName, declaration.location, std::move(resolvedType), isMut});
        }
    };

    if (const auto *function = dynamic_cast<const FuncDecl *>(&declaration)) {
        functionScopes[function] = &scope;
        functionSources[function] = sourceName;
        functions[function->name].push_back(function);
        functionModulePaths[function] = modulePath;
        Symbol symbol;
        symbol.kind = Symbol::Kind::Func;
        symbol.name = function->name;
        symbol.location = function->location;
        symbol.intrinsicName = function->intrinsicName;
        symbol.funcOverloads.push_back(function);
        if (scope.Define(symbol, diagnostics, sourceName) && isGlobal) {
            publicSymbols.push_back(
                {SemanticSymbol::Kind::Func, function->name, sourceName, function->location, {}, false});
        }
    }
    else if (const auto *structure = dynamic_cast<const StructDecl *>(&declaration)) {
        structs[structure->name] = structure;
        defineSimple(Symbol::Kind::Type, structure->name, SemanticSymbol::Kind::Type, "struct");
    }
    else if (const auto *enumeration = dynamic_cast<const EnumDecl *>(&declaration)) {
        enums[enumeration->name] = enumeration;
        defineSimple(Symbol::Kind::Type, enumeration->name, SemanticSymbol::Kind::Type, "enum");
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
            CollectDeclaration(*item, scope, sourceName, resolveType, packageName, modulePath);
        }
    }
    else if (const auto *module = dynamic_cast<const ModuleDecl *>(&declaration)) {
        Scope *moduleScope = nullptr;
        if (Symbol *existing = scope.Lookup(module->name);
            existing && existing->kind == Symbol::Kind::Module && existing->moduleScope) {
            moduleScope = existing->moduleScope;
        }
        else {
            moduleScope = &CreateScope(scope);
            Symbol symbol;
            symbol.kind = Symbol::Kind::Module;
            symbol.name = module->name;
            symbol.location = declaration.location;
            symbol.moduleScope = moduleScope;
            if (scope.Define(symbol, diagnostics, sourceName) && isGlobal) {
                publicSymbols.push_back(
                    {SemanticSymbol::Kind::Module, module->name, sourceName, declaration.location, {}, false});
            }
        }
        const std::string childPath = JoinModulePath(modulePath, module->name);
        if (packageName) {
            packageScopes[*packageName][childPath] = moduleScope;
        }
        for (const auto &item : module->items) {
            CollectDeclaration(*item, *moduleScope, sourceName, resolveType, packageName, childPath);
        }
    }
    else if (const auto *implementation = dynamic_cast<const ImplDecl *>(&declaration)) {
        implementations.push_back(implementation);
        const std::string typeName = implementation->typeName.starts_with("Slice<")
                                       ? implementation->typeName
                                       : BaseTypeName(implementation->typeName);
        for (const auto &method : implementation->methods) {
            methods[typeName][method->name].push_back(method.get());
            methodImplementations[method.get()] = implementation;
        }
        if (implementation->interfaceName) {
            implementedInterfaces[typeName].insert(*implementation->interfaceName);
        }
    }
}

} // namespace Rux::SemanticDetail
