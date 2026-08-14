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
    func Read(self) -> int;
}

extend Box : Reader {
    func Read(self) -> int { return 1; }
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
    FuncDecl 'Read' (self: self) -> int [signature]
  ImplDecl Reader for Box
    FuncDecl 'Read' (self: self) -> int
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
