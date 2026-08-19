// Package and scope setup for AST-to-HIR lowering: the entry point that walks
// the modules, and the scope stack every other lowering file resolves names
// against.

#include "Lowering/AstToHir/Detail/AstToHirContext.h"

#include "Semantic/PrimitiveCatalog.h"

#include <algorithm>
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
    , cleanupPlanner(inputModel)
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
    package.dropGlues.reserve(model.DropGluePlans().size());
    for (const auto &[_, plan] : model.DropGluePlans()) {
        package.dropGlues.push_back(plan);
    }
    std::ranges::sort(package.dropGlues, {}, [](const DropGluePlan &plan) { return plan.type.ToString(); });
    ResolveDropGlue(package);
    return package;
}

/// Fill in the parts of a destruction recipe that only lowering can answer: which symbol a type's own `Drop` reaches
/// the linker under, and which tag value a variant is stored with.
///
/// The plan is walked here rather than while it is built because naming a generic implementation's `Drop` is also what
/// instantiates it. Nothing else calls it -- destruction has no call site in source -- so a `Vector<File>` that is only
/// ever dropped would otherwise name a body that was never lowered.
void AstToHirContext::ResolveDropGlue(HirPackage &package) {
    if (package.modules.empty()) {
        return;
    }
    // Every name the package already emitted, so that naming a `Drop` an ordinary call site had already instantiated
    // resolves to that body instead of lowering a second one under the same symbol.
    generatedMonomorphizedFuncNames.clear();
    for (const HirModule &module : package.modules) {
        for (const HirFunc &function : module.funcs) {
            generatedMonomorphizedFuncNames.insert(function.name);
        }
        for (const HirImplBlock &implementation : module.impls) {
            for (const std::string &name : implementation.methodLinkerNames) {
                generatedMonomorphizedFuncNames.insert(name);
            }
        }
    }
    for (DropGluePlan &plan : package.dropGlues) {
        ResolveDropGlueSteps(plan.steps);
    }
    // An instantiation requested above belongs to whichever module is lowered last: every module has already been
    // lowered, and a function has to live in one of them to be emitted at all.
    for (HirFunc &instance : monomorphizedFuncs) {
        package.modules.back().funcs.push_back(std::move(instance));
    }
    monomorphizedFuncs.clear();
}

void AstToHirContext::ResolveDropGlueSteps(std::vector<DropGlueStep> &steps) {
    for (DropGlueStep &step : steps) {
        if (step.kind == DropGlueStep::Kind::InvokeDrop) {
            step.dropSymbol = DropMethodSymbol(step.type);
        }
        else if (step.kind == DropGlueStep::Kind::EnumVariant) {
            step.discriminant = LookupEnumVariantDiscriminant(NamedBaseTypeName(step.type), step.name)
                                    .value_or(std::to_string(step.ordinal));
        }
        ResolveDropGlueSteps(step.children);
    }
}

std::string AstToHirContext::DropMethodSymbol(const TypeRef &type) {
    const std::string typeName = NamedBaseTypeName(type);
    const auto byType = methodsByType.find(typeName);
    if (byType == methodsByType.end()) {
        return {};
    }
    const auto byName = byType->second.find("Drop");
    if (byName == byType->second.end() || byName->second.empty()) {
        return {};
    }
    return ConcreteMethodCalleeName(typeName, type, *byName->second.front());
}

void AstToHirContext::PushScope() {
    ownedScopes.push_back(std::make_unique<HirScope>(currentScope));
    currentScope = ownedScopes.back().get();
    constIntegerScopes.emplace_back();
    cleanupPlanner.PushScope();
}

void AstToHirContext::PopScope() {
    assert(currentScope->Parent() != nullptr && "cannot pop global scope");
    currentScope = currentScope->Parent();
    if (constIntegerScopes.size() > 1) {
        constIntegerScopes.pop_back();
    }
    cleanupPlanner.PopScope();
}

void AstToHirContext::Define(HirSymbol symbol) const {
    currentScope->Define(std::move(symbol));
}

std::uint64_t AstToHirContext::RegisterCleanupBinding(const std::string &name, const TypeRef &type,
                                                      SourceLocation origin) {
    return cleanupPlanner.Register(name, type, origin);
}

std::vector<HirDropAction> AstToHirContext::CurrentScopeCleanups() const {
    return cleanupPlanner.CurrentScopeActions();
}

std::vector<HirDropAction> AstToHirContext::FunctionCleanups() const {
    return cleanupPlanner.FunctionExitActions();
}

void AstToHirContext::AppendCurrentScopeCleanups(HirBlock &block) const {
    for (HirDropAction action : CurrentScopeCleanups()) {
        auto cleanup = std::make_unique<HirDropStmt>();
        cleanup->location = block.location;
        cleanup->action = std::move(action);
        block.stmts.push_back(std::move(cleanup));
    }
}

std::uint64_t AstToHirContext::BindingId(const HirExpr &expression) const {
    std::string_view name;
    if (const auto *variable = dynamic_cast<const HirVarExpr *>(&expression)) {
        name = variable->name;
    }
    else if (dynamic_cast<const HirSelfExpr *>(&expression)) {
        name = "self";
    }
    if (name.empty()) {
        return 0;
    }
    const HirSymbol *symbol = currentScope->Lookup(std::string(name));
    return symbol && symbol->kind == HirSymbol::Kind::Var ? symbol->bindingId : 0;
}

std::uint64_t AstToHirContext::ConsumedBindingId(const HirExpr &expression) const {
    return BindingId(expression);
}

std::optional<HirDropAction> AstToHirContext::OverwriteCleanup(const HirExpr &target, SourceLocation origin) const {
    const std::uint64_t bindingId = BindingId(target);
    if (const std::optional<HirDropAction> action = cleanupPlanner.ActionFor(bindingId)) {
        return action;
    }
    const DropGluePlan *glue = model.TryGetDropGlue(target.type);
    if (!glue) {
        return std::nullopt;
    }
    return HirDropAction{0, "<place>", target.type, glue->symbol, origin};
}

std::optional<HirPartialDropAction> AstToHirContext::PartialCleanup(const HirPartialDropAction::Kind kind,
                                                                    const TypeRef &type, const std::size_t ordinal,
                                                                    std::string name, SourceLocation origin) const {
    const DropGluePlan *glue = model.TryGetDropGlue(type);
    if (!glue) {
        return std::nullopt;
    }
    return HirPartialDropAction{kind, ordinal, std::move(name), type, glue->symbol, origin};
}

void AstToHirContext::AppendFailureCleanup(std::vector<HirFailureCleanup> &edges,
                                           const std::vector<HirPartialDropAction> &completed) {
    edges.emplace_back(completed.rbegin(), completed.rend());
}

void AstToHirContext::RegisterBuiltins() {
    const auto add = [this](const std::string_view name, TypeRef type) {
        HirSymbol symbol;
        symbol.kind = HirSymbol::Kind::Type;
        symbol.name = name;
        symbol.type = std::move(type);
        globalScope.Define(std::move(symbol));
    };
    add("opaque", TypeRef::MakeOpaque());
    // Semantic analysis has already rejected any use of a reserved primitive, so lowering binds only the spellings
    // that carry a representation.
    for (const PrimitiveInfo &primitive : PrimitiveCatalog()) {
        if (primitive.implemented) {
            add(primitive.name, TypeRef::MakePrimitive(primitive.kind));
        }
    }
    for (const PrimitiveAlias &alias : PrimitiveAliases()) {
        add(alias.name, TypeRef::MakePrimitive(alias.kind));
    }
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
        symbol.type = MakeFuncType(function->params, function->returnType, TypeParameterNames(function->typeParams));
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
/// The module path a source file corresponds to, relative to the package root, so the identity does not depend on where
/// the package was checked out.
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

    // Lowering an instantiation's fields resolves types of its own, so the queue is walked by index rather than
    // iterated: entries appended along the way are lowered in the same pass.
    for (std::size_t pending = 0; pending < pendingStructInstantiations.size(); ++pending) {
        const std::string name = pendingStructInstantiations[pending];
        const auto declaration = structDecls.find(BaseTypeName(name));
        if (declaration == structDecls.end()) {
            continue;
        }
        loweredModule.structs.push_back(
            LowerStructInstantiation(*declaration->second, name, ParseTypeArgsFromTypeName(name)));
    }
    pendingStructInstantiations.clear();
    seenStructInstantiations.clear();
    return loweredModule;
}

void AstToHirContext::LowerTopLevelDecl(const Decl &decl, HirModule &module) {
    if (const auto *function = dynamic_cast<const FuncDecl *>(&decl)) {
        if (!function->intrinsicName.empty() && !function->body && !function->isAsm) {
            return;
        }
        // A constrained generic exists only as its instantiations. Its body calls operations whose target is chosen per
        // type argument, so the symbolic form has nothing to call -- the same reason a generic extend block below is
        // lowered only through the instances its uses ask for.
        if (std::ranges::any_of(function->typeParams,
                                [](const TypeParameter &parameter) { return !parameter.bounds.empty(); })) {
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
