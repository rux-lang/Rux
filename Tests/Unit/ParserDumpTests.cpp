// Human-readable AST dump ownership and compatibility tests.

#include "Lexer/Lexer.h"
#include "Syntax/Parser/Parser.h"

#include <doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace Rux;

TEST_CASE("AST declaration and type dumps preserve their text contract") {
    constexpr std::string_view source = R"(
import Core::{ Thing, Value };

pub struct Box<T> {
    pub value: T;
    array: (*var uint8)[4];
    tuple: (int32,);
    path: Core::Thing;
}

pub enum Choice<T>: uint8 {
    Empty = 0,
    Pair(T, int32),
    Named { value: T; }
}

pub union Bits {
    signed: int32,
    unsigned: uint32
}

pub interface Reader {
    func Read(self: *self) -> int;
}

extend Box : Reader {
    func Read(self: *Box) -> int { return 1; }
}

pub module Nested {
    pub type Callback = func(int32) -> bool;
    pub const Limit: uint32 = 4;
}

when true {
    const Enabled = 1;
} else {
    const Disabled = 0;
}

#Link("library", "symbol")
#Abi(.C)
pub extern func Imported(var buffer: *var uint8, ...) -> int;
pub extern Shared: Core::Thing;

asm func Raw() {
    mov rax, [rbx + 8]
    ret
}
)";

    Lexer lexer(std::string(source), "dump.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());

    Parser parser(std::move(lexed.tokens), "dump.rux", Target::Arch::X86_64);
    auto parsed = parser.Parse();
    for (const auto &diagnostic : parsed.diagnostics) {
        INFO("unexpected diagnostic: ", diagnostic.message);
        REQUIRE(diagnostic.severity != Diagnostic::Severity::Error);
    }

    const auto path = std::filesystem::temp_directory_path() / "rux-parser-declaration-dump.ast";
    REQUIRE(Parser::DumpAst(parsed, path));
    std::ifstream input(path);
    const std::string output{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    input.close();
    std::filesystem::remove(path);

    constexpr std::string_view expected = R"(Module "dump.rux"
  ImportDecl 'Core::{Thing, Value}'
  pub StructDecl 'Box'<T>
    pub Field 'value' : T
    Field 'array' : (*var uint8)[N]
    Field 'tuple' : (int32,)
    Field 'path' : Core::Thing
  pub EnumDecl 'Choice'<T> : uint8
    Variant 'Empty' = 0
    Variant 'Pair' (T, int32)
    Variant 'Named' { value: T; }
  pub UnionDecl 'Bits'
    Field 'signed' : int32
    Field 'unsigned' : uint32
  pub InterfaceDecl 'Reader'
    FuncDecl 'Read' (self: *self) -> int [signature]
  ImplDecl Reader for Box
    FuncDecl 'Read' (self: *Box) -> int
      Block [1 stmt]
        ReturnStmt
          LiteralExpr (int) '1'
  pub ModuleDecl 'Nested'
    pub TypeAliasDecl 'Callback' = <type>
    pub ConstDecl 'Limit' : uint32
      LiteralExpr (int) '4'
  WhenDecl
    Branch
      Condition
        LiteralExpr (bool8) 'true'
      ConstDecl 'Enabled'
        LiteralExpr (int) '1'
    Else
      ConstDecl 'Disabled'
        LiteralExpr (int) '0'
  #Link("library", "symbol")
  #Abi(.C)
  pub ExternFuncDecl 'Imported' (var buffer: *var uint8, ...) -> int
  pub ExternVarDecl 'Shared' : Core::Thing
  asm FuncDecl 'Raw' ()
    mov rax, [rbx + 8]
    ret
)";
    CHECK_EQ(output, expected);
}

TEST_CASE("AST statement and pattern dumps preserve their text contract") {
    constexpr std::string_view source = R"(
func Main() -> int {
    var count: int = 0;
    let (left, right) = (1, 2);
    const Local = 7;

    if true {
        count = 1;
    } else if false {
        count = 2;
    } else {
        count = 3;
    }

    when true {
        count = 4;
    } else when false {
        count = 5;
    } else {
        count = 6;
    }

    while count < 10 {
        count += 1;
        break;
    }
    for item in 0..2 {
        continue;
    }

    match count {
        0 => 0,
        1...2 => 1,
        Choice::Pair(value, _) => 2,
        .Named { field: named } => 3,
        Point { x: 0, y: other } => 4,
        (first, second) if true => { left + right; },
        else => 6
    }
    return count;
}
)";

    Lexer lexer(std::string(source), "statements.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());

    Parser parser(std::move(lexed.tokens), "statements.rux", Target::Arch::X86_64);
    auto parsed = parser.Parse();
    for (const auto &diagnostic : parsed.diagnostics) {
        INFO("unexpected diagnostic: ", diagnostic.message);
        REQUIRE(diagnostic.severity != Diagnostic::Severity::Error);
    }

    const auto path = std::filesystem::temp_directory_path() / "rux-parser-statement-dump.ast";
    REQUIRE(Parser::DumpAst(parsed, path));
    std::ifstream input(path);
    const std::string output{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    input.close();
    std::filesystem::remove(path);

    constexpr std::string_view expected = R"(Module "statements.rux"
  FuncDecl 'Main' () -> int
    Block [9 stmts]
      LetStmt 'count' (var) : int
        LiteralExpr (int) '0'
      LetStmt '<pattern>' (let)
        TuplePattern [2]
          IdentPattern 'left'
          IdentPattern 'right'
      ConstDecl 'Local'
        LiteralExpr (int) '7'
      IfStmt
        Condition
          LiteralExpr (bool8) 'true'
        Then
          Block [1 stmt]
            ExprStmt
              AssignExpr =
                IdentExpr 'count'
                LiteralExpr (int) '1'
        ElseIf
          Condition
            LiteralExpr (bool8) 'false'
          Block [1 stmt]
            ExprStmt
              AssignExpr =
                IdentExpr 'count'
                LiteralExpr (int) '2'
        Else
          Block [1 stmt]
            ExprStmt
              AssignExpr =
                IdentExpr 'count'
                LiteralExpr (int) '3'
      WhenStmt
        Condition
          LiteralExpr (bool8) 'true'
        Then
          Block [1 stmt]
            ExprStmt
              AssignExpr =
                IdentExpr 'count'
                LiteralExpr (int) '4'
        ElseIf
          Condition
            LiteralExpr (bool8) 'false'
          Block [1 stmt]
            ExprStmt
              AssignExpr =
                IdentExpr 'count'
                LiteralExpr (int) '5'
        Else
          Block [1 stmt]
            ExprStmt
              AssignExpr =
                IdentExpr 'count'
                LiteralExpr (int) '6'
      WhileStmt
        Condition
          BinaryExpr <
            IdentExpr 'count'
            LiteralExpr (int) '10'
        Block [2 stmts]
          ExprStmt
            AssignExpr +=
              IdentExpr 'count'
              LiteralExpr (int) '1'
          BreakStmt
      ForStmt 'item' in
        RangeExpr ..
          LiteralExpr (int) '0'
          LiteralExpr (int) '2'
        Block [1 stmt]
          ContinueStmt
      MatchStmt
        Subject
          IdentExpr 'count'
        Arm
          LiteralPattern '0'
          LiteralExpr (int) '0'
        Arm
          RangePattern ...
            LiteralPattern '1'
            LiteralPattern '2'
          LiteralExpr (int) '1'
        Arm
          EnumPattern 'Choice.Pair' [2 bindings]
            IdentPattern 'value'
            WildcardPattern
          LiteralExpr (int) '2'
        Arm
          EnumPattern '.Named' [1 fields]
            .field:
              IdentPattern 'named'
          LiteralExpr (int) '3'
        Arm
          StructPattern 'Point'
            .x:
              LiteralPattern '0'
            .y:
              IdentPattern 'other'
          LiteralExpr (int) '4'
        Arm
          GuardedPattern
            TuplePattern [2]
              IdentPattern 'first'
              IdentPattern 'second'
            Guard
              LiteralExpr (bool8) 'true'
          Block [1 stmt]
            ExprStmt
              BinaryExpr +
                IdentExpr 'left'
                IdentExpr 'right'
        Arm
          WildcardPattern
          LiteralExpr (int) '6'
      ReturnStmt
        IdentExpr 'count'
)";
    CHECK_EQ(output, expected);
}
