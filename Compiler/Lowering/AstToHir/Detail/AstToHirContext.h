#pragma once

#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/AstToHir/Detail/CleanupPlanner.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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
    std::uint64_t bindingId = 0;
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

/// Private state shared only by AST-to-HIR implementation files. Public callers interact with AstToHirLowering and
/// never depend on this orchestration layer.
class AstToHirContext {
public:
    AstToHirContext(const SemanticModel &inputModel, const std::vector<const Module *> &inputModules,
                    const CompileTimeContext &inputCompileTimeContext, std::vector<Diagnostic> &outputDiagnostics);
    ~AstToHirContext();

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
    /// Distinguishes the bindings two propagations in one expression introduce; only their uniqueness matters.
    std::size_t propagationOrdinal = 0;
    /// The same for the iterator and item bindings nested convention-driven loops introduce.
    std::size_t iterationOrdinal = 0;
    /// The same for the operands and result a checked operation names.
    std::size_t checkedArithmeticOrdinal = 0;
    bool inImpl = false;
    TypeRef currentSelfType = TypeRef::MakeUnknown();
    std::vector<std::string> currentTypeParams;
    std::unordered_map<std::string, TypeRef> currentSubstitutions;
    std::vector<HirFunc> monomorphizedFuncs;
    std::unordered_set<std::string> generatedMonomorphizedFuncNames;
    /// Generic struct instantiations seen while resolving types, in the order their layouts must be computed: an
    /// argument's own instantiation is recorded before the instantiation that uses it. Recording happens inside const
    /// resolution helpers, so the queue is mutable and drained once the module has been lowered.
    mutable std::vector<std::string> pendingStructInstantiations;
    mutable std::unordered_set<std::string> seenStructInstantiations;
    std::unordered_map<std::string, const StructDecl *> structDecls;
    std::unordered_map<std::string, const EnumDecl *> enumDecls;
    std::unordered_map<std::string, const UnionDecl *> unionDecls;
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<const FuncDecl *>>> methodsByType;
    std::unordered_map<std::string, const InterfaceDecl *> interfaceDecls;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> typeInterfaceVtables;
    std::unordered_map<const FuncDecl *, const ImplDecl *> methodImpl;
    std::vector<std::unordered_map<std::string, std::uint64_t>> constIntegerScopes{{}};
    CleanupPlanner cleanupPlanner;
    std::string declModulePath;
    std::vector<Diagnostic> &diagnostics;

    void PushScope();
    void PopScope();
    void Define(HirSymbol symbol) const;
    [[nodiscard]] std::uint64_t RegisterCleanupBinding(const std::string &name, const TypeRef &type,
                                                       SourceLocation origin);
    [[nodiscard]] std::vector<HirDropAction> CurrentScopeCleanups() const;
    [[nodiscard]] std::vector<HirDropAction> FunctionCleanups() const;
    void AppendCurrentScopeCleanups(HirBlock &block) const;
    [[nodiscard]] std::uint64_t BindingId(const HirExpr &expression) const;
    [[nodiscard]] std::uint64_t ConsumedBindingId(const HirExpr &expression) const;
    [[nodiscard]] std::optional<HirDropAction> OverwriteCleanup(const HirExpr &target, SourceLocation origin) const;
    [[nodiscard]] std::optional<HirPartialDropAction> PartialCleanup(HirPartialDropAction::Kind kind,
                                                                     const TypeRef &type, std::size_t ordinal,
                                                                     std::string name, SourceLocation origin) const;
    static void AppendFailureCleanup(std::vector<HirFailureCleanup> &edges,
                                     const std::vector<HirPartialDropAction> &completed);
    void CollectDecl(const Decl &decl);
    [[nodiscard]] TypeRef MakeFuncType(const std::vector<Param> &params, const std::optional<TypeExprPtr> &returnType,
                                       const std::vector<std::string> &typeParams = {});

    [[nodiscard]] HirBlock LowerBlock(const Block &block);
    [[nodiscard]] HirStmtPtr LowerStmt(const Stmt &stmt);
    [[nodiscard]] HirPatternPtr LowerLetPattern(const Pattern &pattern, const TypeRef &type, bool isMutable);
    [[nodiscard]] HirPatternPtr LowerPattern(const Pattern &pattern,
                                             const TypeRef &subjectType = TypeRef::MakeUnknown());
    [[nodiscard]] HirExprPtr LowerBasicExpr(const Expr &expression);
    [[nodiscard]] HirExprPtr LowerTryExpr(const TryExpr &expression);
    [[nodiscard]] HirExprPtr LowerCheckedArithmeticCall(const std::string &intrinsicName, const CallExpr &call);
    [[nodiscard]] HirStmtPtr LowerIteratorFor(const ForStmt &statement, const ResolvedIteration &fact);
    [[nodiscard]] HirExprPtr LowerConventionCall(const FuncDecl &method, HirExprPtr receiver, SourceLocation location);
    [[nodiscard]] HirExprPtr LowerAggregateExpr(const Expr &expression);
    [[nodiscard]] HirExprPtr LowerCallExpr(const CallExpr &expression);
    [[nodiscard]] HirExprPtr LowerExprAs(const Expr &expression, const TypeRef &targetType);
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
    [[nodiscard]] HirExprPtr LowerBoundConstrainedCall(const CallExpr &call, const ResolvedCallableBinding &binding);
    [[nodiscard]] HirExprPtr LowerBoundIndirectCall(const CallExpr &call, const ResolvedCallableBinding &binding);
    [[nodiscard]] HirExprPtr LowerBoundEnumCall(const CallExpr &call, const ResolvedCallableBinding &binding);
    [[nodiscard]] std::uint64_t ResolvedTypeQuery(const TypeQueryExpr &expression);
    [[nodiscard]] std::string GenericStructInitName(const StructInitExpr &expression);
    [[nodiscard]] std::pair<const EnumDecl *, const EnumDecl::Variant *>
    LookupEnumVariantInitializer(const std::string &typeName) const;
    [[nodiscard]] HirExprPtr LowerCompilerParamField(const std::string &root, const std::string &field,
                                                     SourceLocation location);
    [[nodiscard]] HirExprPtr LowerCompilerParamObject(const std::string &root, const TypeRef &type,
                                                      SourceLocation location);
    [[nodiscard]] HirExprPtr LowerCompilerParamCall(const std::string &root, const std::string &member,
                                                    const CallExpr &call) const;
    [[nodiscard]] HirExprPtr LowerCompilerParamIdentifier(const IdentExpr &expression);
    [[nodiscard]] HirExprPtr LowerCompilerParamFieldExpression(const FieldExpr &expression);
    [[nodiscard]] HirExprPtr LowerIntrinsicExpr(const IntrinsicExpr &expression) const;
    [[nodiscard]] TypeRef StructInitFieldType(const StructInitExpr &expression, const std::string &fieldName);
    [[nodiscard]] std::optional<TypeRef> InterfaceImplementationType(const TypeRef &expressionType,
                                                                     const TypeRef &targetType) const;

    [[nodiscard]] TypeRef ResolveType(const TypeExpr &expression);
    [[nodiscard]] TypeRef ResolveTypeWithSubstitution(const TypeExpr &expression,
                                                      const std::unordered_map<std::string, TypeRef> &substitutions);
    [[nodiscard]] TypeRef MakeFuncTypeWithSubstitution(const std::vector<Param> &params,
                                                       const std::optional<TypeExprPtr> &returnType,
                                                       const std::unordered_map<std::string, TypeRef> &substitutions,
                                                       const std::vector<std::string> &typeParams = {});
    [[nodiscard]] TypeRef EnumType(const EnumDecl &decl, const std::vector<TypeRef> &typeArguments = {});
    [[nodiscard]] TypeRef EnumVariantConstructorType(const EnumDecl &decl, const EnumDecl::Variant &variant,
                                                     const std::vector<TypeRef> &typeArguments = {});
    [[nodiscard]] std::string BaseTypeName(const std::string &name) const;
    [[nodiscard]] std::vector<std::string> ImplTypeParams(const ImplDecl &decl) const;
    [[nodiscard]] bool ReceiverIsByValue(const FuncDecl &method) const;
    [[nodiscard]] HirExprPtr LowerReceiverFor(const FuncDecl &method, HirExprPtr receiver);
    [[nodiscard]] TypeRef MethodType(const TypeRef &receiverType, const FuncDecl &method);
    [[nodiscard]] TypeRef AssociatedFunctionType(const TypeRef &receiverType, const FuncDecl &method);
    [[nodiscard]] const FuncDecl *LookupMethod(const TypeRef &receiverType, const std::string &methodName,
                                               const std::vector<TypeRef> &argumentTypes = {});
    [[nodiscard]] std::string CalleeName(const std::string &typeName, const std::string &methodName,
                                         const TypeRef &receiverType, const FuncDecl &declaration);
    [[nodiscard]] bool MethodIsFromConcreteImpl(const FuncDecl &method) const;
    [[nodiscard]] const std::string &FunctionCalleeName(const FuncDecl &decl) const;
    [[nodiscard]] HirFunc LowerFunc(const FuncDecl &decl, bool isMethod = false,
                                    const std::unordered_map<std::string, TypeRef> &substitutions = {},
                                    const std::string &overrideName = "");
    [[nodiscard]] HirStruct LowerStruct(const StructDecl &decl);
    [[nodiscard]] HirStruct LowerStructInstantiation(const StructDecl &decl, const std::string &name,
                                                     const std::vector<TypeRef> &typeArgs);
    /// Record `type` as a generic struct instantiation needing its own declaration, along with any instantiation nested
    /// in its type arguments. A generic struct declares one set of fields for every instantiation, so `Box<int32>` and
    /// `Box<Two>` would otherwise share the layout computed from the unsubstituted `T` and be sized alike.
    void NoteStructInstantiation(const TypeRef &type) const;
    [[nodiscard]] HirEnum LowerEnum(const EnumDecl &decl);
    [[nodiscard]] HirUnion LowerUnion(const UnionDecl &decl);
    [[nodiscard]] HirInterface LowerInterface(const InterfaceDecl &decl);
    [[nodiscard]] HirImplBlock LowerImpl(const ImplDecl &decl);
    [[nodiscard]] HirConst LowerConst(const ConstDecl &decl);
    [[nodiscard]] HirExternFunc LowerExternFunc(const ExternFuncDecl &decl);
    [[nodiscard]] HirExternVar LowerExternVar(const ExternVarDecl &decl);
    [[nodiscard]] HirTypeAlias LowerTypeAlias(const TypeAliasDecl &decl);
    [[nodiscard]] HirExprPtr LowerExpr(const Expr &expression);
    void ReportUnsupportedExpression(const Expr &expression);
    [[nodiscard]] bool UnsuffixedIntegerLiteralFits(const Expr &expression, const TypeRef &targetType) const;
    [[nodiscard]] std::optional<TypeRef> SliceElementType(const TypeRef &type) const;
    [[nodiscard]] std::string LowerLiteralValue(const LiteralExpr &expression) const;
    [[nodiscard]] HirExprPtr TryLowerOverloadedBinary(const BinaryExpr &expression, HirExprPtr &left,
                                                      HirExprPtr &right);
    [[nodiscard]] HirExprPtr LowerOverloadedBinaryCall(const BinaryExpr &expression, HirExprPtr &left,
                                                       HirExprPtr &right, const FuncDecl &resolved);
    [[nodiscard]] HirExprPtr LowerDerivedOrderingCompare(const BinaryExpr &expression, HirExprPtr &left,
                                                         HirExprPtr &right, const FuncDecl &lessThan,
                                                         const FuncDecl &equals);
    [[nodiscard]] std::optional<TypeRef> IndexElementType(const TypeRef &type) const;
    [[nodiscard]] TypeRef LiteralType(const Token &token) const;
    [[nodiscard]] std::string StripNumericLiteralSuffix(const std::string &text) const;
    [[nodiscard]] const EnumDecl::Variant *LookupEnumVariant(const std::string &enumName,
                                                             const std::string &variantName) const;
    [[nodiscard]] std::optional<std::string> LookupEnumVariantDiscriminant(const std::string &enumName,
                                                                           const std::string &variantName) const;
    [[nodiscard]] std::vector<TypeRef> ParseTypeArgsFromTypeName(const std::string &typeName) const;

    [[nodiscard]] const TypeRef &ResolvedType(const TypeExpr &type) const;
    [[nodiscard]] const ResolvedTypeLayout &ResolvedLayout(const TypeRef &type) const;
    [[nodiscard]] std::string GenericTypeName(const NamedTypeExpr &type);
    [[nodiscard]] static std::string SliceTypeName(const TypeRef &elementType);
    [[nodiscard]] static std::string BaseTypeNameImpl(const std::string &name);
    [[nodiscard]] static TypeRef ParseTypeRefFromString(std::string text);
    [[nodiscard]] static TypeRef StringLiteralElementType(const Token &token);
    [[nodiscard]] static TypeRef StringLiteralType(const Token &token);
    [[nodiscard]] static TypeRef CharLiteralType(const Token &token);
    [[nodiscard]] static std::string NumericLiteralSuffix(std::string_view text);
    [[nodiscard]] static std::string StripNumericLiteralSuffixImpl(const std::string &text);
    [[nodiscard]] static std::optional<std::uint64_t> ParseUnsuffixedIntegerLiteral(const Token &token);
    [[nodiscard]] static std::optional<std::uint64_t> ParseUnsignedIntegerText(const std::string &text);
    [[nodiscard]] std::optional<std::uint64_t> LookupConstInteger(const std::string &name) const;
    void RegisterConstInteger(const std::string &name, const HirExpr &value);
    [[nodiscard]] static std::optional<std::int64_t> ParseEnumDiscriminant(const std::string &text);
    [[nodiscard]] static std::optional<std::uint64_t> UnsignedIntegerMax(const TypeRef &type);
    [[nodiscard]] static std::optional<std::pair<std::int64_t, std::int64_t>> SignedIntegerRange(const TypeRef &type);
    [[nodiscard]] static bool IsNullLiteral(const Expr &expression);
    [[nodiscard]] static std::string NamedBaseTypeName(const TypeRef &type);
    [[nodiscard]] std::unordered_map<std::string, TypeRef>
    StructTypeSubstitutions(const StructDecl &decl, const std::vector<TypeExprPtr> &typeArguments);
    [[nodiscard]] static TypeRef SuffixedLiteralType(const Token &token);
    [[nodiscard]] static std::optional<TypeRef> BuiltinTypeFromName(const std::string &name);
    [[nodiscard]] TypeRef StructFieldType(const TypeRef &objectType, const std::string &fieldName);
    [[nodiscard]] std::unordered_map<std::string, TypeRef> MethodTypeSubstitutions(const TypeRef &receiverType) const;
    [[nodiscard]] bool MethodIsOverloaded(const std::string &typeName, const std::string &methodName) const;
    [[nodiscard]] static std::string MangleTypeName(const TypeRef &type);
    [[nodiscard]] std::string ConcreteMethodCalleeName(const std::string &typeName, const TypeRef &receiverType,
                                                       const FuncDecl &method);
    [[nodiscard]] TypeRef EnumBaseType(const EnumDecl &decl);

    [[nodiscard]] static std::uint32_t DecodeUtf8CodePoint(const std::string &text, std::size_t index);
    static void AppendUtf8(std::string &output, std::uint32_t codePoint);
    [[nodiscard]] static std::size_t ParseUnicodeEscape(const std::string &text, std::size_t position,
                                                        std::uint32_t &codePoint);
    [[nodiscard]] static std::string DecodeCharLiteral(const std::string &text);
    [[nodiscard]] static std::string DecodeStringLiteral(const std::string &text);
    [[nodiscard]] std::vector<HirParam> LowerParams(const std::vector<Param> &params, bool skipReceiver = false);
};
} // namespace Rux::AstToHirDetail
