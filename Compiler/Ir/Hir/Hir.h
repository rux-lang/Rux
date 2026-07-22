#pragma once

#include "Lexer/Token.h"
#include "Semantic/Type.h"
#include "Source/SourceLocation.h"
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
    std::optional<std::string> discriminant;
    bool hasPayload = false;
    std::vector<std::string> unitDiscriminants;
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

// a + b, a && b, a == b, etc.
struct HirBinaryExpr : HirExpr {
    TokenKind op;
    HirExprPtr left;
    HirExprPtr right;
};

// a = b, a += b, etc.
struct HirAssignExpr : HirExpr {
    TokenKind op;
    HirExprPtr target;
    HirExprPtr value;
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
    // Populated for compiler builtins that need to report their call site.
    std::string sourceFile;
    std::string sourceFunction;
    std::uint32_t sourceLine = 0;
    std::uint32_t sourceColumn = 0;
};

// Wrap a concrete value into an interface fat pointer {data_ptr,
// vtable_ptr}
struct HirCoerceToInterfaceExpr : HirExpr {
    HirExprPtr value;
    std::string vtableLabel;
};

// A fixed inline array viewed as a non-owning Slice<T>.
struct HirArrayToSliceExpr : HirExpr {
    HirExprPtr value;
    TypeRef elementType;
    std::uint64_t length = 0;
};

// Call a method through an interface fat pointer (dynamic dispatch via
// vtable)
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
};

// [a, b, c]
struct HirArrayExpr : HirExpr {
    TypeRef elementType;
    std::vector<HirExprPtr> elements;
};

// (a, b, c)
struct HirTupleExpr : HirExpr {
    std::vector<HirExprPtr> elements;
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
};

struct HirMatchArm {
    SourceLocation location;
    HirPatternPtr pattern;
    HirExprPtr body;
};

// match expr { pat => expr, ... }
struct HirMatchExpr : HirExpr {
    HirExprPtr subject;
    std::vector<HirMatchArm> arms;
};

struct HirEnumConstructExpr : HirExpr {
    std::vector<HirExprPtr> payloads;
    std::string discriminant;
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
    // True when `variable` names a mutable variable already in scope, which the
    // loop reuses as its induction variable rather than introducing a fresh
    // binding. The loop then mutates that outer variable, so its final value
    // persists after the loop (e.g. `for k in k..7` leaves k == 7).
    bool reusesOuterVar = false;
};

struct HirMatchStmt : HirStmt {
    HirExprPtr subject;
    std::vector<HirMatchArm> arms;
};

// return [expr];
struct HirReturnStmt : HirStmt {
    std::optional<HirExprPtr> value;
};

// break [label];
struct HirBreakStmt : HirStmt {
    std::string label;
};

// continue [label];
struct HirContinueStmt : HirStmt {
    std::string label;
};

// local declaration inside a block (func, const, type alias declared
// locally)
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
    std::string name;
    bool isPublic = false;
    std::vector<std::string> typeParams;
    TypeRef baseType;
    std::vector<HirEnumVariant> variants;
    SourceLocation location;
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
    // The name imported from the DLL, when it differs from `name`. Empty means
    // the two are the same.
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
};

inline HirGuardedPattern::~HirGuardedPattern() = default;
inline HirBlockExpr::~HirBlockExpr() = default;
} // namespace Rux
