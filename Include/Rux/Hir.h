/*
    Rux Compiler
    Copyright © 2026 Ivan Muzyka
    Licensed under the MIT License
*/

#pragma once

#include "Rux/Ast.h"
#include "Rux/Type.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Rux {

// ── Forward declarations ──────────────────────────────────────────────────────

struct HirExpr;
struct HirStmt;
struct HirPattern;
struct HirBlock;

using HirExprPtr    = std::unique_ptr<HirExpr>;
using HirStmtPtr    = std::unique_ptr<HirStmt>;
using HirPatternPtr = std::unique_ptr<HirPattern>;

// ── HIR Block ─────────────────────────────────────────────────────────────────

struct HirBlock {
    SourceLocation location;
    std::vector<HirStmtPtr> stmts;
};

// ── HIR Patterns ─────────────────────────────────────────────────────────────

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

// lo..hi  or  lo...hi
struct HirRangePattern : HirPattern {
    HirPatternPtr lo;
    HirPatternPtr hi;
    bool inclusive;
};

// Event.Click(x, y)
struct HirEnumPattern : HirPattern {
    std::vector<std::string> path;
    TypeRef resolvedType;
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
    HirPatternPtr inner;
    HirExprPtr guard;
};

// ── HIR Expressions ──────────────────────────────────────────────────────────

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

// lo..hi  or  lo...hi
struct HirRangeExpr : HirExpr {
    HirExprPtr lo;
    HirExprPtr hi;
    bool inclusive;
};

// f(a, b, c)
struct HirCallExpr : HirExpr {
    HirExprPtr callee;
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
struct HirSliceExpr : HirExpr {
    TypeRef elementType;
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
    HirBlock block;
};

// ── HIR Statements ────────────────────────────────────────────────────────────

struct HirStmt {
    SourceLocation location;
    virtual ~HirStmt() = default;
};

// expr;
struct HirExprStmt : HirStmt {
    HirExprPtr expr;
};

// let [mut] name: Type = expr;
struct HirLetStmt : HirStmt {
    bool isMut = false;
    std::string name;
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
    HirExprPtr condition;
    HirBlock body;
};

// for var in iterable { }
struct HirForStmt : HirStmt {
    std::string variable;
    TypeRef varType;
    HirExprPtr iterable;
    HirBlock body;
};

// match expr { arm, ... }
struct HirMatchArm {
    SourceLocation location;
    HirPatternPtr pattern;
    HirExprPtr body;
};

struct HirMatchStmt : HirStmt {
    HirExprPtr subject;
    std::vector<HirMatchArm> arms;
};

// return [expr];
struct HirReturnStmt : HirStmt {
    std::optional<HirExprPtr> value;
};

// break;
struct HirBreakStmt : HirStmt {};

// continue;
struct HirContinueStmt : HirStmt {};

// local declaration inside a block (func, const, type alias declared locally)
struct HirLocalDecl : HirStmt {
    std::string description;
};

// ── HIR Declarations ──────────────────────────────────────────────────────────

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
    CallingConvention callConv = CallingConvention::Default;
    std::vector<std::string> typeParams;
    std::vector<HirParam> params;
    TypeRef returnType;
    std::optional<HirBlock> body; // absent for interface/extern signatures
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
};

struct HirEnum {
    std::string name;
    bool isPublic = false;
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

// impl TypeName [for InterfaceName] { func ... }
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
    bool isPublic = false;
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

// ── HIR Module ────────────────────────────────────────────────────────────────

struct HirModule {
    std::string name;
    std::vector<HirFunc>       funcs;
    std::vector<HirStruct>     structs;
    std::vector<HirEnum>       enums;
    std::vector<HirUnion>      unions;
    std::vector<HirInterface>  interfaces;
    std::vector<HirImplBlock>  impls;
    std::vector<HirConst>      consts;
    std::vector<HirExternFunc> externFuncs;
    std::vector<HirExternVar>  externVars;
    std::vector<HirTypeAlias>  typeAliases;
};

struct HirPackage {
    std::vector<HirModule> modules;
};

// ── Generator ─────────────────────────────────────────────────────────────────

// Lowers a set of parsed AST modules into typed HIR.
// Input modules must have passed semantic analysis without errors.
class Hir {
public:
    explicit Hir(std::vector<const Module*> modules);

    [[nodiscard]] HirPackage Generate();

    // Write a human-readable dump of the HIR package to `path`.
    static bool Dump(const HirPackage& package, const std::filesystem::path& path);

private:
    std::vector<const Module*> modules_;
};

} // namespace Rux
