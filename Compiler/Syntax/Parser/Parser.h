#pragma once

#include "Diagnostics/Diagnostics.h"
#include "Lexer/Lexer.h"
#include "Syntax/ParseResult.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
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
    /// Whether the statement being read is in a value block (an `if` branch, or a match or catch arm block), where the
    /// last expression may leave out its ';' to be the value the block yields.
    bool blockYieldsValue = false;
    /// Whether the then branch of a conditional `c ? a : b` is being read, where `x is name: T` would take the ':'
    /// separating the branches; there `is` keeps reading a type.
    bool inConditionalThen = false;
    /// Whether the operand of a `try` is being read, where `catch` ends the operand and starts the arms of `try ...
    /// catch` instead of being a postfix recovery. A nested expression -- an argument, an element, a grouped value --
    /// is read whole, so it clears this again.
    bool tryOperandEndsAtCatch = false;
    /// Whether a block so far held a `when` statement or a nested declaration (`Module::bodiesNeedConditionalFolding`).
    bool bodiesNeedConditionalFolding = false;
    /// Whether a block so far declared anything but a constant (`Module::bodiesDeclareItems`).
    bool bodiesDeclareItems = false;
    /// How many calls have been read, so a constant can tell whether its initializer holds one.
    std::size_t callsRead = 0;
    /// Whether a constant's initializer so far held a call (`Module::constantsCall`).
    bool constantsCall = false;

    /// What `Self` and a shorthand receiver such as `&self` stand for where the parser is. Inside `extend T` both name
    /// `T` with its type arguments, read again from the tokens the header spelled it with, so every later stage sees
    /// the type written out; inside an interface they name the implementing type, which only a bound supplies, so they
    /// stay `Self`; anywhere else they name nothing.
    struct SelfScope {
        enum class Kind : std::uint8_t {
            None,
            Interface,
            Extend,
        };

        Kind kind = Kind::None;
        /// Inside `extend`: the extended type's tokens and an end-of-file token after them. Empty when the header did
        /// not parse, whose error has then already been reported.
        std::vector<Token> typeTokens;
    };

    SelfScope selfScope;

    // Token helpers, defined here because every level of every expression asks them, from every parser file.
    [[nodiscard]] const Token &Peek(const std::size_t ahead = 0) const noexcept {
        const std::size_t index = pos + ahead;
        return index < tokens.size() ? tokens[index] : tokens.back(); // the last token is the EOF sentinel
    }

    const Token &Advance() noexcept {
        if (!IsAtEnd()) {
            ++pos;
        }
        return tokens[pos - 1];
    }

    [[nodiscard]] bool Check(const TokenKind kind) const noexcept {
        return Peek().kind == kind;
    }

    [[nodiscard]] bool CheckAny(const std::initializer_list<TokenKind> kinds) const noexcept {
        const TokenKind current = Peek().kind;
        for (const auto kind : kinds) {
            if (current == kind) {
                return true;
            }
        }
        return false;
    }

    /// Whether the current token closes a generic argument list. A `>>` or `>>>` written where two or three of them
    /// end at once is a run of closers rather than a shift, and only the parser knows which it is looking at.
    [[nodiscard]] bool CheckCloseAngle() const noexcept;

    /// Consumes one closing `>`. A longer run is narrowed in place rather than advanced past, leaving the rest for
    /// the enclosing list to close with.
    void ConsumeCloseAngle() noexcept;

    bool Match(const TokenKind kind) noexcept {
        if (!Check(kind)) {
            return false;
        }
        Advance();
        return true;
    }

    const Token &Expect(TokenKind kind, std::string_view message);
    /// Grammar-aware diagnostics name both the role of a missing token and the token that prevented it from being
    /// parsed.
    const Token &ExpectBefore(TokenKind kind, std::string_view expected, std::optional<std::string> help = {});
    bool ConsumeBodyStart(std::string_view role);

    [[nodiscard]] bool IsAtEnd() const noexcept {
        return Peek().kind == TokenKind::EndOfFile;
    }

    [[nodiscard]] const Token &Previous() const noexcept;

    [[nodiscard]] SourceLocation CurrentLocation() const noexcept {
        return Peek().location;
    }

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
        bool usedTest = false;
        bool usedUnsendable = false;
        bool usedNoCopy = false;
        bool usedNoMove = false;
        bool usedAbi = false;
        SourceLocation linkLocation;
        SourceLocation noReturnLocation;
        SourceLocation testLocation;
        SourceLocation unsendableLocation;
        SourceLocation noCopyLocation;
        SourceLocation noMoveLocation;
        SourceLocation abiLocation;
        SourceLocation allowLocation;
    };

    /// Parses the `#Name` and `#Name(...)` attributes before a declaration. The former `#{...}` metadata-block form is
    /// rejected.
    ParsedAttrs ParseAttrs();
    static std::string DecodeStringLiteralText(const std::string &text);
    void ParseAttributeCall(ParsedAttrs &attrs);
    /// The optional `()` after an attribute that takes no arguments, such as `#Test`: reports anything written inside.
    void ParseEmptyAttributeArguments(std::string_view name);
    /// Parses one `#Name` attribute written before a parameter, with the current token on its '#'. `#Format` is the
    /// only parameter attribute.
    void ParseParameterAttribute(Param &parameter);
    /// Checks where `#Format` was written in one parameter list: once, on a parameter that is neither the receiver
    /// nor variadic, in a list whose last parameter is variadic.
    void ValidateFormatParameters(const std::vector<Param> &params);
    DeclPtr ApplyAttrs(DeclPtr decl, ParsedAttrs &attrs);
    DeclPtr ParseExternDecl(bool isPublic, ParsedAttrs &attrs);
    DeclPtr ParseIntrinsicDecl(bool isPublic, ParsedAttrs &attrs, SourceLocation intrinsicLoc);

    // Declarations
    /// A function declaration from its `func`, or from an `unsafe` written before it.
    std::unique_ptr<FuncDecl> ParseFuncDecl(bool isPublic, bool isAsm,
                                            CallingConvention callConv = CallingConvention::Default);
    /// Whether a function declaration starts here: `func`, or `unsafe func`.
    [[nodiscard]] bool CheckFuncStart() const noexcept;
    /// Consumes an `unsafe` that no `func` follows and reports it, naming the extern or `asm` declaration it was
    /// written before, which is unsafe to use already.
    void RejectMisplacedUnsafe();
    /// Reports `unsafe` on a method nothing calls by name: a destructor, an operator, or an interface implementation.
    void RejectImplicitlyCalledUnsafe(const FuncDecl &method, const std::optional<std::string> &interfaceName);
    std::unique_ptr<StructDecl> ParseStructDecl(bool isPublic);
    std::unique_ptr<EnumDecl> ParseEnumDecl(bool isPublic);
    std::unique_ptr<UnionDecl> ParseUnionDecl(bool isPublic);
    std::unique_ptr<InterfaceDecl> ParseInterfaceDecl(bool isPublic);
    std::unique_ptr<ImplDecl> ParseImplDecl();
    std::unique_ptr<ModuleDecl> ParseModuleDecl(bool isPublic);
    std::unique_ptr<UseDecl> ParseUseDecl(bool requireSemicolon = true);
    std::unique_ptr<ConstDecl> ParseConstDecl(bool isPublic);
    std::unique_ptr<GlobalDecl> ParseGlobalDecl(bool isPublic);
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

    /// Parses the type an `extend` header names into `type` and returns its tokens for `Self` to read again: empty
    /// when the type did not parse cleanly.
    std::vector<Token> ParseExtendedType(TypeExprPtr &type, std::string_view help);
    /// The type `Self` names at `location` in the current scope: the extended type, `Self` itself in an interface,
    /// and, reported as an error, `Self` anywhere else.
    TypeExprPtr SelfTypeAt(SourceLocation location);

    /// `Self` read as the name of the extended type in an expression or pattern: its path and, for a generic type,
    /// its type arguments. Reports an error and returns nothing where `Self` cannot be spelled as a name.
    struct SelfName {
        std::vector<std::string> segments;
        std::vector<TypeExprPtr> typeArgs;

        [[nodiscard]] std::string Joined() const;
    };

    std::optional<SelfName> SelfNameAt(SourceLocation location);
    /// The expression `Self` starts, with `Self` already consumed: a struct literal `Self { ... }`, a constructor call
    /// `Self(...)`, or a path such as `Self::Function(...)`, each written as if the extended type were spelled out.
    ExprPtr ParseSelfExpr(SourceLocation location);
    /// Whether the current tokens are a shorthand receiver: `self`, `&self`, `&var self`, `*self` or `*var self`
    /// followed by ',' or ')'.
    [[nodiscard]] bool CheckShorthandReceiver() const noexcept;
    /// Parses a shorthand receiver into `parameter`, giving it the type its form names in the current scope.
    void ParseShorthandReceiver(Param &parameter);

    // Shared declaration helpers
    Param ParseParam(bool allowVariadic = false);
    std::vector<Param> ParseParamList(bool allowVariadic = false);
    std::vector<TypeParameter> ParseTypeParams(); ///< <T, U: Display + Debug, ...>
    TypeExprPtr ParseInterfaceBound();            ///< Display or Namespace::Display
    std::vector<TypeExprPtr> ParseTypeArgs();     ///< <int32, T[], ...>

    // Type expressions
    TypeExprPtr ParseType(std::optional<std::string> help = {});        ///< any type, including T ! E and A | B
    TypeExprPtr ParseSumType(std::optional<std::string> help = {});     ///< a type that stops before an ungrouped '!'
    TypeExprPtr ParseRangeType(std::optional<std::string> help = {});   ///< var T[..] and T..T; stops before '|' or '!'
    TypeExprPtr ParsePostfixType(std::optional<std::string> help = {}); ///< a type without a range operator after it
    TypeExprPtr ParseBaseType(std::optional<std::string> help = {});    ///< named, path, pointer, tuple, self
    TypeExprPtr ParseFunctionType();                                    ///< func(params) -> T
    /// A `throws E` clause after a function's return type, or after its parameters when it has none: it makes
    /// `returnType` the fallible `T ! E` the clause spells, marked as written with `throws`.
    void ParseThrowsClause(std::optional<TypeExprPtr> &returnType);
    [[nodiscard]] bool CanStartType() const noexcept; ///< whether the next token begins a type

    // Blocks and statements
    /// A braced block. A value block lets its last expression leave out the ';' to be the value the block yields.
    std::unique_ptr<Block> ParseBlock(std::string_view role = "the block", bool valueBlock = false);
    StmtPtr ParseStmt();
    /// `unsafe { ... }`, with the current token on `unsafe`: a value block in which unsafe operations are allowed.
    std::unique_ptr<BlockExpr> ParseUnsafeBlock();
    std::unique_ptr<LetStmt> ParseLetStmt();
    std::unique_ptr<IfStmt> ParseIfStmt();
    /// An `if` in value position: an `if` statement read whole and written as the match on its condition that
    /// `MatchExpr::Form::If` describes. It needs an `else`.
    ExprPtr ParseIfExpr();
    /// `func(x: int) -> bool { ... }` in value position: a function literal, whose parameter and result types may be
    /// left out for the expected function type to supply.
    ExprPtr ParseFunctionLiteral();
    /// The match `MatchExpr::Form::If` describes for a run-time `if` chain, which `statement` gives up its parts to.
    static ExprPtr IfAsExpression(IfStmt &statement);
    /// Whether a run-time `if` that ends a value block yields a value: it has an `else` and a branch ends with an
    /// expression written without its ';'.
    [[nodiscard]] static bool IfYieldsValue(const IfStmt &statement) noexcept;
    std::unique_ptr<WhileStmt> ParseWhileStmt();
    std::unique_ptr<DoWhileStmt> ParseDoWhileStmt();
    std::unique_ptr<LoopStmt> ParseLoopStmt();
    std::unique_ptr<ForStmt> ParseForStmt();
    /// Whether `pattern` matches every value of the type it takes apart, as a `for` pattern must: names, `_`, and
    /// tuples and structures made of them.
    [[nodiscard]] static bool PatternMatchesEveryElement(const Pattern &pattern) noexcept;
    std::unique_ptr<MatchStmt> ParseMatchStmt();
    std::unique_ptr<ReturnStmt> ParseReturnStmt();
    /// `throw error;` or the older `fail error;`.
    std::unique_ptr<FailStmt> ParseFailStmt();
    /// `try { ... } catch { arms }`.
    std::unique_ptr<TryStmt> ParseTryStmt();
    /// Reports `finally`, which Rux does not have, and skips the block after it.
    void RejectFinally();
    /// Report a `catch`, `?`, or `??` written directly after a match statement, which takes no postfix operator.
    void RejectPostfixAfterMatchStatement();
    std::unique_ptr<DeferStmt> ParseDeferStmt();

    // Expressions (Pratt / precedence-climbing)
    ExprPtr ParseExpr();
    ExprPtr ParseExprImpl();
    ExprPtr ParseRequiredExpr(std::string_view context = {});
    void EmitMissingExpression(std::string_view context = {});
    /// An expression that may start with `try`, which covers everything to its right.
    ExprPtr ParseTryPrefix();
    /// `try expr`, `try expr catch { arms }` or `try? expr`, with `try` the current token. `misplaced` is a `try` found
    /// where an operand was expected, to the right of an operator, which is reported.
    ExprPtr ParseTryExpr(bool misplaced);
    ExprPtr ParseAssign();
    ExprPtr ParseRange();
    ExprPtr ParseTernary();
    /// Report `...` where a range operator is read: the inclusive range is spelled `..=`.
    void RejectEllipsisRange();
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
    /// The fields of a struct or case literal from its '{' through its '}'. A field written as a name alone takes the
    /// binding of that name, as if written `name: name`.
    void ParseStructInitFields(StructInitExpr &initializer);
    /// Whether the current token begins `fail`, `throw`, `return`, `break`, or `continue`.
    [[nodiscard]] bool CheckDivergingKeyword() const noexcept;
    /// One of the diverging forms in a position that accepts it: a whole arm, mapping, or `??` fallback body.
    ExprPtr ParseDivergingExpr();
    /// A match or catch arm body: a block, a diverging form, or an expression.
    ExprPtr ParseArmBody(std::string_view construct);
    /// The comma-separated arms of a match or catch after its opening brace, through the closing brace. A comma may
    /// follow the last arm, and an arm whose body is a block may leave it out. `form` names what the brace closes.
    template <typename Arm>
    void ParseArmList(std::vector<Arm> &arms, std::string_view construct, std::string_view form = "expression");
    /// Reports a default arm (`_` or `else`) at `location` that other arms follow.
    void ReportNonfinalDefaultArm(SourceLocation location, std::string_view construct);
    /// Whether the current tokens begin the default arm of a compile-time `when` match: `_ =>` or `else`.
    [[nodiscard]] bool CheckWhenDefaultArm() const noexcept;
    /// The patterns of one compile-time `when` arm, up to its `=>`: alternatives separated by `|` or `,`.
    void ParseWhenArmPatterns(std::vector<ExprPtr> &patterns);
    /// The `else (binding => body)` mapping after a tight `?`, which wraps `operand` in a mapped propagation.
    ExprPtr ParseErrorMapper(SourceLocation location, ExprPtr operand);

    // Patterns
    PatternPtr ParsePattern();
    PatternPtr ParsePatternImpl();
    PatternPtr ParseRequiredPattern(std::string_view context = {});
    PatternPtr ParsePrimaryPattern();
    /// One pattern without alternatives or a guard: a primary pattern, its presence suffix, and a range.
    PatternPtr ParseSinglePattern();
    PatternPtr ParseMatchArmPattern();
    /// Whether the tokens after `is` begin a pattern rather than the type `x is T` tests.
    [[nodiscard]] bool CheckPatternAfterIs() const noexcept;
    /// The pattern after `is`: alternatives of single patterns, without a guard.
    PatternPtr ParseIsPattern();
    /// Wrap `inner` in one presence pattern per `?` that follows it; `??` is two.
    PatternPtr ParsePresenceSuffix(PatternPtr inner);

    // Expression argument list
    std::vector<ExprPtr> ParseArgList(); ///< ( expr, ... )
};

extern template void Parser::ParseArmList<MatchExpr::Arm>(std::vector<MatchExpr::Arm> &, std::string_view,
                                                          std::string_view);
extern template void Parser::ParseArmList<MatchStmt::Arm>(std::vector<MatchStmt::Arm> &, std::string_view,
                                                          std::string_view);
} // namespace Rux
