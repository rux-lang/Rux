#pragma once

#include "Semantic/Detail/MovePlace.h"
#include "Semantic/Detail/MoveStateTracker.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Semantic/SemanticProgramIndex.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Rux::SemanticDetail {
/// The name an interface uses for the type implementing it. There are no generic interfaces, so this is the only way
/// an interface method can say that an operand or a result is the same type as its receiver.
inline constexpr std::string_view SelfTypeName = "Self";

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
                            std::unordered_map<const EnumPattern *, ResolvedCasePattern> &inputCasePatterns,
                            std::unordered_map<const BinaryExpr *, ResolvedVariantEquality> &inputVariantEqualities,
                            std::unordered_map<std::string, VariantEqualityPlan> &inputVariantEqualityPlans,
                            std::unordered_map<const Expr *, ValueConsumption> &inputValueConsumptions,
                            std::unordered_map<const Expr *, ValueCopy> &inputValueCopies,
                            std::unordered_map<const CallExpr *, ResolvedCallableBinding> &inputCallableBindings,
                            std::unordered_map<const LetStmt *, ResolvedDefaultConstructor> &inputDefaultConstructors,
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
    [[nodiscard]] std::unordered_map<const IndexExpr *, ResolvedIndexOperator> TakeIndexOperators();
    [[nodiscard]] std::unordered_map<const ForStmt *, ResolvedIteration> TakeIterations();
    [[nodiscard]] std::unordered_map<const Decl *, bool> EffectiveVisibilities() const;

protected:
    void EmitError(SourceLocation location, std::string message, std::vector<std::string> notes = {},
                   std::optional<std::string> help = {}) const;
    void EmitWarning(SourceLocation location, std::string message) const;
    void EmitUndefinedName(SourceLocation location, const std::string &name) const;
    [[nodiscard]] bool IsAccessible(const Symbol &symbol) const;
    [[nodiscard]] bool IsAccessible(const Decl &declaration) const;
    [[nodiscard]] bool IsMemberAccessible(const Decl &owner, bool memberIsPublic) const;
    void EmitPrivacyError(SourceLocation useLocation, const Symbol &symbol) const;
    void EmitPrivacyError(SourceLocation useLocation, const Decl &declaration, std::string_view kind,
                          std::string_view name) const;
    void EmitPrivateMemberError(SourceLocation useLocation, const Decl &owner, std::string_view kind,
                                std::string_view name) const;
    void ValidatePublicType(const TypeExpr &type, std::string_view subject,
                            const std::unordered_set<std::string> &typeParameters = {});
    void ValidatePublicResolvedType(const TypeRef &type, const Decl &declaration, std::string_view subject);
    void ValidatePublicTypeParameters(const std::vector<TypeParameter> &parameters, std::string_view subject,
                                      const std::unordered_set<std::string> &typeParameterNames);
    void ValidatePublicFunction(const FuncDecl &function, std::string_view subject,
                                const TypeExpr *extendedType = nullptr);
    void ValidatePublicDeclaration(const Decl &declaration);

    struct FunctionSignature {
        std::size_t typeParamCount = 0;
        std::vector<TypeRef> paramTypes;
        std::vector<bool> variadicParams;
    };

    [[nodiscard]] TypeRef MakeFuncType(const std::vector<Param> &parameters,
                                       const std::optional<TypeExprPtr> &returnType,
                                       const std::vector<std::string> &typeParameters = {}, bool cVariadic = false);
    [[nodiscard]] TypeRef MakeFuncTypeWithSubstitution(const std::vector<Param> &parameters,
                                                       const std::optional<TypeExprPtr> &returnType,
                                                       const std::unordered_map<std::string, TypeRef> &substitutions,
                                                       const std::vector<std::string> &typeParameters,
                                                       bool cVariadic = false);
    [[nodiscard]] std::optional<FunctionSignature> ResolveFunctionSignature(const FuncDecl &declaration, bool isMethod);
    [[nodiscard]] static bool SameFunctionSignature(const FunctionSignature &left, const FunctionSignature &right);
    void ValidateFunctionSignature(const FuncDecl &declaration, const std::vector<const FuncDecl *> &overloads,
                                   bool isMethod = false);
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
    void ReportUntypedExpression(const Expr &expression) const;
    void MarkTrackedAssignment(const Expr &target, SourceLocation location);
    [[nodiscard]] std::optional<MoveStateTracker::Issue> MoveTrackedExpression(const Expr &expression,
                                                                               SourceLocation location);
    [[nodiscard]] bool ValidateMoveSource(const Expr &expression, SourceLocation location);
    [[nodiscard]] bool RejectSelfMove(const Expr &target, const Expr &value, SourceLocation location);
    [[nodiscard]] TypeProperties ClassifyTypeProperties(const TypeRef &type);
    [[nodiscard]] const FuncDecl *LookupSourceSpecialOperation(const TypeRef &type, std::string_view operation,
                                                               SourceLocation location);
    [[nodiscard]] static bool IsSpecialOperationName(std::string_view name);
    [[nodiscard]] static bool IsDestructorName(std::string_view name);
    void ValidateSpecialOperation(const FuncDecl &method, const TypeRef &extendedType);
    void ValidateDestructor(const FuncDecl &method, const TypeRef &extendedType);
    void ValidateConstructor(const FuncDecl &method, const TypeRef &extendedType);
    [[nodiscard]] bool IsConstructorCandidate(const FuncDecl &method, const TypeRef &type);
    [[nodiscard]] std::vector<const FuncDecl *> ConstructorCandidates(const TypeRef &type);
    [[nodiscard]] std::vector<const FuncDecl *> AccessibleMethodCandidates(const TypeRef &receiverType,
                                                                           const std::string &methodName) const;

    struct BorrowPlace {
        const Symbol *root = nullptr;
        std::vector<MovePlace::Projection> projections;

        [[nodiscard]] std::string Display() const;
    };

    struct ActiveBorrow {
        const Symbol *alias = nullptr;
        const Symbol *parentAlias = nullptr;
        BorrowPlace place;
        bool exclusive = false;
        SourceLocation location;
    };

    using BorrowSnapshot = std::vector<ActiveBorrow>;

    void PrepareBorrowAnalysis(const FuncDecl &function);
    void FinishBorrowAnalysis();
    void ExpireDeadBorrowsAfter(const Stmt &statement);
    void ExpireBorrowAtLastUse(const Symbol &symbol, SourceLocation location);
    void EndBorrowScope(const Scope &scope);
    void RegisterReferenceBinding(const Symbol &alias, const Expr &initializer, const TypeRef &referenceType);
    void RegisterReferenceAssignment(const Expr &target, const Expr &initializer, const TypeRef &referenceType);
    void CheckBorrowedRead(const Symbol &symbol, SourceLocation location);
    void CheckBorrowedPlaceRead(const Expr &expression, SourceLocation location);
    void CheckBorrowedMutation(const Expr &target, SourceLocation location);
    [[nodiscard]] bool CheckBorrowedMove(const Expr &expression, SourceLocation location);
    void ValidateCallReferenceBorrows(const CallExpr &call, const std::vector<TypeRef> &parameterTypes);
    void BeginReceiverReferenceBorrow(const CallExpr &call, const Expr &receiver, const TypeRef &referenceType);
    [[nodiscard]] bool TypeStoresReference(const TypeRef &type);
    void ValidateStoredType(const TypeRef &type, SourceLocation location, std::string_view subject);
    [[nodiscard]] BorrowSnapshot SaveBorrows() const;
    void RestoreBorrows(const BorrowSnapshot &snapshot);
    [[nodiscard]] static BorrowSnapshot MergeBorrows(std::span<const BorrowSnapshot> snapshots);
    [[nodiscard]] static BorrowSnapshot ProjectBorrows(const BorrowSnapshot &source, const BorrowSnapshot &shape);
    [[nodiscard]] static bool BorrowPlacesOverlap(const BorrowPlace &left, const BorrowPlace &right);
    [[nodiscard]] static bool SameBorrowPlace(const BorrowPlace &left, const BorrowPlace &right);
    [[nodiscard]] std::optional<BorrowPlace> ResolveBorrowPlace(const Expr &expression) const;
    [[nodiscard]] const Symbol *ReferenceSourceAlias(const Expr &expression) const;
    [[nodiscard]] bool ReportBorrowConflict(const BorrowPlace &place, bool exclusive, const Symbol *parentAlias,
                                            SourceLocation location, std::string_view action) const;

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
    bool DeduceTypeArguments(const FuncDecl &declaration, const std::vector<TypeRef> &argumentTypes,
                             std::unordered_map<std::string, TypeRef> &substitutions);
    bool DeduceTypeArgument(const TypeRef &paramType, const TypeRef &argType,
                            const std::unordered_set<std::string> &typeParamNames,
                            std::unordered_map<std::string, TypeRef> &substitutions);
    [[nodiscard]] TypeRef EnumVariantConstructorType(const EnumDecl &declaration, const EnumDecl::Variant &variant,
                                                     const std::vector<TypeRef> &typeArguments = {});

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
    [[nodiscard]] bool RejectImplicitMove(const Expr &expression, const TypeRef &type, ValueConsumptionKind kind,
                                          SourceLocation location);
    void ConsumeValue(const Expr &expression, const TypeRef &type, ValueConsumptionKind kind, SourceLocation location);
    void ConsumeExplicitValue(const Expr &expression, const TypeRef &type, SourceLocation location);
    void ConsumeRecordedValue(const Expr &expression, ValueConsumptionKind kind, SourceLocation location);
    [[nodiscard]] std::vector<TypeRef> CheckCallArgumentValues(const CallExpr &call);
    void ConsumeCallArguments(const CallExpr &call, const std::vector<TypeRef> &argumentTypes,
                              const std::vector<TypeRef> *parameterTypes = nullptr);
    template <typename Arm>
    void ConsumeMatchSubject(const Expr &subject, const TypeRef &subjectType, const std::vector<Arm> &arms,
                             SourceLocation location);
    void ConsumeMethodReceiver(const CallExpr &call, const Expr &receiver, const TypeRef &receiverType,
                               const FuncDecl &method);

    struct TrackedFlow {
        MoveStateTracker::Snapshot states;
        BorrowSnapshot borrows;
        bool reachable;
    };

    [[nodiscard]] TrackedFlow SaveTrackedFlow() const;
    void RestoreTrackedFlow(const TrackedFlow &flow);
    void MergeTrackedFlows(const std::vector<TrackedFlow> &flows);

    struct TrackedLoop {
        std::string label;
        MoveStateTracker::Snapshot shape;
        BorrowSnapshot borrowShape;
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
    void CheckCast(const TypeRef &operand, const TypeRef &target, SourceLocation location);
    [[nodiscard]] bool CastTypesAreCompatible(const TypeRef &operand, const TypeRef &target) const;

    /// A type `?` can propagate from, recognized by its declared variant cases rather than by a built-in identity:
    /// `Result` carries a payload and a failure, `Option` only a payload.
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
    [[nodiscard]] std::optional<std::string>
    PropagationShapeIssue(const TypeRef &type, std::optional<PropagationShape::Kind> expectedKind = std::nullopt) const;
    [[nodiscard]] static std::string_view PropagationKindName(PropagationShape::Kind kind);
    [[nodiscard]] static std::string_view PropagationKindPhrase(PropagationShape::Kind kind);
    [[nodiscard]] std::optional<TypeRef> CheckTryExpression(const TryExpr &expression);
    [[nodiscard]] TypeRef CheckTypeQueryExpression(const TypeQueryExpr &expression);
    [[nodiscard]] static bool IsCheckedArithmeticIntrinsic(std::string_view intrinsicName);
    void ValidateCheckedArithmeticIntrinsic(const FuncDecl &declaration);
    [[nodiscard]] static bool IsZeroizeIntrinsic(std::string_view intrinsicName);
    void ValidateZeroizeIntrinsic(const FuncDecl &declaration);

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

    struct ResolvedCase {
        const EnumDecl *declaration = nullptr;
        const EnumDecl::Variant *selectedCase = nullptr;
        EnumDecl::Form form = EnumDecl::Form::Enumeration;
    };

    [[nodiscard]] std::optional<ResolvedCase> LookupCase(const std::string &typeName,
                                                         const std::string &caseName) const;
    [[nodiscard]] static std::string SliceTypeName(const TypeRef &elementType);
    /// Whether this expression is an index that resolved to a declared `[]`. Such an expression is a call producing a
    /// value, so it is neither an assignable place nor a projection any borrow or move can reach through, and every
    /// place walk stops at it.
    [[nodiscard]] bool IsIndexOperatorCall(const Expr &expression) const;

    /// The nearest index expression this place path passes through that resolved to a declared `[]`, or null when the
    /// path reaches its root without one. `v[i].field` has no place to write to for the same reason `v[i]` does not.
    [[nodiscard]] const IndexExpr *IndexOperatorInPlace(const Expr &place) const;

    /// Place decomposition that knows which index expressions resolved to a declared `[]`. These hide the namespace
    /// forms of the same names, so every decomposition inside analysis sees an operator index as the call it is.
    [[nodiscard]] MovePlace AnalyzeMovePlace(const Expr &expression) const;
    [[nodiscard]] bool SameStoragePlace(const Expr &left, const Expr &right) const;

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
    std::unordered_map<const EnumPattern *, ResolvedCasePattern> &casePatterns;
    std::unordered_map<const BinaryExpr *, ResolvedVariantEquality> &variantEqualities;
    std::unordered_map<std::string, VariantEqualityPlan> &variantEqualityPlans;
    std::unordered_map<const Expr *, ValueConsumption> &valueConsumptions;
    std::unordered_map<const Expr *, ValueCopy> &valueCopies;
    std::unordered_map<const CallExpr *, ResolvedCallableBinding> &callableBindings;
    std::unordered_map<const LetStmt *, ResolvedDefaultConstructor> &defaultConstructors;
    std::unordered_map<const Decl *, ResolvedSymbolIdentity> &symbolIdentities;
    std::unordered_map<const ImplDecl *, ResolvedVtableIdentity> &vtableIdentities;
    std::unordered_map<std::string, ResolvedTypeLayout> &typeLayouts;
    std::unordered_map<std::string, TypeProperties> typeProperties;
    std::unordered_map<std::string, DropGluePlan> dropGluePlans;
    /// Types an instantiation named for the first time, which nothing written concretely spells. A generic body
    /// composing `Option<V>` mentions no such type until `V` is decided, so the walks over what the program wrote
    /// never reach it and it would have neither a layout nor a destructor.
    std::vector<TypeRef> instantiatedTypes;
    /// One entry per (bound, concrete type) pair a use site proved, so lowering can call the satisfying method directly
    /// in each instantiation instead of dispatching through the interface.
    std::unordered_map<std::string, ResolvedConstraintWitness> constraintWitnesses;
    /// One entry per accepted `expr?`, so lowering builds its early return without recognizing Result or Option again.
    std::unordered_map<const TryExpr *, ResolvedPropagation> propagations;
    /// One entry per accepted index expression that resolved to a declared `[]`, so lowering calls the operator
    /// without resolving it again and analysis knows the expression is a call rather than a place.
    std::unordered_map<const IndexExpr *, ResolvedIndexOperator> indexOperators;
    /// One entry per accepted `for`, so lowering drives the subject the way analysis decided it is driven.
    std::unordered_map<const ForStmt *, ResolvedIteration> iterations;
    std::unordered_map<const TypeQueryExpr *, std::uint64_t> &typeQueryValues;

    SemanticProgramIndex programIndex;
    Scope &globalScope;
    const SemanticProgramIndex::PackageScopes &packageModuleScopes;
    std::string currentFile;
    std::string currentPackage;
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
    const std::unordered_map<const Decl *, SemanticProgramIndex::DeclarationInfo> &declarationInfos;
    std::unordered_set<const TypeExpr *> reportedPrivateApiTypes;
    std::unordered_set<const Decl *> reportedPrivateApiDeclarations;
    Scope *currentScope;
    MoveStateTracker moveStates;
    std::vector<MoveStateTracker> savedMoveStates;
    bool trackedFlowReachable = true;
    std::vector<bool> savedTrackedFlowReachability;
    std::vector<TrackedLoop> trackedLoops;
    std::vector<std::vector<TrackedLoop>> savedTrackedLoops;
    std::unordered_map<const Symbol *, ActiveBorrow> activeBorrows;
    std::unordered_map<const CallExpr *, std::vector<ActiveBorrow>> pendingCallBorrows;
    std::unordered_map<const Stmt *, std::unordered_set<std::string>> borrowLiveAfter;
    std::unordered_map<const Stmt *, std::unordered_map<std::string, std::uint32_t>> borrowLastUseOffsets;
    std::unordered_map<const Symbol *, ActiveBorrow> endedBorrowProvenance;
    std::vector<std::unordered_map<const Symbol *, ActiveBorrow>> savedActiveBorrows;
    std::vector<std::unordered_map<const CallExpr *, std::vector<ActiveBorrow>>> savedPendingCallBorrows;
    std::vector<std::unordered_map<const Stmt *, std::unordered_set<std::string>>> savedBorrowLiveAfter;
    std::vector<std::unordered_map<const Stmt *, std::unordered_map<std::string, std::uint32_t>>>
        savedBorrowLastUseOffsets;
    std::vector<std::unordered_map<const Symbol *, ActiveBorrow>> savedEndedBorrowProvenance;
    const Stmt *currentBorrowStatement = nullptr;
    std::vector<const Stmt *> savedBorrowStatements;
    std::unordered_set<std::string> referenceStorageChecks;
    bool checkingPlainAssignmentTarget = false;
    bool checkingBorrowProjectionRoot = false;

    [[nodiscard]] static bool IsUnimplementedPrimitiveType(std::string_view name);

    /// The enum `name` names, as seen from the file being checked.
    ///
    /// The struct and enum indexes are keyed by bare name across every package, so the last declaration of a name
    /// wins: a program declaring its own `Option` displaces `Core`'s, and `Core`'s own methods would then be checked
    /// against the wrong shape. A declaration in the file doing the asking is the one that file means.
    ///
    /// @return nullptr when no enum of that name is in scope
    [[nodiscard]] const EnumDecl *EnumNamed(const std::string &name) const;

    struct CaseTypeDeclaration {
        const EnumDecl *declaration = nullptr;
        EnumDecl::Form form = EnumDecl::Form::Enumeration;

        [[nodiscard]] explicit operator bool() const noexcept {
            return declaration != nullptr;
        }

        [[nodiscard]] bool IsVariant() const noexcept {
            return form == EnumDecl::Form::Variant;
        }
    };

    /// Resolve a scoped case-bearing type while retaining whether its source declaration used `enum` or `variant`.
    [[nodiscard]] CaseTypeDeclaration CaseTypeNamed(const std::string &name) const;

    /// Report a suffixed integer literal whose magnitude the suffix's own type cannot hold.
    ///
    /// An unsuffixed literal is checked against whatever it is being assigned to; a suffixed one names its type
    /// itself, so nothing else ever checks it.
    void ValidateSuffixedIntegerLiteral(const LiteralExpr &literal, bool negative);

    /// Whether `type` still stands for something a type parameter in scope decides.
    ///
    /// An instantiation spells its arguments inside its own name -- `Option<T>` is one `Named` whose name says `T`,
    /// with nothing structural to walk -- so a test that reads only structure calls it concrete and every question
    /// deferred until the parameter is known is answered here instead, wrongly.
    [[nodiscard]] bool MentionsTypeParameter(const TypeRef &type) const;

    /// `type` with every type parameter replaced by what `substitutions` says it stands for, including the ones
    /// spelled inside an instantiation's name.
    [[nodiscard]] TypeRef SubstituteTypeParameters(TypeRef type,
                                                   const std::unordered_map<std::string, TypeRef> &substitutions) const;

    /// Whether a concrete type supplies the vtable required by an interface target. Empty interfaces accept every
    /// type, and platform-sized integer aliases share implementations with their fixed-width equivalents.
    [[nodiscard]] bool TypeImplementsInterface(const TypeRef &expressionType, const TypeRef &targetType) const;

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
        const BinaryExpr *binaryExpression;
        SourceLocation location;
    };

    struct DeferredCastCheck {
        TypeRef operand;
        TypeRef target;
        SourceLocation location;
    };

    /// A value whose type mentions a type parameter, so whether handing it over consumes it is not yet knowable.
    /// Answered once per instantiation, when the parameter stands for something with a mobility.
    struct DeferredConsumption {
        const Expr *expression;
        ValueConsumptionKind kind;
        TypeRef type;
        SourceLocation location;
    };

    std::unordered_map<const FuncDecl *, std::vector<DeferredUnaryCheck>> deferredUnaryChecks;
    std::unordered_map<const FuncDecl *, std::vector<DeferredBinaryCheck>> deferredBinaryChecks;
    std::unordered_map<const FuncDecl *, std::vector<DeferredCastCheck>> deferredCastChecks;
    std::unordered_map<const FuncDecl *, std::vector<DeferredConsumption>> deferredConsumptions;
    std::unordered_set<const TypeExpr *> reportedGenericArity;

    /// The suffixed integer literals sitting directly under a unary minus, registered before the operand is checked
    /// so the literal's own check knows its magnitude is a negative one. `-128i8` is in range while `128i8` is not.
    std::unordered_set<const LiteralExpr *> negatedIntegerLiterals;
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
    virtual void EmitDiagnosticIntrinsic(const std::string &intrinsicName, const CallExpr &call) = 0;
    [[nodiscard]] virtual const FuncDecl *LookupFunctionOverload(const Symbol &symbol,
                                                                 const std::vector<TypeRef> &argumentTypes,
                                                                 const std::vector<TypeExprPtr> &typeArguments) = 0;
    virtual void QueueGenericInstantiation(const FuncDecl &declaration,
                                           const std::unordered_map<std::string, TypeRef> &substitutions) = 0;
    [[nodiscard]] virtual const FuncDecl *LookupMethod(const TypeRef &receiverType, const std::string &methodName,
                                                       const std::vector<TypeRef> &argumentTypes,
                                                       bool requireAccessible = true) = 0;
    [[nodiscard]] virtual std::unordered_map<std::string, TypeRef>
    MethodTypeSubstitutions(const TypeRef &receiverType) const = 0;
    [[nodiscard]] virtual const std::vector<TypeParameter> *AggregateTypeParams(const std::string &name) const = 0;
    [[nodiscard]] virtual TypeRef InstantiateAssociatedReceiver(TypeRef receiverType,
                                                                const std::vector<TypeExprPtr> &typeArguments) = 0;
    [[nodiscard]] virtual TypeRef AssociatedFunctionType(const TypeRef &receiverType, const FuncDecl &method) = 0;
    [[nodiscard]] virtual TypeRef ResolveMethodReturnType(const TypeRef &receiverType, const FuncDecl &method) = 0;
    [[nodiscard]] virtual std::vector<TypeRef> ResolveMethodParamTypes(const TypeRef &receiverType,
                                                                       const FuncDecl &method) = 0;
    [[nodiscard]] virtual const FuncDecl *LookupInterfaceMethod(const TypeRef &receiverType,
                                                                const std::string &methodName) const = 0;

    /// Whether any written type of `method` names `Self`, which is what makes it callable only where the implementing
    /// type is known.
    [[nodiscard]] static bool InterfaceMethodMentionsSelf(const FuncDecl &method);
    /// An interface method's written types, with `Self` standing for `selfType`.
    ///
    /// An interface cannot name the type implementing it any other way -- there are no generic interfaces -- so a
    /// binary operation like equality or comparison has no way to say that its other operand is the same type as its
    /// receiver. `Self` is that name, and it is substituted here, where the receiver is known.
    [[nodiscard]] virtual TypeRef ResolveInterfaceMethodReturnType(const FuncDecl &method, const TypeRef &selfType) = 0;
    [[nodiscard]] virtual std::vector<TypeRef> ResolveInterfaceMethodParamTypes(const FuncDecl &method,
                                                                                const TypeRef &selfType) = 0;
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
    [[nodiscard]] virtual std::optional<std::uint64_t> EvalArrayLength(const Expr &expression) const = 0;
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
    void ResolveDeclSignature(const Decl &declaration);
    void ResolveDeclSignatureInScope(const Decl &declaration, Scope &scope);
    void ResolveModuleSignatures(const Module &module);
    void ResolveModuleSignaturesInScope(const Module &module, Scope &scope);
    void CheckModule(const Module &module);
    void CheckModuleInScope(const Module &module, Scope &scope);
    /// Queues the destructor of every droppable type recorded, so its body is analyzed at each type it will
    /// be built for. Nothing in the source calls a destructor -- the generated glue does -- so without this a body
    /// reachable no other way is lowered having never been analyzed, and anything it deferred until its type
    /// arguments were known is never answered.
    virtual void QueueDropMethodInstantiations() = 0;

    virtual void ValidatePendingGenericInstantiations() = 0;
    virtual void RecordResolvedTypeLayouts() = 0;
    void RecordResolvedTypeProperties();
    void SynthesizeResolvedDropGlue();
    [[nodiscard]] std::vector<DropGlueStep> BuildDropGlueSteps(const TypeRef &type,
                                                               std::unordered_set<std::string> &activeTypes);
    [[nodiscard]] bool TypeHasDirectDestructor(const std::string &baseName) const;
    [[nodiscard]] static std::string DropGlueSymbol(const TypeRef &type);
    virtual void BuildFinalSymbolIdentities() = 0;

    [[nodiscard]] TypeRef CheckUnary(TokenKind op, const TypeRef &operand, SourceLocation location);
    [[nodiscard]] TypeRef CheckBinary(TokenKind op, const TypeRef &left, const TypeRef &right,
                                      const Expr &leftExpression, const Expr &rightExpression, SourceLocation location,
                                      const BinaryExpr *binaryExpression = nullptr);
    [[nodiscard]] bool BuildVariantEqualityPlan(const TypeRef &type, SourceLocation useLocation,
                                                std::unordered_set<std::string> &activeTypes);
    [[nodiscard]] bool BuildVariantEqualityPayload(VariantEqualityPayload &payload, SourceLocation useLocation,
                                                   std::string_view variantTypeName, std::string_view declarationName,
                                                   std::string_view caseName,
                                                   std::unordered_set<std::string> &activeTypes);
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
