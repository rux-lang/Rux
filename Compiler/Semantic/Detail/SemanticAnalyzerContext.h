#pragma once

#include "Semantic/SemanticAnalyzer.h"
#include "Semantic/SemanticProgramIndex.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace Rux::SemanticDetail {
// Private state shared by semantic-analysis implementation files. Public
// callers interact only with SemanticAnalyzer and SemanticModel.
class SemanticAnalyzerContext {
public:
    SemanticAnalyzerContext(std::vector<const Module *> &inputModules, std::vector<DepPackage> &inputDependencies,
                            const std::string &inputPackageName, std::vector<SemanticDiagnostic> &inputDiagnostics,
                            std::vector<SemanticSymbol> &inputSymbols, const CompileTimeContext &inputContext,
                            std::unordered_map<const Expr *, TypeRef> &inputExpressionTypes,
                            std::unordered_map<const TypeExpr *, TypeRef> &inputTypeNodeTypes,
                            std::unordered_map<const Pattern *, TypeRef> &inputPatternTypes,
                            std::unordered_map<const CallExpr *, ResolvedCallableBinding> &inputCallableBindings,
                            std::unordered_map<const Decl *, ResolvedSymbolIdentity> &inputSymbolIdentities,
                            std::unordered_map<const ImplDecl *, ResolvedVtableIdentity> &inputVtableIdentities,
                            std::unordered_map<std::string, ResolvedTypeLayout> &inputTypeLayouts,
                            std::unordered_map<const SizeOfExpr *, std::uint64_t> &inputSizeOfValues);
    virtual ~SemanticAnalyzerContext();

    SemanticAnalyzerContext(const SemanticAnalyzerContext &) = delete;
    SemanticAnalyzerContext &operator=(const SemanticAnalyzerContext &) = delete;

    void Run();

protected:
    void EmitError(SourceLocation location, std::string message) const;
    void EmitWarning(SourceLocation location, std::string message) const;

    void PushScope();
    void PopScope();
    bool Define(Symbol symbol) const;

    void CheckBlock(const Block &block);
    void CheckPattern(const Pattern &pattern, const TypeRef &subjectType = TypeRef::MakeUnknown());
    [[nodiscard]] const EnumDecl::Variant *LookupEnumVariant(const std::string &enumName,
                                                             const std::string &variantName) const;

    std::vector<const Module *> &modules;
    std::vector<DepPackage> &deps;
    const std::string &packageName;
    std::vector<SemanticDiagnostic> &diags;
    const CompileTimeContext &context;
    std::unordered_map<const Expr *, TypeRef> &expressionTypes;
    std::unordered_map<const TypeExpr *, TypeRef> &typeNodeTypes;
    std::unordered_map<const Pattern *, TypeRef> &patternTypes;
    std::unordered_map<const CallExpr *, ResolvedCallableBinding> &callableBindings;
    std::unordered_map<const Decl *, ResolvedSymbolIdentity> &symbolIdentities;
    std::unordered_map<const ImplDecl *, ResolvedVtableIdentity> &vtableIdentities;
    std::unordered_map<std::string, ResolvedTypeLayout> &typeLayouts;
    std::unordered_map<const SizeOfExpr *, std::uint64_t> &sizeOfValues;

    SemanticProgramIndex programIndex;
    Scope &globalScope;
    const SemanticProgramIndex::PackageScopes &packageModuleScopes;
    std::string currentFile;
    TypeRef currentReturnType = TypeRef::MakeOpaque();
    bool currentFunctionNoReturn = false;
    int loopDepth = 0;
    std::unordered_set<std::string> activeLabels;
    bool inImpl = false;
    TypeRef currentSelfType = TypeRef::MakeUnknown();
    std::vector<std::string> currentTypeParams;
    const std::unordered_map<std::string, const StructDecl *> &structDecls;
    const std::unordered_map<std::string, const EnumDecl *> &enumDecls;
    const std::unordered_map<std::string, const UnionDecl *> &unionDecls;
    const std::unordered_map<std::string, const InterfaceDecl *> &interfaceDecls;
    const std::unordered_map<std::string, std::unordered_map<std::string, std::vector<const FuncDecl *>>>
        &methodsByType;
    const std::unordered_map<std::string, std::vector<const FuncDecl *>> &functionsByName;
    const std::unordered_map<const FuncDecl *, std::string> &functionModulePaths;
    const std::unordered_map<const FuncDecl *, const ImplDecl *> &methodImpls;
    const std::vector<const ImplDecl *> &implDecls;
    const std::vector<const ExternFuncDecl *> &externFuncDecls;
    const std::unordered_map<std::string, std::unordered_set<std::string>> &typeImplementsInterfaces;
    const FuncDecl *currentFunctionDecl = nullptr;
    const std::unordered_map<const FuncDecl *, Scope *> &functionDeclScopes;
    const std::unordered_map<const FuncDecl *, std::string> &functionDeclFiles;
    Scope *currentScope;

    [[nodiscard]] static bool IsUnimplementedPrimitiveType(std::string_view name);

private:
    void RegisterBuiltins();
    void IndexDeclarations();
    void CollectModule(const Module &module);
    void CheckStatement(const Stmt &statement);
    void CheckLetPattern(const Pattern &pattern, const TypeRef &type, bool isMutable);

    virtual TypeRef ResolveType(const TypeExpr &expression) = 0;
    virtual TypeRef ResolveTypeWithSubstitution(const TypeExpr &expression,
                                                const std::unordered_map<std::string, TypeRef> &substitutions) = 0;
    virtual TypeRef CheckExpr(const Expr &expression) = 0;
    virtual void CheckDecl(const Decl &declaration) = 0;
    virtual void ValidateArrayType(const TypeExpr &type, bool allowFlexibleTail) = 0;
    [[nodiscard]] virtual bool CanAssignExprTo(const Expr &expression, const TypeRef &expressionType,
                                               const TypeRef &targetType) = 0;
    [[nodiscard]] virtual std::string AssignmentErrorMessage(const Expr &expression, const TypeRef &targetType,
                                                             std::string fallback) = 0;
    [[nodiscard]] virtual std::optional<TypeRef> IndexElementType(const TypeRef &type) = 0;
    [[nodiscard]] virtual std::string BaseTypeName(const std::string &name) const = 0;
    [[nodiscard]] virtual std::vector<TypeRef> ParseTypeArgsFromTypeName(const std::string &typeName) const = 0;
    virtual void ApplyModuleImports(const Module &module) = 0;
    virtual void ApplyModuleImportsInScope(const Module &module, Scope &scope) = 0;
    virtual void ResolveModuleSignatures(const Module &module) = 0;
    virtual void ResolveModuleSignaturesInScope(const Module &module, Scope &scope) = 0;
    virtual void CheckModule(const Module &module) = 0;
    virtual void CheckModuleInScope(const Module &module, Scope &scope) = 0;
    virtual void ValidatePendingGenericInstantiations() = 0;
    virtual void RecordResolvedTypeLayouts() = 0;
    virtual void BuildFinalSymbolIdentities() = 0;
};
} // namespace Rux::SemanticDetail
