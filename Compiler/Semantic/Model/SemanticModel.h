#pragma once

#include "Diagnostics/Diagnostics.h"
#include "Semantic/Model/CompileTimeContext.h"
#include "Syntax/Ast/Ast.h"
#include "Types/DropGlue.h"
#include "Types/NativeConversion.h"
#include "Types/Type.h"
#include "Types/TypeProperties.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Rux {
using SemanticDiagnostic = Diagnostic;

/// One module-level name analysis recorded, flattened for reporting rather than for lookup. Every global that was
/// successfully defined lands here, so this is the module's declared surface and not a visibility-filtered export list.
/// The scope tables resolution actually reads live in `SemanticProgramIndex`.
struct SemanticSymbol {
    enum class Kind {
        Var,
        Func,
        Type,
        Const,
        Module,
        Interface,
    };

    Kind kind = Kind::Var;
    std::string name;
    std::string sourceName;
    SourceLocation location;
    std::string resolvedType;
    bool isMut = false;
};

/// The callable selected for an accepted CallExpr. Declaration pointers refer into the analyzed AST and therefore have
/// the same lifetime requirements as the model's node-keyed type facts.
struct ResolvedCallableBinding {
    enum class DispatchKind {
        Direct,
        Method,
        Constructor,
        Interface,
        Indirect,
        EnumVariant,
        /// An operation of an interface bound, called on a value whose type is a generic parameter. The target is only
        /// known per instantiation, so the selected declaration is the interface's method and the concrete one comes
        /// from the constraint witness recorded for the substituted type.
        Constrained,
    };

    DispatchKind dispatch = DispatchKind::Indirect;
    const Decl *selectedDeclaration = nullptr;
    const EnumDecl::Variant *selectedVariant = nullptr;
    /// Source form of the case constructor selected above. Keeping this beside the selected case prevents generic
    /// instantiation and lowering from reconstructing the kind from whether that case happens to carry a payload.
    EnumDecl::Form caseTypeForm = EnumDecl::Form::Enumeration;
    std::unordered_map<std::string, TypeRef> substitutions;
    std::optional<TypeRef> receiverType;
    std::optional<std::size_t> variadicBoundary;
    CallingConvention callingConvention = CallingConvention::Default;
    std::string importedSymbolOverride;
    /// Final linker-visible name for direct calls. Interface and indirect dispatch do not have one statically selected
    /// target.
    std::string linkerName;
    /// A semantic identity recipe for calls inside generic declarations. The recorded linkerName is the identity in the
    /// declaration's symbolic context; lowering supplies the concrete substitutions of each emitted instance without
    /// recreating overload or mangling rules.
    std::string linkerNameBase;
    bool linkerNameHasOverloadSignature = false;
    std::vector<TypeRef> linkerOverloadTypes;
    std::vector<std::string> linkerSpecializationParameters;
    /// Constrained dispatch only: the bound whose operation is called, and that operation's slot in the interface.
    std::string constraintInterface;
    std::size_t constraintOperationIndex = 0;

    [[nodiscard]] std::string
    LinkerNameFor(const std::unordered_map<std::string, TypeRef> &concreteSubstitutions) const;
    [[nodiscard]] ResolvedCallableBinding
    Instantiate(const std::unordered_map<std::string, TypeRef> &contextSubstitutions) const;
};

/// A typed local declaration whose omitted initializer resolves to the type's accessible zero-argument constructor.
/// Lowering synthesizes the call from this selected declaration rather than repeating constructor lookup.
struct ResolvedDefaultConstructor {
    const FuncDecl *declaration = nullptr;
    TypeRef type;
};

/// The nominal case selected by one accepted case pattern. Lowering consumes this instead of repeating name lookup or
/// inferring whether the declaration is an enum or variant from its payload shape.
struct ResolvedCasePattern {
    const EnumDecl *declaration = nullptr;
    const EnumDecl::Variant *selectedCase = nullptr;
    EnumDecl::Form form = EnumDecl::Form::Enumeration;
    TypeRef subjectType;
    std::unordered_map<std::string, TypeRef> substitutions;
};

/// How a binding under a borrowed native match subject refers to the payload it names. An alias is the payload in
/// place, at its offset inside the subject; a view reads several members of a sum through the subject's own tag.
enum class PatternBindingMode {
    Alias,
    View,
};

struct VariantEqualityPayload {
    enum class Operation {
        Builtin,
        Custom,
        Variant,
        Tuple,
        /// A native sum, optional, or fallible: the tags first, then the active case's payload. Each element is one
        /// case's payload plan, with its case tag in `index`; an optional's absent case has no payload and no element.
        Native,
        Structure,
        Array,
        Deferred,
    };

    std::size_t index = 0;
    std::string name;
    TypeRef type;
    Operation operation = Operation::Builtin;
    const FuncDecl *customEquality = nullptr;
    std::string nestedVariantType;
    std::vector<VariantEqualityPayload> elements;
};

struct VariantEqualityCase {
    std::string name;
    std::string discriminant;
    std::vector<VariantEqualityPayload> payloads;
};

/// A reusable structural comparison recipe for one concrete variant type.
struct VariantEqualityPlan {
    const EnumDecl *declaration = nullptr;
    TypeRef type;
    std::vector<VariantEqualityCase> cases;
};

/// The structural recipe selected for one accepted variant equality expression.
struct ResolvedVariantEquality {
    TypeRef type;
    bool negated = false;
};

/// Which concrete method satisfies each operation of one interface bound, for one type argument that satisfied it.
/// Analysis proves a bound at the use site, so the witness is what lets a constrained call be lowered to a direct call
/// per instantiation instead of through a vtable. Entries are ordered by the interface's method declarations.
struct ResolvedConstraintWitness {
    std::string interfaceName;
    std::string typeName;
    std::vector<const FuncDecl *> operations;
};

/// The `[]` operator one accepted index expression resolved to. Built-in array, slice, and pointer indexing records
/// nothing; only an index expression that reached a source-declared indexer has this fact. Analysis picks the overload
/// from the index type and substitutes the receiver's type arguments, so lowering builds the call from this rather
/// than resolving the operator again. Its presence is also what tells analysis the expression is a call producing a
/// value, not a place projection into the receiver.
struct ResolvedIndexOperator {
    const FuncDecl *method = nullptr;
    TypeRef receiverType;
    TypeRef indexType;
    TypeRef resultType;
};

/// The `[]=` operator one accepted indexed assignment resolved to. Analysis picks the overload from the index type and
/// checks the assigned value against the setter's value parameter, so lowering builds the call from this rather than
/// resolving the operator again. `v[i] = x` is that call and nothing else: there is no store, and the index expression
/// it was written as is never lowered as a value.
struct ResolvedIndexAssignment {
    const FuncDecl *method = nullptr;
    TypeRef receiverType;
    TypeRef indexType;
    TypeRef valueType;
};

/// What one accepted `expr?` propagates. Analysis checks a native fallible or optional against the enclosing return
/// type: a fallible's success continues and its error leaves as the enclosing outer failure, injected or widened into
/// that error channel, and an optional's payload continues while its absence leaves.
struct ResolvedPropagation {
    /// What the expression evaluates to, and the error a fallible's early return carries. Absence carries no payload,
    /// so an optional leaves the second unset.
    TypeRef payloadType;
    std::optional<TypeRef> failureType;
    TypeRef returnType;
};

/// What one accepted `option ?? fallback` coalesces. Analysis checks the native optional and fixes the payload type,
/// so lowering only has to build the lazy match it describes.
struct ResolvedCoalescing {
    TypeRef payloadType;
};

/// How one accepted `for` loop reads its subject. Analysis decides whether the subject is driven directly or through
/// the iterator convention and which methods drive it, so lowering builds the loop from this rather than deciding
/// again.
struct ResolvedIteration {
    enum class Kind {
        Range,
        Indexed,
        Iterator,
        Iterable
    };

    Kind kind = Kind::Indexed;
    TypeRef itemType;
    /// Convention-driven loops only: the iterator the loop advances, the `Next` that advances it, and the `Iterate`
    /// that produced it when the subject was a container rather than an iterator itself.
    TypeRef iteratorType;
    const FuncDecl *advance = nullptr;
    const FuncDecl *entry = nullptr;
    /// The native optional `Next` reports: presence continues the loop and outer absence ends it.
    TypeRef reportedType;
};

/// The key analysis and lowering both use to name one proven bound. A pointer receiver and a generic instantiation
/// reduce to the type that owns the methods, so `*Cell<int>` and `Cell<int>` name the same witness.
[[nodiscard]] std::string ConstraintWitnessKey(const std::string &interfaceName, const TypeRef &type);

/// Final linker-visible identity of a declaration that emits or imports a symbol. Accepted generic calls record their
/// concrete instance separately.
struct ResolvedSymbolIdentity {
    std::string linkerName;
};

/// Final identity and slot targets of an emitted interface vtable.
struct ResolvedVtableIdentity {
    std::string interfaceName;
    std::string linkerName;
    std::vector<std::string> entries;
};

/// Target-specific compile-time layout of a fully resolved type. Layout facts are only published after every component
/// type has a valid, finite layout.
struct ResolvedTypeLayout {
    std::uint64_t size = 0;
    std::uint64_t alignment = 1;
};

/// A by-value expression whose ownership transfers to a new storage location or callable. Lowering uses this fact to
/// avoid treating a move as an implicit clone and later cleanup passes use it to suppress destruction of the source.
struct ValueConsumption {
    ValueConsumptionKind kind;
    TypeRef type;
    SourceLocation location;
    const FuncDecl *customOperation = nullptr;
    bool constructsDestination = false;
};

/// A named place copied into a by-value destination. Generated copies are represented without a declaration; custom
/// copies in concrete code retain the exact special operation selected by analysis so lowering does not repeat
/// overload resolution. A record made for a generic body keeps the unsubstituted type and no operation instead,
/// because it is shared by every instantiation: each one substitutes its own type argument and resolves its own
/// operation when its copy plan is built.
struct ValueCopy {
    ValueConsumptionKind kind;
    TypeRef targetType;
    const FuncDecl *customOperation = nullptr;
    SourceLocation location;
};

/// Accepted node-keyed facts. The analyzer fills one instance and transfers it into the immutable model.
/// Node addresses continue to refer to the caller-owned AST throughout analysis and lowering.
struct EvaluatedAssociatedConstant {
    TypeRef type;
    std::string literal;
};

struct SemanticFacts {
    std::unordered_map<const TypeExpr *, const Decl *> intrinsicTypeBindings;
    std::unordered_map<const Expr *, const ConstDecl *> associatedConstants;
    std::unordered_map<const Expr *, const ConstDecl *> constantReferences;
    std::unordered_map<const ConstDecl *, EvaluatedAssociatedConstant> evaluatedAssociatedConstants;
    std::unordered_map<const Expr *, TypeRef> expressionTypes;
    /// Accepted implicit loads of Copy primitive scalars. The expression's type still describes its reference.
    std::unordered_set<const Expr *> borrowedScalarReads;
    /// Names of references written through rather than rebound: an assignment, compound assignment, `++`, or `--` of a
    /// reference to a Copy primitive scalar, and a `=` or `<-` that replaces any other referent whole. The store
    /// reaches the referent rather than the binding. The name's type still describes its reference.
    std::unordered_set<const Expr *> referenceWrites;
    /// The one accepted route of each implicit conversion into a native type, outermost level first, keyed by the
    /// converted expression. Lowering builds the destination value from it instead of choosing a route again.
    std::unordered_map<const Expr *, std::vector<NativeConversionStep>> nativeConversions;
    std::unordered_map<const TypeExpr *, TypeRef> typeNodeTypes;
    std::unordered_map<const Pattern *, TypeRef> patternTypes;
    std::unordered_map<const EnumPattern *, ResolvedCasePattern> casePatterns;
    /// The resolved annotation of each accepted typed pattern, before generic substitution. What it selects follows
    /// from the substituted subject and annotation through `ClassifyNativeSelection`.
    std::unordered_map<const TypedPattern *, TypeRef> typedPatternTypes;
    /// The sum member a qualified case, struct, literal, or tuple pattern selects, keyed by that pattern; the pattern's
    /// own recorded type stays the sum.
    std::unordered_map<const Pattern *, TypeRef> sumMemberPatterns;
    /// The binding mode of each binding pattern under a borrowed native match subject; owned subjects bind values.
    std::unordered_map<const Pattern *, PatternBindingMode> patternBindingModes;
    std::unordered_map<const BinaryExpr *, ResolvedVariantEquality> variantEqualities;
    std::unordered_map<std::string, VariantEqualityPlan> variantEqualityPlans;
    // An expression records only whether to negate; each concrete instantiation owns its element recipe.
    std::unordered_map<const BinaryExpr *, bool> aggregateEqualities;
    /// The one type both operands of a structural tuple comparison are built as, when one operand is a literal that
    /// takes the other's type, as in `pair == (3, true)`.
    std::unordered_map<const BinaryExpr *, TypeRef> aggregateEqualityOperandTypes;
    std::unordered_map<std::string, VariantEqualityPayload> aggregateEqualityPlans;
    std::unordered_map<const Expr *, ValueConsumption> valueConsumptions;
    std::unordered_map<const Expr *, ValueCopy> valueCopies;
    std::unordered_map<const CallExpr *, ResolvedCallableBinding> callableBindings;
    std::unordered_map<const LetStmt *, ResolvedDefaultConstructor> defaultConstructors;
    std::unordered_map<const Decl *, bool> effectiveVisibilities;
    /// Effectively public declarations owned by the package being compiled, rather than by one of its dependencies.
    std::unordered_set<const Decl *> exportedDeclarations;
    std::unordered_map<const Decl *, ResolvedSymbolIdentity> symbolIdentities;
    std::unordered_map<const ImplDecl *, ResolvedVtableIdentity> vtableIdentities;
    std::unordered_map<std::string, ResolvedConstraintWitness> constraintWitnesses;
    std::unordered_map<const TryExpr *, ResolvedPropagation> propagations;
    std::unordered_map<const MappedTryExpr *, ResolvedPropagation> mappedPropagations;
    std::unordered_map<const BinaryExpr *, ResolvedCoalescing> coalescings;
    std::unordered_map<const IndexExpr *, ResolvedIndexOperator> indexOperators;
    std::unordered_map<const IndexExpr *, ResolvedIndexAssignment> indexAssignments;
    std::unordered_map<const ForStmt *, ResolvedIteration> iterations;
    /// Every accepted `match` whose arms cover each value of its subject's type, so no value can leave it unmatched.
    std::unordered_set<const MatchExpr *> exhaustiveMatchExpressions;
    std::unordered_set<const MatchStmt *> exhaustiveMatchStatements;
    /// Every accepted `match` or `catch` subject whose arms own it: one handed over with `<-`, a temporary, or the
    /// copy a by-value match makes of a named copyable value. An arm owns what it binds and destroys what it leaves.
    std::unordered_set<const Expr *> ownedMatchSubjects;
    std::unordered_map<std::string, ResolvedTypeLayout> typeLayouts;
    std::unordered_map<std::string, TypeProperties> typeProperties;
    std::unordered_map<std::string, DropGluePlan> dropGluePlans;
    std::unordered_map<const TypeQueryExpr *, std::uint64_t> typeQueryValues;
};

/**
 * @brief Persistent output of semantic analysis.
 *
 * Besides diagnostics and exported symbols it owns the ordered, validated module view and resolved type facts consumed
 * by lowering.
 *
 * The model does not own the AST: every Module supplied to SemanticAnalyzer must outlive the model and remain unchanged
 * while its node-keyed facts are queried. Facts are keyed by node address, so moving or rebuilding a node silently
 * detaches everything analysis recorded about it.
 */
struct SemanticModel {
    [[nodiscard]] const Decl *TryGetIntrinsicTypeBinding(const TypeExpr &type) const noexcept;
    [[nodiscard]] const ConstDecl *TryGetAssociatedConstant(const Expr &expression) const noexcept;
    [[nodiscard]] const ConstDecl *TryGetConstantReference(const Expr &expression) const noexcept;
    [[nodiscard]] const EvaluatedAssociatedConstant *TryGetConstantValue(const ConstDecl &declaration) const noexcept;
    std::vector<SemanticDiagnostic> diagnostics;
    std::vector<SemanticSymbol> symbols;
    std::vector<const Module *> modules;
    CompileTimeContext compileTimeContext;

    SemanticModel(std::vector<SemanticDiagnostic> inputDiagnostics, std::vector<SemanticSymbol> inputSymbols,
                  std::vector<const Module *> inputModules, CompileTimeContext inputCompileTimeContext,
                  SemanticFacts inputFacts);

    [[nodiscard]] bool HasErrors() const noexcept;

    /// Returns null when analysis did not accept the node with a resolved type. Returned pointers remain valid for the
    /// lifetime of this model.
    [[nodiscard]] const TypeRef *TryGetType(const Expr &expression) const noexcept;
    [[nodiscard]] bool HasBorrowedScalarRead(const Expr &expression) const noexcept;
    [[nodiscard]] bool HasReferenceWrite(const Expr &expression) const noexcept;
    /// Returns null when `expression` reaches its destination without a native conversion.
    [[nodiscard]] const std::vector<NativeConversionStep> *
    TryGetNativeConversion(const Expr &expression) const noexcept;
    [[nodiscard]] const TypeRef *TryGetType(const TypeExpr &typeNode) const noexcept;
    [[nodiscard]] const TypeRef *TryGetType(const Pattern &pattern) const noexcept;

    /// Returns null for non-case patterns and case patterns rejected during semantic analysis.
    [[nodiscard]] const ResolvedCasePattern *TryGetCasePattern(const EnumPattern &pattern) const noexcept;
    /// Returns null for a typed pattern whose annotation did not resolve.
    [[nodiscard]] const TypeRef *TryGetTypedPatternType(const TypedPattern &pattern) const noexcept;
    /// Returns null unless the pattern selects one member of a sum subject.
    [[nodiscard]] const TypeRef *TryGetSumMember(const Pattern &pattern) const noexcept;
    /// Returns null for a binding of an owned subject, which binds a value rather than referring to the subject.
    [[nodiscard]] const PatternBindingMode *TryGetPatternBindingMode(const Pattern &pattern) const noexcept;

    [[nodiscard]] const ResolvedVariantEquality *TryGetVariantEquality(const BinaryExpr &expression) const noexcept;
    [[nodiscard]] const VariantEqualityPlan *TryGetVariantEqualityPlan(const TypeRef &type) const noexcept;
    [[nodiscard]] const bool *TryGetAggregateEquality(const BinaryExpr &expression) const noexcept;
    [[nodiscard]] const TypeRef *TryGetAggregateEqualityOperandType(const BinaryExpr &expression) const noexcept;
    [[nodiscard]] const VariantEqualityPayload *TryGetAggregateEqualityPlan(const TypeRef &type) const noexcept;

    /// Returns null for Copy expressions and expressions that are only borrowed or observed.
    [[nodiscard]] const ValueConsumption *TryGetConsumption(const Expr &expression) const noexcept;

    /// Returns null for direct temporary transfers, moves, borrows, and observations.
    [[nodiscard]] const ValueCopy *TryGetCopy(const Expr &expression) const noexcept;

    /// Returns null for rejected calls and nodes outside the analyzed modules. Returned pointers remain valid for the
    /// lifetime of this model.
    [[nodiscard]] const ResolvedCallableBinding *TryGetCallableBinding(const CallExpr &call) const noexcept;

    /// Returns null for initialized declarations and declarations whose type has no eligible default constructor.
    [[nodiscard]] const ResolvedDefaultConstructor *TryGetDefaultConstructor(const LetStmt &statement) const noexcept;

    /// Whether a declaration is public after every containing module and owning type has capped its visibility.
    [[nodiscard]] bool IsEffectivelyPublic(const Decl &declaration) const noexcept;

    /// Whether a library artifact exports the declaration: it is effectively public and owned by the package being
    /// compiled. A dependency's public declaration is compiled into the same artifact and stays linkable across its
    /// objects, but is not part of the artifact's interface.
    [[nodiscard]] bool IsExported(const Decl &declaration) const noexcept;

    /// Returns null for declarations that do not emit/import a symbol and for nodes outside the analyzed modules.
    [[nodiscard]] const ResolvedSymbolIdentity *TryGetSymbolIdentity(const Decl &declaration) const noexcept;

    /// Returns null for extend blocks that do not emit an interface vtable.
    [[nodiscard]] const ResolvedVtableIdentity *TryGetVtableIdentity(const ImplDecl &declaration) const noexcept;

    /// Returns null when no use site proved that this type satisfies this bound, which is also the only case in which
    /// no instantiation needs the witness.
    [[nodiscard]] const ResolvedConstraintWitness *TryGetConstraintWitness(const std::string &interfaceName,
                                                                           const TypeRef &type) const noexcept;

    /// Returns null for a rejected `?`, which has no early return to build.
    [[nodiscard]] const ResolvedPropagation *TryGetPropagation(const TryExpr &expression) const noexcept;
    /// Returns null for a rejected `? else`, which has no early return to build.
    [[nodiscard]] const ResolvedPropagation *TryGetPropagation(const MappedTryExpr &expression) const noexcept;

    /// Returns null for a rejected `??` and for every ordinary binary expression.
    [[nodiscard]] const ResolvedCoalescing *TryGetCoalescing(const BinaryExpr &expression) const noexcept;

    /// Returns null for built-in array, slice, and pointer indexing, which needs no declared operator.
    [[nodiscard]] const ResolvedIndexOperator *TryGetIndexOperator(const IndexExpr &expression) const noexcept;

    /// Returns null unless this index expression is the target of an assignment that resolved to a declared `[]=`.
    [[nodiscard]] const ResolvedIndexAssignment *TryGetIndexAssignment(const IndexExpr &expression) const noexcept;

    /// Returns null for a `for` loop whose subject analysis rejected as not iterable.
    [[nodiscard]] const ResolvedIteration *TryGetIteration(const ForStmt &statement) const noexcept;

    /// Whether analysis accepted this `match` as covering every value of its subject's type. A value outside those
    /// the type declares, such as an integer converted to an enum with `as`, then has no arm to fall to.
    [[nodiscard]] bool IsExhaustiveMatch(const MatchExpr &expression) const noexcept;
    [[nodiscard]] bool IsExhaustiveMatch(const MatchStmt &statement) const noexcept;
    /// Whether the arms of the `match` or `catch` that reads `subject` own it, so lowering gives each binding its
    /// cleanup and destroys whatever the selected arm leaves; a subject left with its owner or borrowed answers false.
    [[nodiscard]] bool IsOwnedMatchSubject(const Expr &subject) const noexcept;

    /// Returns null when the type is unresolved, unsized, recursive, or was not validated in this analysis.
    /// Type-expression queries first use the resolved type fact for that AST node.
    [[nodiscard]] const ResolvedTypeLayout *TryGetLayout(const TypeRef &type) const noexcept;
    [[nodiscard]] const ResolvedTypeLayout *TryGetLayout(const TypeExpr &typeNode) const noexcept;
    [[nodiscard]] const std::unordered_map<std::string, ResolvedTypeLayout> &TypeLayouts() const noexcept;

    /// Returns null only when analysis never encountered the type. Unresolved generic declarations retain an explicit
    /// property record whose mobility is Unresolved.
    [[nodiscard]] const TypeProperties *TryGetProperties(const TypeRef &type) const noexcept;
    [[nodiscard]] const TypeProperties *TryGetProperties(const Expr &expression) const noexcept;
    [[nodiscard]] const TypeProperties *TryGetProperties(const TypeExpr &typeNode) const noexcept;
    [[nodiscard]] const TypeProperties *TryGetProperties(const Pattern &pattern) const noexcept;

    /// Returns the preordered destruction recipe for a concrete droppable type, or null for Copy/unresolved types.
    [[nodiscard]] const DropGluePlan *TryGetDropGlue(const TypeRef &type) const noexcept;

    /// Returns every synthesized recipe keyed by concrete type spelling. Entries remain valid for this model's life.
    [[nodiscard]] const std::unordered_map<std::string, DropGluePlan> &DropGluePlans() const noexcept;

    /// Returns the constant folded for an accepted sizeof expression. Rejected sizeof expressions deliberately have no
    /// usable value.
    [[nodiscard]] const std::uint64_t *TryGetTypeQueryValue(const TypeQueryExpr &expression) const noexcept;

private:
    SemanticFacts facts;
};
} // namespace Rux
