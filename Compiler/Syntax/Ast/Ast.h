#pragma once

#include "Lexer/Token.h"
#include "Target/AsmInstr.h"
#include "Target/CallingConvention.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Rux {
// Forward declarations
struct TypeExpr;
struct Pattern;
struct Expr;
struct Stmt;
struct Decl;
struct Block;

using TypeExprPtr = std::unique_ptr<TypeExpr>;
using PatternPtr = std::unique_ptr<Pattern>;
using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;
using DeclPtr = std::unique_ptr<Decl>;

// Type Expressions

struct TypeExpr {
    SourceLocation location;
    virtual ~TypeExpr() = default;
};

// int32, String, MyStruct
struct NamedTypeExpr : TypeExpr {
    std::string name;
    std::vector<TypeExprPtr> typeArgs;
};

// Text::String, Rux::Primitives::Bool
struct PathTypeExpr : TypeExpr {
    std::vector<std::string> segments;
};

// Inline arrays: T[] is a flexible struct tail and T[N] has a fixed extent.
struct ArrayTypeExpr : TypeExpr {
    ~ArrayTypeExpr() override;

    TypeExprPtr element;
    ExprPtr size; // null for a flexible tail (T[]), non-null for T[N]
};

// *uint8  or  *var uint8
struct PointerTypeExpr : TypeExpr {
    TypeExprPtr pointee;
    bool pointeeMut = false; // *var T: the pointee is writable (default is read-only)
};

// (int32, float64)
struct TupleTypeExpr : TypeExpr {
    std::vector<TypeExprPtr> elements;
};

// self used as a type (interface method params)
struct SelfTypeExpr : TypeExpr {};

// func(x: int32, y: int32) -> bool
// Parameter names are optional and for readability only; only the parameter
// types and the return type participate in type identity.
struct FunctionTypeExpr : TypeExpr {
    std::vector<TypeExprPtr> params;
    std::optional<TypeExprPtr> returnType; // nullopt => no return value (opaque)
    bool isVariadic = false;               // trailing C-style ...
};

// Block

struct Block {
    SourceLocation location;
    std::vector<StmtPtr> stmts;
};

// Patterns

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

// lo..hi
struct RangePattern : Pattern {
    PatternPtr lo;
    PatternPtr hi;
    bool inclusive;
};

// Event::Click(x, y) or contextual shorthand .Click(x, y)
struct EnumPattern : Pattern {
    struct NamedArg {
        SourceLocation location;
        std::string name;
        PatternPtr pattern;
    };

    // ["Event", "Click"] for a full pattern; ["Click"] for shorthand.
    std::vector<std::string> path;
    std::vector<PatternPtr> args; // bound positions
    std::vector<NamedArg> namedArgs;
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
    ~GuardedPattern() override;
    PatternPtr inner;
    ExprPtr guard;
};

// Expressions
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

// sizeof(T)
struct SizeOfExpr : Expr {
    TypeExprPtr type;
};

// .Windows — an enum variant named without its type, which the surrounding
// context supplies. Currently the context is a `#if` condition, where the
// variant is matched against the enum on the other side of the comparison.
struct EnumShorthandExpr : Expr {
    std::string variant;
};

// A compiler-provided value such as #source.line, #target.arch or #config.Get("name").
struct IntrinsicExpr : Expr {
    enum class Kind {
        Line,
        Column,
        File,
        FileName,
        FilePath,
        Function,
        Date,
        Time,
        Module,
        Os,
        Arch,
        Abi,
        Endian,
        PointerBits,
        DataModel,
        ObjectFormat,
        TargetTriple,
        TargetFeature,
        BuildProfile,
        BuildMode,
        Optimization,
        DebugAssertions,
        DebugInfo,
        IsTest,
        OutputKind,
        BuildTimestamp,
        CompilerVersion,
        CompilerHasFeature,
        Config,
        HasConfig,
    };

    Kind kind;
    std::vector<ExprPtr> args;
};

// !x, -x, ~x, *x, &x
struct UnaryExpr : Expr {
    TokenKind op;
    ExprPtr operand;
};

// i++, i--
struct PostfixExpr : Expr {
    TokenKind op; // PlusPlus or MinusMinus
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

// lo..hi
struct RangeExpr : Expr {
    ExprPtr lo; // may be null for prefix ranges
    ExprPtr hi; // may be null for suffix ranges
    bool inclusive;
};

// f(a, b, c)  or  f<T1, T2>(a, b, c)
struct CallExpr : Expr {
    ExprPtr callee;
    std::vector<TypeExprPtr> typeArgs;
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
    std::vector<TypeExprPtr> typeArgs;

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

// expr... (spread a slice into a variadic call)
struct SpreadExpr : Expr {
    ExprPtr operand;
};

// (a, b, c)
struct TupleExpr : Expr {
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
    ~BlockExpr() override;
    std::unique_ptr<Block> block;
};

// match expr { pat => expr, ... }
struct MatchExpr : Expr {
    struct Arm {
        SourceLocation location;
        PatternPtr pattern;
        ExprPtr body;
    };

    ExprPtr subject;
    std::vector<Arm> arms;
};

// Statements

struct Stmt {
    SourceLocation location;
    virtual ~Stmt() = default;
};

// expr;
struct ExprStmt : Stmt {
    ExprPtr expr;
};

// let/var name [: Type] = expr;
struct LetStmt : Stmt {
    bool isMut = false;
    std::string name;
    PatternPtr pattern;
    std::optional<TypeExprPtr> type;
    ExprPtr init;
};

// if cond { } else if cond { } else { }
// `when` sets isCompileTime: the conditions are folded during compilation and
// only the taken branch reaches semantic analysis; the others are discarded
// without being type-checked. A compile-time chain spells every arm `when`, so
// `elseIfs` holds `else when` arms.
struct IfStmt : Stmt {
    bool isCompileTime = false;
    ExprPtr condition;
    std::unique_ptr<Block> thenBlock;

    struct ElseIf {
        SourceLocation location;
        ExprPtr condition;
        std::unique_ptr<Block> block;
    };

    std::vector<ElseIf> elseIfs;
    std::unique_ptr<Block> elseBlock; // null if no else

    // Compile-time match form `when subject { patterns => block, ... else =>
    // block }` (always with isCompileTime). `matchSubject` is the value being
    // matched, and `matchArms` replaces the if/else-if fields: each arm holds a
    // list of pattern expressions (an arm is taken when any matches) and a block;
    // an arm with no patterns is the `else`. If no arm matches and there is no
    // `else`, the fold reports an error.
    struct MatchArm {
        SourceLocation location;
        std::vector<ExprPtr> patterns; // empty = the `else` arm
        std::unique_ptr<Block> block;
    };

    ExprPtr matchSubject;
    std::vector<MatchArm> matchArms;
};

// while cond { }
struct WhileStmt : Stmt {
    std::string label; // empty = no label
    ExprPtr condition;
    std::unique_ptr<Block> body;
};

// do { } while cond;
struct DoWhileStmt : Stmt {
    std::string label;
    std::unique_ptr<Block> body;
    ExprPtr condition;
};

// loop { }
struct LoopStmt : Stmt {
    std::string label;
    std::unique_ptr<Block> body;
};

// for var in iterable { }
struct ForStmt : Stmt {
    std::string label;
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

// break [label];
struct BreakStmt : Stmt {
    std::string label; // empty = break innermost loop
};

// continue [label];
struct ContinueStmt : Stmt {
    std::string label; // empty = continue innermost loop
};

// declaration inside a block
struct DeclStmt : Stmt {
    ~DeclStmt() override;
    DeclPtr decl;
};

// Declarations
struct Decl {
    SourceLocation location;
    bool isPublic = false;
    // Non-empty for declarations whose implementation/value is supplied by
    // the compiler rather than Rux source: the `intrinsic` keyword sets it to
    // the registry key derived from the declaration itself (see ParseDecl).
    std::string intrinsicName;
    std::string warnMessage;  // non-empty = emit this warning at each call site
    std::string errorMessage; // non-empty = emit this error at each call site
    // Lint rules explicitly suppressed for this declaration. A type-level
    // suppression also covers the fields and variants owned by that type.
    std::vector<std::string> allowedLints;
    virtual ~Decl() = default;
};

struct Param {
    SourceLocation location;
    std::string name;
    TypeExprPtr type;
    bool isMut = false;      // `var name: T`; parameters are immutable by default
    bool isVariadic = false; // for extern ...
    std::optional<ExprPtr> defaultValue;
};

// when cond { decls } else when cond { decls } else { decls }
// Conditional compilation at declaration level. The taken branch's items are
// spliced into the enclosing declaration list before semantic analysis, so no
// later stage ever sees this node.
struct WhenDecl : Decl {
    // A `#Error`/`#Warn` directive used as a match arm body at declaration level
    // (`else => #Error("...")`): it emits its diagnostic when the arm is taken.
    enum class Directive {
        None,
        Error,
        Warn
    };

    struct Branch {
        SourceLocation location;
        ExprPtr condition; // null for the trailing `else`
        std::vector<DeclPtr> items;

        // Match form only: the arm patterns compared against `matchSubject` (the
        // arm is taken when any matches; empty for the `else` arm). `condition`
        // is unused in that case.
        std::vector<ExprPtr> patterns;
        Directive directive = Directive::None;
        std::string directiveMessage;
        SourceLocation directiveLocation;
    };

    std::vector<Branch> branches;

    // When set, this is the compile-time match form; each branch's `pattern`
    // is compared against it. See IfStmt::matchSubject.
    ExprPtr matchSubject;
};

// func [asm] Name<T>(params) -> Type { body }
// body is null for interface method signatures
struct FuncDecl : Decl {
    bool isAsm = false;
    bool isNoReturn = false;
    CallingConvention callConv = CallingConvention::Default;
    std::string name;
    std::vector<std::string> typeParams;
    std::vector<Param> params;
    std::optional<TypeExprPtr> returnType;
    std::unique_ptr<Block> body;   // null = signature only
    std::vector<AsmInstr> asmBody; // instructions when isAsm; body is null
};

// struct Name { field: Type; ... }
struct StructDecl : Decl {
    std::string name;
    std::vector<std::string> typeParams;

    struct Field {
        SourceLocation location;
        bool isPublic = false;
        std::string name;
        TypeExprPtr type;
    };

    std::vector<Field> fields;
};

// enum Name<T> { Variant, Variant(Type, ...), ... }
struct EnumDecl : Decl {
    std::string name;
    std::vector<std::string> typeParams;
    TypeExprPtr baseType;

    struct Variant {
        SourceLocation location;
        std::string name;
        std::vector<TypeExprPtr> fields; // empty = unit variant

        struct NamedField {
            SourceLocation location;
            std::string name;
            TypeExprPtr type;
        };

        std::vector<NamedField> namedFields;
        std::optional<std::string> discriminant;
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

// extend TypeName [for InterfaceName] { func ... }
// TypeName may be a compound type such as a slice (int[]); `typeName` holds the
// canonical string key used for method lookup, while `extendedType` preserves
// the full type expression so semantic analysis can recover the receiver type.
struct ImplDecl : Decl {
    std::string typeName;
    TypeExprPtr extendedType;
    std::optional<std::string> interfaceName;
    std::vector<std::unique_ptr<FuncDecl>> methods;

    // `when` chains written between the methods. Conditional compilation folds
    // each one and moves the methods of the taken branch into `methods`, so by
    // the time anything else looks at an ImplDecl this is empty.
    std::vector<std::unique_ptr<WhenDecl>> conditionals;
};

// module Name { decls... }
struct ModuleDecl : Decl {
    std::string name;
    std::vector<DeclPtr> items;
};

// import path[.* | ::{a,b}];
struct UseDecl : Decl {
    std::vector<std::string> path;

    enum class Kind {
        Single,
        Glob,
        Multi,
    } kind = Kind::Single;

    std::vector<std::string> names; // for Multi
};

// const Name[: Type] = expr;
// `intrinsic #name: Type;` has no value: `intrinsicName` names the
// compiler-supplied one, and is the only thing marking it.
struct ConstDecl : Decl {
    std::string name;
    std::optional<TypeExprPtr> type;
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
    bool isNoReturn = false;
    std::string dll;
    // A compile-time string constant used as the first #Link argument. The
    // conditional-compilation pass resolves it into `dll` after `when` folds.
    std::string dllConst;
    // The optional second #Link argument: the name to import from the DLL when it differs from
    // the Rux-visible `name`. Empty means the two are the same.
    std::string symbolName;
    std::string symbolNameConst;
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

// #Link("...") extern { func ...; ... }
struct ExternBlockDecl : Decl {
    std::string dll;
    std::string dllConst;
    CallingConvention callConv = CallingConvention::Default;
    std::vector<DeclPtr> items; // ExternFuncDecl or ExternVarDecl
};

// Module (AST root)
struct Module {
    std::string name; // source file name
    std::vector<DeclPtr> items;
};

inline ArrayTypeExpr::~ArrayTypeExpr() = default;
inline BlockExpr::~BlockExpr() = default;
inline DeclStmt::~DeclStmt() = default;
inline GuardedPattern::~GuardedPattern() = default;
} // namespace Rux
