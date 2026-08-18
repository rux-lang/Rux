// Shared analyzer state: module collection, declaration indexing, and the
// diagnostic helpers every checking file reports through.

#include "Semantic/Detail/SemanticAnalyzerContext.h"

#include <algorithm>
#include <array>
#include <format>
#include <utility>

namespace Rux::SemanticDetail {
namespace {
constexpr std::array<std::string_view, 20> UnimplementedPrimitiveTypes{
    "int128",   "int256",   "int512", "uint128", "uint256", "uint512", "float8", "float16", "float80", "float128",
    "float256", "float512", "bool64", "bool128", "bool256", "bool512", "char64", "char128", "char256", "char512",
};
} // namespace

std::string SemanticAnalyzerContext::SliceTypeName(const TypeRef &elementType) {
    return "Slice<" + elementType.ToString() + ">";
}

SemanticAnalyzerContext::SemanticAnalyzerContext(
    std::vector<const Module *> &inputModules, std::vector<DepPackage> &inputDependencies,
    const std::string &inputPackageName, std::vector<SemanticDiagnostic> &inputDiagnostics,
    std::vector<SemanticSymbol> &inputSymbols, const CompileTimeContext &inputContext,
    std::unordered_map<const Expr *, TypeRef> &inputExpressionTypes,
    std::unordered_map<const TypeExpr *, TypeRef> &inputTypeNodeTypes,
    std::unordered_map<const Pattern *, TypeRef> &inputPatternTypes,
    std::unordered_map<const CallExpr *, ResolvedCallableBinding> &inputCallableBindings,
    std::unordered_map<const Decl *, ResolvedSymbolIdentity> &inputSymbolIdentities,
    std::unordered_map<const ImplDecl *, ResolvedVtableIdentity> &inputVtableIdentities,
    std::unordered_map<std::string, ResolvedTypeLayout> &inputTypeLayouts,
    std::unordered_map<const SizeOfExpr *, std::uint64_t> &inputSizeOfValues)
    : modules(inputModules)
    , deps(inputDependencies)
    , packageName(inputPackageName)
    , diags(inputDiagnostics)
    , context(inputContext)
    , expressionTypes(inputExpressionTypes)
    , typeNodeTypes(inputTypeNodeTypes)
    , patternTypes(inputPatternTypes)
    , callableBindings(inputCallableBindings)
    , symbolIdentities(inputSymbolIdentities)
    , vtableIdentities(inputVtableIdentities)
    , typeLayouts(inputTypeLayouts)
    , sizeOfValues(inputSizeOfValues)
    , programIndex(diags, inputSymbols)
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
    , currentScope(&globalScope) {
}

SemanticAnalyzerContext::~SemanticAnalyzerContext() = default;

std::unordered_map<std::string, TypeProperties> SemanticAnalyzerContext::TakeTypeProperties() {
    return std::move(typeProperties);
}

bool SemanticAnalyzerContext::IsUnimplementedPrimitiveType(const std::string_view name) {
    return std::ranges::find(UnimplementedPrimitiveTypes, name) != UnimplementedPrimitiveTypes.end();
}

void SemanticAnalyzerContext::EmitGenericArityError(const TypeExpr &expression, std::string subject,
                                                    const std::size_t expectedCount, const std::size_t actualCount) {
    if (!reportedGenericArity.insert(&expression).second) {
        return;
    }
    EmitError(expression.location, std::format("{} requires {} type argument{}, but {} provided", std::move(subject),
                                               expectedCount, expectedCount == 1 ? "" : "s",
                                               actualCount == 1 ? "1 was" : std::format("{} were", actualCount)));
}

std::optional<TypeRef> SemanticAnalyzerContext::ResolveStructTypeReference(const TypeExpr &expression,
                                                                           const std::string &name,
                                                                           const std::vector<TypeRef> &typeArguments) {
    const auto declaration = structDecls.find(name);
    if (declaration == structDecls.end()) {
        return std::nullopt;
    }
    if (typeArguments.size() != declaration->second->typeParams.size()) {
        EmitGenericArityError(expression, std::format("struct type '{}'", name), declaration->second->typeParams.size(),
                              typeArguments.size());
        return TypeRef::MakeUnknown();
    }
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

void SemanticAnalyzerContext::RegisterBuiltins() {
    auto add = [&](const char *name, TypeRef type) {
        Symbol symbol;
        symbol.kind = Symbol::Kind::Type;
        symbol.name = name;
        symbol.type = std::move(type);
        globalScope.Define(std::move(symbol), diags, "<builtin>");
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
    for (const std::string_view name : UnimplementedPrimitiveTypes) {
        add(name.data(), TypeRef::MakeUnknown());
    }
}

void SemanticAnalyzerContext::CollectModule(const Module &module) {
    currentFile = module.name;
    const std::string *selfPackageName = packageName.empty() ? nullptr : &packageName;
    programIndex.CollectModule(module, selfPackageName, [this](const TypeExpr &type) { return ResolveType(type); });
}

void SemanticAnalyzerContext::IndexDeclarations() {
    RegisterBuiltins();
    for (auto &package : deps) {
        Scope &rootScope = programIndex.CreatePackageRoot(package.name);
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
    for (const Module *module : modules) {
        CollectModule(*module);
    }
}

void SemanticAnalyzerContext::Run() {
    IndexDeclarations();

    for (auto &package : deps) {
        for (auto &entry : package.modules) {
            ApplyModuleImportsInScope(*entry.module, *packageModuleScopes.at(package.name).at(""));
        }
    }
    for (auto &package : deps) {
        for (auto &entry : package.modules) {
            ResolveModuleSignaturesInScope(*entry.module, *packageModuleScopes.at(package.name).at(""));
        }
    }
    for (auto &package : deps) {
        for (auto &entry : package.modules) {
            CheckModuleInScope(*entry.module, *packageModuleScopes.at(package.name).at(""));
        }
    }
    for (const Module *module : modules) {
        ApplyModuleImports(*module);
    }
    for (const Module *module : modules) {
        ResolveModuleSignatures(*module);
    }
    for (const Module *module : modules) {
        CheckModule(*module);
    }
    ValidatePendingGenericInstantiations();
    RecordResolvedTypeProperties();
    RecordResolvedTypeLayouts();
    BuildFinalSymbolIdentities();
}
} // namespace Rux::SemanticDetail
