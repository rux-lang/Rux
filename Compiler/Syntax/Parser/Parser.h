#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "Diagnostics/Diagnostics.h"
#include "Lexer/Lexer.h"
#include "Syntax/Ast/Ast.h"

namespace Rux {
using ParserDiagnostic = Diagnostic;

struct ParseResult {
    Module module;
    std::vector<ParserDiagnostic> diagnostics;
    [[nodiscard]] bool HasErrors() const noexcept;
};

class Parser {
public:
    explicit Parser(std::vector<Token> inputTokens, std::string inputSourceName = "<input>");

    // Convenience: lex and parse in one step.
    [[nodiscard]] static std::optional<ParseResult> FromLexResult(const LexerResult &lex,
                                                                  const std::string &sourceName = "<input>");
    [[nodiscard]] ParseResult Parse();

    // Dump the parsed AST to a file for debugging.
    // Path defaults to sourceName + ".ast" if not specified.
    static bool DumpAst(const ParseResult &result, const std::filesystem::path &path = {});

private:
    std::vector<Token> tokens;
    std::string sourceName;
    std::size_t pos = 0;
    std::vector<ParserDiagnostic> diagnostics;
    bool structInitAllowed = true; // disabled inside if/while/for/match conditions

    // Token helpers
    [[nodiscard]] const Token &Peek(std::size_t ahead = 0) const noexcept;
    const Token &Advance() noexcept;
    [[nodiscard]] bool Check(TokenKind kind) const noexcept;
    [[nodiscard]] bool CheckAny(std::initializer_list<TokenKind> kinds) const noexcept;
    bool Match(TokenKind kind) noexcept;
    const Token &Expect(TokenKind kind, std::string_view message);
    [[nodiscard]] bool IsAtEnd() const noexcept;
    [[nodiscard]] const Token &Previous() const noexcept;
    [[nodiscard]] SourceLocation CurrentLocation() const noexcept;
    [[nodiscard]] bool IsGenericStructInitAhead() const noexcept;
    [[nodiscard]] bool IsTypeArgListAhead() const noexcept;

    // Diagnostics
    void EmitError(SourceLocation loc, std::string message);
    void EmitWarning(SourceLocation loc, std::string message);

    // Skip tokens until a safe recovery point (statement/declaration
    // boundary).
    void Synchronize();
    void Recover();

    // Top-level
    DeclPtr ParseDecl();

    // Attribute parsing
    struct ParsedAttrs {
        std::string importLib;
        CallingConvention callConv = CallingConvention::Default;
        std::string targetOs;
        std::string warnMessage;
        std::string errorMessage;
    };

    // Parses zero or more @[AttrName(...)] attributes before a declaration.
    ParsedAttrs ParseAttrs();
    DeclPtr ParseExternDecl(bool isPublic, ParsedAttrs attrs);

    // Declarations
    std::unique_ptr<FuncDecl> ParseFuncDecl(bool isPublic, bool isAsm,
                                            CallingConvention callConv = CallingConvention::Default);
    std::unique_ptr<StructDecl> ParseStructDecl(bool isPublic);
    std::unique_ptr<EnumDecl> ParseEnumDecl(bool isPublic);
    std::unique_ptr<UnionDecl> ParseUnionDecl(bool isPublic);
    std::unique_ptr<InterfaceDecl> ParseInterfaceDecl(bool isPublic);
    std::unique_ptr<ImplDecl> ParseImplDecl();
    std::unique_ptr<ModuleDecl> ParseModuleDecl(bool isPublic);
    std::unique_ptr<UseDecl> ParseUseDecl(ParsedAttrs attrs);
    std::unique_ptr<ConstDecl> ParseConstDecl(bool isPublic);
    std::unique_ptr<TypeAliasDecl> ParseTypeAliasDecl(bool isPublic);

    // Inline-assembly body parsing (asm func)
    std::vector<AsmInstr> ParseAsmBody();
    [[nodiscard]] bool CanStartAsmOperand() const noexcept;
    AsmOperand ParseAsmOperand();
    void ParseAsmMemory(AsmOperand &op);
    std::int64_t ParseAsmInt();

    // Shared declaration helpers
    Param ParseParam(bool allowVariadic = false);
    std::vector<Param> ParseParamList(bool allowVariadic = false);
    std::vector<std::string> ParseTypeParams(); // <T, U, ...>
    std::vector<TypeExprPtr> ParseTypeArgs();   // <int32, T[], ...>

    // Type expressions
    TypeExprPtr ParseType();
    TypeExprPtr ParseBaseType(); // named, path, pointer, tuple, self

    // Blocks and statements
    std::unique_ptr<Block> ParseBlock();
    StmtPtr ParseStmt();
    std::unique_ptr<LetStmt> ParseLetStmt();
    std::unique_ptr<IfStmt> ParseIfStmt();
    std::unique_ptr<WhileStmt> ParseWhileStmt();
    std::unique_ptr<DoWhileStmt> ParseDoWhileStmt();
    std::unique_ptr<LoopStmt> ParseLoopStmt();
    std::unique_ptr<ForStmt> ParseForStmt();
    std::unique_ptr<MatchStmt> ParseMatchStmt();
    std::unique_ptr<ReturnStmt> ParseReturnStmt();

    // Expressions (Pratt / precedence-climbing)
    ExprPtr ParseExpr();
    ExprPtr ParseAssign();
    ExprPtr ParseRange();
    ExprPtr ParseTernary();
    ExprPtr ParseOr();
    ExprPtr ParseAnd();
    ExprPtr ParseBitOr();
    ExprPtr ParseBitXor();
    ExprPtr ParseBitAnd();
    ExprPtr ParseEquality();
    ExprPtr ParseComparison();
    ExprPtr ParseCast();
    ExprPtr ParseShift();
    ExprPtr ParseAdd();
    ExprPtr ParseMul();
    ExprPtr ParseExp(); // ** right-associative
    ExprPtr ParseUnary();
    ExprPtr ParsePostfix();
    ExprPtr ParsePrimary();

    // Patterns
    PatternPtr ParsePattern();
    PatternPtr ParsePrimaryPattern();

    // Expression argument list
    std::vector<ExprPtr> ParseArgList(); // ( expr, ... )
};
} // namespace Rux
