#pragma once

#include "SourceModel/SourceLocation.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace Rux {
/// Every kind of token the language has. The trailing comment on each enumerator is the source spelling it stands for,
/// which is the only documentation most of them need.
enum class TokenKind : std::uint8_t {
    // Literals
    IntLiteral,    // 42  0xFF  0b1010  0o77
    FloatLiteral,  // 3.14  1.0e-9
    StringLiteral, // "hello"  s8"hello"  s16"hello"  s32"hello"
    CharLiteral,   // 'A'
    BoolLiteral,   // true  false

    // Identifiers
    Ident, // foo  Bar  _x

    // Keywords, alphabetized by source spelling
    AsKeyword,        // as
    BreakKeyword,     // break
    ConstKeyword,     // const
    ContinueKeyword,  // continue
    DeferKeyword,     // defer
    DoKeyword,        // do
    ElseKeyword,      // else
    EnumKeyword,      // enum
    ExtendKeyword,    // extend
    ExternKeyword,    // extern
    ForKeyword,       // for
    FuncKeyword,      // func
    IfKeyword,        // if
    ImportKeyword,    // import
    InKeyword,        // in
    InterfaceKeyword, // interface
    IntrinsicKeyword, // intrinsic
    IsKeyword,        // is
    LetKeyword,       // let
    LoopKeyword,      // loop
    MatchKeyword,     // match
    ModuleKeyword,    // module
    NullKeyword,      // null
    PubKeyword,       // pub
    ReturnKeyword,    // return
    SelfKeyword,      // self
    StructKeyword,    // struct
    TypeKeyword,      // type
    UnionKeyword,     // union
    VarKeyword,       // var
    VariantKeyword,   // variant
    WhenKeyword,      // when
    WhileKeyword,     // while

    // Punctuation
    LeftParen,        // (
    RightParen,       // )
    LeftBrace,        // {
    RightBrace,       // }
    LeftBracket,      // [
    RightBracket,     // ]
    Comma,            // ,
    Semicolon,        // ;
    Colon,            // :
    ColonColon,       // ::
    Dot,              // .
    DotDot,           // ..
    DotDotDot,        // ...
    DotDotEqual,      // ..=
    Arrow,            // ->
    FatArrow,         // =>
    At,               // @
    Hash,             // #
    Question,         // ?
    QuestionQuestion, // ?? -- built-in Option coalescing, not a declarable operator

    // Arithmetic operators
    Plus,       // +
    Minus,      // -
    Star,       // *
    Slash,      // /
    Percent,    // %
    PlusPlus,   // ++
    MinusMinus, // --

    // Bitwise operators
    Amp,                   // &
    Pipe,                  // |
    Caret,                 // ^
    Tilde,                 // ~
    LessLess,              // <<
    GreaterGreater,        // >>
    GreaterGreaterGreater, // >>>

    // Logical operators
    AmpAmp,   // &&
    PipePipe, // ||
    Bang,     // !

    // Comparison operators
    Equal,        // ==
    BangEqual,    // !=
    Less,         // <
    LessEqual,    // <=
    Greater,      // >
    GreaterEqual, // >=

    // Assignment operators
    MoveArrow,                   // <-
    Assign,                      // =
    PlusAssign,                  // +=
    MinusAssign,                 // -=
    StarAssign,                  // *=
    SlashAssign,                 // /=
    PercentAssign,               // %=
    AmpAssign,                   // &=
    PipeAssign,                  // |=
    CaretAssign,                 // ^=
    LessLessAssign,              // <<=
    GreaterGreaterAssign,        // >>=
    GreaterGreaterGreaterAssign, // >>>=

    // Special
    DocComment, // /// outer documentation attached to the following item
    NewLine,    // significant newline (if the grammar uses them)
    EndOfFile,  // end of file
    Unknown,    // unrecognized character — carry it for better errors
};

/// One token, keeping the text it was written as. The spelling is retained even where the kind implies it, so a
/// diagnostic can quote what the author actually typed.
struct Token {
    TokenKind kind = TokenKind::Unknown;
    std::string text; ///< original source spelling
    SourceLocation location;
    /// Half-open end of this token when the lexer needs its complete spelling for tooling. Documentation tokens always
    /// set it; older hand-built and ordinary tokens may leave it at the default location.
    SourceLocation endLocation;
    /// Whether whitespace, a newline or a comment separates this token from the one before it. Only `?` reads it: `x?`
    /// propagates a failure while `x ? a : b` selects between two values, and the separation is what tells them apart.
    /// Defaults to separated, so a token built by hand rather than scanned keeps the older of the two meanings.
    bool precededBySpace = true;
    /// Documentation attachment metadata. `lineLeading` means only indentation precedes a documentation token on its
    /// source line. `precededByOrdinaryComment` applies to every token and records ordinary trivia since the last
    /// token.
    bool lineLeading = false;
    bool precededByOrdinaryComment = false;

    // Convenience predicates
    [[nodiscard]] bool Is(const TokenKind k) const noexcept {
        return kind == k;
    }

    [[nodiscard]] bool IsKeyword() const noexcept;
    [[nodiscard]] bool IsLiteral() const noexcept;
    [[nodiscard]] bool IsOperator() const noexcept;

    [[nodiscard]] bool IsEof() const noexcept {
        return kind == TokenKind::EndOfFile;
    }

    /// Human-readable description for diagnostics. Names this token, spelling included, where `TokenKindName` names
    /// only its kind.
    [[nodiscard]] std::string Describe() const;
};

/// Map a keyword string to its TokenKind; returns TokenKind::Ident if not a keyword.
[[nodiscard]] TokenKind KeywordKind(std::string_view text) noexcept;

/// Name of a TokenKind suitable for error messages.
[[nodiscard]] std::string_view TokenKindName(TokenKind kind) noexcept;
} // namespace Rux
