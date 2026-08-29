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
    shared: &T;
    exclusive: &var T;
    references: (&uint8)[4];
    tuple: (int32,);
    path: Core::Thing;
}

pub variant Choice<T> {
    Empty,
    Pair(T, int32),
    Named { value: T; }
}

pub union Bits {
    pub signed: int32,
    unsigned: uint32
}

pub interface Reader {
    func Read(self: &Self) -> int;
}

extend Box : Reader {
    func Read(self: &Box) -> int { return 1; }
}

pub module Nested::Leaf {
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
pub extern func Imported(buffer: *var uint8, ...) -> int;
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
    Field 'shared' : &T
    Field 'exclusive' : &var T
    Field 'references' : (&uint8)[N]
    Field 'tuple' : (int32,)
    Field 'path' : Core::Thing
  pub VariantDecl 'Choice'<T>
    Case 'Empty'
    Case 'Pair' (T, int32)
    Case 'Named' { value: T; }
  pub UnionDecl 'Bits'
    pub Field 'signed' : int32
    Field 'unsigned' : uint32
  pub InterfaceDecl 'Reader'
    FuncDecl 'Read' (self: &Self) -> int [signature]
  ImplDecl Reader for Box
    FuncDecl 'Read' (self: &Box) -> int
      Block [1 stmt]
        ReturnStmt
          LiteralExpr (int) '1'
  pub ModuleDecl 'Nested'
    pub ModuleDecl 'Leaf'
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
  pub ExternFuncDecl 'Imported' (buffer: *var uint8, ...) -> int
  pub ExternVarDecl 'Shared' : Core::Thing
  asm FuncDecl 'Raw' ()
    mov rax, [rbx + 8]
    ret
)";
    CHECK_EQ(output, expected);
}

TEST_CASE("AST dumps spell references and lifecycle operations canonically") {
    constexpr std::string_view source = R"(
struct Cell { value: int32; }
extend Cell {
    func =(self: &var Cell, other: &Cell);
    func <-(self: &var Cell, other: Cell) {}
    func ~Cell(self: &var Cell) {}
}
func Transfer(source: Cell) -> Cell {
    let result <- source;
    return <- result;
}
)";

    Lexer lexer(std::string(source), "ownership-dump.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "ownership-dump.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    const auto path = std::filesystem::temp_directory_path() / "rux-parser-ownership-dump.ast";
    REQUIRE(Parser::DumpAst(parsed, path));
    std::ifstream input(path);
    const std::string output{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    input.close();
    std::filesystem::remove(path);

    CHECK(output.contains("FuncDecl '=' (self: &var Cell, other: &Cell) [signature]"));
    CHECK(output.contains("FuncDecl '<-' (self: &var Cell, other: Cell)"));
    CHECK(output.contains("FuncDecl '~Cell' (self: &var Cell)"));
    const std::size_t firstMove = output.find("MoveExpr <-");
    REQUIRE_NE(firstMove, std::string::npos);
    CHECK_NE(output.find("MoveExpr <-", firstMove + 1), std::string::npos);
}

TEST_CASE("AST dumps adjacent stars as multiplication followed by dereference") {
    constexpr std::string_view source = R"(
func Main(value: int, pointer: *int) -> int {
    let tight = value**pointer;
    let spaced = value * *pointer;
    return tight + spaced;
}
)";

    Lexer lexer(std::string(source), "adjacent-stars.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "adjacent-stars.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    const auto path = std::filesystem::temp_directory_path() / "rux-parser-adjacent-stars.ast";
    REQUIRE(Parser::DumpAst(parsed, path));
    std::ifstream input(path);
    const std::string output{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    input.close();
    std::filesystem::remove(path);

    constexpr std::string_view multiplicationAndDereference = R"(BinaryExpr *
          IdentExpr 'value'
          UnaryExpr *
            IdentExpr 'pointer')";
    const std::size_t tight = output.find(multiplicationAndDereference);
    REQUIRE_NE(tight, std::string::npos);
    CHECK_NE(output.find(multiplicationAndDereference, tight + 1), std::string::npos);
}

TEST_CASE("AST dumps render generic interface bounds in declaration order") {
    constexpr std::string_view source = R"(
func Convert<T: Display + Core::Debug, U>(value: T) -> U;
struct Index<Key: Hash + Equal, Value> { key: Key; value: Value; }
variant Result<Value, Error: Display> { Ok(Value), Fail(Error) }
)";

    Lexer lexer(std::string(source), "generic-bounds.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());

    Parser parser(std::move(lexed.tokens), "generic-bounds.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    const auto path = std::filesystem::temp_directory_path() / "rux-parser-generic-bounds.ast";
    REQUIRE(Parser::DumpAst(parsed, path));
    std::ifstream input(path);
    const std::string output{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    input.close();
    std::filesystem::remove(path);

    constexpr std::string_view expected = R"(Module "generic-bounds.rux"
  FuncDecl 'Convert'<T: Display + Core::Debug, U> (value: T) -> U [signature]
  StructDecl 'Index'<Key: Hash + Equal, Value>
    Field 'key' : Key
    Field 'value' : Value
  VariantDecl 'Result'<Value, Error: Display>
    Case 'Ok' (Value)
    Case 'Fail' (Error)
)";
    CHECK_EQ(output, expected);
}

TEST_CASE("AST dumps preserve variant declarations and case forms") {
    constexpr std::string_view source = R"(
variant Maybe<T> {
    None,
    Some(T)
}

pub variant NetworkResult<Value, Error: Display> {
    Pending,
    Success(Value),
    Failure { code: int32; error: Error; }
}

enum State: uint8 {
    Idle = 0,
    Busy = 1
}
)";

    Lexer lexer(std::string(source), "variant-dump.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());

    Parser parser(std::move(lexed.tokens), "variant-dump.rux");
    auto parsed = parser.Parse();
    for (const auto &diagnostic : parsed.diagnostics) {
        INFO("unexpected diagnostic: ", diagnostic.message);
        REQUIRE(diagnostic.severity != Diagnostic::Severity::Error);
    }
    REQUIRE_EQ(parsed.module.items.size(), 3);

    const auto *maybe = dynamic_cast<const EnumDecl *>(parsed.module.items[0].get());
    REQUIRE(maybe != nullptr);
    CHECK(maybe->IsVariant());
    CHECK(maybe->form == EnumDecl::Form::Variant);
    CHECK_EQ(maybe->variants.size(), 2);

    const auto *networkResult = dynamic_cast<const EnumDecl *>(parsed.module.items[1].get());
    REQUIRE(networkResult != nullptr);
    CHECK(networkResult->IsVariant());
    CHECK(networkResult->isPublic);
    REQUIRE_EQ(networkResult->typeParams.size(), 2);
    CHECK_EQ(networkResult->typeParams[1].bounds.size(), 1);
    CHECK(networkResult->baseType == nullptr);
    REQUIRE_EQ(networkResult->variants.size(), 3);
    CHECK_EQ(networkResult->variants[0].name, "Pending");
    CHECK(networkResult->variants[0].fields.empty());
    CHECK(networkResult->variants[0].namedFields.empty());
    CHECK_FALSE(networkResult->variants[0].discriminant.has_value());
    CHECK_EQ(networkResult->variants[1].name, "Success");
    CHECK_EQ(networkResult->variants[1].fields.size(), 1);
    CHECK_FALSE(networkResult->variants[1].discriminant.has_value());
    CHECK_EQ(networkResult->variants[2].name, "Failure");
    CHECK_EQ(networkResult->variants[2].namedFields.size(), 2);
    CHECK_EQ(networkResult->variants[2].namedFields[0].name, "code");
    CHECK_EQ(networkResult->variants[2].namedFields[1].name, "error");
    CHECK_FALSE(networkResult->variants[2].discriminant.has_value());

    const auto *state = dynamic_cast<const EnumDecl *>(parsed.module.items[2].get());
    REQUIRE(state != nullptr);
    CHECK_FALSE(state->IsVariant());
    CHECK(state->form == EnumDecl::Form::Enumeration);
    CHECK_EQ(state->variants.size(), 2);

    const auto path = std::filesystem::temp_directory_path() / "rux-parser-variant-dump.ast";
    REQUIRE(Parser::DumpAst(parsed, path));
    std::ifstream input(path);
    const std::string output{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    input.close();
    std::filesystem::remove(path);

    constexpr std::string_view expected = R"(Module "variant-dump.rux"
  VariantDecl 'Maybe'<T>
    Case 'None'
    Case 'Some' (T)
  pub VariantDecl 'NetworkResult'<Value, Error: Display>
    Case 'Pending'
    Case 'Success' (Value)
    Case 'Failure' { code: int32; error: Error; }
  EnumDecl 'State' : uint8
    Variant 'Idle' = 0
    Variant 'Busy' = 1
)";
    CHECK_EQ(output, expected);
}

TEST_CASE("nested variant declarations keep their source form") {
    constexpr std::string_view source = R"(
func Parse() {
    variant Local<T> {
        Missing,
        Present { value: T; }
    }
}
)";

    Lexer lexer(std::string(source), "nested-variant.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "nested-variant.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    const auto path = std::filesystem::temp_directory_path() / "rux-parser-nested-variant.ast";
    REQUIRE(Parser::DumpAst(parsed, path));
    std::ifstream input(path);
    const std::string output{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    input.close();
    std::filesystem::remove(path);

    CHECK(output.contains("VariantDecl 'Local'<T>"));
    CHECK(output.contains("Case 'Missing'"));
    CHECK(output.contains("Case 'Present' { value: T; }"));
    CHECK_FALSE(output.contains("EnumDecl 'Local'"));
}

TEST_CASE("AST dumps name each compile-time layout query") {
    constexpr std::string_view source = R"(
func Main() -> int {
    let size = sizeof(int32);
    let alignment = alignof(int32);
    return 0;
}
)";

    Lexer lexer(std::string(source), "layout.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());

    Parser parser(std::move(lexed.tokens), "layout.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    const auto path = std::filesystem::temp_directory_path() / "rux-parser-layout.ast";
    REQUIRE(Parser::DumpAst(parsed, path));
    std::ifstream input(path);
    const std::string output{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    input.close();
    std::filesystem::remove(path);

    constexpr std::string_view expected = R"(Module "layout.rux"
  FuncDecl 'Main' () -> int
    Block [3 stmts]
      LetStmt 'size' (let)
        SizeOfExpr int32
      LetStmt 'alignment' (let)
        AlignOfExpr int32
      ReturnStmt
        LiteralExpr (int) '0'
)";
    CHECK_EQ(output, expected);
}

TEST_CASE("AST dumps separate the propagation operator from the conditional operator") {
    // The two share one token: written tight against its operand, `?` propagates a failure; separated, it opens a
    // conditional expression. The dump is where that decision becomes visible.
    constexpr std::string_view source = R"(
func Main() -> int {
    let propagated = Read()?;
    let chained = Read()?.field;
    let selected = ready ? first : second;
    let nested = Read(ready ? first : second)?;
    return 0;
}
)";

    Lexer lexer(std::string(source), "propagation.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());

    Parser parser(std::move(lexed.tokens), "propagation.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    const auto path = std::filesystem::temp_directory_path() / "rux-parser-propagation.ast";
    REQUIRE(Parser::DumpAst(parsed, path));
    std::ifstream input(path);
    const std::string output{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    input.close();
    std::filesystem::remove(path);

    constexpr std::string_view expected = R"(Module "propagation.rux"
  FuncDecl 'Main' () -> int
    Block [5 stmts]
      LetStmt 'propagated' (let)
        TryExpr
          CallExpr
            Callee
              IdentExpr 'Read'
      LetStmt 'chained' (let)
        FieldExpr '.field'
          TryExpr
            CallExpr
              Callee
                IdentExpr 'Read'
      LetStmt 'selected' (let)
        TernaryExpr
          Condition
            IdentExpr 'ready'
          Then
            IdentExpr 'first'
          Else
            IdentExpr 'second'
      LetStmt 'nested' (let)
        TryExpr
          CallExpr
            Callee
              IdentExpr 'Read'
            Args [1]
              TernaryExpr
                Condition
                  IdentExpr 'ready'
                Then
                  IdentExpr 'first'
                Else
                  IdentExpr 'second'
      ReturnStmt
        LiteralExpr (int) '0'
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
