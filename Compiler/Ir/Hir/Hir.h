#pragma once

#include "Ir/CaseTypeForm.h"
#include "Lexer/Token.h"
#include "Semantic/DropGlue.h"
#include "Semantic/Type.h"
#include "Semantic/TypeProperties.h"
#include "SourceModel/SourceLocation.h"
#include "Target/AsmInstr.h"
#include "Target/CallingConvention.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Rux {
// Forward declarations
struct HirExpr;
struct HirStmt;
struct HirPattern;
struct HirBlock;

using HirExprPtr = std::unique_ptr<HirExpr>;
using HirStmtPtr = std::unique_ptr<HirStmt>;
using HirPatternPtr = std::unique_ptr<HirPattern>;

/// One compiler-inserted destruction of a named binding. The action is conditional on the binding's drop flag: a
/// successful initialization sets the flag and a consuming expression clears it after evaluating the source value.
struct HirDropAction {
    std::uint64_t bindingId = 0;
    std::string name;
    TypeRef type;
    std::string glueSymbol;
    /// Declaration site retained for cleanup diagnostics and later drop-glue lowering.
    SourceLocation origin;
};

/// A completed subobject that must be destroyed if evaluation of a later aggregate component exits early.
struct HirPartialDropAction {
    enum class Kind {
        Field,
        Element,
        TupleElement,
        EnumPayload,
    };

    Kind kind = Kind::Element;
    std::size_t ordinal = 0;
    std::string name;
    TypeRef type;
    std::string glueSymbol;
    SourceLocation origin;
};

using HirFailureCleanup = std::vector<HirPartialDropAction>;

// HIR Block
struct HirBlock {
    SourceLocation location;
    std::vector<HirStmtPtr> stmts;
};

// HIR Patterns
struct HirPattern {
    SourceLocation location;
    virtual ~HirPattern() = default;
};

// _
struct HirWildcardPattern : HirPattern {};

// 42, "str", true
struct HirLiteralPattern : HirPattern {
    std::string value;
    TypeRef type;
};

// x  (binds the matched value to x)
struct HirBindingPattern : HirPattern {
    std::string name;
    TypeRef type;
    /// Nonzero only when this pattern owns a value whose destruction is scheduled at the enclosing scope exit.
    std::uint64_t bindingId = 0;
};

// lo..hi
struct HirRangePattern : HirPattern {
    HirPatternPtr lo;
    HirPatternPtr hi;
    bool inclusive;
};

// Event.Click(x, y)
struct HirEnumPattern : HirPattern {
    std::vector<std::string> path;
    TypeRef resolvedType;
    CaseTypeForm form = CaseTypeForm::Enumeration;
    std::optional<std::string> discriminant;
    bool hasPayload = false;
    std::vector<std::string> unitDiscriminants;
    std::vector<TypeRef> payloadTypes;
    std::vector<std::size_t> argIndices;
    std::vector<HirPatternPtr> args;
};

// Point { x: 0, y: 0 }
struct HirStructPatternField {
    std::string name;
    HirPatternPtr pattern;
};

struct HirStructPattern : HirPattern {
    std::string typeName;
    TypeRef resolvedType;
    std::vector<HirStructPatternField> fields;
};

// (a, b)
struct HirTuplePattern : HirPattern {
    std::vector<HirPatternPtr> elements;
};

// pattern if guard
struct HirGuardedPattern : HirPattern {
    ~HirGuardedPattern() override;
    HirPatternPtr inner;
    HirExprPtr guard;
};

// HIR Expressions
struct HirExpr {
    TypeRef type;
    SourceLocation location;
    std::optional<ValueConsumptionKind> consumption;
    /// The storage identity invalidated after this expression has been evaluated. Zero denotes a consumed temporary
    /// or aggregate value that is not backed by a named binding. Cleanup lowering uses this identity as a drop flag,
    /// so a path that consumes a local cannot destroy the same value again at scope exit.
    std::uint64_t consumedBindingId = 0;
    virtual ~HirExpr() = default;
};

// 42, 3.14, "hello", 'A', true, null
struct HirLiteralExpr : HirExpr {
    std::string value;
};

// foo, Bar
struct HirVarExpr : HirExpr {
    std::string name;
};

// self
struct HirSelfExpr : HirExpr {};

// a::b::c
struct HirPathExpr : HirExpr {
    std::vector<std::string> segments;
};

// !x, -x, ~x, *x, &x
struct HirUnaryExpr : HirExpr {
    TokenKind op;
    HirExprPtr operand;
};

// i++, i--
struct HirPostfixExpr : HirExpr {
    TokenKind op; // PlusPlus or MinusMinus
    HirExprPtr operand;
};

/// a + b, a && b, a == b, etc.
struct HirBinaryExpr : HirExpr {
    TokenKind op;
    HirExprPtr left;
    HirExprPtr right;
};

/// One payload comparison in a structural variant equality recipe. Aggregate operations recursively compare their
/// elements; custom operations call the concrete `==` implementation selected during semantic analysis.
struct HirVariantEqualityCase;

struct HirVariantEqualityPayload {
    enum class Operation {
        Builtin,
        Custom,
        Variant,
        Tuple,
        Array,
    };

    TypeRef type;
    Operation operation = Operation::Builtin;
    std::string customCallee;
    TypeRef customReceiverType;
    TypeRef customArgumentType;
    std::vector<HirVariantEqualityPayload> elements;
    std::vector<HirVariantEqualityCase> variantCases;
};

struct HirVariantEqualityCase {
    std::string discriminant;
    std::vector<HirVariantEqualityPayload> payloads;
};

/// Case-aware equality evaluates both operands once, compares their tags, and reads only payloads belonging to the
/// shared active case. `negated` derives `!=` from the same traversal.
struct HirVariantEqualityExpr : HirExpr {
    HirExprPtr left;
    HirExprPtr right;
    TypeRef variantType;
    std::vector<HirVariantEqualityCase> cases;
    bool negated = false;
};

/// Recursive recipe for constructing an independent value from a named source place. Trivial leaves are copied as
/// bits; custom leaves call the selected `=` operation; aggregate nodes visit their components in declaration order.
struct HirCopyPlan {
    enum class Kind {
        Trivial,
        Custom,
        Structure,
        Array,
        Tuple,
        Enum,
    };

    Kind kind = Kind::Trivial;
    CaseTypeForm form = CaseTypeForm::Enumeration;
    TypeRef type;
    std::string customCallee;
    std::vector<std::string> componentNames;
    std::vector<HirCopyPlan> components;
    std::vector<std::string> variantDiscriminants;
    std::vector<std::vector<TypeRef>> variantPayloadTypes;
    std::vector<std::vector<HirCopyPlan>> variantComponents;
};

struct HirCopyExpr : HirExpr {
    HirExprPtr value;
    HirCopyPlan plan;
};

/// Recursive recipe for relocating a named value into fresh destination storage. Generated aggregate nodes move each
/// component; custom leaves invoke the selected `<-` operation with the source value exactly once.
struct HirMovePlan {
    enum class Kind {
        Trivial,
        Custom,
        Structure,
        Array,
        Tuple,
        Variant,
    };

    Kind kind = Kind::Trivial;
    CaseTypeForm form = CaseTypeForm::Enumeration;
    TypeRef type;
    std::string customCallee;
    std::vector<std::string> componentNames;
    std::vector<HirMovePlan> components;
    std::vector<std::string> variantDiscriminants;
    std::vector<std::vector<TypeRef>> variantPayloadTypes;
    std::vector<std::vector<HirMovePlan>> variantComponents;
};

struct HirMoveExpr : HirExpr {
    HirExprPtr value;
    HirMovePlan plan;
};

/// a = b, a <- b, a += b, etc.
struct HirAssignExpr : HirExpr {
    TokenKind op;
    HirExprPtr target;
    HirExprPtr value;
    /// For plain assignment, destroy the old target after evaluating `value` and before storing it. A nonzero binding
    /// identity makes the action conditional; the completed store then marks that binding live, allowing assignment to
    /// initialize previously uninitialized or moved storage.
    std::optional<HirDropAction> overwriteCleanup;
    /// Set for a write that must reach memory even though nothing reads it back, which is what zeroizing a secret is.
    bool isVolatile = false;
};

// cond ? thenExpr : elseExpr
struct HirTernaryExpr : HirExpr {
    HirExprPtr condition;
    HirExprPtr thenExpr;
    HirExprPtr elseExpr;
};

// lo..hi
struct HirRangeExpr : HirExpr {
    HirExprPtr lo;
    HirExprPtr hi;
    bool inclusive;
};

// f(a, b, c)
struct HirCallExpr : HirExpr {
    HirExprPtr callee;
    std::vector<HirExprPtr> args;
    bool isNoReturn = false;
    /// Populated for compiler builtins that need to report their call site.
    std::string sourceFile;
    std::string sourceFunction;
    std::uint32_t sourceLine = 0;
    std::uint32_t sourceColumn = 0;
};

/// Wrap a concrete value into an interface fat pointer {data_ptr, vtable_ptr}.
struct HirCoerceToInterfaceExpr : HirExpr {
    HirExprPtr value;
    std::string vtableLabel;
    /// A reference-to-interface view points at the source place directly. A by-value interface keeps the legacy
    /// compatibility behavior of materializing its own concrete storage.
    bool borrowed = false;
};

/// A fixed inline array viewed as a non-owning Slice<T>.
struct HirArrayToSliceExpr : HirExpr {
    HirExprPtr value;
    TypeRef elementType;
    std::uint64_t length = 0;
};

/// Call a method through an interface fat pointer (dynamic dispatch via vtable)
struct HirInterfaceCallExpr : HirExpr {
    HirExprPtr fatPtrExpr; // expression that yields the fat-pointer address
    // (8 bytes)
    int methodIdx = 0; // index of the method in the vtable
    std::vector<HirExprPtr> args;
};

// a[i]
struct HirIndexExpr : HirExpr {
    HirExprPtr object;
    HirExprPtr index;
};

// a.field
struct HirFieldExpr : HirExpr {
    HirExprPtr object;
    std::string field;
};

// Point { x: 1.0, y: 2.0 }
struct HirStructInitField {
    std::string name;
    HirExprPtr value;
};

struct HirStructInitExpr : HirExpr {
    std::string typeName;
    std::vector<HirStructInitField> fields;
    /// Entry i destroys components completed before field i when evaluation of field i exits early.
    std::vector<HirFailureCleanup> failureCleanups;
};

// [a, b, c]
struct HirArrayExpr : HirExpr {
    TypeRef elementType;
    std::vector<HirExprPtr> elements;
    /// Entry i destroys elements [0, i) in reverse order if element i does not finish initialization.
    std::vector<HirFailureCleanup> failureCleanups;
};

// (a, b, c)
struct HirTupleExpr : HirExpr {
    std::vector<HirExprPtr> elements;
    /// Entry i destroys tuple elements [0, i) in reverse order if element i exits early.
    std::vector<HirFailureCleanup> failureCleanups;
};

// expr as Type
struct HirCastExpr : HirExpr {
    HirExprPtr operand;
    TypeRef targetType;
};

// expr is Type
struct HirIsExpr : HirExpr {
    HirExprPtr operand;
    TypeRef checkType;
};

// { stmts; value }  — block used as expression
struct HirBlockExpr : HirExpr {
    ~HirBlockExpr() override;
    HirBlock block;

    /// The expression the block evaluates to, when it has one. A block lowered from source has none — Rux blocks are
    /// statements — but a lowering that needs to bind an operand before using it twice builds one that does, so the
    /// operand is evaluated once and named twice.
    HirExprPtr value;
};

struct HirMatchArm {
    SourceLocation location;
    HirPatternPtr pattern;
    HirExprPtr body;
    /// Pattern bindings are destroyed after the arm body has produced its value.
    std::vector<HirDropAction> cleanups;
};

// match expr { pat => expr, ... }
struct HirMatchExpr : HirExpr {
    HirExprPtr subject;
    std::vector<HirMatchArm> arms;
};

struct HirEnumConstructExpr : HirExpr {
    CaseTypeForm form = CaseTypeForm::Enumeration;
    std::vector<HirExprPtr> payloads;
    std::string discriminant;
    /// Entry i destroys payloads [0, i) in reverse order if payload i exits early.
    std::vector<HirFailureCleanup> failureCleanups;
};

// HIR Statements

struct HirStmt {
    SourceLocation location;
    virtual ~HirStmt() = default;
};

// expr;
struct HirExprStmt : HirStmt {
    HirExprPtr expr;
};

// let name: Type = expr; or var name: Type = expr;
struct HirLetStmt : HirStmt {
    bool isMut = false;
    std::string name;
    HirPatternPtr pattern;
    TypeRef type;
    HirExprPtr init;
    /// Identity of the conditional drop flag established by this declaration, or zero for a Copy binding.
    std::uint64_t bindingId = 0;
};

// Compiler-inserted cleanup on ordinary block fallthrough.
struct HirDropStmt : HirStmt {
    HirDropAction action;
};

// if cond { } else if cond { } else { }
struct HirIfStmt : HirStmt {
    struct ElseIf {
        SourceLocation location;
        HirExprPtr condition;
        HirBlock block;
    };

    HirExprPtr condition;
    HirBlock thenBlock;
    std::vector<ElseIf> elseIfs;
    std::optional<HirBlock> elseBlock;
};

// while cond { }
struct HirWhileStmt : HirStmt {
    std::string label;
    HirExprPtr condition;
    HirBlock body;
};

// do { } while cond;
struct HirDoWhileStmt : HirStmt {
    std::string label;
    HirBlock body;
    HirExprPtr condition;
};

// loop { }
struct HirLoopStmt : HirStmt {
    std::string label;
    HirBlock body;
};

// for var in iterable { }
struct HirForStmt : HirStmt {
    std::string label;
    std::string variable;
    TypeRef varType;
    HirExprPtr iterable;
    HirBlock body;
    /// Nonzero for an owned induction value; every iteration marks this flag live before entering the body.
    std::uint64_t bindingId = 0;
    /// True when `variable` names a mutable variable already in scope, which the loop reuses as its induction variable
    /// rather than introducing a fresh binding. The loop then mutates that outer variable, so its final value persists
    /// after the loop (e.g. `for k in k..7` leaves k == 7).
    bool reusesOuterVar = false;
};

struct HirMatchStmt : HirStmt {
    HirExprPtr subject;
    std::vector<HirMatchArm> arms;
};

/// A nested scope, which source has no syntax for. A desugaring that needs a binding of its own -- the iterator a `for`
/// over a user-written container advances -- puts it here so the binding lives exactly as long as the construct does.
struct HirScopeStmt : HirStmt {
    HirBlock block;
};

// return [expr];
struct HirReturnStmt : HirStmt {
    std::optional<HirExprPtr> value;
    /// Cleanups run after evaluating `value` and clearing its consumed binding, but before control leaves the function.
    std::vector<HirDropAction> cleanups;
};

// break [label];
struct HirBreakStmt : HirStmt {
    std::string label;
    std::vector<HirDropAction> cleanups;
};

// continue [label];
struct HirContinueStmt : HirStmt {
    std::string label;
    std::vector<HirDropAction> cleanups;
};

/// local declaration inside a block (func, const, type alias declared locally)
struct HirLocalDecl : HirStmt {
    std::string description;
    bool hasConstant = false;
    std::string constantName;
    TypeRef constantType;
    HirExprPtr constantValue;
};

// HIR Declarations
struct HirParam {
    std::string name;
    TypeRef type;
    bool isVariadic = false;
    /// Identity of an initially live parameter drop flag; Copy and borrowed parameters retain zero.
    std::uint64_t bindingId = 0;
};

// func [asm] Name<T>(params) -> RetType { body }
struct HirFunc {
    std::string name;
    bool isPublic = false;
    bool isAsm = false;
    bool isNoReturn = false;
    CallingConvention callConv = CallingConvention::Default;
    std::vector<std::string> typeParams;
    std::vector<HirParam> params;
    TypeRef returnType;
    std::optional<HirBlock> body;  // absent for interface/extern signatures
    std::vector<AsmInstr> asmBody; // instructions when isAsm; body is absent
    SourceLocation location;
};

// struct Name { field: Type; ... }
struct HirStructField {
    std::string name;
    bool isPublic = false;
    TypeRef type;
};

struct HirStruct {
    std::string name;
    bool isPublic = false;
    std::vector<std::string> typeParams;
    std::vector<HirStructField> fields;
    SourceLocation location;
};

// enum Name { Variant, Variant(Type, ...) }
struct HirEnumVariant {
    std::string name;
    std::vector<TypeRef> fields;
    std::optional<std::string> discriminant;
};

struct HirEnum {
    CaseTypeForm form = CaseTypeForm::Enumeration;
    std::string name;
    bool isPublic = false;
    std::vector<std::string> typeParams;
    TypeRef baseType;
    std::vector<HirEnumVariant> variants;
    SourceLocation location;

    [[nodiscard]] bool IsVariant() const noexcept {
        return form == CaseTypeForm::Variant;
    }
};

// union Name { field: Type, ... }
struct HirUnionField {
    std::string name;
    TypeRef type;
};

struct HirUnion {
    std::string name;
    bool isPublic = false;
    std::vector<HirUnionField> fields;
    SourceLocation location;
};

// interface Name { func Sig(); ... }
struct HirInterfaceMethod {
    std::string name;
    std::vector<HirParam> params;
    TypeRef returnType;
    SourceLocation location;
};

struct HirInterface {
    std::string name;
    bool isPublic = false;
    std::vector<HirInterfaceMethod> methods;
    SourceLocation location;
};

// extend TypeName [for InterfaceName] { func ... }
struct HirImplBlock {
    std::string typeName;
    std::optional<std::string> interfaceName;
    std::vector<HirFunc> methods;
    std::vector<std::string> methodLinkerNames;
    std::string vtableLabel;
    std::vector<std::string> vtableEntries;
    SourceLocation location;
};

// const Name: Type = expr;
struct HirConst {
    std::string name;
    bool isPublic = false;
    TypeRef type;
    HirExprPtr value;
    SourceLocation location;
};

// extern func Name(params) -> Type from "DLL";
struct HirExternFunc {
    std::string name;
    std::string dll;
    /// The name imported from the DLL, when it differs from `name`. Empty means the two are the same.
    std::string symbolName;
    bool isPublic = false;
    bool isNoReturn = false;
    CallingConvention callConv = CallingConvention::Default;
    std::vector<HirParam> params;
    bool isVariadic = false;
    TypeRef returnType;
    SourceLocation location;
};

// extern Name: Type;
struct HirExternVar {
    std::string name;
    bool isPublic = false;
    TypeRef type;
    SourceLocation location;
};

// type Name = Type;
struct HirTypeAlias {
    std::string name;
    bool isPublic = false;
    TypeRef type;
    SourceLocation location;
};

// HIR Module
struct HirModule {
    std::string name;
    std::vector<HirFunc> funcs;
    std::vector<HirStruct> structs;
    std::vector<HirEnum> enums;
    std::vector<HirUnion> unions;
    std::vector<HirInterface> interfaces;
    std::vector<HirImplBlock> impls;
    std::vector<HirConst> consts;
    std::vector<HirExternFunc> externFuncs;
    std::vector<HirExternVar> externVars;
    std::vector<HirTypeAlias> typeAliases;
};

struct HirPackage {
    std::vector<HirModule> modules;
    std::vector<DropGluePlan> dropGlues;
};

inline HirGuardedPattern::~HirGuardedPattern() = default;
inline HirBlockExpr::~HirBlockExpr() = default;
} // namespace Rux
