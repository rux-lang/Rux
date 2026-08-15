#include "Lowering/AstToHir/Detail/AstToHirContext.h"

#include <cassert>
#include <filesystem>
#include <utility>

namespace Rux::AstToHirDetail {
HirScope::HirScope(HirScope *parentScope)
    : parent(parentScope) {
}

void HirScope::Define(HirSymbol symbol) {
    if (auto it = table.find(symbol.name); it != table.end()) {
        if (it->second.kind == HirSymbol::Kind::Func && symbol.kind == HirSymbol::Kind::Func) {
            it->second.funcOverloads.insert(it->second.funcOverloads.end(), symbol.funcOverloads.begin(),
                                            symbol.funcOverloads.end());
            if (it->second.type.IsUnknown() && !symbol.type.IsUnknown()) {
                it->second.type = std::move(symbol.type);
            }
        }
        return;
    }
    table.emplace(symbol.name, std::move(symbol));
}

HirSymbol *HirScope::Lookup(const std::string &name) {
    if (auto it = table.find(name); it != table.end()) {
        return &it->second;
    }
    return parent ? parent->Lookup(name) : nullptr;
}

HirScope *HirScope::Parent() const {
    return parent;
}

AstToHirContext::AstToHirContext(const SemanticModel &inputModel, const std::vector<const Module *> &inputModules,
                                 const CompileTimeContext &inputCompileTimeContext,
                                 std::vector<Diagnostic> &outputDiagnostics)
    : model(inputModel)
    , modules(inputModules)
    , context(inputCompileTimeContext)
    , globalScope(nullptr)
    , currentScope(&globalScope)
    , diagnostics(outputDiagnostics) {
}

AstToHirContext::~AstToHirContext() = default;

HirPackage AstToHirContext::Run() {
    RegisterBuiltins();
    for (const Module *module : modules) {
        CollectModule(*module);
    }

    HirPackage package;
    for (const Module *module : modules) {
        package.modules.push_back(LowerModule(*module));
    }
    return package;
}

void AstToHirContext::PushScope() {
    ownedScopes.push_back(std::make_unique<HirScope>(currentScope));
    currentScope = ownedScopes.back().get();
    constIntegerScopes.emplace_back();
}

void AstToHirContext::PopScope() {
    assert(currentScope->Parent() != nullptr && "cannot pop global scope");
    currentScope = currentScope->Parent();
    if (constIntegerScopes.size() > 1) {
        constIntegerScopes.pop_back();
    }
}

void AstToHirContext::Define(HirSymbol symbol) const {
    currentScope->Define(std::move(symbol));
}

void AstToHirContext::RegisterBuiltins() {
    const auto add = [this](const char *name, TypeRef type) {
        HirSymbol symbol;
        symbol.kind = HirSymbol::Kind::Type;
        symbol.name = name;
        symbol.type = std::move(type);
        globalScope.Define(std::move(symbol));
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
    add("byte", TypeRef::MakeByte());
    add("uint8", TypeRef::MakeUInt8());
    add("uint16", TypeRef::MakeUInt16());
    add("uint32", TypeRef::MakeUInt32());
    add("uint64", TypeRef::MakeUInt64());
    add("uint", TypeRef::MakeUInt());
    add("float32", TypeRef::MakeFloat32());
    add("float64", TypeRef::MakeFloat64());
    add("float", TypeRef::MakeFloat());
}

void AstToHirContext::CollectModule(const Module &module) {
    currentFile = module.name;
    for (const auto &declaration : module.items) {
        CollectDecl(*declaration);
    }
}

TypeRef AstToHirContext::MakeFuncType(const std::vector<Param> &params, const std::optional<TypeExprPtr> &returnType,
                                      const std::vector<std::string> &typeParams) {
    const auto savedTypeParams = currentTypeParams;
    currentTypeParams = typeParams;

    std::vector<TypeRef> paramTypes;
    for (const auto &param : params) {
        if (!param.isVariadic) {
            paramTypes.push_back(ResolveType(*param.type));
        }
    }
    TypeRef resultType = returnType ? ResolveType(**returnType) : TypeRef::MakeOpaque();

    currentTypeParams = savedTypeParams;
    return TypeRef::MakeFunc(std::move(paramTypes), std::move(resultType));
}

void AstToHirContext::CollectDecl(const Decl &decl) {
    const auto simple = [this](HirSymbol::Kind kind, const std::string &name, TypeRef type = {}) {
        HirSymbol symbol;
        symbol.kind = kind;
        symbol.name = name;
        symbol.type = std::move(type);
        globalScope.Define(std::move(symbol));
    };
    if (const auto *function = dynamic_cast<const FuncDecl *>(&decl)) {
        HirSymbol symbol;
        symbol.kind = HirSymbol::Kind::Func;
        symbol.name = function->name;
        symbol.type = MakeFuncType(function->params, function->returnType, function->typeParams);
        symbol.isNoReturn = function->isNoReturn;
        symbol.intrinsicName = function->intrinsicName;
        symbol.funcOverloads.push_back(function);
        globalScope.Define(std::move(symbol));
    }
    else if (const auto *structDecl = dynamic_cast<const StructDecl *>(&decl)) {
        structDecls[structDecl->name] = structDecl;
        simple(HirSymbol::Kind::Type, structDecl->name, TypeRef::MakeNamed(structDecl->name));
    }
    else if (const auto *enumDecl = dynamic_cast<const EnumDecl *>(&decl)) {
        enumDecls[enumDecl->name] = enumDecl;
        simple(HirSymbol::Kind::Type, enumDecl->name, EnumType(*enumDecl));
    }
    else if (const auto *unionDecl = dynamic_cast<const UnionDecl *>(&decl)) {
        unionDecls[unionDecl->name] = unionDecl;
        simple(HirSymbol::Kind::Type, unionDecl->name, TypeRef::MakeNamed(unionDecl->name));
    }
    else if (const auto *interfaceDecl = dynamic_cast<const InterfaceDecl *>(&decl)) {
        simple(HirSymbol::Kind::Interface, interfaceDecl->name, TypeRef::MakeNamed(interfaceDecl->name));
        interfaceDecls[interfaceDecl->name] = interfaceDecl;
    }
    else if (const auto *constDecl = dynamic_cast<const ConstDecl *>(&decl)) {
        HirSymbol symbol;
        symbol.kind = HirSymbol::Kind::Const;
        symbol.name = constDecl->name;
        symbol.type = constDecl->type ? ResolveType(**constDecl->type) : TypeRef{};
        symbol.intrinsicName = constDecl->intrinsicName;
        globalScope.Define(std::move(symbol));
    }
    else if (const auto *aliasDecl = dynamic_cast<const TypeAliasDecl *>(&decl)) {
        simple(HirSymbol::Kind::Type, aliasDecl->name, ResolveType(*aliasDecl->type));
    }
    else if (const auto *externFunction = dynamic_cast<const ExternFuncDecl *>(&decl)) {
        HirSymbol symbol;
        symbol.kind = HirSymbol::Kind::Func;
        symbol.name = externFunction->name;
        symbol.type = MakeFuncType(externFunction->params, externFunction->returnType);
        symbol.isNoReturn = externFunction->isNoReturn;
        globalScope.Define(std::move(symbol));
    }
    else if (const auto *externVariable = dynamic_cast<const ExternVarDecl *>(&decl)) {
        HirSymbol symbol;
        symbol.kind = HirSymbol::Kind::Var;
        symbol.name = externVariable->name;
        symbol.isMut = true;
        globalScope.Define(std::move(symbol));
    }
    else if (const auto *externBlock = dynamic_cast<const ExternBlockDecl *>(&decl)) {
        for (const auto &item : externBlock->items) {
            CollectDecl(*item);
        }
    }
    else if (const auto *moduleDecl = dynamic_cast<const ModuleDecl *>(&decl)) {
        for (const auto &item : moduleDecl->items) {
            CollectDecl(*item);
        }
    }
    else if (const auto *implDecl = dynamic_cast<const ImplDecl *>(&decl)) {
        const std::string typeName =
            implDecl->typeName.starts_with("Slice<") ? implDecl->typeName : BaseTypeName(implDecl->typeName);
        for (const auto &method : implDecl->methods) {
            methodsByType[typeName][method->name].push_back(method.get());
            methodImpl[method.get()] = implDecl;
        }
        if (implDecl->interfaceName) {
            if (const auto *identity = model.TryGetVtableIdentity(*implDecl)) {
                typeInterfaceVtables[typeName][*implDecl->interfaceName] = identity->linkerName;
            }
        }
    }
}

namespace {
std::string FilePathToModulePath(const std::string &filePath) {
    const std::string generic = std::filesystem::path(filePath).generic_string();
    std::vector<std::string> parts;
    std::string current;
    for (const char character : generic) {
        if (character == '/') {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
        }
        else {
            current += character;
        }
    }
    if (!current.empty()) {
        parts.push_back(current);
    }

    std::size_t sourceIndex = std::string::npos;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (parts[i] == "Src" || parts[i] == "src") {
            sourceIndex = i;
        }
    }

    std::vector<std::string> moduleParts;
    if (sourceIndex != std::string::npos && sourceIndex + 1 < parts.size()) {
        for (std::size_t i = sourceIndex + 1; i < parts.size(); ++i) {
            std::string part = parts[i];
            if (i + 1 == parts.size()) {
                if (const auto dot = part.rfind('.'); dot != std::string::npos) {
                    part = part.substr(0, dot);
                }
            }
            moduleParts.push_back(std::move(part));
        }
    }
    else {
        std::string stem = parts.empty() ? filePath : parts.back();
        if (const auto dot = stem.rfind('.'); dot != std::string::npos) {
            stem = stem.substr(0, dot);
        }
        moduleParts.push_back(std::move(stem));
    }

    std::string result;
    for (std::size_t i = 0; i < moduleParts.size(); ++i) {
        if (i != 0) {
            result += "::";
        }
        result += moduleParts[i];
    }
    return result;
}
} // namespace

HirModule AstToHirContext::LowerModule(const Module &module) {
    currentFile = module.name;
    currentModulePath = FilePathToModulePath(module.name);
    declModulePath.clear();
    HirModule loweredModule;
    loweredModule.name = module.name;
    for (const auto &declaration : module.items) {
        LowerTopLevelDecl(*declaration, loweredModule);
    }
    std::size_t processed = 0;
    while (processed < monomorphizedFuncs.size()) {
        loweredModule.funcs.push_back(std::move(monomorphizedFuncs[processed]));
        ++processed;
    }
    monomorphizedFuncs.clear();
    generatedMonomorphizedFuncNames.clear();
    return loweredModule;
}

void AstToHirContext::LowerTopLevelDecl(const Decl &decl, HirModule &module) {
    if (const auto *function = dynamic_cast<const FuncDecl *>(&decl)) {
        if (!function->intrinsicName.empty() && !function->body && !function->isAsm) {
            return;
        }
        HirFunc loweredFunction = LowerFunc(*function);
        loweredFunction.name = FunctionCalleeName(*function);
        module.funcs.push_back(std::move(loweredFunction));
    }
    else if (const auto *structDecl = dynamic_cast<const StructDecl *>(&decl)) {
        module.structs.push_back(LowerStruct(*structDecl));
    }
    else if (const auto *enumDecl = dynamic_cast<const EnumDecl *>(&decl)) {
        module.enums.push_back(LowerEnum(*enumDecl));
    }
    else if (const auto *unionDecl = dynamic_cast<const UnionDecl *>(&decl)) {
        module.unions.push_back(LowerUnion(*unionDecl));
    }
    else if (const auto *interfaceDecl = dynamic_cast<const InterfaceDecl *>(&decl)) {
        module.interfaces.push_back(LowerInterface(*interfaceDecl));
    }
    else if (const auto *implDecl = dynamic_cast<const ImplDecl *>(&decl)) {
        if (ImplTypeParams(*implDecl).empty()) {
            module.impls.push_back(LowerImpl(*implDecl));
        }
    }
    else if (const auto *constDecl = dynamic_cast<const ConstDecl *>(&decl)) {
        if (constDecl->intrinsicName.empty()) {
            module.consts.push_back(LowerConst(*constDecl));
        }
    }
    else if (const auto *externFunction = dynamic_cast<const ExternFuncDecl *>(&decl)) {
        module.externFuncs.push_back(LowerExternFunc(*externFunction));
    }
    else if (const auto *externVariable = dynamic_cast<const ExternVarDecl *>(&decl)) {
        module.externVars.push_back(LowerExternVar(*externVariable));
    }
    else if (const auto *externBlock = dynamic_cast<const ExternBlockDecl *>(&decl)) {
        for (const auto &item : externBlock->items) {
            LowerTopLevelDecl(*item, module);
        }
    }
    else if (const auto *aliasDecl = dynamic_cast<const TypeAliasDecl *>(&decl)) {
        module.typeAliases.push_back(LowerTypeAlias(*aliasDecl));
    }
    else if (const auto *moduleDecl = dynamic_cast<const ModuleDecl *>(&decl)) {
        const std::string savedModulePath = currentModulePath;
        const std::string savedDeclModulePath = declModulePath;
        currentModulePath = currentModulePath.empty() ? moduleDecl->name : currentModulePath + "::" + moduleDecl->name;
        declModulePath = declModulePath.empty() ? moduleDecl->name : declModulePath + "::" + moduleDecl->name;
        for (const auto &item : moduleDecl->items) {
            LowerTopLevelDecl(*item, module);
        }
        currentModulePath = savedModulePath;
        declModulePath = savedDeclModulePath;
    }
}
} // namespace Rux::AstToHirDetail
