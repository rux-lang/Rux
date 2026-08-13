#pragma once

#include "Lowering/AstToHir/AstToHir.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Rux::AstToHirDetail {
struct HirSymbol {
    enum class Kind {
        Var,
        Func,
        Type,
        Const,
        Interface,
    };

    Kind kind = Kind::Var;
    std::string name;
    TypeRef type;
    bool isMut = false;
    bool isNoReturn = false;
    std::string intrinsicName;
    std::vector<const FuncDecl *> funcOverloads;
};

class HirScope {
public:
    explicit HirScope(HirScope *parentScope = nullptr);

    void Define(HirSymbol symbol);
    [[nodiscard]] HirSymbol *Lookup(const std::string &name);
    [[nodiscard]] HirScope *Parent() const;

private:
    HirScope *parent;
    std::unordered_map<std::string, HirSymbol> table;
};

// Private state shared only by AST-to-HIR implementation files. Public callers
// interact with AstToHirLowering and never depend on this orchestration layer.
class AstToHirContext {
public:
    AstToHirContext(const SemanticModel &inputModel, const std::vector<const Module *> &inputModules,
                    const CompileTimeContext &inputCompileTimeContext);
    virtual ~AstToHirContext();

    AstToHirContext(const AstToHirContext &) = delete;
    AstToHirContext &operator=(const AstToHirContext &) = delete;

    [[nodiscard]] HirPackage Run();

protected:
    const SemanticModel &model;
    const std::vector<const Module *> &modules;
    const CompileTimeContext &context;
    HirScope globalScope;
    HirScope *currentScope;
    std::vector<std::unique_ptr<HirScope>> ownedScopes;
    std::string currentFile;
    std::string currentFunctionName;
    std::string currentModulePath;
    TypeRef currentReturnType = TypeRef::MakeOpaque();
    bool inImpl = false;
    TypeRef currentSelfType = TypeRef::MakeUnknown();
    std::vector<std::string> currentTypeParams;
    std::unordered_map<std::string, TypeRef> currentSubstitutions;
    std::vector<HirFunc> monomorphizedFuncs;
    std::unordered_set<std::string> generatedMonomorphizedFuncNames;
    std::unordered_map<std::string, const StructDecl *> structDecls;
    std::unordered_map<std::string, const EnumDecl *> enumDecls;
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<const FuncDecl *>>> methodsByType;
    std::unordered_map<std::string, const InterfaceDecl *> interfaceDecls;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> typeInterfaceVtables;
    std::unordered_map<const FuncDecl *, const ImplDecl *> methodImpl;
    std::vector<std::unordered_map<std::string, std::uint64_t>> constIntegerScopes{{}};
    std::string declModulePath;

    void PushScope();
    void PopScope();
    void Define(HirSymbol symbol) const;
    void CollectDecl(const Decl &decl);
    [[nodiscard]] TypeRef MakeFuncType(const std::vector<Param> &params, const std::optional<TypeExprPtr> &returnType,
                                       const std::vector<std::string> &typeParams = {});

    [[nodiscard]] HirBlock LowerBlock(const Block &block);
    [[nodiscard]] HirStmtPtr LowerStmt(const Stmt &stmt);
    [[nodiscard]] HirPatternPtr LowerLetPattern(const Pattern &pattern, const TypeRef &type, bool isMutable);
    [[nodiscard]] HirPatternPtr LowerPattern(const Pattern &pattern,
                                             const TypeRef &subjectType = TypeRef::MakeUnknown());
    [[nodiscard]] HirExprPtr LowerBasicExpr(const Expr &expression);
    [[nodiscard]] HirExprPtr LowerCallExpr(const CallExpr &expression);
    [[nodiscard]] TypeRef ResolvedExpressionType(const Expr &expression) const;
    [[nodiscard]] std::optional<std::string> CompilerParamRoot(const Expr &expression) const;
    [[nodiscard]] std::string LogicalCurrentFilePath() const;

private:
    void RegisterBuiltins();
    void CollectModule(const Module &module);
    [[nodiscard]] HirModule LowerModule(const Module &module);
    void LowerTopLevelDecl(const Decl &decl, HirModule &module);
    [[nodiscard]] bool IsDiagnosticIntrinsicCall(const Expr &expression) const;
    [[nodiscard]] ResolvedCallableBinding ResolvedCall(const CallExpr &call) const;
    [[nodiscard]] HirExprPtr LowerDefaultArgument(const Expr &expression, const TypeRef &targetType,
                                                  const SourceLocation &callSiteLocation);
    [[nodiscard]] std::vector<HirExprPtr> LowerBoundArguments(const CallExpr &call, const FuncDecl &declaration,
                                                              const ResolvedCallableBinding &binding,
                                                              const TypeRef &functionType, bool hasReceiver);
    void EnsureBoundFunctionInstance(const FuncDecl &declaration, const ResolvedCallableBinding &binding);
    void EnsureBoundMethodInstance(const FuncDecl &method, const ResolvedCallableBinding &binding);
    [[nodiscard]] HirExprPtr LowerBoundDirectCall(const CallExpr &call, const ResolvedCallableBinding &binding);
    [[nodiscard]] HirExprPtr LowerBoundMethodCall(const CallExpr &call, const ResolvedCallableBinding &binding);
    [[nodiscard]] HirExprPtr LowerBoundInterfaceCall(const CallExpr &call, const ResolvedCallableBinding &binding);
    [[nodiscard]] HirExprPtr LowerBoundIndirectCall(const CallExpr &call, const ResolvedCallableBinding &binding);
    [[nodiscard]] HirExprPtr LowerBoundEnumCall(const CallExpr &call, const ResolvedCallableBinding &binding);

    [[nodiscard]] virtual TypeRef ResolveType(const TypeExpr &expression) = 0;
    [[nodiscard]] virtual TypeRef
    ResolveTypeWithSubstitution(const TypeExpr &expression,
                                const std::unordered_map<std::string, TypeRef> &substitutions) = 0;
    [[nodiscard]] virtual TypeRef
    MakeFuncTypeWithSubstitution(const std::vector<Param> &params, const std::optional<TypeExprPtr> &returnType,
                                 const std::unordered_map<std::string, TypeRef> &substitutions,
                                 const std::vector<std::string> &typeParams = {}) = 0;
    [[nodiscard]] virtual TypeRef EnumType(const EnumDecl &decl, const std::vector<TypeRef> &typeArguments = {}) = 0;
    [[nodiscard]] virtual TypeRef EnumVariantConstructorType(const EnumDecl &decl, const EnumDecl::Variant &variant,
                                                             const std::vector<TypeRef> &typeArguments = {}) = 0;
    [[nodiscard]] virtual std::string BaseTypeName(const std::string &name) const = 0;
    [[nodiscard]] virtual std::vector<std::string> ImplTypeParams(const ImplDecl &decl) const = 0;
    [[nodiscard]] virtual TypeRef MethodType(const TypeRef &receiverType, const FuncDecl &method) = 0;
    [[nodiscard]] virtual TypeRef AssociatedFunctionType(const TypeRef &receiverType, const FuncDecl &method) = 0;
    [[nodiscard]] virtual bool MethodIsFromConcreteImpl(const FuncDecl &method) const = 0;
    [[nodiscard]] virtual const std::string &FunctionCalleeName(const FuncDecl &decl) const = 0;
    [[nodiscard]] virtual HirFunc LowerFunc(const FuncDecl &decl, bool isMethod = false,
                                            const std::unordered_map<std::string, TypeRef> &substitutions = {},
                                            const std::string &overrideName = "") = 0;
    [[nodiscard]] virtual HirStruct LowerStruct(const StructDecl &decl) = 0;
    [[nodiscard]] virtual HirEnum LowerEnum(const EnumDecl &decl) = 0;
    [[nodiscard]] virtual HirUnion LowerUnion(const UnionDecl &decl) = 0;
    [[nodiscard]] virtual HirInterface LowerInterface(const InterfaceDecl &decl) = 0;
    [[nodiscard]] virtual HirImplBlock LowerImpl(const ImplDecl &decl) = 0;
    [[nodiscard]] virtual HirConst LowerConst(const ConstDecl &decl) = 0;
    [[nodiscard]] virtual HirExternFunc LowerExternFunc(const ExternFuncDecl &decl) = 0;
    [[nodiscard]] virtual HirExternVar LowerExternVar(const ExternVarDecl &decl) = 0;
    [[nodiscard]] virtual HirTypeAlias LowerTypeAlias(const TypeAliasDecl &decl) = 0;
    [[nodiscard]] virtual HirExprPtr LowerExpr(const Expr &expression) = 0;
    [[nodiscard]] virtual HirExprPtr LowerExprAs(const Expr &expression, const TypeRef &targetType) = 0;
    [[nodiscard]] virtual HirExprPtr LowerCompilerParamCall(const std::string &root, const std::string &member,
                                                            const CallExpr &call) const = 0;
    [[nodiscard]] virtual std::string LowerLiteralValue(const LiteralExpr &expression) const = 0;
    [[nodiscard]] virtual HirExprPtr LowerCompilerParamIdentifier(const IdentExpr &expression) = 0;
    [[nodiscard]] virtual HirExprPtr LowerCompilerParamFieldExpression(const FieldExpr &expression) = 0;
    [[nodiscard]] virtual HirExprPtr TryLowerOverloadedBinary(const BinaryExpr &expression, HirExprPtr &left,
                                                              HirExprPtr &right) = 0;
    [[nodiscard]] virtual std::optional<TypeRef> IndexElementType(const TypeRef &type) const = 0;
    [[nodiscard]] virtual TypeRef LiteralType(const Token &token) const = 0;
    [[nodiscard]] virtual std::string StripNumericLiteralSuffix(const std::string &text) const = 0;
    [[nodiscard]] virtual const EnumDecl::Variant *LookupEnumVariant(const std::string &enumName,
                                                                     const std::string &variantName) const = 0;
    [[nodiscard]] virtual std::optional<std::string>
    LookupEnumVariantDiscriminant(const std::string &enumName, const std::string &variantName) const = 0;
    [[nodiscard]] virtual std::vector<TypeRef> ParseTypeArgsFromTypeName(const std::string &typeName) const = 0;
};
} // namespace Rux::AstToHirDetail
