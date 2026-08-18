#pragma once

#include "Semantic/Detail/MoveStateTracker.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Semantic/SemanticProgramIndex.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace Rux::SemanticDetail {
/// Private state shared by semantic-analysis implementation files. Public callers interact only with SemanticAnalyzer
/// and SemanticModel.
class SemanticAnalyzerContext {
public:
    SemanticAnalyzerContext(std::vector<const Module *> &inputModules, std::vector<DepPackage> &inputDependencies,
                            const std::string &inputPackageName, std::vector<SemanticDiagnostic> &inputDiagnostics,
                            std::vector<SemanticSymbol> &inputSymbols, const CompileTimeContext &inputContext,
                            std::unordered_map<const Expr *, TypeRef> &inputExpressionTypes,
                            std::unordered_map<const TypeExpr *, TypeRef> &inputTypeNodeTypes,
                            std::unordered_map<const Pattern *, TypeRef> &inputPatternTypes,
                            std::unordered_map<const Expr *, ValueConsumption> &inputValueConsumptions,
                            std::unordered_map<const CallExpr *, ResolvedCallableBinding> &inputCallableBindings,
                            std::unordered_map<const Decl *, ResolvedSymbolIdentity> &inputSymbolIdentities,
                            std::unordered_map<const ImplDecl *, ResolvedVtableIdentity> &inputVtableIdentities,
                            std::unordered_map<std::string, ResolvedTypeLayout> &inputTypeLayouts,
                            std::unordered_map<const TypeQueryExpr *, std::uint64_t> &inputSizeOfValues);
    virtual ~SemanticAnalyzerContext();

    SemanticAnalyzerContext(const SemanticAnalyzerContext &) = delete;
    SemanticAnalyzerContext &operator=(const SemanticAnalyzerContext &) = delete;

    void Run();
    [[nodiscard]] std::unordered_map<std::string, TypeProperties> TakeTypeProperties();
    [[nodiscard]] std::unordered_map<std::string, DropGluePlan> TakeDropGluePlans();
    [[nodiscard]] std::unordered_map<std::string, ResolvedConstraintWitness> TakeConstraintWitnesses();
    [[nodiscard]] std::unordered_map<const TryExpr *, ResolvedPropagation> TakePropagations();
    [[nodiscard]] std::unordered_map<const ForStmt *, ResolvedIteration> TakeIterations();

protected:
    void EmitError(SourceLocation location, std::string message, std::vector<std::string> notes = {},
                   std::optional<std::string> help = {}) const;
    void EmitWarning(SourceLocation location, std::string message) const;
    void EmitUndefinedName(SourceLocation location, const std::string &name) const;
    void EmitGenericArityError(const TypeExpr &expression, std::string subject, std::size_t expectedCount,
                               std::size_t actualCount);
    [[nodiscard]] std::optional<TypeRef> ResolveStructTypeReference(const TypeExpr &expression, const std::string &name,
                                                                    const std::vector<TypeRef> &typeArguments);

    void PushScope();
    void PopScope();
    Symbol *Define(Symbol symbol);
    Symbol *DefineTrackedLocal(Symbol symbol, bool initialized);
    [[nodiscard]] const FuncDecl *BeginTrackedFunction(const FuncDecl &function);
    void EndTrackedFunction(const FuncDecl *previousFunction);
    void CheckTrackedRead(const Symbol &symbol, SourceLocation location);
    [[nodiscard]] TypeRef ReadTrackedSymbol(const Symbol &symbol, SourceLocation location);
    void RecordCheckedExpression(const Expr &expression, const TypeRef &type);
    void MarkTrackedAssignment(const Expr &target, SourceLocation location);
    [[nodiscard]] std::optional<MoveStateTracker::Issue> MoveTrackedExpression(const Expr &expression,
                                                                               SourceLocation location);
    [[nodiscard]] bool ValidateMoveSource(const Expr &expression, const TypeRef &type, SourceLocation location);
    [[nodiscard]] bool RejectSelfMove(const Expr &target, const Expr &value, const TypeRef &type,
                                      SourceLocation location);
    [[nodiscard]] TypeProperties ClassifyTypeProperties(const TypeRef &type);

    /// One interface bound written on a generic parameter, resolved to the interface it names. `interface` is null when
    /// the bound named nothing, or named something that is not an interface; the declaration site reports that, and
    /// every use site then skips the bound rather than reporting the same mistake at each call.
    struct ResolvedTypeBound {
        std::string name;
        const InterfaceDecl *interface = nullptr;
        SourceLocation location;
    };

    [[nodiscard]] std::vector<ResolvedTypeBound> ResolveTypeParameterBounds(const TypeParameter &parameter,
                                                                            bool report);
    void DeclareTypeParameterBounds(const std::vector<TypeParameter> &parameters);

    /// Holds the bounds of the type parameters a declaration owns for as long as that declaration is being checked,
    /// then restores the enclosing declaration's. Bounds are scoped exactly like `currentTypeParams`, and a checker
    /// that saved one without restoring the other would report the wrong declaration's promises. A null `parameters`
    /// declares none of its own, which is how an extend block over a non-generic type enters.
    class ScopedTypeParameterBounds {
    public:
        ScopedTypeParameterBounds(SemanticAnalyzerContext &owner, const std::vector<TypeParameter> *parameters,
                                  bool replaceEnclosing = true);
        ~ScopedTypeParameterBounds();

        ScopedTypeParameterBounds(const ScopedTypeParameterBounds &) = delete;
        ScopedTypeParameterBounds &operator=(const ScopedTypeParameterBounds &) = delete;

    private:
        SemanticAnalyzerContext &context;
        std::unordered_map<std::string, std::vector<ResolvedTypeBound>> saved;
    };

    void CheckTypeArgumentConstraints(const std::vector<TypeParameter> &parameters,
                                      const std::unordered_map<std::string, TypeRef> &substitutions,
                                      SourceLocation location, const std::string &subject);
    void CheckTypeReferenceConstraints(const TypeExpr &expression, const std::vector<TypeParameter> &parameters,
                                       const std::vector<TypeRef> &typeArguments, const std::string &subject);
    void CheckWrittenTypeArgumentConstraints(const std::vector<TypeParameter> &parameters,
                                             const std::vector<TypeExprPtr> &typeArguments, SourceLocation location,
                                             const std::string &subject);
    [[nodiscard]] bool TypeArgumentsSatisfyBounds(const std::vector<TypeParameter> &parameters,
                                                  const std::unordered_map<std::string, TypeRef> &substitutions);
    [[nodiscard]] bool TypeSatisfiesBound(const TypeRef &argument, const InterfaceDecl &interface, std::string &reason);
    [[nodiscard]] const FuncDecl *SelectBoundOperation(const std::string &typeName, const FuncDecl &required) const;

    /// The bound operation a constrained receiver's method name resolves to, and where it sits in its interface.
    struct ConstrainedOperation {
        std::string parameterName;
        std::string interfaceName;
        const FuncDecl *operation = nullptr;
        std::size_t operationIndex = 0;
    };

    [[nodiscard]] std::optional<ConstrainedOperation> LookupConstrainedOperation(const TypeRef &receiverType,
                                                                                 const std::string &methodName) const;
    void EmitMissingConstrainedOperation(SourceLocation location, const TypeRef &receiverType,
                                         const std::string &methodName) const;
    void RecordConstrainedBinding(const CallExpr &call, const ConstrainedOperation &operation,
                                  const TypeRef &receiverType);
    void ConsumeValue(const Expr &expression, const TypeRef &type, ValueConsumptionKind kind, SourceLocation location);
    void ConsumeRecordedValue(const Expr &expression, ValueConsumptionKind kind, SourceLocation location);
    [[nodiscard]] std::vector<TypeRef> CheckCallArgumentValues(const CallExpr &call);
    void ConsumeCallArguments(const CallExpr &call, const std::vector<TypeRef> &argumentTypes);
    void ConsumeMethodReceiver(const CallExpr &call, const Expr &receiver, const TypeRef &receiverType,
                               const FuncDecl &method);

    struct TrackedFlow {
        MoveStateTracker::Snapshot states;
        bool reachable;
    };

    [[nodiscard]] TrackedFlow SaveTrackedFlow() const;
    void RestoreTrackedFlow(const TrackedFlow &flow);
    void MergeTrackedFlows(const std::vector<TrackedFlow> &flows);

    struct TrackedLoop {
        std::string label;
        MoveStateTracker::Snapshot shape;
        std::vector<TrackedFlow> breaks;
        std::vector<TrackedFlow> continues;
    };

    void BeginTrackedLoop(std::string_view label);
    [[nodiscard]] TrackedLoop EndTrackedLoop();
    void RecordTrackedLoopExit(std::string_view label, bool isContinue);
    [[nodiscard]] TypeRef CheckShortCircuitExpression(const BinaryExpr &expression);
    [[nodiscard]] TypeRef CheckTernaryExpression(const TernaryExpr &expression);
    [[nodiscard]] TypeRef CheckMatchExpression(const MatchExpr &expression);

    void CheckBlock(const Block &block);
    void CheckFunctionBody(const Block &block, const FuncDecl &function, const TypeRef &returnType);
    void CheckBooleanCondition(const TypeRef &type, SourceLocation location, std::string_view construct) const;
    void CheckPattern(const Pattern &pattern, const TypeRef &subjectType = TypeRef::MakeUnknown());
    void ValidateMatchPatterns(const std::vector<const Pattern *> &patterns, const TypeRef &subjectType);
    void ValidateMatchPatterns(const MatchExpr &expression, const TypeRef &subjectType);
    [[nodiscard]] bool MatchPatternsAreExhaustive(const std::vector<const Pattern *> &patterns,
                                                  const TypeRef &subjectType) const;
    [[nodiscard]] bool BlockDefinitelyReturns(const Block &block) const;
    [[nodiscard]] std::optional<TypeRef> CheckBasicExpression(const Expr &expression);

    /// A type `?` can propagate from, recognized by its variants rather than by a built-in identity: `Result` carries a
    /// payload and a failure, `Option` only a payload.
    struct PropagationShape {
        enum class Kind {
            Result,
            Option
        };

        Kind kind = Kind::Result;
        const EnumDecl *declaration = nullptr;
        TypeRef payload = TypeRef::MakeUnknown();
        std::optional<TypeRef> failure;
    };

    [[nodiscard]] std::optional<PropagationShape> PropagationShapeOf(const TypeRef &type);
    [[nodiscard]] static std::string_view PropagationKindName(PropagationShape::Kind kind);
    [[nodiscard]] static std::string_view PropagationKindPhrase(PropagationShape::Kind kind);
    [[nodiscard]] std::optional<TypeRef> CheckTryExpression(const TryExpr &expression);
    [[nodiscard]] TypeRef CheckTypeQueryExpression(const TypeQueryExpr &expression);
    [[nodiscard]] static bool IsCheckedArithmeticIntrinsic(std::string_view intrinsicName);
    void ValidateCheckedArithmeticIntrinsic(const FuncDecl &declaration);

    /// How a `for` loop reads a subject. An array, a slice and a range are driven directly; anything else is driven
    /// through the iterator convention, either because it is an iterator or because it hands one out.
    struct IterationShape {
        enum class Kind {
            Range,
            Indexed,
            Iterator,
            Iterable
        };

        Kind kind = Kind::Indexed;
        TypeRef itemType = TypeRef::MakeUnknown();
        TypeRef iteratorType = TypeRef::MakeUnknown();
        const FuncDecl *advance = nullptr;
        const FuncDecl *entry = nullptr;
        /// What `Next` reports, and the enum declaration that reports it.
        TypeRef reportedType = TypeRef::MakeUnknown();
        const EnumDecl *reportedDeclaration = nullptr;
    };

    [[nodiscard]] const FuncDecl *LookupIteratorAdvance(const TypeRef &type) const;
    [[nodiscard]] const FuncDecl *LookupIterableEntry(const TypeRef &type) const;

    /// What one `Next` reports: the item it carries, the Option type it is wrapped in, and that enum's declaration.
    struct ReportedItem {
        TypeRef itemType;
        TypeRef reportedType;
        const EnumDecl *declaration = nullptr;
    };

    [[nodiscard]] std::optional<ReportedItem> ReportedItemOf(const TypeRef &iteratorType, const FuncDecl &advance);
    [[nodiscard]] std::optional<IterationShape> IterationShapeOf(const TypeRef &subject);
    void EmitNotIterable(SourceLocation location, const TypeRef &subject) const;
    void ValidateIteratorConvention(const FuncDecl &declaration, bool isMethod);
    void RecordIteration(const ForStmt &statement, const IterationShape &shape);
    [[nodiscard]] TypeRef CheckCallExpression(const CallExpr &expression);
    [[nodiscard]] std::optional<TypeRef> CheckAggregateExpression(const Expr &expression);
    void ValidateDeferredBasicExpressionChecks(const FuncDecl &declaration,
                                               const std::unordered_map<std::string, TypeRef> &substitutions);
    [[nodiscard]] bool PlaceIsImmutable(const Expr &place);
    [[nodiscard]] TypeRef DeclareReceiver(const FuncDecl &declaration, bool isMethod);
    void CheckReceiverType(const FuncDecl &declaration, const Param &receiver, const TypeRef &declared);
    void CheckReceiverPlacement(const FuncDecl &declaration, bool isMethod);
    [[nodiscard]] const EnumDecl::Variant *LookupEnumVariant(const std::string &enumName,
                                                             const std::string &variantName) const;
    [[nodiscard]] static std::string SliceTypeName(const TypeRef &elementType);
    [[nodiscard]] std::string NamedBaseTypeName(const TypeRef &type) const;
    [[nodiscard]] std::optional<TypeRef> SliceElementType(const TypeRef &type) const;
    [[nodiscard]] std::optional<TypeRef> IndexElementType(const TypeRef &type);

    std::vector<const Module *> &modules;
    std::vector<DepPackage> &deps;
    const std::string &packageName;
    std::vector<SemanticDiagnostic> &diags;
    const CompileTimeContext &context;
    std::unordered_map<const Expr *, TypeRef> &expressionTypes;
    std::unordered_map<const TypeExpr *, TypeRef> &typeNodeTypes;
    std::unordered_map<const Pattern *, TypeRef> &patternTypes;
    std::unordered_map<const Expr *, ValueConsumption> &valueConsumptions;
    std::unordered_map<const CallExpr *, ResolvedCallableBinding> &callableBindings;
    std::unordered_map<const Decl *, ResolvedSymbolIdentity> &symbolIdentities;
    std::unordered_map<const ImplDecl *, ResolvedVtableIdentity> &vtableIdentities;
    std::unordered_map<std::string, ResolvedTypeLayout> &typeLayouts;
    std::unordered_map<std::string, TypeProperties> typeProperties;
    std::unordered_map<std::string, DropGluePlan> dropGluePlans;
    /// One entry per (bound, concrete type) pair a use site proved, so lowering can call the satisfying method directly
    /// in each instantiation instead of dispatching through the interface.
    std::unordered_map<std::string, ResolvedConstraintWitness> constraintWitnesses;
    /// One entry per accepted `expr?`, so lowering builds its early return without recognizing Result or Option again.
    std::unordered_map<const TryExpr *, ResolvedPropagation> propagations;
    /// One entry per accepted `for`, so lowering drives the subject the way analysis decided it is driven.
    std::unordered_map<const ForStmt *, ResolvedIteration> iterations;
    std::unordered_map<const TypeQueryExpr *, std::uint64_t> &typeQueryValues;

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
    /// The extend block a method is being checked in, and the type it extends. A method's declared receiver has to name
    /// that type, and an extend block that names an interface constrains the receiver further, so both travel from
    /// CheckImplDecl down to the method being checked.
    const ImplDecl *currentImpl = nullptr;
    TypeRef currentExtendedType = TypeRef::MakeUnknown();
    std::vector<std::string> currentTypeParams;
    /// Interface bounds of the generic parameters currently in scope, keyed by parameter name. A type parameter passed
    /// on as a type argument satisfies a bound exactly when its own declaration carries that bound, so a generic body
    /// is checked once against what it promised rather than again at every instantiation.
    std::unordered_map<std::string, std::vector<ResolvedTypeBound>> currentTypeParamBounds;
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
    MoveStateTracker moveStates;
    std::vector<MoveStateTracker> savedMoveStates;
    bool trackedFlowReachable = true;
    std::vector<bool> savedTrackedFlowReachability;
    std::vector<TrackedLoop> trackedLoops;
    std::vector<std::vector<TrackedLoop>> savedTrackedLoops;
    bool checkingPlainAssignmentTarget = false;

    [[nodiscard]] static bool IsUnimplementedPrimitiveType(std::string_view name);

private:
    struct DeferredUnaryCheck {
        TokenKind op;
        TypeRef operand;
        SourceLocation location;
    };

    struct DeferredBinaryCheck {
        TokenKind op;
        TypeRef left;
        TypeRef right;
        const Expr *leftExpression;
        const Expr *rightExpression;
        SourceLocation location;
    };

    std::unordered_map<const FuncDecl *, std::vector<DeferredUnaryCheck>> deferredUnaryChecks;
    std::unordered_map<const FuncDecl *, std::vector<DeferredBinaryCheck>> deferredBinaryChecks;
    std::unordered_set<const TypeExpr *> reportedGenericArity;
    /// A type expression is resolved once per place it is read -- a parameter's type is resolved again for its symbol,
    /// its signature, and each overload attempt -- so an unsatisfied bound is reported for the spelling, not per read.
    std::unordered_set<const TypeExpr *> reportedTypeArgumentConstraints;

    void RegisterBuiltins();
    void IndexDeclarations();
    void CollectModule(const Module &module);
    void CheckStatement(const Stmt &statement);
    void CheckLetPattern(const Pattern &pattern, const TypeRef &type, bool isMutable);

    virtual TypeRef ResolveType(const TypeExpr &expression) = 0;
    /// The target layout of a written type, or nothing when it has none: recursive, unsized, or not yet validated.
    [[nodiscard]] virtual std::optional<ResolvedTypeLayout> LayoutOfTypeExpression(const TypeExpr &expression) = 0;
    virtual TypeRef ResolveTypeWithSubstitution(const TypeExpr &expression,
                                                const std::unordered_map<std::string, TypeRef> &substitutions) = 0;
    virtual TypeRef CheckExpr(const Expr &expression) = 0;
    virtual TypeRef MakeFuncTypeWithSubstitution(const std::vector<Param> &parameters,
                                                 const std::optional<TypeExprPtr> &returnType,
                                                 const std::unordered_map<std::string, TypeRef> &substitutions,
                                                 const std::vector<std::string> &typeParameters,
                                                 bool cVariadic = false) = 0;
    virtual void EmitDiagnosticIntrinsic(const std::string &intrinsicName, const CallExpr &call) = 0;
    [[nodiscard]] virtual const FuncDecl *LookupFunctionOverload(const Symbol &symbol,
                                                                 const std::vector<TypeRef> &argumentTypes,
                                                                 const std::vector<TypeExprPtr> &typeArguments) = 0;
    virtual void QueueGenericInstantiation(const FuncDecl &declaration,
                                           const std::unordered_map<std::string, TypeRef> &substitutions) = 0;
    [[nodiscard]] virtual const FuncDecl *LookupMethod(const TypeRef &receiverType, const std::string &methodName,
                                                       const std::vector<TypeRef> &argumentTypes) = 0;
    [[nodiscard]] virtual std::unordered_map<std::string, TypeRef>
    MethodTypeSubstitutions(const TypeRef &receiverType) const = 0;
    [[nodiscard]] virtual TypeRef InstantiateAssociatedReceiver(TypeRef receiverType,
                                                                const std::vector<TypeExprPtr> &typeArguments) = 0;
    [[nodiscard]] virtual TypeRef ResolveMethodReturnType(const TypeRef &receiverType, const FuncDecl &method) = 0;
    [[nodiscard]] virtual std::vector<TypeRef> ResolveMethodParamTypes(const TypeRef &receiverType,
                                                                       const FuncDecl &method) = 0;
    [[nodiscard]] virtual const FuncDecl *LookupInterfaceMethod(const TypeRef &receiverType,
                                                                const std::string &methodName) const = 0;
    [[nodiscard]] virtual TypeRef ResolveInterfaceMethodReturnType(const FuncDecl &method) = 0;
    [[nodiscard]] virtual std::vector<TypeRef> ResolveInterfaceMethodParamTypes(const FuncDecl &method) = 0;
    [[nodiscard]] virtual TypeRef EnumVariantConstructorType(const EnumDecl &declaration,
                                                             const EnumDecl::Variant &variant,
                                                             const std::vector<TypeRef> &typeArguments) = 0;
    [[nodiscard]] virtual TypeRef EnumType(const EnumDecl &declaration,
                                           const std::vector<TypeRef> &typeArguments = {}) = 0;
    [[nodiscard]] virtual TypeRef LiteralType(const Token &token) const = 0;
    virtual void ValidateCastConstant(const CastExpr &expression, const TypeRef &operandType,
                                      const TypeRef &targetType) const = 0;
    [[nodiscard]] virtual const FuncDecl *LookupOperatorMethod(const TypeRef &receiverType,
                                                               const std::string &operatorName,
                                                               const std::vector<TypeRef> &argumentTypes) = 0;
    [[nodiscard]] virtual std::vector<TypeRef> ResolveOperatorParameterTypes(const TypeRef &receiverType,
                                                                             const FuncDecl &method) = 0;
    [[nodiscard]] virtual TypeRef ResolveOperatorReturnType(const TypeRef &receiverType, const FuncDecl &method) = 0;
    virtual void CheckDecl(const Decl &declaration) = 0;
    virtual void ValidateArrayType(const TypeExpr &type, bool allowFlexibleTail) = 0;
    [[nodiscard]] virtual bool CanAssignExprTo(const Expr &expression, const TypeRef &expressionType,
                                               const TypeRef &targetType) = 0;
    [[nodiscard]] virtual std::string AssignmentErrorMessage(const Expr &expression, const TypeRef &targetType,
                                                             std::string fallback) = 0;
    [[nodiscard]] virtual std::string BaseTypeName(const std::string &name) const = 0;
    [[nodiscard]] virtual TypeRef ParseTypeRefFromString(std::string typeName) const = 0;
    [[nodiscard]] virtual std::vector<TypeRef> ParseTypeArgsFromTypeName(const std::string &typeName) const = 0;
    virtual void ApplyModuleImports(const Module &module) = 0;
    virtual void ApplyModuleImportsInScope(const Module &module, Scope &scope) = 0;
    virtual void ResolveModuleSignatures(const Module &module) = 0;
    virtual void ResolveModuleSignaturesInScope(const Module &module, Scope &scope) = 0;
    virtual void CheckModule(const Module &module) = 0;
    virtual void CheckModuleInScope(const Module &module, Scope &scope) = 0;
    virtual void ValidatePendingGenericInstantiations() = 0;
    virtual void RecordResolvedTypeLayouts() = 0;
    void RecordResolvedTypeProperties();
    void SynthesizeResolvedDropGlue();
    [[nodiscard]] std::vector<DropGlueStep> BuildDropGlueSteps(const TypeRef &type,
                                                               std::unordered_set<std::string> &activeTypes);
    [[nodiscard]] bool TypeImplementsDrop(const std::string &baseName) const;
    [[nodiscard]] static std::string DropGlueSymbol(const TypeRef &type);
    virtual void BuildFinalSymbolIdentities() = 0;

    [[nodiscard]] TypeRef CheckUnary(TokenKind op, const TypeRef &operand, SourceLocation location);
    [[nodiscard]] TypeRef CheckBinary(TokenKind op, const TypeRef &left, const TypeRef &right,
                                      const Expr &leftExpression, const Expr &rightExpression, SourceLocation location);
    [[nodiscard]] Symbol *LookupCalleeSymbol(const Expr &callee) const;
    void EmitCallSiteDiagnostics(const Decl &declaration, SourceLocation location) const;
    void RecordFunctionBinding(const CallExpr &call, const FuncDecl &declaration,
                               ResolvedCallableBinding::DispatchKind dispatch,
                               std::unordered_map<std::string, TypeRef> substitutions = {},
                               std::optional<TypeRef> receiverType = std::nullopt);
    void RecordExternBinding(const CallExpr &call, const ExternFuncDecl &declaration);
    [[nodiscard]] std::string GenericStructInitName(const StructInitExpr &expression);
    [[nodiscard]] std::pair<const EnumDecl *, const EnumDecl::Variant *>
    LookupEnumVariantInitializer(const std::string &typeName) const;
    [[nodiscard]] std::unordered_map<std::string, TypeRef>
    StructTypeSubstitutions(const StructDecl &declaration, const std::vector<TypeExprPtr> &typeArguments);
    [[nodiscard]] TypeRef StructFieldType(const TypeRef &objectType, const std::string &fieldName);
    void CheckStructInitExpression(const StructInitExpr &expression);
    [[nodiscard]] bool PlaceIsWritable(const Expr &place, const TypeRef &placeType);
    [[nodiscard]] bool CheckAssignableTarget(const Expr &target, const TypeRef &targetType,
                                             std::string_view operatorName);
    void CheckMutability(const Expr &target);
    [[nodiscard]] bool CheckReceiverMutability(const CallExpr &call, const Expr &receiver, const TypeRef &receiverType,
                                               const FuncDecl &method);
    /// The receiver the method declares, resolved for this call's receiver type. Empty for an associated function,
    /// which takes none.
    [[nodiscard]] std::optional<TypeRef> ResolveMethodReceiverType(const TypeRef &receiverType, const FuncDecl &method);
};
} // namespace Rux::SemanticDetail
