// Package and scope setup for AST-to-HIR lowering: the entry point that walks
// the modules, and the scope stack every other lowering file resolves names
// against.

#include "Lowering/AstToHir/Detail/AstToHirContext.h"

#include "Types/NominalName.h"
#include "Types/PrimitiveCatalog.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
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
    , diagnostics(&outputDiagnostics) {
}

AstToHirContext::AstToHirContext(const AstToHirContext &prepared, std::vector<Diagnostic> &outputDiagnostics)
    : model(prepared.model)
    , modules(prepared.modules)
    , context(prepared.context)
    , globalScope(prepared.globalScope)
    , currentScope(&globalScope)
    , structDecls(prepared.structDecls)
    , enumDecls(prepared.enumDecls)
    , unionDecls(prepared.unionDecls)
    , methodsByType(prepared.methodsByType)
    , interfaceDecls(prepared.interfaceDecls)
    , typeInterfaceVtables(prepared.typeInterfaceVtables)
    , methodImpl(prepared.methodImpl)
    , cleanupPlanner(prepared.model)
    , diagnostics(&outputDiagnostics) {
}

AstToHirContext::~AstToHirContext() = default;

void AstToHirContext::ResetModuleState() {
    propagationOrdinal = 0;
    coalescingOrdinal = 0;
    iterationOrdinal = 0;
    checkedArithmeticOrdinal = 0;
    zeroizationOrdinal = 0;
    nativeOrdinal = 0;
    returnOrdinal = 0;
    cleanupPlanner.ResetBindingIds();
}

HirPackage AstToHirContext::Run(Parallel::WorkerPool *pool) {
    RegisterBuiltins();
    for (const Module *module : modules) {
        CollectModule(*module);
    }

    HirPackage package;
    package.modules.resize(modules.size());
    if (pool == nullptr || pool->Jobs() == 1) {
        for (std::size_t index = 0; index < modules.size(); ++index) {
            package.modules[index] = LowerModule(*modules[index]);
        }
    }
    else {
        // Once the declarations are collected a module lowers on its own: the semantic model is read-only, and each
        // module emits the generic instances it uses itself. Every thread lowers with its own copy of the tables, and
        // diagnostics merge in module order.
        std::vector<std::unique_ptr<AstToHirContext>> workers(pool->Jobs());
        std::vector<std::vector<Diagnostic>> moduleDiagnostics(modules.size());
        pool->ForEachOnWorker(modules.size(), [&](const std::size_t index, const std::size_t worker) {
            auto &lowering = workers[worker];
            if (!lowering)
                lowering = std::make_unique<AstToHirContext>(*this, moduleDiagnostics[index]);
            lowering->diagnostics = &moduleDiagnostics[index];
            package.modules[index] = lowering->LowerModule(*modules[index]);
        });
        for (auto &reported : moduleDiagnostics) {
            diagnostics->insert(diagnostics->end(), std::make_move_iterator(reported.begin()),
                                std::make_move_iterator(reported.end()));
        }
    }
    package.dropGlues.reserve(model.DropGluePlans().size());
    for (const auto &[_, plan] : model.DropGluePlans()) {
        package.dropGlues.push_back(plan);
    }
    std::ranges::sort(package.dropGlues, {}, [](const DropGluePlan &plan) { return plan.type.ToString(); });
    // Destructor instances are lowered from the state the last module starts from, however the modules themselves were
    // lowered.
    if (!modules.empty()) {
        BeginModule(*modules.back());
    }
    ResolveDropGlue(package);
    for (const auto &[name, layout] : model.TypeLayouts()) {
        package.typeLayouts.emplace(name, HirTypeLayout{layout.size, layout.alignment});
    }
    return package;
}

/// Fill in the parts of a destruction recipe that only lowering can answer: which symbol a type's destructor reaches
/// the linker under, and which tag value a variant is stored with.
///
/// The plan is walked here rather than while it is built because naming a generic destructor is also what
/// instantiates it. Nothing else calls it -- destruction has no call site in source -- so a `Vector<File>` that is only
/// ever dropped would otherwise name a body that was never lowered.
void AstToHirContext::ResolveDropGlue(HirPackage &package) {
    if (package.modules.empty()) {
        return;
    }
    // The instances form a pool of their own, independent of what the modules instantiated: a module whose glue calls
    // one takes a private copy unless it already lowered that instance itself.
    generatedMonomorphizedFuncNames.clear();
    monomorphizedFuncs.clear();
    // A plan for a type that still names a type parameter describes nothing that can exist: there is no value of
    // `Box<T>` until `T` is chosen, and every choice has a plan of its own. Instantiating one would lower the generic
    // body with the parameter substituted for itself, which is how a `sizeof(T)` inside it ends up with no width.
    std::erase_if(package.dropGlues, [this](const DropGluePlan &plan) { return !TypeIsConcrete(plan.type); });
    for (DropGluePlan &plan : package.dropGlues) {
        ResolveDropGlueSteps(plan.steps);
    }
    package.dropGlueInstances = std::move(monomorphizedFuncs);
    monomorphizedFuncs.clear();
    generatedMonomorphizedFuncNames.clear();
}

void AstToHirContext::ResolveDropGlueSteps(std::vector<DropGlueStep> &steps) {
    for (DropGlueStep &step : steps) {
        if (step.kind == DropGlueStep::Kind::InvokeDrop) {
            step.dropSymbol = DestructorSymbol(step.type);
        }
        else if (step.kind == DropGlueStep::Kind::EnumVariant && step.discriminant.empty()) {
            step.discriminant = LookupEnumVariantDiscriminant(NamedBaseTypeName(step.type), step.name)
                                    .value_or(std::to_string(step.ordinal));
        }
        ResolveDropGlueSteps(step.children);
    }
}

/// Whether a type names a real type rather than a shape still waiting for its arguments.
///
/// A type parameter is not concrete, and neither is an instantiation one of whose arguments is a parameter. Both a
/// parameter and a declared type arrive here as a bare name, so what tells them apart is whether anything declares
/// the name: nothing declares a type parameter, and every struct, enum, union, interface and alias is defined in the
/// global scope by the time this runs.
///
/// Asking instead whether the argument matched a parameter of the type being instantiated -- which is what this did
/// -- catches `Box<T>` written inside `extend Box<T>` and nothing else. A container built on another one passes its
/// own parameter down, so `Tree<T, Unit>` inside `TreeSet<T>` was judged concrete because `T` is not one of `Tree`'s
/// parameters, and lowering then built a destructor for a type that cannot exist -- which asks for the size of a
/// node whose element type is still a parameter, and stops the compiler where there is nothing left to ask.
bool AstToHirContext::TypeIsConcrete(const TypeRef &type) {
    if (type.IsUnknown() || type.kind == TypeRef::Kind::TypeParam) {
        return false;
    }
    for (const TypeRef &inner : type.inner) {
        if (!TypeIsConcrete(inner)) {
            return false;
        }
    }
    if (type.kind != TypeRef::Kind::Named) {
        return true;
    }
    if (type.name.find('<') == std::string::npos) {
        // A bare name that nothing declares is a parameter. Primitives and the builtin aggregates never reach here
        // as a named type -- they are recognized while the name is parsed -- so a name is either declared or a
        // parameter, with nothing in between.
        if (structDecls.contains(type.name) || enumDecls.contains(type.name) || unionDecls.contains(type.name) ||
            interfaceDecls.contains(type.name))
            return true;
        const HirSymbol *declared = globalScope.Lookup(type.name);
        return declared != nullptr &&
               (declared->kind == HirSymbol::Kind::Type || declared->kind == HirSymbol::Kind::Interface);
    }
    for (const TypeRef &argument : ParseTypeArgsFromTypeName(type.name)) {
        if (!TypeIsConcrete(argument)) {
            return false;
        }
    }
    return true;
}

std::string AstToHirContext::DestructorSymbol(const TypeRef &type) {
    const std::string typeName = NamedBaseTypeName(type);
    const auto byType = methodsByType.find(typeName);
    if (byType == methodsByType.end()) {
        return {};
    }
    const auto destructor = byType->second.find("~" + UnqualifiedNominalName(typeName));
    return destructor == byType->second.end() || destructor->second.empty()
             ? std::string{}
             : ConcreteMethodCalleeName(typeName, type, *destructor->second.front());
}

void AstToHirContext::PushScope() {
    ownedScopes.push_back(std::make_unique<HirScope>(currentScope));
    currentScope = ownedScopes.back().get();
    constIntegerScopes.emplace_back();
    deferStack.emplace_back();
    cleanupPlanner.PushScope();
}

void AstToHirContext::PopScope() {
    assert(currentScope->Parent() != nullptr && "cannot pop global scope");
    currentScope = currentScope->Parent();
    if (constIntegerScopes.size() > 1) {
        constIntegerScopes.pop_back();
    }
    if (!deferStack.empty()) {
        deferStack.pop_back();
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
    if (const auto *variable = HirCast<const HirVarExpr>(&expression)) {
        name = variable->name;
    }
    else if (HirCast<const HirSelfExpr>(&expression)) {
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

std::optional<HirDropAction> AstToHirContext::OverwriteCleanup(const AssignExpr &assignment,
                                                               const HirExpr &target) const {
    const std::uint64_t bindingId = BindingId(target);
    if (const std::optional<HirDropAction> action = cleanupPlanner.ActionFor(bindingId)) {
        return action;
    }
    // Any other place holds an old value only where analysis found one; a place reached through a raw pointer is
    // initialized by the write and has nothing to destroy.
    const PlaceReplacement *replacement = model.TryGetPlaceReplacement(assignment);
    const DropGluePlan *glue = replacement ? model.TryGetDropGlue(target.type) : nullptr;
    if (!glue) {
        return std::nullopt;
    }
    HirDropAction action{0, "<place>", target.type, glue->symbol, assignment.location};
    action.destroysTarget = true;
    if (const auto *owner = AstCast<const IdentExpr>(replacement->owner)) {
        const HirSymbol *symbol = currentScope->Lookup(owner->name);
        // A value with a droppable part is droppable itself, so its binding always has a drop flag to consult.
        if (!symbol || symbol->kind != HirSymbol::Kind::Var || symbol->bindingId == 0) {
            return std::nullopt;
        }
        action.ownerBindingId = symbol->bindingId;
    }
    return action;
}

std::optional<HirPartialDropAction> AstToHirContext::PartialCleanup(const HirPartialDropAction::Kind kind,
                                                                    const TypeRef &type, const std::size_t ordinal,
                                                                    std::string name, SourceLocation origin,
                                                                    const CaseTypeForm form) const {
    const DropGluePlan *glue = model.TryGetDropGlue(type);
    if (!glue) {
        return std::nullopt;
    }
    return HirPartialDropAction{kind, form, ordinal, std::move(name), type, glue->symbol, origin};
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
    for (const PrimitiveInfo &primitive : PrimitiveCatalog()) {
        add(primitive.name, TypeRef::MakePrimitive(primitive.kind));
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

std::string AstToHirContext::NominalName(const Decl &declaration) const {
    return model.TryGetSymbolIdentity(declaration)->linkerName;
}

void AstToHirContext::CollectDecl(const Decl &decl) {
    const auto simple = [this](HirSymbol::Kind kind, const std::string &name, TypeRef type = {}) {
        HirSymbol symbol;
        symbol.kind = kind;
        symbol.name = name;
        symbol.type = std::move(type);
        globalScope.Define(std::move(symbol));
    };
    if (const auto *function = AstCast<const FuncDecl>(&decl)) {
        HirSymbol symbol;
        symbol.kind = HirSymbol::Kind::Func;
        symbol.name = function->name;
        symbol.type = MakeFuncType(function->params, function->returnType, TypeParameterNames(function->typeParams));
        symbol.isNoReturn = function->isNoReturn;
        symbol.intrinsicName = function->intrinsicName;
        symbol.funcOverloads.push_back(function);
        globalScope.Define(std::move(symbol));
    }
    else if (const auto *structDecl = AstCast<const StructDecl>(&decl)) {
        structDecls[NominalName(*structDecl)] = structDecl;
        simple(HirSymbol::Kind::Type, structDecl->name, TypeRef::MakeNamed(NominalName(*structDecl)));
    }
    else if (const auto *enumDecl = AstCast<const EnumDecl>(&decl)) {
        enumDecls[NominalName(*enumDecl)] = enumDecl;
        simple(HirSymbol::Kind::Type, enumDecl->name, EnumType(*enumDecl));
    }
    else if (const auto *unionDecl = AstCast<const UnionDecl>(&decl)) {
        unionDecls[NominalName(*unionDecl)] = unionDecl;
        simple(HirSymbol::Kind::Type, unionDecl->name, TypeRef::MakeNamed(NominalName(*unionDecl)));
    }
    else if (const auto *interfaceDecl = AstCast<const InterfaceDecl>(&decl)) {
        simple(HirSymbol::Kind::Interface, interfaceDecl->name, TypeRef::MakeNamed(NominalName(*interfaceDecl)));
        interfaceDecls[NominalName(*interfaceDecl)] = interfaceDecl;
    }
    else if (const auto *constDecl = AstCast<const ConstDecl>(&decl)) {
        HirSymbol symbol;
        symbol.kind = HirSymbol::Kind::Const;
        symbol.name = constDecl->name;
        symbol.type = constDecl->type ? ResolveType(**constDecl->type) : TypeRef{};
        symbol.intrinsicName = constDecl->intrinsicName;
        globalScope.Define(std::move(symbol));
    }
    else if (const auto *globalDecl = AstCast<const GlobalDecl>(&decl)) {
        HirSymbol symbol;
        symbol.kind = HirSymbol::Kind::Var;
        symbol.name = globalDecl->name;
        symbol.type = ResolveType(*globalDecl->type);
        symbol.isMut = true;
        globalScope.Define(std::move(symbol));
    }
    else if (const auto *aliasDecl = AstCast<const TypeAliasDecl>(&decl)) {
        simple(HirSymbol::Kind::Type, aliasDecl->name, ResolveType(*aliasDecl->type));
    }
    else if (const auto *externFunction = AstCast<const ExternFuncDecl>(&decl)) {
        HirSymbol symbol;
        symbol.kind = HirSymbol::Kind::Func;
        symbol.name = externFunction->name;
        symbol.type = MakeFuncType(externFunction->params, externFunction->returnType);
        symbol.isNoReturn = externFunction->isNoReturn;
        globalScope.Define(std::move(symbol));
    }
    else if (const auto *externVariable = AstCast<const ExternVarDecl>(&decl)) {
        HirSymbol symbol;
        symbol.kind = HirSymbol::Kind::Var;
        symbol.name = externVariable->name;
        symbol.isMut = true;
        globalScope.Define(std::move(symbol));
    }
    else if (const auto *externBlock = AstCast<const ExternBlockDecl>(&decl)) {
        for (const auto &item : externBlock->items) {
            CollectDecl(*item);
        }
    }
    else if (const auto *moduleDecl = AstCast<const ModuleDecl>(&decl)) {
        for (const auto &item : moduleDecl->items) {
            CollectDecl(*item);
        }
    }
    else if (const auto *implDecl = AstCast<const ImplDecl>(&decl)) {
        const TypeRef *receiver = implDecl->extendedType ? model.TryGetType(*implDecl->extendedType) : nullptr;
        const std::string typeName = receiver ? NamedBaseTypeName(*receiver) : BaseTypeName(implDecl->typeName);
        for (const auto &method : implDecl->methods) {
            methodsByType[typeName][method->name].push_back(method.get());
            methodImpl[method.get()] = implDecl;
        }
        if (implDecl->interfaceName) {
            if (const auto *identity = model.TryGetVtableIdentity(*implDecl)) {
                typeInterfaceVtables[typeName][identity->interfaceName] = identity->linkerName;
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

void AstToHirContext::BeginModule(const Module &module) {
    ResetModuleState();
    currentFile = module.name;
    currentModulePath = FilePathToModulePath(module.name);
    declModulePath.clear();
}

HirModule AstToHirContext::LowerModule(const Module &module) {
    BeginModule(module);
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
    if (const auto *function = AstCast<const FuncDecl>(&decl)) {
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
        // A fallible entry point is wrapped: the process exit status is an integer, so the wrapper keeps the `Main`
        // symbol and turns the outcome into one.
        if (loweredFunction.name == "Main" && loweredFunction.returnType.IsFallible()) {
            HirFunc entry = FallibleEntryWrapper(loweredFunction);
            loweredFunction.name = std::string(kFallibleMainBody);
            module.funcs.push_back(std::move(loweredFunction));
            module.funcs.push_back(std::move(entry));
            return;
        }
        module.funcs.push_back(std::move(loweredFunction));
    }
    else if (const auto *structDecl = AstCast<const StructDecl>(&decl)) {
        module.structs.push_back(LowerStruct(*structDecl));
    }
    else if (const auto *enumDecl = AstCast<const EnumDecl>(&decl)) {
        module.enums.push_back(LowerEnum(*enumDecl));
    }
    else if (const auto *unionDecl = AstCast<const UnionDecl>(&decl)) {
        module.unions.push_back(LowerUnion(*unionDecl));
    }
    else if (const auto *interfaceDecl = AstCast<const InterfaceDecl>(&decl)) {
        // A generic interface only constrains type parameters, so no value of it is ever laid out or dispatched.
        if (interfaceDecl->typeParams.empty()) {
            module.interfaces.push_back(LowerInterface(*interfaceDecl));
        }
    }
    else if (const auto *implDecl = AstCast<const ImplDecl>(&decl)) {
        if (ImplTypeParams(*implDecl).empty()) {
            module.impls.push_back(LowerImpl(*implDecl));
        }
    }
    else if (const auto *constDecl = AstCast<const ConstDecl>(&decl)) {
        if (constDecl->intrinsicName.empty()) {
            module.consts.push_back(LowerConst(*constDecl));
        }
    }
    else if (const auto *globalDecl = AstCast<const GlobalDecl>(&decl)) {
        module.globals.push_back(LowerGlobal(*globalDecl));
    }
    else if (const auto *externFunction = AstCast<const ExternFuncDecl>(&decl)) {
        module.externFuncs.push_back(LowerExternFunc(*externFunction));
    }
    else if (const auto *externVariable = AstCast<const ExternVarDecl>(&decl)) {
        module.externVars.push_back(LowerExternVar(*externVariable));
    }
    else if (const auto *externBlock = AstCast<const ExternBlockDecl>(&decl)) {
        for (const auto &item : externBlock->items) {
            LowerTopLevelDecl(*item, module);
        }
    }
    else if (const auto *aliasDecl = AstCast<const TypeAliasDecl>(&decl)) {
        module.typeAliases.push_back(LowerTypeAlias(*aliasDecl));
    }
    else if (const auto *moduleDecl = AstCast<const ModuleDecl>(&decl)) {
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

HirCopyPlan AstToHirContext::BuildEnumCopyPlan(const TypeRef &type, const EnumDecl &declaration) {
    HirCopyPlan plan;
    plan.type = type;
    plan.form = declaration.IsVariant() ? CaseTypeForm::Variant : CaseTypeForm::Enumeration;
    if (!declaration.IsVariant()) {
        return plan;
    }

    std::unordered_map<std::string, TypeRef> substitutions;
    const std::vector<TypeRef> arguments = ParseTypeArgsFromTypeName(type.name);
    const std::size_t count = std::min(arguments.size(), declaration.typeParams.size());
    for (std::size_t index = 0; index < count; ++index) {
        substitutions.emplace(declaration.typeParams[index].name, arguments[index]);
    }

    bool needsPlan = false;
    const std::string base = BaseTypeName(type.name);
    for (const EnumDecl::Variant &variant : declaration.variants) {
        plan.variantDiscriminants.push_back(LookupEnumVariantDiscriminant(base, variant.name).value_or("0"));
        std::vector<TypeRef> payloadTypes;
        std::vector<HirCopyPlan> payloadPlans;
        payloadTypes.reserve(variant.fields.size() + variant.namedFields.size());
        payloadPlans.reserve(variant.fields.size() + variant.namedFields.size());
        for (const TypeExprPtr &field : variant.fields) {
            TypeRef fieldType = ResolveTypeWithSubstitution(*field, substitutions);
            payloadTypes.push_back(fieldType);
            payloadPlans.push_back(BuildCopyPlan(fieldType));
            needsPlan = needsPlan || payloadPlans.back().kind != HirCopyPlan::Kind::Trivial;
        }
        for (const EnumDecl::Variant::NamedField &field : variant.namedFields) {
            TypeRef fieldType = ResolveTypeWithSubstitution(*field.type, substitutions);
            payloadTypes.push_back(fieldType);
            payloadPlans.push_back(BuildCopyPlan(fieldType));
            needsPlan = needsPlan || payloadPlans.back().kind != HirCopyPlan::Kind::Trivial;
        }
        plan.variantPayloadTypes.push_back(std::move(payloadTypes));
        plan.variantComponents.push_back(std::move(payloadPlans));
    }
    if (needsPlan) {
        plan.kind = HirCopyPlan::Kind::Enum;
    }
    else {
        plan.variantDiscriminants.clear();
        plan.variantPayloadTypes.clear();
        plan.variantComponents.clear();
    }
    return plan;
}

HirMovePlan AstToHirContext::BuildEnumMovePlan(const TypeRef &type, const EnumDecl &declaration) {
    HirMovePlan plan;
    plan.type = type;
    plan.form = declaration.IsVariant() ? CaseTypeForm::Variant : CaseTypeForm::Enumeration;
    if (!declaration.IsVariant()) {
        return plan;
    }

    std::unordered_map<std::string, TypeRef> substitutions;
    const std::vector<TypeRef> arguments = ParseTypeArgsFromTypeName(type.name);
    const std::size_t count = std::min(arguments.size(), declaration.typeParams.size());
    for (std::size_t index = 0; index < count; ++index) {
        substitutions.emplace(declaration.typeParams[index].name, arguments[index]);
    }

    bool needsPlan = false;
    const std::string base = BaseTypeName(type.name);
    for (const EnumDecl::Variant &variant : declaration.variants) {
        plan.variantDiscriminants.push_back(LookupEnumVariantDiscriminant(base, variant.name).value_or("0"));
        std::vector<TypeRef> payloadTypes;
        std::vector<HirMovePlan> payloadPlans;
        payloadTypes.reserve(variant.fields.size() + variant.namedFields.size());
        payloadPlans.reserve(variant.fields.size() + variant.namedFields.size());
        for (const TypeExprPtr &field : variant.fields) {
            TypeRef fieldType = ResolveTypeWithSubstitution(*field, substitutions);
            payloadTypes.push_back(fieldType);
            payloadPlans.push_back(BuildMovePlan(fieldType));
            needsPlan = needsPlan || payloadPlans.back().kind != HirMovePlan::Kind::Trivial;
        }
        for (const EnumDecl::Variant::NamedField &field : variant.namedFields) {
            TypeRef fieldType = ResolveTypeWithSubstitution(*field.type, substitutions);
            payloadTypes.push_back(fieldType);
            payloadPlans.push_back(BuildMovePlan(fieldType));
            needsPlan = needsPlan || payloadPlans.back().kind != HirMovePlan::Kind::Trivial;
        }
        plan.variantPayloadTypes.push_back(std::move(payloadTypes));
        plan.variantComponents.push_back(std::move(payloadPlans));
    }
    if (needsPlan) {
        plan.kind = HirMovePlan::Kind::Variant;
    }
    else {
        plan.variantDiscriminants.clear();
        plan.variantPayloadTypes.clear();
        plan.variantComponents.clear();
    }
    return plan;
}
} // namespace Rux::AstToHirDetail
