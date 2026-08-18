#pragma once

#include "Diagnostics/Diagnostics.h"
#include "Semantic/CompileTimeContext.h"
#include "Semantic/DropGlue.h"
#include "Semantic/Type.h"
#include "Semantic/TypeProperties.h"
#include "Syntax/Ast/Ast.h"

#include <optional>
#include <string>
#include <unordered_map>
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

/// Which concrete method satisfies each operation of one interface bound, for one type argument that satisfied it.
/// Analysis proves a bound at the use site, so the witness is what lets a constrained call be lowered to a direct call
/// per instantiation instead of through a vtable. Entries are ordered by the interface's method declarations.
struct ResolvedConstraintWitness {
    std::string interfaceName;
    std::string typeName;
    std::vector<const FuncDecl *> operations;
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
    std::vector<SemanticDiagnostic> diagnostics;
    std::vector<SemanticSymbol> symbols;
    std::vector<const Module *> modules;
    CompileTimeContext compileTimeContext;

    SemanticModel(std::vector<SemanticDiagnostic> inputDiagnostics, std::vector<SemanticSymbol> inputSymbols,
                  std::vector<const Module *> inputModules, CompileTimeContext inputCompileTimeContext,
                  std::unordered_map<const Expr *, TypeRef> inputExpressionTypes,
                  std::unordered_map<const TypeExpr *, TypeRef> inputTypeNodeTypes,
                  std::unordered_map<const Pattern *, TypeRef> inputPatternTypes,
                  std::unordered_map<const Expr *, ValueConsumption> inputValueConsumptions,
                  std::unordered_map<const CallExpr *, ResolvedCallableBinding> inputCallableBindings,
                  std::unordered_map<const Decl *, ResolvedSymbolIdentity> inputSymbolIdentities,
                  std::unordered_map<const ImplDecl *, ResolvedVtableIdentity> inputVtableIdentities,
                  std::unordered_map<std::string, ResolvedConstraintWitness> inputConstraintWitnesses,
                  std::unordered_map<std::string, ResolvedTypeLayout> inputTypeLayouts,
                  std::unordered_map<std::string, TypeProperties> inputTypeProperties,
                  std::unordered_map<std::string, DropGluePlan> inputDropGluePlans,
                  std::unordered_map<const SizeOfExpr *, std::uint64_t> inputSizeOfValues);

    [[nodiscard]] bool HasErrors() const noexcept;

    /// Returns null when analysis did not accept the node with a resolved type. Returned pointers remain valid for the
    /// lifetime of this model.
    [[nodiscard]] const TypeRef *TryGetType(const Expr &expression) const noexcept;
    [[nodiscard]] const TypeRef *TryGetType(const TypeExpr &typeNode) const noexcept;
    [[nodiscard]] const TypeRef *TryGetType(const Pattern &pattern) const noexcept;

    /// Returns null for Copy expressions and expressions that are only borrowed or observed.
    [[nodiscard]] const ValueConsumption *TryGetConsumption(const Expr &expression) const noexcept;

    /// Returns null for rejected calls and nodes outside the analyzed modules. Returned pointers remain valid for the
    /// lifetime of this model.
    [[nodiscard]] const ResolvedCallableBinding *TryGetCallableBinding(const CallExpr &call) const noexcept;

    /// Returns null for declarations that do not emit/import a symbol and for nodes outside the analyzed modules.
    [[nodiscard]] const ResolvedSymbolIdentity *TryGetSymbolIdentity(const Decl &declaration) const noexcept;

    /// Returns null for extend blocks that do not emit an interface vtable.
    [[nodiscard]] const ResolvedVtableIdentity *TryGetVtableIdentity(const ImplDecl &declaration) const noexcept;

    /// Returns null when no use site proved that this type satisfies this bound, which is also the only case in which
    /// no instantiation needs the witness.
    [[nodiscard]] const ResolvedConstraintWitness *TryGetConstraintWitness(const std::string &interfaceName,
                                                                           const TypeRef &type) const noexcept;

    /// Returns null when the type is unresolved, unsized, recursive, or was not validated in this analysis.
    /// Type-expression queries first use the resolved type fact for that AST node.
    [[nodiscard]] const ResolvedTypeLayout *TryGetLayout(const TypeRef &type) const noexcept;
    [[nodiscard]] const ResolvedTypeLayout *TryGetLayout(const TypeExpr &typeNode) const noexcept;

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
    [[nodiscard]] const std::uint64_t *TryGetSizeOfValue(const SizeOfExpr &expression) const noexcept;

private:
    std::unordered_map<const Expr *, TypeRef> expressionTypes;
    std::unordered_map<const TypeExpr *, TypeRef> typeNodeTypes;
    std::unordered_map<const Pattern *, TypeRef> patternTypes;
    std::unordered_map<const Expr *, ValueConsumption> valueConsumptions;
    std::unordered_map<const CallExpr *, ResolvedCallableBinding> callableBindings;
    std::unordered_map<const Decl *, ResolvedSymbolIdentity> symbolIdentities;
    std::unordered_map<const ImplDecl *, ResolvedVtableIdentity> vtableIdentities;
    std::unordered_map<std::string, ResolvedConstraintWitness> constraintWitnesses;
    std::unordered_map<std::string, ResolvedTypeLayout> typeLayouts;
    std::unordered_map<std::string, TypeProperties> typeProperties;
    std::unordered_map<std::string, DropGluePlan> dropGluePlans;
    std::unordered_map<const SizeOfExpr *, std::uint64_t> sizeOfValues;
};
} // namespace Rux
