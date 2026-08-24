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
    StringLiteral, // "hello"  c8"hello"  c16"hello"  c32"hello"
    CharLiteral,   // 'A'
    BoolLiteral,   // true  false

    // Identifiers & keywords
    Ident, // foo  Bar  _x

    // Control flow
    IfKeyword,       // if
    WhenKeyword,     // when
    ElseKeyword,     // else
    WhileKeyword,    // while
    DoKeyword,       // do
    LoopKeyword,     // loop
    ForKeyword,      // for
    InKeyword,       // in
    BreakKeyword,    // break
    ContinueKeyword, // continue
    ReturnKeyword,   // return
    MatchKeyword,    // match

    // Declarations
    IntrinsicKeyword, // intrinsic
    FuncKeyword,      // func
    LetKeyword,       // let
    VarKeyword,       // var
    ConstKeyword,     // const
    TypeKeyword,      // type
    StructKeyword,    // struct
    EnumKeyword,      // enum
    UnionKeyword,     // union
    InterfaceKeyword, // interface
    ExtendKeyword,    // extend
    ModuleKeyword,    // module
    ImportKeyword,    // import
    PubKeyword,       // pub
    ExternKeyword,    // extern

    // Other keywords
    AsKeyword,    // as
    IsKeyword,    // is
    NullKeyword,  // null
    SelfKeyword,  // self
    SuperKeyword, // super

    // Punctuation
    LeftParen,    // (
    RightParen,   // )
    LeftBrace,    // {
    RightBrace,   // }
    LeftBracket,  // [
    RightBracket, // ]
    Comma,        // ,
    Semicolon,    // ;
    Colon,        // :
    ColonColon,   // ::
    Dot,          // .
    DotDot,       // ..
    DotDotDot,    // ...
    DotDotEqual,  // ..=
    Arrow,        // ->
    FatArrow,     // =>
    At,           // @
    Hash,         // #
    Question,     // ?

    // Arithmetic operators
    Plus,       // +
    Minus,      // -
    Star,       // *
    Slash,      // /
    Percent,    // %
    StarStar,   // **
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
    /// Whether whitespace, a newline or a comment separates this token from the one before it. Only `?` reads it: `x?`
    /// propagates a failure while `x ? a : b` selects between two values, and the separation is what tells them apart.
    /// Defaults to separated, so a token built by hand rather than scanned keeps the older of the two meanings.
    bool precededBySpace = true;

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
