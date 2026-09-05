#pragma once

#include "Semantic/Analysis/MovePlace.h"
#include "Semantic/Analysis/MoveStateTracker.h"
#include "Semantic/Analysis/ProgramIndex.h"
#include "Semantic/SemanticAnalyzer.h"

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
/// Borrowed inputs for one analysis. All references outlive the context.
struct AnalysisInputs {
    std::vector<const Module *> &modules;
    std::vector<DepPackage> &dependencies;
    const std::string &packageName;
    std::vector<SemanticDiagnostic> &diagnostics;
    std::vector<SemanticSymbol> &symbols;
    const CompileTimeContext &context;
};

class AnalysisContext final {
public:
    AnalysisContext(AnalysisInputs inputs, SemanticFacts &output);
    ~AnalysisContext();

    AnalysisContext(const AnalysisContext &) = delete;
    AnalysisContext &operator=(const AnalysisContext &) = delete;

    void Run();
    [[nodiscard]] std::unordered_map<const Decl *, bool> EffectiveVisibilities() const;

private:
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
        std::size_t typeParamCount;
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
    void ValidateIndexOperator(const FuncDecl &method, const TypeRef &extendedType);
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
        ScopedTypeParameterBounds(AnalysisContext &owner, const std::vector<TypeParameter> *parameters,
                                  bool replaceEnclosing = true);
        ~ScopedTypeParameterBounds();

        ScopedTypeParameterBounds(const ScopedTypeParameterBounds &) = delete;
        ScopedTypeParameterBounds &operator=(const ScopedTypeParameterBounds &) = delete;

    private:
        AnalysisContext &context;
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
        std::size_t operationIndex;
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
    [[nodiscard]] TypeRef CheckCoalesceExpression(const BinaryExpr &expression);
    [[nodiscard]] bool ValidateCoalescingPayload(const TypeRef &payload, SourceLocation location);
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
    /// Whether this expression is an index that resolved to a declared `[]` or `[]=`. Such an expression is a call, so
    /// it is neither an assignable place nor a projection any borrow or move can reach through, and every place walk
    /// stops at it.
    [[nodiscard]] bool IsIndexOperatorCall(const Expr &expression) const;

    /// The nearest index expression this place path passes through that resolved to a declared `[]`, or null when the
    /// path reaches its root without one. `v[i].field` has no place to write to for the same reason `v[i]` does not.
    [[nodiscard]] const IndexExpr *IndexOperatorInPlace(const Expr &place) const;

    /// Resolve the `[]=` an index expression is being written through, returning the type the assigned value must
    /// have. Returns nothing when the receiver declares no `[]=` at all, which leaves the expression to be read
    /// through `[]` and rejected as a target like any other non-place.
    [[nodiscard]] std::optional<TypeRef> ResolveIndexAssignment(const IndexExpr &index, const TypeRef &objectType,
                                                                const TypeRef &indexType);

    /// Check the assigned value of an indexed assignment against the setter's value parameter and consume it, the way
    /// the call this assignment becomes would.
    void FinishIndexedAssignment(const AssignExpr &assignment, const TypeRef &valueParameterType,
                                 const TypeRef &valueType);

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
    std::unordered_map<const TypeExpr *, const Decl *> &intrinsicTypeBindings;
    std::unordered_map<const FieldExpr *, ResolvedIntrinsicMember> &intrinsicMemberBindings;
    std::unordered_map<const Expr *, const ConstDecl *> &associatedConstants;
    std::unordered_map<const ConstDecl *, EvaluatedAssociatedConstant> &evaluatedAssociatedConstants;
    std::unordered_set<const ConstDecl *> checkingAssociatedConstants;
    [[nodiscard]] const ConstDecl *LookupAssociatedConstant(const Symbol &type, const std::string &name) const;
    [[nodiscard]] TypeRef CheckAssociatedConstant(const ConstDecl &declaration);
    void CheckIntrinsicType(const Decl &declaration);
    std::unordered_map<std::string, std::unordered_set<const Decl *>> explicitTypeImports;
    [[nodiscard]] bool IsVisibleTypeSymbol(const Symbol &symbol) const;
    [[nodiscard]] bool RecordIntrinsicMember(const TypeRef &type, const FieldExpr &expression) const;
    [[nodiscard]] const Decl *IntrinsicTypeBinding(const Symbol &symbol) const;
    [[nodiscard]] const Decl *VisibleIntrinsicType(const TypeRef &type, SourceLocation location) const;
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
    std::unordered_map<std::string, TypeProperties> &typeProperties;
    std::unordered_map<std::string, DropGluePlan> &dropGluePlans;
    /// Types an instantiation named for the first time, which nothing written concretely spells. A generic body
    /// composing `Option<V>` mentions no such type until `V` is decided, so the walks over what the program wrote
    /// never reach it and it would have neither a layout nor a destructor.
    std::vector<TypeRef> instantiatedTypes;
    /// One entry per (bound, concrete type) pair a use site proved, so lowering can call the satisfying method directly
    /// in each instantiation instead of dispatching through the interface.
    std::unordered_map<std::string, ResolvedConstraintWitness> &constraintWitnesses;
    /// One entry per accepted `expr?`, so lowering builds its early return without recognizing Result or Option again.
    std::unordered_map<const TryExpr *, ResolvedPropagation> &propagations;
    /// One entry per accepted `??`, so lowering does not repeat structural Option recognition.
    std::unordered_map<const BinaryExpr *, ResolvedCoalescing> &coalescings;
    /// One entry per accepted index expression that resolved to a declared `[]`, so lowering calls the operator
    /// without resolving it again and analysis knows the expression is a call rather than a place.
    std::unordered_map<const IndexExpr *, ResolvedIndexOperator> &indexOperators;
    /// One entry per accepted index expression assigned through a declared `[]=`, for the same two reasons.
    std::unordered_map<const IndexExpr *, ResolvedIndexAssignment> &indexAssignments;
    /// The index expression currently being checked as the direct target of a plain assignment, if any. The setter is
    /// resolved only for that exact node, so `outer[inner[i]] = x` reads its subscript and writes only the outer index.
    const IndexExpr *indexAssignmentTarget = nullptr;
    /// One entry per accepted `for`, so lowering drives the subject the way analysis decided it is driven.
    std::unordered_map<const ForStmt *, ResolvedIteration> &iterations;
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

    struct DeferredCoalescingCheck {
        TypeRef payload;
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
    std::unordered_map<const FuncDecl *, std::vector<DeferredCoalescingCheck>> deferredCoalescingChecks;
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
    TypeRef ResolveType(const TypeExpr &expression);
    /// The target layout of a written type, or nothing when it has none: recursive, unsized, or not yet validated.
    [[nodiscard]] std::optional<ResolvedTypeLayout> LayoutOfTypeExpression(const TypeExpr &expression);
    TypeRef ResolveTypeWithSubstitution(const TypeExpr &expression,
                                        const std::unordered_map<std::string, TypeRef> &substitutions);
    TypeRef CheckExpr(const Expr &expression);
    void EmitDiagnosticIntrinsic(const std::string &intrinsicName, const CallExpr &call);
    [[nodiscard]] const FuncDecl *LookupFunctionOverload(const Symbol &symbol,
                                                         const std::vector<TypeRef> &argumentTypes,
                                                         const std::vector<TypeExprPtr> &typeArguments);
    void QueueGenericInstantiation(const FuncDecl &declaration,
                                   const std::unordered_map<std::string, TypeRef> &substitutions);
    [[nodiscard]] const FuncDecl *LookupMethod(const TypeRef &receiverType, const std::string &methodName,
                                               const std::vector<TypeRef> &argumentTypes = {},
                                               bool requireAccessible = true);
    [[nodiscard]] std::unordered_map<std::string, TypeRef> MethodTypeSubstitutions(const TypeRef &receiverType) const;
    [[nodiscard]] const std::vector<TypeParameter> *AggregateTypeParams(const std::string &name) const;
    [[nodiscard]] TypeRef InstantiateAssociatedReceiver(TypeRef receiverType,
                                                        const std::vector<TypeExprPtr> &typeArguments);
    [[nodiscard]] TypeRef AssociatedFunctionType(const TypeRef &receiverType, const FuncDecl &method);
    [[nodiscard]] TypeRef ResolveMethodReturnType(const TypeRef &receiverType, const FuncDecl &method);
    [[nodiscard]] std::vector<TypeRef> ResolveMethodParamTypes(const TypeRef &receiverType, const FuncDecl &method);
    [[nodiscard]] const FuncDecl *LookupInterfaceMethod(const TypeRef &receiverType,
                                                        const std::string &methodName) const;

    /// Whether any written type of `method` names `Self`, which is what makes it callable only where the implementing
    /// type is known.
    [[nodiscard]] static bool InterfaceMethodMentionsSelf(const FuncDecl &method);
    /// An interface method's written types, with `Self` standing for `selfType`.
    ///
    /// An interface cannot name the type implementing it any other way -- there are no generic interfaces -- so a
    /// binary operation like equality or comparison has no way to say that its other operand is the same type as its
    /// receiver. `Self` is that name, and it is substituted here, where the receiver is known.
    [[nodiscard]] TypeRef ResolveInterfaceMethodReturnType(const FuncDecl &method, const TypeRef &selfType);
    [[nodiscard]] std::vector<TypeRef> ResolveInterfaceMethodParamTypes(const FuncDecl &method,
                                                                        const TypeRef &selfType);
    [[nodiscard]] TypeRef EnumType(const EnumDecl &declaration, const std::vector<TypeRef> &typeArguments = {});
    [[nodiscard]] TypeRef LiteralType(const Token &token) const;
    void ValidateCastConstant(const CastExpr &expression, const TypeRef &operandType, const TypeRef &targetType) const;
    [[nodiscard]] const FuncDecl *LookupOperatorMethod(const TypeRef &receiverType, const std::string &operatorName,
                                                       const std::vector<TypeRef> &argumentTypes);
    [[nodiscard]] std::vector<TypeRef> ResolveOperatorParameterTypes(const TypeRef &receiverType,
                                                                     const FuncDecl &method);
    [[nodiscard]] TypeRef ResolveOperatorReturnType(const TypeRef &receiverType, const FuncDecl &method);
    void CheckDecl(const Decl &declaration);
    [[nodiscard]] std::optional<std::uint64_t> EvalArrayLength(const Expr &expression) const;
    void ValidateArrayType(const TypeExpr &type, bool allowFlexibleTail = false);
    [[nodiscard]] bool CanAssignExprTo(const Expr &expression, const TypeRef &expressionType,
                                       const TypeRef &targetType);
    [[nodiscard]] std::string AssignmentErrorMessage(const Expr &expression, const TypeRef &targetType,
                                                     std::string fallback);
    [[nodiscard]] std::string BaseTypeName(const std::string &name) const;

    [[nodiscard]] TypeRef ParseTypeRefFromString(std::string typeName) const;
    [[nodiscard]] std::vector<TypeRef> ParseTypeArgsFromTypeName(const std::string &typeName) const;
    void ApplyModuleImports(const Module &module);
    void ApplyModuleImportsInScope(const Module &module, Scope &scope);
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
    void QueueDropMethodInstantiations();

    void ValidatePendingGenericInstantiations();
    void RecordResolvedTypeLayouts();
    void RecordResolvedTypeProperties();
    void SynthesizeResolvedDropGlue();
    [[nodiscard]] std::vector<DropGlueStep> BuildDropGlueSteps(const TypeRef &type,
                                                               std::unordered_set<std::string> &activeTypes);
    [[nodiscard]] bool TypeHasDirectDestructor(const std::string &baseName) const;
    [[nodiscard]] static std::string DropGlueSymbol(const TypeRef &type);
    void BuildFinalSymbolIdentities();

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

    struct DeferredGenericCall {
        const FuncDecl *callee;
        std::unordered_map<std::string, TypeRef> substitutions;
    };

    struct PendingGenericInstantiation {
        const FuncDecl *decl;
        std::unordered_map<std::string, TypeRef> substitutions;
    };

    /// An enum a generic body composes out of its own parameters -- `Option<V>` written inside a container -- noted
    /// against the function that writes it, so it can be composed again where the parameters are finally types.
    struct DeferredEnumInstantiation {
        const EnumDecl *decl;
        std::vector<TypeRef> typeArgs;
    };

    std::unordered_map<const FuncDecl *, std::vector<DeferredGenericCall>> deferredGenericCalls;
    std::unordered_map<const FuncDecl *, std::vector<DeferredEnumInstantiation>> deferredEnumInstantiations;
    std::vector<PendingGenericInstantiation> pendingGenericInstantiations;
    std::unordered_map<const FuncDecl *, std::unordered_set<std::string>> validatedGenericInstantiations;
    std::unordered_set<const FuncDecl *> reportedRunawayInstantiations;
    std::unordered_set<std::string> activeLayoutTypes;

    struct ImportScope {
        const std::unordered_map<std::string, Symbol> *table = nullptr;
        std::string displayName;
        std::string ownerPackage;
        std::string modulePath;
    };

    /// A generic that instantiates itself at a strictly larger type argument -- `Grow<T>` calling `Grow<*T>` -- never
    /// closes its set of instantiations. Deduplication does not help, because every instantiation is genuinely new.
    /// The compiler used to follow it until the recursive walks over an ever-deeper type exhausted the stack and it
    /// died with no diagnostic at all.
    ///
    /// Bounding the type argument's size catches it: any infinite set of instantiations must produce ever-larger type
    /// arguments, since there are only finitely many small types. Size rather than nesting depth, so a set that grows
    /// in breadth is caught too. The limit is far above anything written by hand -- `Slice<char8>` is two nodes.
    static constexpr std::size_t kMaxInstantiationTypeNodes = 128;

    void ApplyDeclImports(const Decl &decl);

    Scope &ModuleScopeFor(const std::string &name, Scope &parent);

    // Type resolution
    std::string GenericTypeName(const NamedTypeExpr &type);

    std::vector<std::string> ImplTypeParams(const ImplDecl &decl) const;

    /// The type a string literal has.
    ///
    /// A literal is text: a string of the encoding the prefix names, whose length is counted in that encoding's
    /// code units. The bare form is UTF-8, which is what an unprefixed literal in a UTF-8 source file already is.
    static TypeRef StringLiteralType(const Token &tok);

    // The text of a string-literal token, with the surrounding quotes and any
    // encoding prefix removed and the common escapes decoded, for use as a
    // human-readable diagnostic message.
    static std::string DecodeStringMessage(const std::string &text);

    static TypeRef CharLiteralType(const Token &tok);

    static std::string NumericLiteralSuffix(const std::string_view text);

    /// The type a suffix names, built from the width and signedness the suffix table records rather than from a
    /// second list of them here. A literal with no suffix takes the default: `int`, or `float64` when it has a point.
    static TypeRef SuffixedLiteralType(const Token &tok);

    static std::optional<std::uint64_t> ParseUnsuffixedIntegerLiteral(const Token &tok);

    static std::optional<std::uint64_t> ParseIntegerLiteralValue(const Token &tok);

    /// The width and signedness `type` is range-checked at, with the target's pointer width filled in for `int` and
    /// `uint`.
    ///
    /// @return nullopt when `type` is not an integer
    std::optional<std::pair<std::uint32_t, bool>> IntegerRange(const TypeRef &type) const;

    /// Constant-expression folding still evaluates in a machine word, so these two answer only for the widths that
    /// fit one. A literal is checked by `UnsuffixedIntegerLiteralFits` instead, which is exact at every width.
    std::optional<std::uint64_t> UnsignedIntegerMax(const TypeRef &type) const;

    std::optional<std::pair<std::int64_t, std::int64_t>> SignedIntegerRange(const TypeRef &type) const;

    /// Whether an unsuffixed literal is one `target` holds.
    ///
    /// The magnitude is decoded at the widest width there is and range-checked afterwards, so a literal too large for
    /// its target is told apart from one that is not a literal at all, and both answers are exact however wide the
    /// target is.
    bool UnsuffixedIntegerLiteralFits(const Expr &expr, const TypeRef &target) const;

    static bool IsNullLiteral(const Expr &expr);

    static bool IsUnsuffixedIntegerLiteral(const Expr &expr);

    bool IsIntegerLiteralOutOfRangeFor(const Expr &expr, const TypeRef &targetType) const;

    // Explains why the address of an immutable place cannot initialize a
    // writable pointer. The types alone do not point at the required binding
    // change, so name it when possible.
    std::string ImmutableAddressOfHint(const Expr &expr, const TypeRef &targetType);

    // Folds a compile-time-constant integer expression (unsuffixed integer
    // literals combined with the integer operators) to its int64 value,
    // using the same two's-complement wrapping the generated code produces
    // at run time, so the folded value always matches what the program
    // computes. Returns nullopt when the expression is not such a constant,
    // so callers fall back to ordinary type checking. Division/modulo by
    // zero and the INT64_MIN / -1 overflow are left unfolded and keep their
    // runtime behavior; '**' is not folded (it lowers to a runtime helper
    // call).
    static std::optional<std::int64_t> EvalConstInt(const Expr &expr);

    bool ConstantFitsTarget(std::int64_t value, const TypeRef &target) const;

    static std::optional<std::uint64_t> EvalConstCharCastValue(const Expr &expr);

    Symbol *FindUniquePackageType(const std::string &name) const;

    TypeRef ResolveTypeImpl(const TypeExpr &expr);

    /// `Self` bound to the type the interface is being read for.
    static std::unordered_map<std::string, TypeRef> SelfSubstitution(const TypeRef &selfType);

    TypeRef FunctionType(const FuncDecl &decl);

    static std::optional<std::uint64_t> CheckedAlignUp(const std::uint64_t value, const std::uint64_t alignment);

    std::optional<ResolvedTypeLayout>
    LayoutOfTypeRef(const TypeRef &inputType, const std::unordered_map<std::string, TypeRef> &substitutions = {});

    std::optional<ResolvedTypeLayout>
    LayoutOfFields(const std::vector<TypeRef> &fields,
                   const std::unordered_map<std::string, TypeRef> &substitutions = {});

    std::optional<ResolvedTypeLayout> LayoutOfEnum(const EnumDecl &decl,
                                                   const std::unordered_map<std::string, TypeRef> &substitutions = {});

    TypeRef EnumBaseType(const EnumDecl &decl);

    std::optional<ResolvedTypeLayout>
    LayoutOfStruct(const StructDecl &decl, const std::unordered_map<std::string, TypeRef> &substitutions = {});

    std::optional<ResolvedTypeLayout> LayoutOfUnion(const UnionDecl &decl,
                                                    const std::unordered_map<std::string, TypeRef> &substitutions = {});

    std::optional<ResolvedTypeLayout>
    LayoutOfTypeExpr(const TypeExpr &expr, const std::unordered_map<std::string, TypeRef> &substitutions = {});

    void CheckFuncDecl(const FuncDecl &d, bool isMethod = false);

    // An `asm func` body is written for one architecture, and nothing in the
    // syntax says which: the mnemonics do. Report the first instruction that
    // names an instruction of the architecture the compilation is not for,
    // which is the whole body's mistake rather than that one line's.
    //
    // This runs after `when` folding, so a body a build never reaches is never
    // reported, and it stops at the first offender so a body written for the
    // wrong machine costs one diagnostic rather than one per line. A body that
    // reaches this far is one the build needs and no assembler can encode, so
    // it is an error: `when #target.arch` is how a function written twice
    // reaches both machines, and every first-party body uses it.
    void CheckAsmBodyArchitecture(const FuncDecl &d) const;

    void CheckStructDecl(const StructDecl &d);

    void CheckEnumDecl(const EnumDecl &d);

    void CheckUnionDecl(const UnionDecl &d);

    void CheckInterfaceDecl(const InterfaceDecl &d);

    void CheckImplDecl(const ImplDecl &d);

    void CheckModuleDecl(const ModuleDecl &d);

    static bool IsSliceTypeRef(const TypeRef &type);

    // An element of a constant array must reduce to a literal, since the array
    // is laid out in read-only data rather than evaluated at each use.
    bool IsConstArrayElement(const Expr &e) const;

    void CheckConstDecl(const ConstDecl &d);

    static std::string JoinPathSegments(const std::vector<std::string> &path, std::size_t first,
                                        std::size_t lastExclusive);

    static std::string ModulePathForImport(const UseDecl &d);

    static std::string LogicalModulePathForImport(const UseDecl &d);

    static std::string ImportScopeDisplayName(const std::string &pkgName, const std::string &modulePath);

    ImportScope ResolveImportScope(const UseDecl &d, const std::string &pkgName, const std::string &modulePath);

    [[nodiscard]] const Symbol *InaccessibleModule(const std::string &package, const std::string &path) const;

    [[nodiscard]] std::optional<Symbol> AccessibleImport(const Symbol &symbol) const;

    void PromoteFromPackage(const UseDecl &d, const std::string &pkgName, const std::string &name);

    void DefineImportedSymbol(const Symbol &sym);

    void ImportSignatureDependencies(const Symbol &sym, const std::unordered_map<std::string, Symbol> &sourceTable);

    void CheckUseDecl(const UseDecl &d);

    static std::string MangleTypeName(const TypeRef &type);

    TypeRef SubstituteIdentityType(TypeRef type, const std::unordered_map<std::string, TypeRef> &substitutions) const;

    TypeRef IdentityParameterType(const Param &parameter,
                                  const std::unordered_map<std::string, TypeRef> &substitutions = {}) const;

    std::string MangleFunctionWithParams(const FuncDecl &declaration) const;

    bool FunctionIsOverloadedInModule(const FuncDecl &declaration) const;

    bool MethodIsOverloadedForIdentity(const std::string &typeName, const std::string &methodName) const;

    std::string MethodLinkerName(const FuncDecl &method, const TypeRef &receiverType,
                                 const std::unordered_map<std::string, TypeRef> &substitutions) const;

    void RecordMethodIdentityRecipe(ResolvedCallableBinding &binding, const FuncDecl &method) const;

    TypeRef CheckExprImpl(const Expr &expr);

    /// Counts a type's structural nodes, stopping once the limit is passed so a runaway type costs no more to
    /// measure than a well-behaved one.
    static std::size_t TypeNodeCount(const TypeRef &type, const std::size_t limit);
};
} // namespace Rux::SemanticDetail
