/*
    Rux Compiler
    Copyright © 2026 Ivan Muzyka
    Licensed under the MIT License
*/

#pragma once

#include "Rux/Token.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Rux {

    // ── Calling convention ────────────────────────────────────────────────────

    enum class CallingConvention {
        Default, // platform default (System V AMD64 ABI)
        Win64,   // Microsoft x64 calling convention
    };

    // ── Forward declarations ──────────────────────────────────────────────────

    struct TypeExpr;
    struct Pattern;
    struct Expr;
    struct Stmt;
    struct Decl;
    struct Block;

    using TypeExprPtr = std::unique_ptr<TypeExpr>;
    using PatternPtr  = std::unique_ptr<Pattern>;
    using ExprPtr     = std::unique_ptr<Expr>;
    using StmtPtr     = std::unique_ptr<Stmt>;
    using DeclPtr     = std::unique_ptr<Decl>;

    // ── Type Expressions ─────────────────────────────────────────────────────

    struct TypeExpr {
        SourceLocation location;
        virtual ~TypeExpr() = default;
    };

    // int32, String, MyStruct
    struct NamedTypeExpr : TypeExpr {
        std::string name;
    };

    // Std::Io::Reader
    struct PathTypeExpr : TypeExpr {
        std::vector<std::string> segments;
    };

    // T[], uint8[4]
    struct ArrayTypeExpr : TypeExpr {
        TypeExprPtr element;
        std::optional<ExprPtr> size; // present for fixed-size arrays
    };

    // *uint8
    struct PointerTypeExpr : TypeExpr {
        TypeExprPtr pointee;
    };

    // (int32, float64)
    struct TupleTypeExpr : TypeExpr {
        std::vector<TypeExprPtr> elements;
    };

    // self used as a type (interface method params)
    struct SelfTypeExpr : TypeExpr {};

    // ── Block ─────────────────────────────────────────────────────────────────

    struct Block {
        SourceLocation location;
        std::vector<StmtPtr> stmts;
    };

    // ── Patterns ─────────────────────────────────────────────────────────────

    struct Pattern {
        SourceLocation location;
        virtual ~Pattern() = default;
    };

    // _
    struct WildcardPattern : Pattern {};

    // 42, "str", true
    struct LiteralPattern : Pattern {
        Token value;
    };

    // x  (binds the matched value to x)
    struct IdentPattern : Pattern {
        std::string name;
    };

    // lo..hi  or  lo...hi
    struct RangePattern : Pattern {
        PatternPtr lo;
        PatternPtr hi;
        bool inclusive; // false = ..  true = ...
    };

    // Event.Click(x, y)
    struct EnumPattern : Pattern {
        std::vector<std::string> path; // ["Event", "Click"]
        std::vector<PatternPtr> args;  // bound positions
    };

    // Point { x: 0, y: 0 }
    struct StructPattern : Pattern {
        std::string typeName;
        struct Field {
            SourceLocation location;
            std::string name;
            PatternPtr pattern;
        };
        std::vector<Field> fields;
    };

    // (a, b)
    struct TuplePattern : Pattern {
        std::vector<PatternPtr> elements;
    };

    // pattern if guard
    struct GuardedPattern : Pattern {
        PatternPtr inner;
        ExprPtr guard;
    };

    // ── Expressions ──────────────────────────────────────────────────────────

    struct Expr {
        SourceLocation location;
        virtual ~Expr() = default;
    };

    // 42, 3.14, "hello", 'A', true, null
    struct LiteralExpr : Expr {
        Token token;
    };

    // foo, Bar
    struct IdentExpr : Expr {
        std::string name;
    };

    // self
    struct SelfExpr : Expr {};

    // a::b::c
    struct PathExpr : Expr {
        std::vector<std::string> segments;
    };

    // !x, -x, ~x, *x, &x
    struct UnaryExpr : Expr {
        TokenKind op;
        ExprPtr operand;
    };

    // a + b, a && b, a == b, etc.
    struct BinaryExpr : Expr {
        TokenKind op;
        ExprPtr left;
        ExprPtr right;
    };

    // a = b, a += b, etc.
    struct AssignExpr : Expr {
        TokenKind op;
        ExprPtr target;
        ExprPtr value;
    };

    // cond ? thenExpr : elseExpr
    struct TernaryExpr : Expr {
        ExprPtr condition;
        ExprPtr thenExpr;
        ExprPtr elseExpr;
    };

    // lo..hi  or  lo...hi
    struct RangeExpr : Expr {
        ExprPtr lo; // may be null for prefix ranges
        ExprPtr hi; // may be null for suffix ranges
        bool inclusive;
    };

    // f(a, b, c)
    struct CallExpr : Expr {
        ExprPtr callee;
        std::vector<ExprPtr> args;
    };

    // a[i]
    struct IndexExpr : Expr {
        ExprPtr object;
        ExprPtr index;
    };

    // a.field
    struct FieldExpr : Expr {
        ExprPtr object;
        std::string field;
    };

    // Point { x: 1.0, y: 2.0 }
    struct StructInitExpr : Expr {
        std::string typeName;
        struct Field {
            SourceLocation location;
            std::string name;
            ExprPtr value;
        };
        std::vector<Field> fields;
    };

    // [a, b, c]
    struct ArrayExpr : Expr {
        std::vector<ExprPtr> elements;
    };

    // expr as Type
    struct CastExpr : Expr {
        ExprPtr operand;
        TypeExprPtr type;
    };

    // expr is Type
    struct IsExpr : Expr {
        ExprPtr operand;
        TypeExprPtr type;
    };

    // { stmts; value }  — block used as expression (match arm body)
    struct BlockExpr : Expr {
        std::unique_ptr<Block> block;
    };

    // ── Statements ────────────────────────────────────────────────────────────

    struct Stmt {
        SourceLocation location;
        virtual ~Stmt() = default;
    };

    // expr;
    struct ExprStmt : Stmt {
        ExprPtr expr;
    };

    // let [mut] name [: Type] = expr;
    struct LetStmt : Stmt {
        bool isMut = false;
        std::string name;
        std::optional<TypeExprPtr> type;
        ExprPtr init;
    };

    // if cond { } else if cond { } else { }
    struct IfStmt : Stmt {
        ExprPtr condition;
        std::unique_ptr<Block> thenBlock;
        struct ElseIf {
            SourceLocation location;
            ExprPtr condition;
            std::unique_ptr<Block> block;
        };
        std::vector<ElseIf> elseIfs;
        std::unique_ptr<Block> elseBlock; // null if no else
    };

    // while cond { }
    struct WhileStmt : Stmt {
        ExprPtr condition;
        std::unique_ptr<Block> body;
    };

    // for var in iterable { }
    struct ForStmt : Stmt {
        std::string variable;
        ExprPtr iterable;
        std::unique_ptr<Block> body;
    };

    // match expr { arm, arm, ... }
    struct MatchStmt : Stmt {
        struct Arm {
            SourceLocation location;
            PatternPtr pattern;
            ExprPtr body;
        };
        ExprPtr subject;
        std::vector<Arm> arms;
    };

    // return [expr];
    struct ReturnStmt : Stmt {
        std::optional<ExprPtr> value;
    };

    // break;
    struct BreakStmt : Stmt {};

    // continue;
    struct ContinueStmt : Stmt {};

    // declaration inside a block
    struct DeclStmt : Stmt {
        DeclPtr decl;
    };

    // ── Declarations ──────────────────────────────────────────────────────────

    struct Decl {
        SourceLocation location;
        bool isPublic = false;
        virtual ~Decl() = default;
    };

    struct Param {
        SourceLocation location;
        std::string name;
        TypeExprPtr type;
        bool isVariadic = false; // for extern ...
    };

    // func [asm] Name<T>(params) -> Type { body }
    // body is null for interface method signatures
    struct FuncDecl : Decl {
        bool isAsm = false;
        CallingConvention callConv = CallingConvention::Default;
        std::string name;
        std::vector<std::string> typeParams;
        std::vector<Param> params;
        std::optional<TypeExprPtr> returnType;
        std::unique_ptr<Block> body; // null = signature only
    };

    // struct Name { field: Type; ... }
    struct StructDecl : Decl {
        std::string name;
        struct Field {
            SourceLocation location;
            bool isPublic = false;
            std::string name;
            TypeExprPtr type;
        };
        std::vector<Field> fields;
    };

    // enum Name { Variant, Variant(Type, ...), ... }
    struct EnumDecl : Decl {
        std::string name;
        struct Variant {
            SourceLocation location;
            std::string name;
            std::vector<TypeExprPtr> fields; // empty = unit variant
        };
        std::vector<Variant> variants;
    };

    // union Name { field: Type, ... }
    struct UnionDecl : Decl {
        std::string name;
        struct Field {
            SourceLocation location;
            std::string name;
            TypeExprPtr type;
        };
        std::vector<Field> fields;
    };

    // interface Name { func Sig(); ... }
    struct InterfaceDecl : Decl {
        std::string name;
        std::vector<std::unique_ptr<FuncDecl>> methods;
    };

    // impl TypeName [for InterfaceName] { func ... }
    struct ImplDecl : Decl {
        std::string typeName;
        std::optional<std::string> interfaceName;
        std::vector<std::unique_ptr<FuncDecl>> methods;
    };

    // mod Name { decls... }
    struct ModuleDecl : Decl {
        std::string name;
        std::vector<DeclPtr> items;
    };

    // use path[.* | ::{a,b}];
    struct UseDecl : Decl {
        std::vector<std::string> path;
        enum class Kind { Single, Glob, Multi } kind = Kind::Single;
        std::vector<std::string> names; // for Multi
    };

    // const Name: Type = expr;
    struct ConstDecl : Decl {
        std::string name;
        TypeExprPtr type;
        ExprPtr value;
    };

    // type Name = Type;
    struct TypeAliasDecl : Decl {
        std::string name;
        TypeExprPtr type;
    };

    // extern func Name(params) -> Type from "DLL";
    struct ExternFuncDecl : Decl {
        std::string name;
        std::string dll;
        CallingConvention callConv = CallingConvention::Default;
        std::vector<Param> params;
        bool isVariadic = false;
        std::optional<TypeExprPtr> returnType;
    };

    // extern Name: Type;
    struct ExternVarDecl : Decl {
        std::string name;
        TypeExprPtr type;
    };

    // @[Import(lib: "...")] extern { func ...; ... }
    struct ExternBlockDecl : Decl {
        std::string dll;
        CallingConvention callConv = CallingConvention::Default;
        std::vector<DeclPtr> items; // ExternFuncDecl or ExternVarDecl
    };

    // ── Module (AST root) ─────────────────────────────────────────────────────

    struct Module {
        std::string name; // source file name
        std::vector<DeclPtr> items;
    };

} // namespace Rux
