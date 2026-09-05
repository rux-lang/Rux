// Shared analyzer state: module collection, declaration indexing, and the
// diagnostic helpers every checking file reports through.

#include "Semantic/Analysis/AnalysisContext.h"

#include "Types/PrimitiveCatalog.h"

#include <algorithm>
#include <array>
#include <format>
#include <utility>

namespace Rux::SemanticDetail {
namespace {
/// Whether lowering turns `expression` into a value and so reads a recorded type for it.
/// `AstToHirContext::ResolvedExpressionType` treats a missing type fact for one of these as a broken invariant rather
/// than a reportable case. `null` is the exception the invariant already allows: it carries no type of its own, and
/// lowering gives it the one its context asks for without ever making that query.
[[nodiscard]] bool LoweringRequiresType(const Expr &expression) {
    if (const auto *literal = dynamic_cast<const LiteralExpr *>(&expression)) {
        return literal->token.kind != TokenKind::NullKeyword;
    }
    return dynamic_cast<const IdentExpr *>(&expression) || dynamic_cast<const SelfExpr *>(&expression) ||
           dynamic_cast<const UnaryExpr *>(&expression) || dynamic_cast<const MoveExpr *>(&expression) ||
           dynamic_cast<const PostfixExpr *>(&expression) || dynamic_cast<const BinaryExpr *>(&expression) ||
           dynamic_cast<const AssignExpr *>(&expression) || dynamic_cast<const TernaryExpr *>(&expression) ||
           dynamic_cast<const IndexExpr *>(&expression) || dynamic_cast<const FieldExpr *>(&expression) ||
           dynamic_cast<const CastExpr *>(&expression) || dynamic_cast<const IsExpr *>(&expression);
}
} // namespace

/// An unknown type is dropped rather than recorded, so an expression left unknown by a run that reported nothing
/// reaches lowering with no type fact at all -- and lowering has no route to report that. An error anywhere already
/// stops the pipeline before lowering, so only a silently unknown type needs naming here.
void AnalysisContext::ReportUntypedExpression(const Expr &expression) const {
    if (!LoweringRequiresType(expression)) {
        return;
    }
    const bool reported = std::ranges::any_of(diags, [](const SemanticDiagnostic &diagnostic) {
        return diagnostic.severity == SemanticDiagnostic::Severity::Error;
    });
    if (!reported) {
        EmitError(expression.location, "cannot determine the type of this expression");
    }
}

std::string AnalysisContext::SliceTypeName(const TypeRef &elementType) {
    return "Slice<" + elementType.ToString() + ">";
}

bool AnalysisContext::TypeImplementsInterface(const TypeRef &expressionType, const TypeRef &targetType) const {
    if (targetType.kind != TypeRef::Kind::Named) {
        return false;
    }
    Symbol *symbol = currentScope->Lookup(targetType.name);
    if (!symbol || symbol->kind != Symbol::Kind::Interface) {
        return false;
    }
    if (symbol->interfaceMethods.empty()) {
        return true;
    }
    const auto implements = [&](const TypeRef &type) {
        const auto implementation = typeImplementsInterfaces.find(type.ToString());
        return implementation != typeImplementsInterfaces.end() && implementation->second.contains(targetType.name);
    };
    if (implements(expressionType)) {
        return true;
    }
    if (expressionType.kind == TypeRef::Kind::Int) {
        return implements(TypeRef::MakeInt64());
    }
    if (expressionType.kind == TypeRef::Kind::Int64) {
        return implements(TypeRef::MakeInt());
    }
    if (expressionType.kind == TypeRef::Kind::UInt) {
        return implements(TypeRef::MakeUInt64());
    }
    if (expressionType.kind == TypeRef::Kind::UInt64) {
        return implements(TypeRef::MakeUInt());
    }
    return false;
}

AnalysisContext::AnalysisContext(AnalysisInputs inputs, SemanticFacts &output)
    : modules(inputs.modules)
    , deps(inputs.dependencies)
    , packageName(inputs.packageName)
    , diags(inputs.diagnostics)
    , context(inputs.context)
    , expressionTypes(output.expressionTypes)
    , associatedConstants(output.associatedConstants)
    , typeNodeTypes(output.typeNodeTypes)
    , patternTypes(output.patternTypes)
    , casePatterns(output.casePatterns)
    , variantEqualities(output.variantEqualities)
    , variantEqualityPlans(output.variantEqualityPlans)
    , valueConsumptions(output.valueConsumptions)
    , valueCopies(output.valueCopies)
    , callableBindings(output.callableBindings)
    , defaultConstructors(output.defaultConstructors)
    , symbolIdentities(output.symbolIdentities)
    , vtableIdentities(output.vtableIdentities)
    , typeLayouts(output.typeLayouts)
    , typeProperties(output.typeProperties)
    , dropGluePlans(output.dropGluePlans)
    , constraintWitnesses(output.constraintWitnesses)
    , propagations(output.propagations)
    , coalescings(output.coalescings)
    , indexOperators(output.indexOperators)
    , indexAssignments(output.indexAssignments)
    , iterations(output.iterations)
    , typeQueryValues(output.typeQueryValues)
    , programIndex(diags, inputs.symbols)
    , globalScope(programIndex.GlobalScope())
    , packageModuleScopes(programIndex.Packages())
    , structDecls(programIndex.Structs())
    , enumDecls(programIndex.Enums())
    , unionDecls(programIndex.Unions())
    , interfaceDecls(programIndex.Interfaces())
    , methodsByType(programIndex.Methods())
    , functionsByName(programIndex.Functions())
    , functionModulePaths(programIndex.FunctionModulePaths())
    , methodImpls(programIndex.MethodImplementations())
    , implDecls(programIndex.Implementations())
    , externFuncDecls(programIndex.ExternFunctions())
    , typeImplementsInterfaces(programIndex.ImplementedInterfaces())
    , functionDeclScopes(programIndex.FunctionScopes())
    , functionDeclFiles(programIndex.FunctionSources())
    , declarationInfos(programIndex.DeclarationInfos())
    , currentScope(&globalScope) {
}

AnalysisContext::~AnalysisContext() = default;

std::unordered_map<const Decl *, bool> AnalysisContext::EffectiveVisibilities() const {
    std::unordered_map<const Decl *, bool> result;
    result.reserve(declarationInfos.size());
    for (const auto &[declaration, info] : declarationInfos) {
        result.emplace(declaration, info.isEffectivelyPublic);
    }
    return result;
}

bool AnalysisContext::MentionsTypeParameter(const TypeRef &type) const {
    if (type.kind == TypeRef::Kind::TypeParam) {
        return true;
    }
    if (type.kind == TypeRef::Kind::Named) {
        if (std::ranges::contains(currentTypeParams, BaseTypeName(type.name))) {
            return true;
        }
        for (const TypeRef &argument : ParseTypeArgsFromTypeName(type.name)) {
            if (MentionsTypeParameter(argument)) {
                return true;
            }
        }
    }
    return std::ranges::any_of(type.inner, [this](const TypeRef &inner) { return MentionsTypeParameter(inner); });
}

TypeRef AnalysisContext::SubstituteTypeParameters(TypeRef type,
                                                  const std::unordered_map<std::string, TypeRef> &substitutions) const {
    // An argument spelled inside an instantiation's name is substituted by rebuilding the name around what the
    // arguments became. Walking only `inner` left `Option<T>` exactly as it was, so every question deferred until
    // the parameter was known was answered against a type that still mentioned it.
    if (type.kind == TypeRef::Kind::Named && type.name.find('<') != std::string::npos) {
        std::vector<TypeRef> arguments = ParseTypeArgsFromTypeName(type.name);
        bool changed = false;
        for (TypeRef &argument : arguments) {
            TypeRef substituted = SubstituteTypeParameters(argument, substitutions);
            changed = changed || substituted.ToString() != argument.ToString();
            argument = std::move(substituted);
        }
        if (changed) {
            TypeRef rebuilt = TypeRef::MakeNamed(TypeRef::InstantiationName(BaseTypeName(type.name), arguments));
            rebuilt.isMut = type.isMut;
            rebuilt.isIntrinsicSlice = type.isIntrinsicSlice;
            return rebuilt;
        }
        return type;
    }
    if (type.kind == TypeRef::Kind::Named || type.kind == TypeRef::Kind::TypeParam) {
        if (const auto it = substitutions.find(type.name); it != substitutions.end()) {
            TypeRef substituted = it->second;
            // Pointer and reference types record writability on the substituted slot itself, so the substitution has
            // to carry that mark onto whatever the type parameter becomes.
            substituted.isMut = type.isMut;
            return substituted;
        }
        return type;
    }
    for (TypeRef &inner : type.inner) {
        inner = SubstituteTypeParameters(std::move(inner), substitutions);
    }
    return type;
}

const EnumDecl *AnalysisContext::EnumNamed(const std::string &name) const {
    return CaseTypeNamed(name).declaration;
}

AnalysisContext::CaseTypeDeclaration AnalysisContext::CaseTypeNamed(const std::string &name) const {
    if (const EnumDecl *local = programIndex.EnumIn(currentFile, name)) {
        return {local, local->form};
    }
    const auto enumeration = enumDecls.find(name);
    if (enumeration == enumDecls.end()) {
        return {};
    }
    return {enumeration->second, enumeration->second->form};
}

namespace {
/// Whether `expression` names `Self` anywhere inside it, following indirection, arrays and type arguments.
[[nodiscard]] bool MentionsSelf(const TypeExpr &expression) {
    if (const auto *named = dynamic_cast<const NamedTypeExpr *>(&expression)) {
        if (named->name == SelfTypeName) {
            return true;
        }
        return std::ranges::any_of(named->typeArgs,
                                   [](const TypeExprPtr &argument) { return MentionsSelf(*argument); });
    }
    if (const auto *pointer = dynamic_cast<const PointerTypeExpr *>(&expression)) {
        return MentionsSelf(*pointer->pointee);
    }
    if (const auto *reference = dynamic_cast<const ReferenceTypeExpr *>(&expression)) {
        return MentionsSelf(*reference->pointee);
    }
    if (const auto *array = dynamic_cast<const ArrayTypeExpr *>(&expression)) {
        return MentionsSelf(*array->element);
    }
    if (const auto *tuple = dynamic_cast<const TupleTypeExpr *>(&expression)) {
        return std::ranges::any_of(tuple->elements, [](const TypeExprPtr &element) { return MentionsSelf(*element); });
    }
    return false;
}
} // namespace

bool AnalysisContext::InterfaceMethodMentionsSelf(const FuncDecl &method) {
    if (method.returnType && MentionsSelf(**method.returnType)) {
        return true;
    }
    return std::ranges::any_of(method.params, [](const Param &parameter) {
        return !parameter.isVariadic && parameter.type && MentionsSelf(*parameter.type);
    });
}

bool AnalysisContext::IsUnimplementedPrimitiveType(const std::string_view name) {
    const PrimitiveInfo *info = FindPrimitive(name);
    return info && !info->implemented;
}

void AnalysisContext::EmitGenericArityError(const TypeExpr &expression, std::string subject,
                                            const std::size_t expectedCount, const std::size_t actualCount) {
    if (!reportedGenericArity.insert(&expression).second) {
        return;
    }
    EmitError(expression.location, std::format("{} requires {} type argument{}, but {} provided", std::move(subject),
                                               expectedCount, expectedCount == 1 ? "" : "s",
                                               actualCount == 1 ? "1 was" : std::format("{} were", actualCount)));
}

std::optional<TypeRef> AnalysisContext::ResolveStructTypeReference(const TypeExpr &expression, const std::string &name,
                                                                   const std::vector<TypeRef> &typeArguments) {
    const auto declaration = structDecls.find(name);
    if (declaration == structDecls.end()) {
        return std::nullopt;
    }
    if (!declaration->second->intrinsicName.empty()) {
        const Symbol *symbol = currentScope ? currentScope->Lookup(name) : nullptr;
        if (!symbol || !IsVisibleTypeSymbol(*symbol)) {
            EmitError(expression.location, std::format("intrinsic type '{}' is not imported into this scope", name));
            return TypeRef::MakeUnknown();
        }
        if (const auto primitive = PrimitiveTypeFromName(declaration->second->intrinsicName)) {
            return *primitive;
        }
        if (const auto aggregate = IntrinsicAggregateType(declaration->second->intrinsicName, typeArguments)) {
            return *aggregate;
        }
    }
    if (typeArguments.size() != declaration->second->typeParams.size()) {
        EmitGenericArityError(expression, std::format("struct type '{}'", name), declaration->second->typeParams.size(),
                              typeArguments.size());
        return TypeRef::MakeUnknown();
    }
    CheckTypeReferenceConstraints(expression, declaration->second->typeParams, typeArguments,
                                  std::format("struct '{}'", name));
    std::string instantiatedName = name;
    for (std::size_t index = 0; index < typeArguments.size(); ++index) {
        instantiatedName += index == 0 ? "<" : ", ";
        instantiatedName += typeArguments[index].ToString();
    }
    if (!typeArguments.empty()) {
        instantiatedName += ">";
    }
    return TypeRef::MakeNamed(std::move(instantiatedName));
}

void AnalysisContext::RegisterBuiltins() {
    auto add = [&](const std::string_view name, TypeRef type) {
        Symbol symbol;
        symbol.kind = Symbol::Kind::Type;
        symbol.name = name;
        symbol.type = std::move(type);
        symbol.isPublic = true;
        symbol.isEffectivelyPublic = true;
        symbol.ownerPackage = "<builtin>";
        globalScope.Define(std::move(symbol), diags, "<builtin>");
    };
    add("opaque", TypeRef::MakeOpaque());
    // A reserved primitive is still a declared name, so a use of it is diagnosed as unimplemented rather than as an
    // unknown type; it binds to Unknown because it has no representation to bind to yet.
    for (const PrimitiveInfo &primitive : PrimitiveCatalog()) {
        if (primitive.category == PrimitiveCategory::String) {
            continue;
        }
        add(primitive.name, primitive.implemented ? TypeRef::MakePrimitive(primitive.kind) : TypeRef::MakeUnknown());
    }
    for (const PrimitiveAlias &alias : PrimitiveAliases()) {
        if (TypeRef::MakePrimitive(alias.kind).IsString()) {
            continue;
        }
        add(alias.name, TypeRef::MakePrimitive(alias.kind));
    }
}

void AnalysisContext::CollectModule(const Module &module) {
    currentFile = module.name;
    currentPackage = packageName;
    const std::string *selfPackageName = packageName.empty() ? nullptr : &packageName;
    programIndex.CollectModule(module, selfPackageName, [this](const TypeExpr &type) { return ResolveType(type); });
}

void AnalysisContext::IndexDeclarations() {
    RegisterBuiltins();
    for (auto &package : deps) {
        currentPackage = package.name;
        Scope &rootScope = programIndex.CreatePackageRoot(package.name);
        currentScope = &rootScope;
        for (auto &entry : package.modules) {
            currentFile = entry.module->name;
            for (const auto &declaration : entry.module->items) {
                programIndex.CollectDeclaration(
                    *declaration, rootScope, currentFile, [this](const TypeExpr &type) { return ResolveType(type); },
                    &package.name);
            }
        }
    }

    if (!packageName.empty()) {
        programIndex.RegisterPackageRoot(packageName, globalScope);
    }
    currentScope = &globalScope;
    for (const Module *module : modules) {
        CollectModule(*module);
    }
    programIndex.FinalizeVisibility();
}

bool AnalysisContext::IsAccessible(const Symbol &symbol) const {
    return symbol.ownerPackage == currentPackage || symbol.isEffectivelyPublic;
}

bool AnalysisContext::IsAccessible(const Decl &declaration) const {
    const auto found = declarationInfos.find(&declaration);
    return found == declarationInfos.end() || found->second.ownerPackage == currentPackage ||
           found->second.isEffectivelyPublic;
}

bool AnalysisContext::IsMemberAccessible(const Decl &owner, const bool memberIsPublic) const {
    const auto found = declarationInfos.find(&owner);
    return found == declarationInfos.end() || found->second.ownerPackage == currentPackage ||
           (memberIsPublic && found->second.isEffectivelyPublic);
}

void AnalysisContext::EmitPrivacyError(const SourceLocation useLocation, const Symbol &symbol) const {
    EmitError(useLocation,
              std::format("{} '{}' is private to package '{}'", SymbolKindName(symbol.kind), symbol.name,
                          symbol.ownerPackage),
              {DeclarationNote(symbol)}, std::format("add 'pub' to the declaration of '{}'", symbol.name));
}

void AnalysisContext::EmitPrivacyError(const SourceLocation useLocation, const Decl &declaration,
                                       const std::string_view kind, const std::string_view name) const {
    const auto found = declarationInfos.find(&declaration);
    const std::string owner = found == declarationInfos.end() ? std::string{} : found->second.ownerPackage;
    const std::string source = found == declarationInfos.end() ? std::string{} : found->second.sourceName;
    std::vector<std::string> notes;
    if (!source.empty()) {
        notes.push_back(std::format("'{}' was declared at '{}':{}:{}", name, source, declaration.location.line,
                                    declaration.location.column));
    }
    EmitError(useLocation, std::format("{} '{}' is private to package '{}'", kind, name, owner), std::move(notes),
              std::format("add 'pub' to the declaration of '{}'", name));
}

void AnalysisContext::EmitPrivateMemberError(const SourceLocation useLocation, const Decl &owner,
                                             const std::string_view kind, const std::string_view name) const {
    const auto found = declarationInfos.find(&owner);
    const std::string package = found == declarationInfos.end() ? std::string{} : found->second.ownerPackage;
    EmitError(useLocation, std::format("{} '{}' is private to package '{}'", kind, name, package), {},
              std::format("add 'pub' before the declaration of '{}'", name));
}

bool AnalysisContext::DeduceTypeArgument(const TypeRef &paramType, const TypeRef &argType,
                                         const std::unordered_set<std::string> &typeParamNames,
                                         std::unordered_map<std::string, TypeRef> &substitutions) {
    if (paramType.IsUnknown() || argType.IsUnknown()) {
        return true;
    }
    if (paramType.kind == TypeRef::Kind::TypeParam ||
        (paramType.kind == TypeRef::Kind::Named && typeParamNames.contains(paramType.name))) {
        if (typeParamNames.contains(paramType.name)) {
            TypeRef target = argType;
            if (const auto it = substitutions.find(paramType.name); it != substitutions.end()) {
                const TypeRef &existing = it->second;
                if (existing == target) {
                    return true;
                }
                if (existing.kind == TypeRef::Kind::Int && target.IsInteger()) {
                    substitutions[paramType.name] = target;
                    return true;
                }
                if (target.kind == TypeRef::Kind::Int && existing.IsInteger()) {
                    return true;
                }
                if (existing.kind == TypeRef::Kind::Float && target.IsFloat()) {
                    substitutions[paramType.name] = target;
                    return true;
                }
                if (target.kind == TypeRef::Kind::Float && existing.IsFloat()) {
                    return true;
                }
                if (target.IsAssignableTo(existing)) {
                    return true;
                }
                if (existing.IsAssignableTo(target)) {
                    substitutions[paramType.name] = target;
                    return true;
                }
                return false;
            }
            substitutions.emplace(paramType.name, target);
            return true;
        }
        return true;
    }

    if (paramType.kind == TypeRef::Kind::Reference) {
        if (paramType.inner.empty()) {
            return false;
        }
        if (argType.kind == TypeRef::Kind::Reference && !argType.inner.empty()) {
            return DeduceTypeArgument(paramType.inner[0], argType.inner[0], typeParamNames, substitutions);
        }
        return DeduceTypeArgument(paramType.inner[0], argType, typeParamNames, substitutions);
    }

    if (paramType.kind == TypeRef::Kind::Pointer) {
        if (paramType.inner.empty()) {
            return false;
        }
        if (argType.kind == TypeRef::Kind::Pointer && !argType.inner.empty()) {
            return DeduceTypeArgument(paramType.inner[0], argType.inner[0], typeParamNames, substitutions);
        }
        return false;
    }

    if (paramType.kind == TypeRef::Kind::Array) {
        if (paramType.inner.empty()) {
            return false;
        }
        if (argType.kind == TypeRef::Kind::Array && !argType.inner.empty()) {
            return DeduceTypeArgument(paramType.inner[0], argType.inner[0], typeParamNames, substitutions);
        }
        return false;
    }

    if (paramType.kind == TypeRef::Kind::Tuple) {
        if (argType.kind != TypeRef::Kind::Tuple || paramType.inner.size() != argType.inner.size()) {
            return false;
        }
        for (std::size_t i = 0; i < paramType.inner.size(); ++i) {
            if (!DeduceTypeArgument(paramType.inner[i], argType.inner[i], typeParamNames, substitutions)) {
                return false;
            }
        }
        return true;
    }

    if (paramType.kind == TypeRef::Kind::Func) {
        if (argType.kind != TypeRef::Kind::Func || paramType.inner.size() != argType.inner.size()) {
            return false;
        }
        for (std::size_t i = 0; i < paramType.inner.size(); ++i) {
            if (!DeduceTypeArgument(paramType.inner[i], argType.inner[i], typeParamNames, substitutions)) {
                return false;
            }
        }
        return true;
    }

    if (paramType.kind == TypeRef::Kind::Named) {
        const std::string paramBase = BaseTypeName(paramType.name);
        if (argType.kind == TypeRef::Kind::Named) {
            const std::string argBase = BaseTypeName(argType.name);
            if (paramBase == argBase && paramType.isIntrinsicSlice == argType.isIntrinsicSlice) {
                const auto paramArgs = ParseTypeArgsFromTypeName(paramType.name);
                const auto argArgs = ParseTypeArgsFromTypeName(argType.name);
                if (paramArgs.size() == argArgs.size() && !paramArgs.empty()) {
                    for (std::size_t i = 0; i < paramArgs.size(); ++i) {
                        if (!DeduceTypeArgument(paramArgs[i], argArgs[i], typeParamNames, substitutions)) {
                            return false;
                        }
                    }
                    return true;
                }
            }
        }
        if (paramType.isIntrinsicSlice && argType.kind == TypeRef::Kind::Array && !argType.inner.empty()) {
            const auto paramArgs = ParseTypeArgsFromTypeName(paramType.name);
            if (paramArgs.size() == 1) {
                return DeduceTypeArgument(paramArgs[0], argType.inner[0], typeParamNames, substitutions);
            }
        }
    }

    return true;
}

bool AnalysisContext::DeduceTypeArguments(const FuncDecl &declaration, const std::vector<TypeRef> &argumentTypes,
                                          std::unordered_map<std::string, TypeRef> &substitutions) {
    if (declaration.typeParams.empty()) {
        return true;
    }
    std::unordered_set<std::string> typeParamNames;
    typeParamNames.reserve(declaration.typeParams.size());
    for (const auto &tp : declaration.typeParams) {
        typeParamNames.insert(tp.name);
    }

    const TypeRef templateFuncType = MakeFuncTypeWithSubstitution(declaration.params, declaration.returnType, {},
                                                                  TypeParameterNames(declaration.typeParams));
    if (templateFuncType.kind != TypeRef::Kind::Func || templateFuncType.inner.empty()) {
        return false;
    }
    const std::size_t paramCount = templateFuncType.inner.size() - 1;
    const std::size_t count = std::min(argumentTypes.size(), paramCount);
    for (std::size_t i = 0; i < count; ++i) {
        DeduceTypeArgument(templateFuncType.inner[i], argumentTypes[i], typeParamNames, substitutions);
    }
    return substitutions.size() == declaration.typeParams.size();
}

TypeRef AnalysisContext::EnumVariantConstructorType(const EnumDecl &declaration, const EnumDecl::Variant &variant,
                                                    const std::vector<TypeRef> &typeArguments) {
    const auto savedTypeParams = currentTypeParams;
    for (const auto &tp : declaration.typeParams) {
        currentTypeParams.push_back(tp.name);
    }
    std::unordered_map<std::string, TypeRef> substitutions;
    const std::size_t count = std::min(declaration.typeParams.size(), typeArguments.size());
    for (std::size_t i = 0; i < count; ++i) {
        substitutions.emplace(declaration.typeParams[i].name, typeArguments[i]);
    }
    std::vector<TypeRef> params;
    params.reserve(variant.fields.size() + variant.namedFields.size());
    for (const auto &field : variant.fields) {
        params.push_back(ResolveTypeWithSubstitution(*field, substitutions));
    }
    for (const auto &field : variant.namedFields) {
        params.push_back(ResolveTypeWithSubstitution(*field.type, substitutions));
    }
    currentTypeParams = savedTypeParams;
    return TypeRef::MakeFunc(std::move(params), EnumType(declaration, typeArguments));
}

void AnalysisContext::Run() {
    IndexDeclarations();

    for (auto &package : deps) {
        currentPackage = package.name;
        for (auto &entry : package.modules) {
            ApplyModuleImportsInScope(*entry.module, *packageModuleScopes.at(package.name).at(""));
        }
    }
    for (auto &package : deps) {
        currentPackage = package.name;
        for (auto &entry : package.modules) {
            ResolveModuleSignaturesInScope(*entry.module, *packageModuleScopes.at(package.name).at(""));
        }
    }
    for (auto &package : deps) {
        currentPackage = package.name;
        for (auto &entry : package.modules) {
            CheckModuleInScope(*entry.module, *packageModuleScopes.at(package.name).at(""));
        }
    }
    for (const Module *module : modules) {
        currentPackage = packageName;
        ApplyModuleImports(*module);
    }
    for (const Module *module : modules) {
        currentPackage = packageName;
        ResolveModuleSignatures(*module);
    }
    for (const Module *module : modules) {
        currentPackage = packageName;
        CheckModule(*module);
    }
    QueueDropMethodInstantiations();
    ValidatePendingGenericInstantiations();
    RecordResolvedTypeProperties();
    SynthesizeResolvedDropGlue();
    RecordResolvedTypeLayouts();
    BuildFinalSymbolIdentities();
}
} // namespace Rux::SemanticDetail
