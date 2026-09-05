#pragma once

#include "Diagnostics/Diagnostics.h"
#include "Lexer/Lexer.h"
#include "Syntax/ParseResult.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Rux {
/**
 * @brief Builds the AST for one module from its token stream.
 *
 * Recursive descent for declarations and statements, precedence climbing for expressions. A syntax error does not end
 * the parse: it is recorded and the parser resynchronizes at the next statement or declaration boundary, so one run
 * reports many problems instead of only the first.
 */
class Parser {
public:
    /// `inputArch` is the architecture the source is being compiled for. It only reaches `asm func` bodies, whose
    /// register names and operand syntax are the machine's rather than the language's; everything else parses the same
    /// for every target. It defaults to the host so an embedder or a focused test that names no target still reads the
    /// assembly it would write.
    explicit Parser(std::vector<Token> inputTokens, std::string inputSourceName = "<input>",
                    Target::Arch inputArch = Target::HostArch);

    /// Convenience: lex and parse in one step.
    ///
    /// @return nullopt when lexing failed, since there is no token stream to parse
    [[nodiscard]] static std::optional<ParseResult> FromLexResult(const LexerResult &lex,
                                                                  const std::string &sourceName = "<input>",
                                                                  Target::Arch arch = Target::HostArch);

    /// Parse the token stream into one module. Consumes the parser's state, so call it once per instance.
    [[nodiscard]] ParseResult Parse();

    /// Dump the parsed AST to a file for debugging. Path defaults to sourceName + ".ast" if not specified.
    static bool DumpAst(const ParseResult &result, const std::filesystem::path &path = {});

private:
    std::vector<Token> tokens;
    std::string sourceName;
    Target::Arch arch = Target::HostArch;
    std::size_t pos = 0;
    std::vector<ParserDiagnostic> diagnostics;
    std::vector<Syntax::DocumentationIssue> documentationIssues;
    bool structInitAllowed = true; ///< disabled inside if/while/for/match conditions

    // Token helpers
    [[nodiscard]] const Token &Peek(std::size_t ahead = 0) const noexcept;
    const Token &Advance() noexcept;
    [[nodiscard]] bool Check(TokenKind kind) const noexcept;
    [[nodiscard]] bool CheckAny(std::initializer_list<TokenKind> kinds) const noexcept;

    /// Whether the current token closes a generic argument list. A `>>` or `>>>` written where two or three of them
    /// end at once is a run of closers rather than a shift, and only the parser knows which it is looking at.
    [[nodiscard]] bool CheckCloseAngle() const noexcept;

    /// Consumes one closing `>`. A longer run is narrowed in place rather than advanced past, leaving the rest for
    /// the enclosing list to close with.
    void ConsumeCloseAngle() noexcept;
    bool Match(TokenKind kind) noexcept;
    const Token &Expect(TokenKind kind, std::string_view message);
    /// Grammar-aware diagnostics name both the role of a missing token and the token that prevented it from being
    /// parsed.
    const Token &ExpectBefore(TokenKind kind, std::string_view expected, std::optional<std::string> help = {});
    bool ConsumeBodyStart(std::string_view role);
    [[nodiscard]] bool IsAtEnd() const noexcept;
    [[nodiscard]] const Token &Previous() const noexcept;
    [[nodiscard]] SourceLocation CurrentLocation() const noexcept;
    [[nodiscard]] bool IsGenericStructInitAhead() const noexcept;
    /// Assumes the current token is '{'. True when the brace opens compile-time match arms (`pattern => ...`) rather
    /// than an ordinary `when`/block body, i.e. a top-level '=>' appears before any top-level ';' or the closing '}'.
    [[nodiscard]] bool NextBraceIsMatchArms() const noexcept;
    [[nodiscard]] bool IsGenericCallAhead() const noexcept;
    [[nodiscard]] bool IsTypeArgListAhead() const noexcept;

    // Diagnostics
    void EmitError(SourceLocation loc, std::string message, std::optional<std::string> help = {});
    void EmitExpected(SourceLocation loc, std::string_view expected, std::optional<std::string> help = {});
    void EmitWarning(SourceLocation loc, std::string message);

    /// Skip tokens until a safe recovery point (statement/declaration boundary).
    void Synchronize();
    void Recover();
    bool RecoverDelimitedList(TokenKind closing);

    // Top-level
    DeclPtr ParseDecl();
    [[nodiscard]] Syntax::Documentation ParseDocumentation();
    void RecordDetachedDocumentation(Syntax::Documentation documentation, Syntax::DocumentationIssueKind kind);

    // Attribute parsing
    struct ParsedAttrs {
        std::string importLib;
        std::string importLibConst;
        std::string importSymbol;
        std::string importSymbolConst;
        CallingConvention callConv = CallingConvention::Default;
        std::string warnMessage;
        std::string errorMessage;
        std::vector<std::string> allowedLints;
        bool usedLink = false;
        bool usedLibrary = false;
        bool usedSymbol = false;
        bool usedNoReturn = false;
        bool usedAbi = false;
        SourceLocation linkLocation;
        SourceLocation noReturnLocation;
        SourceLocation abiLocation;
        SourceLocation allowLocation;
    };

    /// Parses `#Name(...)` attribute calls before a declaration. The former `#{...}` metadata-block form is rejected.
    ParsedAttrs ParseAttrs();
    void ParseAttributeCall(ParsedAttrs &attrs);
    DeclPtr ApplyAttrs(DeclPtr decl, ParsedAttrs &attrs);
    DeclPtr ParseExternDecl(bool isPublic, ParsedAttrs &attrs);
    DeclPtr ParseIntrinsicDecl(bool isPublic, ParsedAttrs &attrs, SourceLocation intrinsicLoc);

    // Declarations
    std::unique_ptr<FuncDecl> ParseFuncDecl(bool isPublic, bool isAsm,
                                            CallingConvention callConv = CallingConvention::Default);
    std::unique_ptr<StructDecl> ParseStructDecl(bool isPublic);
    std::unique_ptr<EnumDecl> ParseEnumDecl(bool isPublic);
    std::unique_ptr<UnionDecl> ParseUnionDecl(bool isPublic);
    std::unique_ptr<InterfaceDecl> ParseInterfaceDecl(bool isPublic);
    std::unique_ptr<ImplDecl> ParseImplDecl();
    std::unique_ptr<ModuleDecl> ParseModuleDecl(bool isPublic);
    std::unique_ptr<UseDecl> ParseUseDecl(bool requireSemicolon = true);
    std::unique_ptr<ConstDecl> ParseConstDecl(bool isPublic);
    std::unique_ptr<WhenDecl> ParseWhenDecl();
    std::unique_ptr<WhenDecl> ParseWhenBody(SourceLocation loc);
    /// Compile-time match forms `when subject { pattern => body, ... }`. The subject has already been parsed; the
    /// current token is its opening '{'.
    std::unique_ptr<WhenDecl> ParseWhenMatchBody(SourceLocation loc, ExprPtr subject);
    std::unique_ptr<TypeAliasDecl> ParseTypeAliasDecl(bool isPublic);

    // Inline-assembly body parsing (asm func)
    std::vector<AsmInstr> ParseAsmBody();
    [[nodiscard]] bool CanStartAsmOperand() const noexcept;
    AsmOperand ParseAsmOperand();
    void ParseAsmMemory(AsmOperand &op);
    /// AArch64: the `, LSL #3` / `, UXTW #2` tail a register or immediate operand may carry. Consumes nothing when the
    /// next tokens are not one.
    void ParseAsmShift(AsmOperand &op);
    std::int64_t ParseAsmInt();

    // Shared declaration helpers
    Param ParseParam(bool allowVariadic = false);
    std::vector<Param> ParseParamList(bool allowVariadic = false);
    std::vector<TypeParameter> ParseTypeParams(); ///< <T, U: Display + Debug, ...>
    TypeExprPtr ParseInterfaceBound();            ///< Display or Namespace::Display
    std::vector<TypeExprPtr> ParseTypeArgs();     ///< <int32, T[], ...>

    // Type expressions
    TypeExprPtr ParseType(std::optional<std::string> help = {});
    TypeExprPtr ParseBaseType(std::optional<std::string> help = {}); ///< named, path, pointer, tuple, self
    TypeExprPtr ParseFunctionType();                                 ///< func(params) -> T

    // Blocks and statements
    std::unique_ptr<Block> ParseBlock(std::string_view role = "the block");
    StmtPtr ParseStmt();
    std::unique_ptr<LetStmt> ParseLetStmt();
    std::unique_ptr<IfStmt> ParseIfStmt();
    std::unique_ptr<WhileStmt> ParseWhileStmt();
    std::unique_ptr<DoWhileStmt> ParseDoWhileStmt();
    std::unique_ptr<LoopStmt> ParseLoopStmt();
    std::unique_ptr<ForStmt> ParseForStmt();
    std::unique_ptr<MatchStmt> ParseMatchStmt();
    std::unique_ptr<ReturnStmt> ParseReturnStmt();
    std::unique_ptr<DeferStmt> ParseDeferStmt();

    // Expressions (Pratt / precedence-climbing)
    ExprPtr ParseExpr();
    ExprPtr ParseExprImpl();
    ExprPtr ParseRequiredExpr(std::string_view context = {});
    void EmitMissingExpression(std::string_view context = {});
    ExprPtr ParseAssign();
    ExprPtr ParseRange();
    ExprPtr ParseTernary();
    ExprPtr ParseCoalesce();
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
    ExprPtr ParseUnary();
    ExprPtr ParsePostfix();
    ExprPtr ParsePrimary();

    // Patterns
    PatternPtr ParsePattern();
    PatternPtr ParsePatternImpl();
    PatternPtr ParseRequiredPattern(std::string_view context = {});
    PatternPtr ParsePrimaryPattern();
    PatternPtr ParseMatchArmPattern();

    // Expression argument list
    std::vector<ExprPtr> ParseArgList(); ///< ( expr, ... )
};
} // namespace Rux
