#pragma once

#include "Numeric/WideInteger.h"
#include "Semantic/Analysis/MovePlace.h"
#include "Semantic/Analysis/MoveStateTracker.h"
#include "Semantic/Analysis/ProgramIndex.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Types/PrimitiveCatalog.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
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
    const PackageImportBindings &imports;
    /// Modules whose non-generic function bodies are not checked, because the build reuses their compiled code.
    /// Everything else in them is: declarations, signatures and default arguments, and every generic body, which
    /// other modules instantiate.
    const std::unordered_set<const Module *> *declarationOnlyModules = nullptr;
};

/// Whether a pattern binds anything a value can be taken out into: a looked-at value is not taken apart.
[[nodiscard]] bool PatternBindsValue(const Pattern &pattern);

/// The exact hexadecimal literal of the intrinsic associated constant `name` of the float type `kind`: `Infinity`,
/// a quiet `NaN`, the largest finite `Max` and its negation `Lowest`, the smallest normal `MinPositive`, and `Epsilon`,
/// the gap between one and the next value. The compiler supplies these for every width, so a package never has to
/// spell a decimal for a width whose limits no host type holds.
///
/// @return nullopt when `kind` is not a float or `name` is not a constant the compiler supplies
[[nodiscard]] std::optional<std::string> IntrinsicFloatConstantLiteral(TypeRef::Kind kind, std::string_view name);

/// Whether `type` crosses a C ABI boundary the way C passes it. Only float32 and float64 of the floats do: the other
/// widths are passed by the compiler's own convention, which matches no C type such as `_Float16`, `long double` or
/// `__float128`. Every other type is left to the checks that already hold for it.
[[nodiscard]] bool HasCAbiMapping(const TypeRef &type) noexcept;

/// What an analysis builds up and keeps from one function to the next, apart from the facts it records and the program
/// index. A context that checks some modules' bodies starts as a copy of one that ran the phases before them, and
/// everything declared here is copied with it; state that lives only while one function is checked belongs in
/// `AnalysisContext` instead.
struct AnalysisState {
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

    struct DeferredAtomicCheck {
        TypeRef value;
        std::string operation;
        SourceLocation location;
    };

    struct DeferredScalarRead {
        TypeRef reference;
        SourceLocation location;
    };

    struct DeferredOutcomeCheck {
        TypeRef payload;
        SourceLocation location;
        bool propagation = false;
    };

    /// A value whose type mentions a type parameter, so whether handing it over consumes it is not yet knowable.
    /// Answered once per instantiation, when the parameter stands for something with a mobility.
    struct DeferredConsumption {
        const Expr *expression;
        ValueConsumptionKind kind;
        TypeRef type;
        SourceLocation location;
    };

    /// A native match whose subject or typed annotations mention a type parameter, checked again per instantiation.
    struct DeferredMatchCheck {
        std::vector<const Pattern *> patterns;
        TypeRef subject;
    };

    /// An `is` test whose subject or tested type mentions a type parameter, checked again per instantiation.
    struct DeferredMembershipCheck {
        TypeRef subject;
        TypeRef tested;
        SourceLocation location;
    };

    /// Where a call supplied a generic function's type arguments.
    struct InstantiationSite {
        std::string file;
        SourceLocation location;
    };

    struct DeferredGenericCall {
        const FuncDecl *callee;
        std::unordered_map<std::string, TypeRef> substitutions;
        std::optional<InstantiationSite> site;
    };

    struct PendingGenericInstantiation {
        const FuncDecl *decl;
        std::unordered_map<std::string, TypeRef> substitutions;
        std::optional<InstantiationSite> site;
    };

    /// An enum a generic body composes out of its own parameters -- `Option<V>` written inside a container -- noted
    /// against the function that writes it, so it can be composed again where the parameters are finally types.
    struct DeferredEnumInstantiation {
        const EnumDecl *decl;
        std::vector<TypeRef> typeArgs;
    };

    std::unordered_map<std::string, std::unordered_set<const Decl *>> explicitTypeImports;
    /// Per source file: for each primitive (by canonical kind), the packages whose extensions of it the file imported,
    /// with `import Pkg::int8;` or a glob reaching a module that extends it.
    std::unordered_map<std::string, std::unordered_map<TypeRef::Kind, std::unordered_set<std::string>>>
        primitiveExtensionImports;
    /// Native constructors whose operand has already been handed over, so checking one twice never moves twice.
    std::unordered_set<const NativeConstructExpr *> consumedConstructorOperands;
    /// Whether each native match, keyed by its first arm's pattern, covers its subject, so control flow can ask after
    /// the arms are checked.
    std::unordered_map<const Pattern *, bool> nativeMatchExhaustive;
    /// What each tuple match, keyed by its first arm's pattern, leaves uncovered, or nothing when its arms cover it. An
    /// enum or variant match whose cases are covered only by several arms together, such as '.Light(true)' and
    /// '.Light(false)', is recorded here as covered.
    std::unordered_map<const Pattern *, std::optional<std::string>> tupleMatchMissing;
    /// Types an instantiation named for the first time, which nothing written concretely spells. A generic body
    /// composing `Option<V>` mentions no such type until `V` is decided, so the walks over what the program wrote
    /// never reach it and it would have neither a layout nor a destructor.
    std::vector<TypeRef> instantiatedTypes;
    /// The types a generic function's body was checked with that still name its type parameters, by spelling. A
    /// body is checked once, generically, so each instantiation substitutes these to learn what it holds by value:
    /// a type only an instantiation spells, such as a `Block<Tracked>` built inside `Make<T>`, needs drop glue too.
    std::unordered_map<const FuncDecl *, std::unordered_map<std::string, TypeRef>> genericBodyTypes;
    /// The type a destination expects of an expression it is about to check, recorded before the check for the
    /// expressions whose parts take their type from it, such as the elements of an array literal.
    std::unordered_map<const Expr *, TypeRef> expectedExpressionTypes;
    std::string currentFile;
    std::string currentPackage;
    std::unordered_set<const TypeExpr *> reportedPrivateApiTypes;
    std::unordered_set<const Decl *> reportedPrivateApiDeclarations;
    std::unordered_map<const ConstDecl *, TypeRef> checkedConstantTypes;
    std::unordered_map<const GlobalDecl *, TypeRef> checkedGlobalTypes;
    /// Constants declared by a statement inside a function body rather than at module level.
    std::unordered_set<const ConstDecl *> localConstants;
    std::unordered_map<const FuncDecl *, std::vector<DeferredUnaryCheck>> deferredUnaryChecks;
    std::unordered_map<const FuncDecl *, std::vector<DeferredBinaryCheck>> deferredBinaryChecks;
    std::unordered_map<const FuncDecl *, std::vector<DeferredCastCheck>> deferredCastChecks;
    std::unordered_map<const FuncDecl *, std::vector<DeferredAtomicCheck>> deferredAtomicChecks;
    std::unordered_map<const FuncDecl *, std::vector<DeferredScalarRead>> deferredScalarReads;
    std::unordered_map<const FuncDecl *, std::vector<DeferredOutcomeCheck>> deferredOutcomeChecks;
    std::unordered_map<const FuncDecl *, std::vector<DeferredConsumption>> deferredConsumptions;
    std::unordered_map<const FuncDecl *, std::vector<DeferredMatchCheck>> deferredMatchChecks;
    /// Native types a generic body composes out of its own parameters. Nothing written concretely spells them, so each
    /// instantiation composes them again where the parameters are types, which gives them layouts and drop glue.
    std::unordered_map<const FuncDecl *, std::vector<TypeRef>> deferredNativeTypes;
    std::unordered_map<const FuncDecl *, std::vector<DeferredMembershipCheck>> deferredMembershipChecks;
    std::unordered_set<const TypeExpr *> reportedGenericArity;
    /// The suffixed integer literals sitting directly under a unary minus, registered before the operand is checked
    /// so the literal's own check knows its magnitude is a negative one. `-128i8` is in range while `128i8` is not.
    std::unordered_set<const LiteralExpr *> negatedIntegerLiterals;
    /// A type expression is resolved once per place it is read -- a parameter's type is resolved again for its symbol,
    /// its signature, and each overload attempt -- so an unsatisfied bound is reported for the spelling, not per read.
    std::unordered_set<const TypeExpr *> reportedTypeArgumentConstraints;
    /// The overloads the last `LookupFunctionOverload` found equally good, when it selected none for that reason.
    std::vector<const FuncDecl *> ambiguousOverloads;
    std::unordered_set<const UseDecl *> refreshedImports;
    /// Declarations `PrecheckDecl` checked, which `CheckDecl` then leaves alone.
    std::unordered_set<const Decl *> precheckedDeclarations;
    std::unordered_map<const FuncDecl *, std::vector<DeferredGenericCall>> deferredGenericCalls;
    std::unordered_map<const FuncDecl *, std::vector<DeferredEnumInstantiation>> deferredEnumInstantiations;
    std::vector<PendingGenericInstantiation> pendingGenericInstantiations;
    /// Where each phase left the instantiation queue, so absorbed workers' entries go where a single context would
    /// have queued them.
    std::size_t instantiationsAfterDependencyBegin = 0;
    std::size_t instantiationsAfterDependencyBodies = 0;
    std::size_t instantiationsAfterUserBegin = 0;
    std::unordered_map<const FuncDecl *, std::unordered_set<std::string>> validatedGenericInstantiations;
    std::unordered_set<const FuncDecl *> reportedRunawayInstantiations;
    /// The module-level imports bound before signatures were resolved. Checking their module's declarations binds them
    /// again, and the errors that second binding repeats are dropped.
    std::unordered_set<const UseDecl *> appliedImports;
    /// The uses of a `#Warn` or `#Error` declaration already reported, by declaration, file and offset: a type is
    /// resolved and a generic body checked more than once, and each use is reported once.
    std::set<std::tuple<const Decl *, std::string, std::uint32_t>> reportedDeclarationUses;
    /// The declarations being checked, innermost last: a use of one of them inside it is not reported, and a use found
    /// outside any declaration or function is one an earlier phase found, which checking finds again.
    std::vector<const Decl *> declarationsBeingChecked;
    /// The declaration whose signature or field type is being resolved, for as long as it is.
    const Decl *resolvingTypeOwner = nullptr;
    /// Each type identity qualified by its package because another package declares the same name, longest first,
    /// with the name a diagnostic shows for it: the name it was declared with, under its package's display name. A
    /// manifest path is no name a reader wrote.
    std::vector<std::pair<std::string, std::string>> diagnosticTypeNames;
};

class AnalysisContext final : private AnalysisState {
public:
    AnalysisContext(AnalysisInputs inputs, SemanticFacts &output);
    /// A context that starts where `base` is, between two functions: a copy of its state and of its program index with
    /// scopes of its own. `output` must already hold a copy of the facts `base` records into. `base` is only read.
    AnalysisContext(const AnalysisContext &base, AnalysisInputs inputs, SemanticFacts &output);
    ~AnalysisContext();

    AnalysisContext(const AnalysisContext &) = delete;
    AnalysisContext &operator=(const AnalysisContext &) = delete;

    /// Analyze every module on the calling thread: the phases below, in order, over every module.
    void Run();

    // The phases of Run, so modules' bodies can be checked by several contexts at once (see SemanticAnalyzer). Every
    // context runs all of the Begin phases; each checks the bodies of its own range of modules; one context that
    // checked no body then merges the others' records in module order and finishes.

    /// Index declarations, then apply the dependencies' imports, resolve their signatures and precheck their
    /// declarations.
    void BeginDependencies();
    /// Check the bodies of dependency modules [begin, end), counting every dependency's modules in order.
    void CheckDependencyModules(std::size_t begin, std::size_t end);
    /// Apply the package's own imports, resolve its signatures and precheck its declarations.
    void BeginUserModules();
    /// Check the bodies of the package's own modules [begin, end).
    void CheckUserModules(std::size_t begin, std::size_t end);
    /// Validate generic instantiations and record layouts, destructors and symbol identities.
    void Finish();
    [[nodiscard]] std::size_t DependencyModuleCount() const;

    /// What a context's bodies recorded beyond the shared Begin phases.
    struct BodyRecords;
    /// Take this context's own records out of it, judged against `shared`, a context that ran only the Begin phases
    /// and has not changed since. Runs on this context's thread; `shared` is only read. Records may point into this
    /// context's scopes, so it must outlive the context they are merged into.
    [[nodiscard]] BodyRecords ExtractBodyRecords(const AnalysisContext &shared);
    /// Merge workers' records into this context, which ran only the Begin phases: `records` in the order of the
    /// workers' module ranges, each kind as one context checking every module in order would have left it. Independent
    /// maps merge on `pool`'s threads.
    void AbsorbBodyRecords(std::vector<BodyRecords> &records, Parallel::WorkerPool *pool);

    /// Threads Finish may spread independent work over; null runs it all on the calling thread.
    void SetWorkerPool(Parallel::WorkerPool *inputPool) noexcept {
        workers = inputPool;
    }

    [[nodiscard]] std::unordered_map<const Decl *, bool> EffectiveVisibilities() const;
    /// The effectively public declarations the package being compiled owns itself.
    [[nodiscard]] std::unordered_set<const Decl *> ExportedDeclarations() const;

private:
    AnalysisContext(AnalysisInputs inputs, SemanticFacts &output, const AnalysisContext *base);

    void EmitError(SourceLocation location, std::string message, std::vector<std::string> notes = {},
                   std::optional<std::string> help = {}) const;
    void EmitWarning(SourceLocation location, std::string message) const;
    void EmitUndefinedName(SourceLocation location, const std::string &name) const;
    [[nodiscard]] bool IsAccessible(const Symbol &symbol) const;
    [[nodiscard]] bool IsAccessible(const Decl &declaration) const;
    [[nodiscard]] bool IsMemberAccessible(const Decl &owner, bool memberIsPublic) const;
    /// How a diagnostic names the package with identity `package`: as its manifest spells it when it is a dependency.
    [[nodiscard]] std::string PackageDisplayName(const std::string &package) const;
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
    /// Whether a value of `type` needs destruction, or may once its type parameters are substituted.
    [[nodiscard]] bool MayNeedDestruction(const TypeRef &type);
    [[nodiscard]] const FuncDecl *BeginTrackedFunction(const FuncDecl &function);
    void EndTrackedFunction(const FuncDecl *previousFunction);
    /// Reports reading `symbol`, or with `part` only that part of it, while it may hold no value there.
    void CheckTrackedRead(const Symbol &symbol, SourceLocation location,
                          std::optional<std::string_view> part = std::nullopt);
    /// Whether `parts`, written into a value of `type` below `prefix`, make up all of it: every field of a structure
    /// and every element of a tuple, at any depth, or one member of a union.
    [[nodiscard]] bool PartsMakeWhole(const TypeRef &type, std::span<const std::string> parts,
                                      const std::string &prefix);
    /// The fixed arrays with nothing to destroy inside a value of `type`, by path below `prefix`. Like such an array
    /// local, each holds its storage from the declaration, so a loop can fill it.
    void FillableArrayParts(const TypeRef &type, const std::string &prefix, std::vector<std::string> &parts);
    [[nodiscard]] TypeRef ReadTrackedSymbol(const Symbol &symbol, SourceLocation location);
    void RecordCheckedExpression(const Expr &expression, const TypeRef &type);
    void ReportUntypedExpression(const Expr &expression) const;
    void MarkTrackedAssignment(const Expr &target, SourceLocation location);
    /// Reports writing a part of `target`'s local — a field, tuple element, or array element at any depth — while that
    /// local is not definitely initialized and its type needs destruction. The local's drop flag covers it whole, so a
    /// value written into one of its parts would never be destroyed. `action` names the write, such as `write`.
    void ReportPartWriteIntoEmptyLocal(const Expr &target, std::string_view action, SourceLocation location);
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
    /// The type declared `#Unsendable()` that `type` is or holds, through fields, elements and payloads, or nothing
    /// when every value of `type` may move to another thread. Raw pointers and functions hold nothing here: what a
    /// pointer shares is its user's to vouch for.
    [[nodiscard]] std::optional<std::string> UnsendablePart(const TypeRef &type);
    void ValidateStoredType(const TypeRef &type, SourceLocation location, std::string_view subject);
    /// Refuses an executable's `Main` that takes parameters, is generic, or returns anything but nothing, an integer,
    /// or a fallible whose success is either.
    void CheckMainSignature(const FuncDecl &declaration, const TypeRef &returnType);
    /// Whether a value of `type` holds a view -- a slice, in itself or in any field, element or payload -- whose
    /// storage must outlive the value. Pointers do not count: what a raw pointer reaches is its user's to vouch for.
    [[nodiscard]] bool TypeHoldsView(const TypeRef &type);
    /// The arrays local to the current function, parameters passed by value included, that a view in `expression`
    /// may point into once it is converted to `targetType`: slices of them, the views locals were given from them,
    /// and aggregates built from either.
    [[nodiscard]] std::vector<const Symbol *> LocalArraysViewed(const Expr &expression, const TypeRef &targetType);
    /// Records what a local's views may point into after it takes `value`, adding to what it held before.
    void RecordViewSources(const Symbol &local, const Expr &value, const TypeRef &type);
    /// Reports a value that leaves the function, as `what` says, while it views one of the function's local arrays.
    void CheckViewEscape(const Expr &value, const TypeRef &type, SourceLocation location, std::string_view what);
    /// After an assignment: a store into a local records the views it took; a store that outlives the call, into a
    /// module variable or through a pointer or reference, may not take a view of a local array.
    void CheckViewAssignment(const Expr &target, const Expr &value, const TypeRef &targetType, SourceLocation location);
    /// Rejects a type that `HasCAbiMapping` refuses where it would cross a C ABI boundary as `subject`: the type
    /// itself, or any scalar a by-value aggregate of it holds -- a struct or union field, an array element or a tuple
    /// element, however deeply nested. A pointer, reference or slice crosses as an address, so nothing behind one is
    /// examined.
    void ValidateCAbiType(const TypeRef &type, SourceLocation location, std::string_view subject);

    /// The first scalar `type` holds by value that has no C ABI mapping, with the field path leading to it ("" when
    /// it is `type` itself), or nothing when every scalar it holds crosses as C passes it.
    /// The path of fields through which `type` holds a value of the type named `target` in place, as `next` or
    /// `inner.next`, or nothing when every way there goes through a pointer, a reference, a view or an interface.
    [[nodiscard]] std::optional<std::string> FindByValueCycle(const TypeRef &type, std::string_view target,
                                                              std::unordered_set<std::string> &visiting);
    /// Refuses a struct, enum or union whose own fields hold a value of it in place, which would have no finite size.
    void CheckByValueCycle(const Decl &declaration, std::string_view kind, const std::string &name);

    struct CAbiViolation {
        std::string path;
        TypeRef type;
    };

    [[nodiscard]] std::optional<CAbiViolation> FindCAbiViolation(const TypeRef &type,
                                                                 std::unordered_set<std::string> &visiting);
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
        /// The type arguments of a generic interface bound, as in `I: Cursor<T>`, in terms of the declaring generic's
        /// own type parameters. Empty for an interface that takes none.
        std::vector<TypeRef> typeArguments;
    };

    /// The bounds written on `parameter`. A declaration resolves its own bounds with `report` set; a use site passes
    /// the declaration's whole parameter list, so that type arguments of a generic interface bound resolve against the
    /// declaring generic's parameters rather than the caller's.
    [[nodiscard]] std::vector<ResolvedTypeBound>
    ResolveTypeParameterBounds(const TypeParameter &parameter, bool report,
                               const std::vector<TypeParameter> *declaringParameters = nullptr);
    /// Fills in type parameters that appear only in the type arguments of another parameter's generic interface bound,
    /// such as `T` in `I: Cursor<T>`, from the method the bounded type argument already deduced provides.
    void DeduceTypeArgumentsFromBounds(const std::vector<TypeParameter> &parameters,
                                       std::unordered_map<std::string, TypeRef> &substitutions);
    void DeclareTypeParameterBounds(const std::vector<TypeParameter> &parameters);

    /// Holds the bounds of the type parameters a declaration owns for as long as that declaration is being checked,
    /// then restores the enclosing declaration's. Bounds are scoped exactly like `currentTypeParams`, and a checker
    /// that saved one without restoring the other would report the wrong declaration's promises. A null `parameters`
    /// declares none of its own, which is how an extend block over a non-generic type enters.
    /// Marks a declaration as being checked for as long as it lives, so uses of it inside it are not reported.
    class CheckingDeclaration {
    public:
        CheckingDeclaration(AnalysisContext &owner, const Decl &declaration)
            : context(owner) {
            context.declarationsBeingChecked.push_back(&declaration);
        }

        ~CheckingDeclaration() {
            context.declarationsBeingChecked.pop_back();
        }

        CheckingDeclaration(const CheckingDeclaration &) = delete;
        CheckingDeclaration &operator=(const CheckingDeclaration &) = delete;

    private:
        AnalysisContext &context;
    };

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
    [[nodiscard]] bool TypeSatisfiesBound(const TypeRef &argument, const InterfaceDecl &interface, std::string &reason,
                                          const std::vector<TypeRef> &interfaceArguments = {});
    [[nodiscard]] const FuncDecl *SelectBoundOperation(const TypeRef &receiver, const std::string &typeName,
                                                       const FuncDecl &required) const;
    /// Whether a method's extension is one of `receiver`'s: `extend Pair<int32>` extends `Pair<int32>` and no other
    /// instantiation, while `extend Pair<T>` and `extend Pair` extend them all.
    [[nodiscard]] bool ExtensionAppliesTo(const FuncDecl &method, const TypeRef &receiver) const;
    [[nodiscard]] bool ImplementationAppliesTo(const ImplDecl &implementation, const TypeRef &receiver) const;
    /// The generic extension that implements `interface` for `type`, an instantiation of the type it extends.
    [[nodiscard]] const ImplDecl *GenericInterfaceImplementation(const TypeRef &type,
                                                                 const InterfaceDecl &interface) const;
    /// The methods of `receiver` named `methodName`, accessible or not, whose extension is one of the receiver's.
    [[nodiscard]] std::vector<const FuncDecl *> ApplicableMethods(const TypeRef &receiver,
                                                                  const std::string &methodName) const;
    /// Deduces type arguments from the fixed arguments and, past them, from each argument the variadic parameter
    /// collects. `call` says which argument is spread, so it deduces from the variadic slice rather than an element.
    bool DeduceTypeArguments(const FuncDecl &declaration, const std::vector<TypeRef> &argumentTypes,
                             std::unordered_map<std::string, TypeRef> &substitutions, const CallExpr *call = nullptr);
    /// The element type of a function's trailing variadic parameter with `substitutions` applied, or none when its last
    /// parameter is fixed. The function's own type parameters are in scope, as they are for its fixed parameters.
    [[nodiscard]] std::optional<TypeRef>
    ResolveVariadicElementType(const FuncDecl &declaration,
                               const std::unordered_map<std::string, TypeRef> &substitutions = {});
    /// The parameter type each of `argumentCount` arguments binds: a fixed parameter's type, then the variadic element,
    /// or the variadic slice for one spread argument. None when that many arguments cannot bind the signature: fewer
    /// than the parameters without a default value, or more than the fixed ones when nothing is variadic. A method's
    /// receiver is not one of the parameters an argument binds.
    [[nodiscard]] static std::optional<std::vector<TypeRef>>
    BoundParameterTypes(const FuncDecl &declaration, bool isMethod, const std::vector<TypeRef> &fixedTypes,
                        const std::optional<TypeRef> &variadicElement, std::size_t argumentCount, const CallExpr *call);
    /// The type of a generic function named as a value, instantiated with the type arguments the function type it
    /// is assigned to supplies, with the instantiation recorded and queued. None when the name is not a generic
    /// function or nothing says what it is instantiated with.
    [[nodiscard]] std::optional<TypeRef> InstantiateFunctionReference(const IdentExpr &identifier,
                                                                      const Symbol &symbol);
    /// The type of an overloaded function named as a value: the overload its destination's function type names, or an
    /// error when no overload or no function type says which. Nullopt when the name has one overload.
    std::optional<TypeRef> SelectOverloadedFunctionValue(const Expr &reference, const Symbol &symbol);
    /// Record that `reference`, a name or module path used as a value, stands for `function`.
    void RecordFunctionValue(const Expr &reference, const FuncDecl &function);
    bool DeduceTypeArgument(const TypeRef &paramType, const TypeRef &argType,
                            const std::unordered_set<std::string> &typeParamNames,
                            std::unordered_map<std::string, TypeRef> &substitutions);
    /// The field names of a named variant case, comma-separated, for a help line.
    [[nodiscard]] static std::string FieldNameList(const EnumDecl::Variant &variant);
    /// Refuses a shift whose count, known while compiling, is negative or at least the width of the shifted type.
    void CheckShiftCount(const TypeRef &shifted, const Expr &count, std::string_view operatorName);
    /// Refuses a constant bound of a range index past the end of a fixed array of `length` elements.
    void CheckConstantSliceBounds(const RangeExpr &range, std::uint64_t length);
    /// The type two operands of an arithmetic or comparison operator meet in, the same rule a range's bounds follow:
    /// an unsuffixed literal or constant takes the other operand's type when it fits, and otherwise the narrower
    /// integer widens to the wider one of the same signedness. None when neither converts to the other.
    [[nodiscard]] std::optional<TypeRef> CommonOperandType(const Expr &leftExpr, const TypeRef &leftType,
                                                           const Expr &rightExpr, const TypeRef &rightType);
    /// Refuses building a named variant case with call syntax, which would give its fields by position.
    void EmitNamedCaseCalled(SourceLocation location, std::string_view variantName, const EnumDecl::Variant &variant);
    [[nodiscard]] TypeRef EnumVariantConstructorType(const EnumDecl &declaration, const EnumDecl::Variant &variant,
                                                     const std::vector<TypeRef> &typeArguments = {});

    /// The bound operation a constrained receiver's method name resolves to, and where it sits in its interface.
    struct ConstrainedOperation {
        std::string parameterName;
        std::string interfaceName;
        const FuncDecl *operation = nullptr;
        std::size_t operationIndex;
        const InterfaceDecl *interface = nullptr;
        /// The bound's type arguments for a generic interface, which the operation's signature is read with.
        std::vector<TypeRef> interfaceArguments;
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
    /// Whether an assigned `T` value is a fresh temporary — a call result, literal, constructor call, or the value a
    /// conditional or `match` produces — or a value handed over with `<-`, rather than a named place, borrowed storage,
    /// or a reference. Such a source transfers into its destination as it stands, so no copy is recorded for it.
    [[nodiscard]] bool IsFreshTemporarySource(const Expr &expression, const TypeRef &type,
                                              const TypeRef &targetType) const;
    void ConsumeExplicitValue(const Expr &expression, const TypeRef &type, SourceLocation location);
    void ConsumeRecordedValue(const Expr &expression, ValueConsumptionKind kind, SourceLocation location);
    [[nodiscard]] std::vector<TypeRef> CheckCallArgumentValues(const CallExpr &call);
    void ConsumeCallArguments(const CallExpr &call, const std::vector<TypeRef> &argumentTypes,
                              const std::vector<TypeRef> *parameterTypes = nullptr);
    /// Consumes a match subject the arms take from and records whether they own it, which is what the result says.
    template <typename Arm>
    bool ConsumeMatchSubject(const Expr &subject, const TypeRef &subjectType, const std::vector<Arm> &arms,
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
    /// Checks a loop body and returns its loop record, leaving the flow at the end of the body. A pass that leaves an
    /// outer local less available than it entered sends that state round the back edge, so the body is checked again
    /// from the merged state and a use only a later pass reaches is reported. `rebound` is a `for` variable, which
    /// every pass binds afresh.
    [[nodiscard]] TrackedLoop CheckTrackedLoopBody(const Block &body, std::string_view label,
                                                   const Symbol *rebound = nullptr);
    void RecordTrackedLoopExit(std::string_view label, bool isContinue);
    [[nodiscard]] TypeRef CheckShortCircuitExpression(const BinaryExpr &expression);
    [[nodiscard]] TypeRef CheckTernaryExpression(const TernaryExpr &expression);
    [[nodiscard]] TypeRef CheckMatchExpression(const MatchExpr &expression);
    /// `outcome catch { arms }`: the success passes through, and the arms recover from the error or leave.
    [[nodiscard]] TypeRef CheckCatchExpression(const CatchExpr &expression);
    /// `value? else (e => mapper)`: the success continues, and on failure the mapper's value fails the function.
    [[nodiscard]] TypeRef CheckMappedTry(const MappedTryExpr &expression);
    /// Checks recovery arms whose subject was rejected, so their bindings and bodies report nothing spurious.
    void CheckRecoveryArmsWithoutSubject(const std::vector<MatchExpr::Arm> &arms);

    void CheckBlock(const Block &block);
    void CheckFunctionBody(const Block &block, const FuncDecl &function, const TypeRef &returnType);
    void CheckBooleanCondition(const TypeRef &type, SourceLocation location, std::string_view construct) const;
    void CheckPattern(const Pattern &pattern, const TypeRef &subjectType = TypeRef::MakeUnknown());
    void ValidateMatchPatterns(const std::vector<const Pattern *> &patterns, const TypeRef &subjectType);
    void ValidateMatchPatterns(const MatchExpr &expression, const TypeRef &subjectType);
    /// Reports a match expression on a bool or an integer that leaves some value without an arm.
    void ReportNonExhaustiveMatchExpression(const MatchExpr &expression, const std::vector<const Pattern *> &patterns,
                                            const TypeRef &subjectType);
    [[nodiscard]] bool MatchPatternsAreExhaustive(const std::vector<const Pattern *> &patterns,
                                                  const TypeRef &subjectType) const;

    // Native patterns and membership tests (PatternCoverage.cpp).
    friend class NativePatternTranslator;

    /// How the subject of the native match whose patterns are being checked is held: bindings of a borrowed subject
    /// refer to its storage, and only an exclusively borrowed one can be written through them.
    enum class PatternBorrow {
        Owned,
        Shared,
        Exclusive,
    };

    /// Whether a copy of this type out of named or borrowed storage needs a recorded copy plan: a declared type, an
    /// array, a tuple, or a native sum, optional, or fallible.
    [[nodiscard]] static bool IsStoredAggregate(const TypeRef &type) noexcept;
    /// Whether an argument of an unsuffixed literal's default type can reach a native parameter, by the literal rule
    /// of the native conversions; overload resolution asks before the argument's own conversion is checked.
    [[nodiscard]] static bool DefaultLiteralReachesNative(const TypeRef &argument, const TypeRef &parameter);
    /// How the arms of a native match over `subject` hold it.
    [[nodiscard]] PatternBorrow MatchSubjectBorrow(const Expr &subject, const TypeRef &expressionType) const;
    /// Gives a binding pattern under a borrowed native subject its binding mode.
    void RecordPatternBinding(const Pattern &pattern, Symbol &symbol, bool view);
    /// Rejects `use` of a subset view, reporting it once; true when `expression` names one.
    bool RejectSubsetViewUse(const Expr &expression, SourceLocation location, std::string_view use);
    /// Rejects a guard that moved a binding of its own arm, which a later arm still needs when the guard fails.
    void CheckGuardKeepsBindings(const GuardedPattern &pattern);

    /// A pattern diagnostic computed before it is known whether, or with which instantiation note, it is reported.
    struct PatternIssue {
        SourceLocation location;
        std::string message;
        std::optional<std::string> help;
    };

    /// Whether a match over this subject is checked by native coverage: the unit, or a type that mentions a sum,
    /// optional, or fallible at any level.
    [[nodiscard]] static bool IsNativeMatchSubject(const TypeRef &subjectType);
    /// Rejects a binding pattern whose name already names a type, alias, generic parameter, function, constant,
    /// module, or interface, or a case of the matched value; shadowing a variable or parameter stays legal.
    bool CheckFreeBindingName(const std::string &name, SourceLocation location, const TypeRef &subjectType);
    /// The member of `sum` that a qualified case, struct, literal, or tuple pattern selects, or nothing with the
    /// reason in `issue` and `help`.
    [[nodiscard]] std::optional<std::size_t> SumMemberOfPattern(const Pattern &pattern, const TypeRef &sum,
                                                                std::string *issue, std::optional<std::string> *help);
    void CheckTypedPattern(const TypedPattern &pattern, const TypeRef &subjectType);
    void CheckNativeCasePattern(const EnumPattern &pattern, const TypeRef &subjectType);
    /// Defines every name a rejected pattern binds, with no type, so its arm does not cascade into unknown names.
    void DefinePatternBindings(const Pattern &pattern);
    [[nodiscard]] std::optional<PatternIssue>
    TypedSelectionIssue(const TypedPattern &pattern, const TypeRef &subjectType, const TypeRef &annotation) const;
    void CheckMembershipTest(const IsExpr &expression, const TypeRef &operandType, const TypeRef &testedType);
    [[nodiscard]] std::optional<PatternIssue> MembershipIssue(const TypeRef &subject, const TypeRef &tested,
                                                              SourceLocation location) const;
    /// The constructors coverage splits a type into, each with its field types, or nothing for an open type.
    [[nodiscard]] std::optional<std::vector<std::vector<TypeRef>>> CoverageConstructors(const TypeRef &type);
    /// Whether the arms cover every value of the subject; unreachable arms and missing values go to `issues`.
    bool NativeMatchCoverage(const std::vector<const Pattern *> &patterns, const TypeRef &subjectType,
                             const std::unordered_map<std::string, TypeRef> &substitutions,
                             std::vector<PatternIssue> *issues);
    void ValidateNativeMatch(const std::vector<const Pattern *> &patterns, const TypeRef &subjectType);
    /// The values no unguarded arm matches, spelled as patterns and joined, or nothing when the arms cover the subject.
    [[nodiscard]] std::optional<std::string> MissingMatchValues(const std::vector<const Pattern *> &patterns,
                                                                const TypeRef &subjectType);
    [[nodiscard]] static bool PatternContains(const Pattern &pattern, const Pattern &target);
    void ValidateDeferredPatternChecks(const FuncDecl &declaration,
                                       const std::unordered_map<std::string, TypeRef> &substitutions);
    /// Notes `type` against the generic function being checked when it is a native type built from its parameters.
    void NoteDeferredNativeType(const TypeRef &type);
    [[nodiscard]] const TypeRef *TypedPatternAnnotation(const TypedPattern &pattern) const;
    [[nodiscard]] bool BlockDefinitelyReturns(const Block &block) const;
    /// Whether a pattern matches every value of the subject: a binding, a wildcard, or a typed pattern naming exactly
    /// the subject's type.
    [[nodiscard]] bool MatchesWholeSubject(const Pattern &pattern, const TypeRef &subjectType) const;
    [[nodiscard]] std::optional<TypeRef> CheckBasicExpression(const Expr &expression);
    void CheckCast(const TypeRef &operand, const TypeRef &target, SourceLocation location);
    [[nodiscard]] bool CastTypesAreCompatible(const TypeRef &operand, const TypeRef &target) const;

    /// The case names the retired Result and Option protocols recognized. Such a variant is ordinary and earns no
    /// operator or iterator behavior; the shape only lets the diagnostics that reject it name the native replacement.
    enum class LegacyOutcomeShape {
        None,
        Result,
        Option
    };

    [[nodiscard]] LegacyOutcomeShape LegacyOutcomeShapeOf(const TypeRef &type) const;
    [[nodiscard]] std::optional<TypeRef> CheckTryExpression(const TryExpr &expression);
    /// `?` on a native fallible: the success continues and the error leaves as the enclosing outer failure.
    [[nodiscard]] TypeRef CheckNativeTry(const TryExpr &expression, const TypeRef &operandType);
    /// `?` on a native optional: the payload continues and absence leaves as the enclosing absence.
    [[nodiscard]] TypeRef CheckNativeOptionalTry(const TryExpr &expression, const TypeRef &operandType);
    /// `option ?? fallback` on a native optional: the payload, or the fallback evaluated only for absence.
    [[nodiscard]] TypeRef CheckNativeCoalesce(const BinaryExpr &expression, const TypeRef &leftType);
    /// Whether an error enters a failure channel by identity, sum member injection, or subset widening.
    [[nodiscard]] static bool ErrorFitsChannel(const TypeRef &error, const TypeRef &channel);
    [[nodiscard]] TypeRef CheckCoalesceExpression(const BinaryExpr &expression);
    [[nodiscard]] bool ValidateOutcomePayload(const TypeRef &payload, SourceLocation location,
                                              bool propagation = false);
    [[nodiscard]] TypeRef CheckTypeQueryExpression(const TypeQueryExpr &expression);
    [[nodiscard]] static bool IsCheckedArithmeticIntrinsic(std::string_view intrinsicName);
    void ValidateCheckedArithmeticIntrinsic(const FuncDecl &declaration);
    [[nodiscard]] static bool IsAtomicIntrinsic(std::string_view intrinsicName);
    /// Holds a call of an atomic intrinsic to a value type the instructions move and to orders they define.
    void CheckAtomicCall(const CallExpr &call, const FuncDecl &declaration,
                         const std::unordered_map<std::string, TypeRef> &substitutions);
    void CheckAtomicValueType(const TypeRef &type, std::string_view operation, SourceLocation location);
    void ValidateDeferredAtomicChecks(const FuncDecl &declaration,
                                      const std::unordered_map<std::string, TypeRef> &substitutions);
    [[nodiscard]] static bool IsZeroizeIntrinsic(std::string_view intrinsicName);
    void ValidateZeroizeIntrinsic(const FuncDecl &declaration);
    /// Rejects an intrinsic function the compiler does not implement, and holds a diagnostic intrinsic (`Assert`,
    /// `DebugAssert`, `Panic`, `#Error`, `#Warn`) to the signature its calls are emitted for. Checked arithmetic and
    /// zeroization have validators of their own.
    void ValidateIntrinsicFunction(const FuncDecl &declaration);

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
        /// The native optional `Next` reports.
        TypeRef reportedType = TypeRef::MakeUnknown();
    };

    [[nodiscard]] const FuncDecl *LookupIteratorAdvance(const TypeRef &type) const;
    [[nodiscard]] const FuncDecl *LookupIterableEntry(const TypeRef &type) const;

    /// What one `Next` reports: the item it carries and the native optional it is wrapped in.
    struct ReportedItem {
        TypeRef itemType;
        TypeRef reportedType;
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
    /// The declared `[]` that accepts the index expression of `index`, recorded for lowering; nullopt when the type
    /// declares none, and Unknown once a mismatch or an ambiguity is reported.
    std::optional<TypeRef> ResolveIndexOperator(const IndexExpr &index, const TypeRef &objectType,
                                                const TypeRef &indexType);
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
    const PackageImportBindings &imports;
    const std::unordered_set<const Module *> *declarationOnlyModules = nullptr;
    /// Whether the module being checked is one of `declarationOnlyModules`.
    bool checkingDeclarationsOnly = false;
    /// The facts this context records into, so a parallel analysis can extract and merge what its bodies recorded.
    SemanticFacts &outputFacts;
    std::unordered_map<const Expr *, TypeRef> &expressionTypes;
    std::unordered_set<const Expr *> &borrowedScalarReads;
    std::unordered_set<const Expr *> &referenceWrites;
    std::unordered_map<const Expr *, PlaceReplacement> &placeReplacements;
    std::unordered_map<const Expr *, std::vector<NativeConversionStep>> &nativeConversions;
    std::unordered_map<const Expr *, const ConstDecl *> &associatedConstants;
    std::unordered_map<const Expr *, const ConstDecl *> &constantReferences;
    std::unordered_map<const Expr *, const Decl *> &globalReferences;
    std::unordered_map<const ConstDecl *, EvaluatedAssociatedConstant> &evaluatedAssociatedConstants;
    std::unordered_set<const ConstDecl *> checkingAssociatedConstants;
    std::unordered_set<const TypeAliasDecl *> checkingTypeAliases;

    /// The associated constant `type::name` names here. Several packages may extend one primitive, so a name two of
    /// them declare is reported rather than chosen.
    struct AssociatedConstantLookup {
        const ConstDecl *constant = nullptr;
        /// Every package declaring the name, when more than one does.
        std::vector<std::string> conflictingPackages;
    };

    [[nodiscard]] AssociatedConstantLookup LookupAssociatedConstant(const Symbol &type, const std::string &name) const;
    /// For a primitive constant nothing in scope declares, an import that would reach one a dependency does.
    [[nodiscard]] std::optional<std::string> AssociatedConstantImportHelp(const Symbol &type,
                                                                          const std::string &name) const;
    [[nodiscard]] TypeRef CheckAssociatedConstant(const ConstDecl &declaration);
    /// Checks `intrinsic #name: Root;`: the compiler supplies a value only for a known root, and fills in only the
    /// fields `CompilerParameters.h` lists for it.
    void CheckCompilerParameterDeclaration(const ConstDecl &declaration, const TypeRef &type);
    [[nodiscard]] bool IsVisibleTypeSymbol(const Symbol &symbol) const;
    /// The primitive a type symbol names, following aliases, with the packages whose extensions of it apply where the
    /// symbol is used: the current package, the current file's imports and, for an alias, those of every file that
    /// declares an alias in the chain.
    ///
    /// @return nullopt when `symbol` names no primitive
    [[nodiscard]] std::optional<TypeRef::Kind> PrimitiveProviders(const Symbol &symbol,
                                                                  std::unordered_set<std::string> &providers) const;
    std::unordered_map<const TypeExpr *, TypeRef> &typeNodeTypes;
    std::unordered_map<const Pattern *, TypeRef> &patternTypes;
    std::unordered_map<const EnumPattern *, ResolvedCasePattern> &casePatterns;
    std::unordered_map<const TypedPattern *, TypeRef> &typedPatternTypes;
    std::unordered_map<const Pattern *, TypeRef> &sumMemberPatterns;
    std::unordered_map<const Pattern *, PatternBindingMode> &patternBindingModes;

    PatternBorrow currentPatternBorrow = PatternBorrow::Owned;
    /// Whether the arm whose pattern is being checked owns what it binds: its match owns its subject, or it is a
    /// `catch` arm taking the error. A structure pattern binding a part there takes the value apart.
    bool currentPatternTakesParts = false;

    /// A `let` holding a native fallible, checked for a read when its scope ends.
    struct FallibleLocal {
        const Symbol *symbol = nullptr;
        const Scope *scope = nullptr;
    };

    std::vector<FallibleLocal> fallibleLocals;
    /// Locals read as values; an assignment target is not a read.
    std::unordered_set<const Symbol *> readSymbols;
    /// Reports a native fallible result that an expression statement or a bare match-statement arm discards.
    void CheckFallibleDiscard(const TypeRef &type, SourceLocation location);
    /// Reports the fallible locals of the scope being closed that were never read.
    void CheckUnreadFallibleLocals();
    std::unordered_map<const BinaryExpr *, ResolvedVariantEquality> &variantEqualities;
    std::unordered_map<std::string, VariantEqualityPlan> &variantEqualityPlans;
    std::unordered_map<const BinaryExpr *, bool> &aggregateEqualities;
    std::unordered_map<const BinaryExpr *, TypeRef> &aggregateEqualityOperandTypes;
    std::unordered_map<std::string, VariantEqualityPayload> &aggregateEqualityPlans;
    std::unordered_map<const Expr *, ValueConsumption> &valueConsumptions;
    std::unordered_map<const Expr *, ValueCopy> &valueCopies;
    std::unordered_map<const CallExpr *, ResolvedCallableBinding> &callableBindings;
    std::unordered_map<const Expr *, ResolvedCallableBinding> &functionReferenceBindings;
    std::unordered_map<const LetStmt *, ResolvedDefaultConstructor> &defaultConstructors;
    std::unordered_map<const Decl *, ResolvedSymbolIdentity> &symbolIdentities;
    std::unordered_map<const ImplDecl *, ResolvedVtableIdentity> &vtableIdentities;
    std::unordered_map<std::string, ResolvedTypeLayout> &typeLayouts;
    std::unordered_map<std::string, TypeProperties> &typeProperties;
    std::unordered_map<std::string, DropGluePlan> &dropGluePlans;
    /// One entry per (bound, concrete type) pair a use site proved, so lowering can call the satisfying method directly
    /// in each instantiation instead of dispatching through the interface.
    std::unordered_map<std::string, ResolvedConstraintWitness> &constraintWitnesses;
    /// One entry per accepted `expr?`, so lowering builds its early return without recognizing Result or Option again.
    std::unordered_map<const TryExpr *, ResolvedPropagation> &propagations;
    std::unordered_map<const MappedTryExpr *, ResolvedPropagation> &mappedPropagations;
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
    /// One entry per accepted `match` that covers its subject, so lowering stops a value no arm matches.
    std::unordered_set<const MatchExpr *> &exhaustiveMatchExpressions;
    std::unordered_set<const MatchStmt *> &exhaustiveMatchStatements;
    /// One entry per `match` or `catch` subject whose arms own it, so lowering destroys exactly what they own.
    std::unordered_set<const Expr *> &ownedMatchSubjects;
    std::unordered_map<const TypeQueryExpr *, std::uint64_t> &typeQueryValues;

    SemanticProgramIndex programIndex;
    Scope &globalScope;
    const SemanticProgramIndex::PackageScopes &packageModuleScopes;
    TypeRef currentReturnType = TypeRef::MakeOpaque();
    bool currentFunctionNoReturn = false;
    int loopDepth = 0;
    std::unordered_multiset<std::string> activeLabels;

    /// While a deferred statement is checked: the loop depth and the labels outside it, which it cannot leave to.
    struct DeferredContext {
        int loopDepth = 0;
        std::unordered_multiset<std::string> labels;
    };

    std::optional<DeferredContext> checkingDeferred;
    /// For each loop being checked, innermost last, the expressions its body has recorded types for.
    std::vector<std::vector<const Expr *>> loopCheckedExpressions;
    /// The parameters of the function whose default value is being checked, none of which a default may read.
    std::unordered_set<std::string> defaultValueParameters;
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
    std::unordered_set<const ConstDecl *> checkingConstants;
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
    /// The types `UnsendablePart` is inside, so a recursive type ends its own walk.
    std::unordered_set<std::string> sendableChecks;
    /// The answers of `TypeHoldsView`, and the types it is inside, so a recursive type ends its own walk.
    std::unordered_map<std::string, bool> viewHoldingTypes;
    std::unordered_set<std::string> viewHoldingChecks;
    /// The locals of the functions being checked, parameters included: the storage that ends with the call.
    std::unordered_set<const Symbol *> functionLocals;
    /// For each local holding views, the local arrays those views may point into.
    std::unordered_map<const Symbol *, std::vector<const Symbol *>> viewSources;
    bool checkingPlainAssignmentTarget = false;
    bool checkingBorrowProjectionRoot = false;
    /// The field path, such as `start.x`, that the local about to be read is read through, so a part written on its
    /// own is readable before the whole local is. Set by the outermost field read and taken by the local's read.
    std::optional<std::string> partReadPath;

    /// The enum `name` names, as seen from the file being checked.
    ///
    /// The struct and enum indexes are keyed by bare name across every package, so the last declaration of a name
    /// wins: a program declaring its own `Option` displaces `Core`'s, and `Core`'s own methods would then be checked
    /// against the wrong shape. A declaration in the file doing the asking is the one that file means.
    ///
    /// @return nullptr when no enum of that name is in scope
    [[nodiscard]] std::string NominalTypeName(const std::string &name) const;

    class ScopedTypeOwner {
    public:
        ScopedTypeOwner(AnalysisContext &context, const TypeExpr &type);
        ~ScopedTypeOwner();

    private:
        AnalysisContext &context;
        Scope *scope;
        std::string file;
        std::string package;
        const Decl *owner;
    };

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
    /// The enum or variant a cast operand or target is a value of, read through a reference but never a pointer: a
    /// pointer is an address whatever it points at, so it casts by the pointer rules alone.
    [[nodiscard]] CaseTypeDeclaration CastCaseType(const TypeRef &type) const;

    /// Report a suffixed integer literal whose magnitude the suffix's own type cannot hold.
    ///
    /// An unsuffixed literal is checked against whatever it is being assigned to; a suffixed one names its type
    /// itself, so nothing else ever checks it.
    void ValidateSuffixedIntegerLiteral(const LiteralExpr &literal, bool negative);
    /// Reports a prefixed character literal whose character is not one code unit of its width, such as `c8'é'`.
    void ValidateCharLiteral(const Token &literal, SourceLocation location);

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
    [[nodiscard]] bool CanConvertToInterface(const TypeRef &argument, const TypeRef &parameter) const;

private:
    void RegisterBuiltins();
    void IndexDeclarations();
    /// A declaration's type while indexing, with no diagnostics: checking the declaration reports them.
    [[nodiscard]] TypeRef ResolveIndexedType(const TypeExpr &type);
    void CollectModule(const Module &module);
    void CheckStatement(const Stmt &statement);
    /// Make `label` the target of `break label` and `continue label` inside a loop, reporting a reuse of an enclosing
    /// loop's label.
    void EnterLoopLabel(const std::string &label, SourceLocation location);
    void ExitLoopLabel(const std::string &label);
    void CheckLetPattern(const Pattern &pattern, const TypeRef &type, bool isMutable);
    /// Checks a structure pattern against `subjectType` without its field patterns: the structure it names, that a
    /// pattern taking its value apart (`takesParts`) does not split one whose destructor runs on the whole, and that
    /// every field it writes exists, is written once, and is visible here. Returns the type of each written field, in
    /// source order, with a generic subject's type arguments in place, or unknown where it could not be resolved.
    [[nodiscard]] std::vector<TypeRef> CheckStructPatternShape(const StructPattern &pattern, const TypeRef &subjectType,
                                                               bool takesParts);
    TypeRef ResolveType(const TypeExpr &expression);
    /// The target layout of a written type, or nothing when it has none: recursive, unsized, or not yet validated.
    [[nodiscard]] std::optional<ResolvedTypeLayout> LayoutOfTypeExpression(const TypeExpr &expression);
    TypeRef ResolveTypeWithSubstitution(const TypeExpr &expression,
                                        const std::unordered_map<std::string, TypeRef> &substitutions);
    TypeRef ResolveRangeType(const RangeTypeExpr &range, const std::function<TypeRef(const TypeExpr &)> &resolveBound);
    TypeRef CheckExpr(const Expr &expression);
    [[nodiscard]] TypeRef ReadBorrowedScalar(const Expr &expression, const TypeRef &type);
    void EmitDiagnosticIntrinsic(const std::string &intrinsicName, const CallExpr &call);

    /// The one overload of `matches` that a call with `argumentTypes` selects, or null when none does; when
    /// several tie, they are left in `ambiguousOverloads`.
    struct OverloadMatch {
        const FuncDecl *decl;
        std::vector<TypeRef> parameterTypes;
    };

    /// Whether every argument reaches its parameter: exactly, through a borrow, a scalar read, an interface view or an
    /// assignable conversion, or as an unsuffixed literal that fits. `call`, when known, tells a literal from a value.
    [[nodiscard]] bool ArgumentsReachOverload(const std::vector<TypeRef> &argTypes,
                                              const std::vector<TypeRef> &parameterTypes,
                                              const CallExpr *call = nullptr);
    const FuncDecl *SelectOverload(const std::vector<OverloadMatch> &matches,
                                   const std::vector<TypeRef> &argumentTypes);
    /// The help for a call `ambiguousOverloads` tie on: `authorHelp`, which asks for a change to the overloads, when
    /// one of them is the calling package's own, and otherwise how the caller makes its arguments pick one.
    [[nodiscard]] std::string AmbiguousCallHelp(std::string_view authorHelp) const;
    [[nodiscard]] const FuncDecl *LookupFunctionOverload(const Symbol &symbol,
                                                         const std::vector<TypeRef> &argumentTypes,
                                                         const std::vector<TypeExprPtr> &typeArguments,
                                                         const CallExpr *call = nullptr);
    /// Queue a body to re-check with concrete type arguments. `site` is the call that supplies them, which an error
    /// found in the instantiated body names.
    void QueueGenericInstantiation(const FuncDecl &declaration,
                                   const std::unordered_map<std::string, TypeRef> &substitutions,
                                   std::optional<SourceLocation> site = std::nullopt);
    /// The note an error inside an instantiated generic body carries: the instantiation and the call that caused it.
    [[nodiscard]] std::string InstantiationNote(const FuncDecl &declaration,
                                                const std::unordered_map<std::string, TypeRef> &substitutions) const;
    /// The method `argumentTypes` select. `call`, when the lookup is for one, says which argument is spread into a
    /// variadic parameter; without it every argument past the fixed parameters is an element. With
    /// `receiverIsArgument`, as for a call written through the type, a method's receiver is the first argument.
    [[nodiscard]] const FuncDecl *LookupMethod(const TypeRef &receiverType, const std::string &methodName,
                                               const std::vector<TypeRef> &argumentTypes = {},
                                               bool requireAccessible = true, const CallExpr *call = nullptr,
                                               bool receiverIsArgument = false);
    [[nodiscard]] std::unordered_map<std::string, TypeRef> MethodTypeSubstitutions(const TypeRef &receiverType) const;
    [[nodiscard]] const FuncDecl *LookupMethodCall(const TypeRef &receiverType, const std::string &methodName,
                                                   const CallExpr &call, const std::vector<TypeRef> &argumentTypes,
                                                   std::size_t typeArgumentOffset,
                                                   std::unordered_map<std::string, TypeRef> &substitutions,
                                                   bool receiverIsArgument = false);
    [[nodiscard]] const std::vector<TypeParameter> *AggregateTypeParams(const std::string &name) const;
    [[nodiscard]] TypeRef InstantiateAssociatedReceiver(TypeRef receiverType,
                                                        const std::vector<TypeExprPtr> &typeArguments);
    [[nodiscard]] TypeRef AssociatedFunctionType(const TypeRef &receiverType, const FuncDecl &method);
    [[nodiscard]] TypeRef
    ResolveMethodReturnType(const TypeRef &receiverType, const FuncDecl &method,
                            const std::unordered_map<std::string, TypeRef> &methodSubstitutions = {});
    /// The types of a method's fixed parameters for `receiverType`, without the receiver unless `includeReceiver`.
    [[nodiscard]] std::vector<TypeRef>
    ResolveMethodParamTypes(const TypeRef &receiverType, const FuncDecl &method,
                            const std::unordered_map<std::string, TypeRef> &methodSubstitutions = {},
                            bool includeReceiver = false);
    /// The element type of a method's trailing variadic parameter, resolved for `receiverType` as its fixed parameter
    /// types are, or none when its last parameter is fixed.
    [[nodiscard]] std::optional<TypeRef>
    ResolveMethodVariadicType(const TypeRef &receiverType, const FuncDecl &method,
                              const std::unordered_map<std::string, TypeRef> &methodSubstitutions = {});
    [[nodiscard]] const FuncDecl *LookupInterfaceMethod(const TypeRef &receiverType,
                                                        const std::string &methodName) const;

    /// Whether any written type of `method` other than its receiver names `Self`, which is what makes it callable only
    /// where the implementing type is known. The receiver is the data half of the interface value, so `self: &Self`
    /// pairs it with nothing.
    [[nodiscard]] static bool InterfaceMethodMentionsSelf(const FuncDecl &method);
    /// Whether an interface requirement writes through its receiver. A requirement declaring `self: &var Self` does;
    /// one declaring no receiver or `self: &Self` only reads it.
    [[nodiscard]] static bool RequirementWritesReceiver(const FuncDecl &requirement);
    /// Whether a method's declared receiver is written `&var T` or `*var T`.
    [[nodiscard]] static bool DeclaresWritableReceiver(const FuncDecl &method);
    /// Rejects a requirement receiver other than `self: &Self` or `self: &var Self`.
    void ValidateRequirementReceiver(const FuncDecl &requirement, const std::string &interfaceName);
    /// Rejects an implementation whose receiver is writable where the requirement it satisfies only reads.
    void CheckImplementationReceiver(const FuncDecl &implementation, const FuncDecl &requirement,
                                     const std::string &interfaceName, const std::string &typeName);
    /// An interface method's written types, with `Self` standing for `selfType`.
    ///
    /// An interface cannot name the type implementing it any other way, so a binary operation like equality or
    /// comparison has no way to say that its other operand is the same type as its receiver. `Self` is that name, and
    /// it is substituted here, where the receiver is known. The parameters of a generic interface are substituted with
    /// `interfaceArguments` the same way.
    [[nodiscard]] TypeRef ResolveInterfaceMethodReturnType(const FuncDecl &method, const TypeRef &selfType,
                                                           const InterfaceDecl *interface = nullptr,
                                                           const std::vector<TypeRef> &interfaceArguments = {});

    /// The written parameters, variadic element and result of `method` on `receiver` against those `requirement` of
    /// `interface` asks for, spelled `(int32, bool) -> float64` for a diagnostic. Empty when they agree, or when either
    /// side names a type that is not known yet.
    struct SignatureMismatch {
        std::string required;
        std::string declared;
    };

    [[nodiscard]] std::optional<SignatureMismatch>
    RequirementSignatureMismatch(const TypeRef &receiver, const FuncDecl &method, const FuncDecl &requirement,
                                 const InterfaceDecl &interface, const std::vector<TypeRef> &interfaceArguments);
    [[nodiscard]] std::vector<TypeRef>
    ResolveInterfaceMethodParamTypes(const FuncDecl &method, const TypeRef &selfType,
                                     const InterfaceDecl *interface = nullptr,
                                     const std::vector<TypeRef> &interfaceArguments = {});
    /// The element type of an interface method's trailing variadic parameter, with `Self` standing for `selfType`.
    [[nodiscard]] std::optional<TypeRef>
    ResolveInterfaceMethodVariadicType(const FuncDecl &method, const TypeRef &selfType,
                                       const InterfaceDecl *interface = nullptr,
                                       const std::vector<TypeRef> &interfaceArguments = {});
    /// Resolves a type written in an interface requirement with `Self` and the interface's own parameters substituted.
    [[nodiscard]] TypeRef ResolveRequirementType(const TypeExpr &type, const TypeRef &selfType,
                                                 const InterfaceDecl *interface,
                                                 const std::vector<TypeRef> &interfaceArguments);
    [[nodiscard]] TypeRef EnumType(const EnumDecl &declaration, const std::vector<TypeRef> &typeArguments = {});
    [[nodiscard]] TypeRef LiteralType(const Token &token) const;
    void ValidateCastConstant(const CastExpr &expression, const TypeRef &operandType, const TypeRef &targetType) const;
    [[nodiscard]] const FuncDecl *LookupOperatorMethod(const TypeRef &receiverType, const std::string &operatorName,
                                                       const std::vector<TypeRef> &argumentTypes);
    [[nodiscard]] std::vector<TypeRef> ResolveOperatorParameterTypes(const TypeRef &receiverType,
                                                                     const FuncDecl &method);
    [[nodiscard]] TypeRef ResolveOperatorReturnType(const TypeRef &receiverType, const FuncDecl &method);
    void CheckDecl(const Decl &declaration);
    /// Whether `function` of a module whose code the build reuses needs no checking at all: it was checked when that
    /// code was compiled, against the same declarations, and records nothing another module or lowering reads. A
    /// generic body is instantiated from what checking it records, a default argument is lowered at every call, and an
    /// assembly body is lowered whole.
    void ResolveReusedParameters(const FuncDecl &function);
    [[nodiscard]] bool SkipsReusedFunction(const FuncDecl &function) const;
    [[nodiscard]] std::optional<std::uint64_t> EvalArrayLength(const Expr &expression) const;
    void ValidateArrayType(const TypeExpr &type, bool allowFlexibleTail = false);
    /// The type an expression was already checked to have, checking it only when it has not been: asking twice must
    /// not report its errors twice.
    [[nodiscard]] TypeRef CheckedExpressionType(const Expr &expression);
    [[nodiscard]] bool CanAssignExprTo(const Expr &expression, const TypeRef &expressionType,
                                       const TypeRef &targetType);
    [[nodiscard]] std::string AssignmentErrorMessage(const Expr &expression, const TypeRef &targetType,
                                                     std::string fallback);
    [[nodiscard]] static std::optional<std::string> UnfitCharacterLiteralMessage(const Expr &expression,
                                                                                 const TypeRef &targetType);
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
    /// Check the declarations other modules' bodies read before any body is checked: struct, enum and union
    /// declarations, module constants and variables, and associated constants. A body then finds them checked rather
    /// than checking them itself in the middle of its own flow, so what they record does not depend on which module
    /// reached them first.
    void PrecheckModule(const Module &module);
    void PrecheckModuleInScope(const Module &module, Scope &scope);
    void PrecheckDecl(const Decl &declaration);
    /// Bind a module's imports again once signatures and prechecked declarations are resolved, so the copies they put
    /// in scope carry resolved types before any body is checked; CheckDecl then leaves those imports alone. Imports are
    /// bound exactly twice however many modules a context checks, which keeps every context's scopes alike.
    void RefreshModuleImports(const Module &module);
    void RefreshModuleImportsInScope(const Module &module, Scope &scope);
    void RefreshDeclImports(const Decl &declaration);
    void ReapplyUseDecl(const UseDecl &useDecl);
    /// Queues the destructor of every droppable type recorded, so its body is analyzed at each type it will
    /// be built for. Nothing in the source calls a destructor -- the generated glue does -- so without this a body
    /// reachable no other way is lowered having never been analyzed, and anything it deferred until its type
    /// arguments were known is never answered.
    void QueueDropMethodInstantiations(const std::vector<TypeRef> &recordedTypes);
    /// Queues the destructor instantiation a droppable `type` needs, if its destructor is generic.
    void QueueDropMethodInstantiation(const TypeRef &type);
    /// Records a type the body of the function being checked holds, when it names a type parameter.
    void NoteGenericBodyType(const TypeRef &type);

    void ValidatePendingGenericInstantiations();
    /// Every type recorded for an expression, a type node or a pattern, one per spelling, in the order first met. The
    /// passes of Finish depend only on a type's spelling, and most recorded types repeat.
    [[nodiscard]] std::vector<TypeRef> DistinctRecordedTypes() const;
    void RecordResolvedTypeLayouts(const std::vector<TypeRef> &recordedTypes);
    void RecordResolvedTypeProperties(const std::vector<TypeRef> &recordedTypes);
    void SynthesizeResolvedDropGlue(const std::vector<TypeRef> &recordedTypes);
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
    void RecordDiagnosticTypeNames();
    /// `text` with every package-qualified type identity spelled as `diagnosticTypeNames` says.
    [[nodiscard]] std::string DiagnosticTypeNames(std::string text) const;
    /// Reports a use of a declaration that carries `#Warn` or `#Error`, once per place it is written, unless the use is
    /// inside the declaration itself or an extension of the type it declares.
    void NoteDeclarationUse(const Decl &declaration, SourceLocation location);
    /// `NoteDeclarationUse` for the declaration a name resolved to: an extern function, a function with one overload,
    /// or whatever else the symbol was declared by.
    void NoteSymbolUse(const Symbol &symbol, SourceLocation location);
    void RecordFunctionBinding(const CallExpr &call, const FuncDecl &declaration,
                               ResolvedCallableBinding::DispatchKind dispatch,
                               std::unordered_map<std::string, TypeRef> substitutions = {},
                               std::optional<TypeRef> receiverType = std::nullopt);
    void RecordExternBinding(const CallExpr &call, const ExternFuncDecl &declaration);
    /// Counts the placeholders of a string literal passed for a `#Format()` parameter against the arguments the
    /// declaration's variadic parameter collects, and reports a mismatch. A spread argument, a format that is not a
    /// literal, and a malformed pattern are left to the run-time check.
    void CheckFormatArguments(const CallExpr &call, const FuncDecl &declaration, bool isMethod);
    /// The element type the destination of `array` fixes, when it fixes one its elements can be converted to.
    [[nodiscard]] std::optional<TypeRef> ExpectedArrayElementType(const ArrayExpr &array) const;
    /// The type arguments of a generic variant case written without them, when its destination names an instantiation
    /// of the same variant.
    [[nodiscard]] std::optional<std::vector<TypeRef>> ExpectedCaseTypeArguments(const Expr &expression,
                                                                                const EnumDecl &declaration);
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
    /// The reference binding a write to `target` stores through, or null when `target` is not a name bound to a
    /// reference the write can reach through. `replaces` is set for `=` and `<-`, which replace a referent of any
    /// concrete or generic type whole; other writes reach only a Copy primitive scalar. `rebinds` is set for a plain
    /// assignment, which points a `var` reference binding at other storage rather than writing through it.
    [[nodiscard]] const Symbol *ReferenceWriteTarget(const Expr &target, bool replaces, bool rebinds) const;
    /// Accepts a write through the reference `target` names and records it for lowering. Returns false after reporting
    /// a write through an immutable reference.
    [[nodiscard]] bool CheckReferenceWrite(const Expr &target, const Symbol &reference);
    /// Records whether the accepted `=` or `<-` `assignment` replaces a value that already lives at its place, and who
    /// owns that value, so lowering destroys it before installing the new one. `wholeReferent` is set for a referent
    /// written whole through `&var T`. A binding written whole is left to its own cleanup, and a place reached through
    /// a raw pointer may hold no value yet, so neither is recorded.
    void RecordPlaceReplacement(const AssignExpr &assignment, bool wholeReferent);
    void CheckMutability(const Expr &target);
    /// The fix offered for writing the immutable `binding`: declaring a local with `var`, or for a parameter, which
    /// has no `var` form, taking it by `&var T` instead. Null when no declaration would make the write legal.
    [[nodiscard]] static std::optional<std::string> ImmutableBindingHelp(const Symbol &binding);
    [[nodiscard]] bool CheckReceiverMutability(const CallExpr &call, const Expr &receiver, const TypeRef &receiverType,
                                               const FuncDecl &method);
    /// The check `CheckReceiverMutability` applies once a method is known to write through `declared`: a reference or
    /// pointer receiver has to be writable, and a receiver named as a place has to be a writable place.
    [[nodiscard]] bool CheckWritableReceiver(const CallExpr &call, const Expr &receiver, const TypeRef &receiverType,
                                             const std::string &methodName, const std::string &declared);
    /// A call of an interface requirement that declares `self: &var Self`, through an interface view or a bounded
    /// generic receiver, needs a receiver it may write.
    [[nodiscard]] bool CheckRequirementReceiverMutability(const CallExpr &call, const Expr &receiver,
                                                          const TypeRef &receiverType, const FuncDecl &requirement);
    /// The receiver the method declares, resolved for this call's receiver type. Empty for an associated function,
    /// which takes none.
    [[nodiscard]] std::optional<TypeRef> ResolveMethodReceiverType(const TypeRef &receiverType, const FuncDecl &method);

    /// The instantiation whose body is being re-checked, so errors found there can say what caused it.
    const PendingGenericInstantiation *activeInstantiation = nullptr;

    Parallel::WorkerPool *workers = nullptr;

public:
    struct BodyRecords {
        SemanticFacts facts;
        std::unordered_map<const ConstDecl *, TypeRef> checkedConstantTypes;
        std::unordered_map<const GlobalDecl *, TypeRef> checkedGlobalTypes;
        std::unordered_set<const ConstDecl *> localConstants;
        /// Where each of `localConstants` was declared; its scope belongs to the context that checked it.
        std::vector<std::pair<const Decl *, SemanticProgramIndex::DeclarationInfo>> localConstantInfos;
        std::unordered_map<const Expr *, TypeRef> expectedExpressionTypes;
        std::unordered_set<const NativeConstructExpr *> consumedConstructorOperands;
        std::unordered_map<const Pattern *, bool> nativeMatchExhaustive;
        std::unordered_map<const Pattern *, std::optional<std::string>> tupleMatchMissing;
        std::unordered_set<const LiteralExpr *> negatedIntegerLiterals;
        std::unordered_map<const FuncDecl *, std::unordered_map<std::string, TypeRef>> genericBodyTypes;
        std::unordered_map<const FuncDecl *, std::vector<DeferredUnaryCheck>> deferredUnaryChecks;
        std::unordered_map<const FuncDecl *, std::vector<DeferredBinaryCheck>> deferredBinaryChecks;
        std::unordered_map<const FuncDecl *, std::vector<DeferredCastCheck>> deferredCastChecks;
        std::unordered_map<const FuncDecl *, std::vector<DeferredAtomicCheck>> deferredAtomicChecks;
        std::unordered_map<const FuncDecl *, std::vector<DeferredScalarRead>> deferredScalarReads;
        std::unordered_map<const FuncDecl *, std::vector<DeferredOutcomeCheck>> deferredOutcomeChecks;
        std::unordered_map<const FuncDecl *, std::vector<DeferredConsumption>> deferredConsumptions;
        std::unordered_map<const FuncDecl *, std::vector<DeferredMatchCheck>> deferredMatchChecks;
        std::unordered_map<const FuncDecl *, std::vector<TypeRef>> deferredNativeTypes;
        std::unordered_map<const FuncDecl *, std::vector<DeferredMembershipCheck>> deferredMembershipChecks;
        std::unordered_map<const FuncDecl *, std::vector<DeferredGenericCall>> deferredGenericCalls;
        std::unordered_map<const FuncDecl *, std::vector<DeferredEnumInstantiation>> deferredEnumInstantiations;
        /// Instantiations queued while checking dependency bodies and the package's own bodies.
        std::vector<PendingGenericInstantiation> dependencyInstantiations;
        std::vector<PendingGenericInstantiation> userInstantiations;
    };

private:
    /// Queue the destructor of every type a drop glue plan destroys through it, which no checked body may have named.
    void QueueDropGlueDestructors();
    [[nodiscard]] std::size_t ValidatedInstantiationCount() const;
    std::unordered_set<std::string> activeLayoutTypes;

    struct ImportScope {
        const std::unordered_map<std::string, Symbol> *table = nullptr;
        std::string displayName;
        std::string ownerPackage;
        std::string modulePath;
    };

    /// Records `import ...::name;` of a primitive's extension, or reports that the module extends no such primitive.
    void ImportPrimitiveExtension(const UseDecl &declaration, const ImportScope &scope, const PrimitiveInfo &primitive,
                                  const std::string &name);

    /// A generic that instantiates itself at a strictly larger type argument -- `Grow<T>` calling `Grow<*T>` -- never
    /// closes its set of instantiations. Deduplication does not help, because every instantiation is genuinely new.
    /// The compiler used to follow it until the recursive walks over an ever-deeper type exhausted the stack and it
    /// died with no diagnostic at all.
    ///
    /// Bounding the type argument's size catches it: any infinite set of instantiations must produce ever-larger type
    /// arguments, since there are only finitely many small types. Size rather than nesting depth, so a set that grows
    /// in breadth is caught too. The limit is far above anything written by hand -- `char8[..]` is two nodes.
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
    /// Whether `expr` converts to `targetType` under the native conversion rules, recording the accepted route. An
    /// unsuffixed numeric literal targets a sum by its literal kind: exactly one integer (or floating-point) member.
    /// Whether `null` can be a value of `target`: a pointer, or a native form with exactly one pointer to reach.
    [[nodiscard]] static bool NullLiteralReaches(const TypeRef &target);
    bool CanConvertToNativeType(const Expr &expr, const TypeRef &exprType, const TypeRef &targetType);
    /// Whether a native constructor builds a value of `targetType`, checking its operand against the channel it
    /// selects.
    bool CanConstructNative(const NativeConstructExpr &construct, const TypeRef &targetType);
    /// Whether a function returning `returnType` may complete without a written value: a unit result, or a fallible
    /// whose success is the unit.
    [[nodiscard]] static bool CompletesWithoutValue(const TypeRef &returnType) noexcept;
    /// The checks a `return`, with or without `value`, makes against the enclosing function.
    void CheckReturn(const Expr *value, SourceLocation location);
    /// The checks a `fail value` makes: an enclosing fallible function and an operand that fits its error channel.
    void CheckFail(const Expr *value, SourceLocation location);
    /// The checks a `break` or `continue`, with its optional label, makes against the enclosing loops.
    /// Refuses leaving a deferred statement by `keyword`, which would leave a scope already being left.
    void EmitExitFromDeferred(SourceLocation location, std::string_view keyword, std::string help);
    void CheckLoopExit(bool isContinue, const std::string &label, SourceLocation location);
    /// Whether evaluating `expression` never produces a value: a diverging form, or a call to `Panic` or a
    /// no-return function. Such an expression never decides the type of the expression it stands in.
    [[nodiscard]] bool IsDivergingExpression(const Expr &expression) const;

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

    /// Folds an integer the compiler must know, such as an array length or a repeat count: the operators
    /// `EvalConstInt` folds over integer literals of any suffix, integer casts, and named constants, each read through
    /// its own initializer and held to its declared type.
    ///
    /// @param expression the expression to fold, resolved in the current scope
    /// @returns the value, or nullopt when the expression is not a compile-time integer
    /// The value of `sizeof` or `alignof` while folding a constant, laid out in `scope`. None for a type with no layout
    /// yet, such as a type parameter.
    [[nodiscard]] std::optional<std::uint64_t> TypeQueryValue(const TypeQueryExpr &query, Scope &scope) const;
    [[nodiscard]] std::optional<std::int64_t> EvalConstInteger(const Expr &expression) const;
    [[nodiscard]] std::optional<std::int64_t> EvalConstInteger(const Expr &expression, Scope &scope,
                                                               std::unordered_set<const ConstDecl *> &active) const;

    bool ConstantFitsTarget(std::int64_t value, const TypeRef &target) const;

    /// The constant a cast into a character width converts, held at `WideInteger::MaxBits` so an integer literal of
    /// any width is range-checked by its whole magnitude rather than skipped for not fitting a machine word.
    ///
    /// @return nullopt when the operand is not a compile-time character or non-negative integer
    static std::optional<WideInteger> EvalConstCharCastValue(const Expr &expr);

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
    /// Refuses an x86-64 calling convention named by '#Abi' in a build for a machine that has only its own.
    /// Whether a call is to a function that never returns: a '#NoReturn' function or extern, or 'Panic'.
    [[nodiscard]] bool CallNeverReturns(const CallExpr &call) const;
    void CheckAbiForTarget(CallingConvention convention, const std::optional<SourceLocation> &location);
    /// Whether every instruction of an asm body belongs to the target architecture; reports the first that does not.
    bool CheckAsmBodyArchitecture(const FuncDecl &d) const;
    /// Report each name an asm body reaches that is not one of its labels, a function or a module variable.
    void CheckAsmBodySymbols(const FuncDecl &d);

    void CheckStructDecl(const StructDecl &d);

    void CheckEnumDecl(const EnumDecl &d);

    void CheckUnionDecl(const UnionDecl &d);

    void CheckInterfaceDecl(const InterfaceDecl &d);

    /// Check a parameter's default value, which is evaluated at the call and so may not read any of `parameters`.
    TypeRef CheckDefaultValue(const Expr &value, const std::vector<Param> &parameters);
    void CheckImplDecl(const ImplDecl &d);
    [[nodiscard]] std::string UndefinedSliceElement(const ImplDecl &d) const;

    void CheckModuleDecl(const ModuleDecl &d);

    // An element of a constant array must reduce to a literal, since the array
    // is laid out in read-only data rather than evaluated at each use.
    bool IsConstArrayElement(const Expr &e) const;

    void CheckConstDecl(const ConstDecl &d);
    void CheckConstDeclValue(const ConstDecl &d);
    void CheckGlobalDecl(const GlobalDecl &d);
    /// Whether `expression` is a value the compiler computes, as a `const` initializer must be: literals, operators,
    /// casts, type queries, compile-time intrinsics, other constants, and aggregates and cases built from them.
    ///
    /// @param expression the checked initializer, or one part of it
    /// @param constant the constant it initializes, named in the help
    /// @returns false after reporting the first part computed at run time
    bool CheckCompileTimeInitializer(const Expr &expression, const Decl &owner);
    [[nodiscard]] TypeRef CheckNamedConstant(const ConstDecl &declaration);

    static std::string JoinPathSegments(const std::vector<std::string> &path, std::size_t first,
                                        std::size_t lastExclusive);

    static std::string ModulePathForImport(const UseDecl &d);

    static std::string LogicalModulePathForImport(const UseDecl &d);

    static std::string ImportScopeDisplayName(const std::string &pkgName, const std::string &modulePath);

    /// The module an import names: the package-relative `modulePath` in the imported package, else the non-empty
    /// logical path, which repeats the package name, in the importing or the imported package. `ambiguousPackages`
    /// lists every package offering the logical path when more than one does. Reports nothing.
    struct ImportModuleLookup {
        ImportScope scope;
        std::vector<std::string> ambiguousPackages;
    };

    [[nodiscard]] ImportModuleLookup FindImportModule(const std::string &pkgName, const std::string &modulePath,
                                                      const std::string &logicalModulePath) const;

    /// The scope a single, grouped or glob import reads its items from, reporting a missing or ambiguous module.
    ImportScope ResolveImportScope(const UseDecl &d, const std::string &pkgName, const std::string &modulePath);

    /// The module symbol at `path` within `package`, reached segment by segment from the package root, and the first
    /// segment the current package may not name, if any. A package's own modules are always accessible to it.
    struct ModuleWalk {
        const Symbol *module = nullptr;
        const Symbol *inaccessible = nullptr;
    };

    [[nodiscard]] ModuleWalk WalkModulePath(const std::string &package, const std::string &path) const;

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
