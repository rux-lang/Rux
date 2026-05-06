/*
    Rux Compiler
    Copyright © 2026 Ivan Muzyka
    Licensed under the MIT License
*/

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace Rux
{
    enum class TokenKind : std::uint8_t
    {
        // Literals
        IntLiteral, // 42  0xFF  0b1010  0o77
        FloatLiteral, // 3.14  1.0e-9
        StringLiteral, // "hello"  c8"hello"  c16"hello"  c32"hello"
        CharLiteral, // 'A'
        BoolLiteral, // true  false

        // Identifiers & keywords
        Ident, // foo  Bar  _x

        // Control flow
        IfKeyword, // if
        ElseKeyword, // else
        WhileKeyword, // while
        DoKeyword, // do
        LoopKeyword, // loop
        ForKeyword, // for
        InKeyword, // in
        BreakKeyword, // break
        ContinueKeyword, // continue
        ReturnKeyword, // return
        MatchKeyword, // match

        // Declarations
        FuncKeyword, // func
        LetKeyword, // let
        VarKeyword, // var
        ConstKeyword, // const
        TypeKeyword, // type
        StructKeyword, // struct
        EnumKeyword, // enum
        UnionKeyword, // union
        InterfaceKeyword, // interface
        ImplKeyword, // impl
        ModKeyword, // mod
        UseKeyword, // use
        ImportKeyword, // import
        PubKeyword, // pub
        ExternKeyword, // extern

        // Other keywords
        AsKeyword, // as
        IsKeyword, // is
        NullKeyword, // null
        SelfKeyword, // self
        SuperKeyword, // super

        // Punctuation
        LeftParen, // (
        RightParen, // )
        LeftBrace, // {
        RightBrace, // }
        LeftBracket, // [
        RightBracket, // ]
        Comma, // ,
        Semicolon, // ;
        Colon, // :
        ColonColon, // ::
        Dot, // .
        DotDot, // ..
        DotDotDot, // ...
        DotDotEqual, // ..=
        Arrow, // ->
        FatArrow, // =>
        At, // @
        Hash, // #
        Question, // ?

        // Arithmetic operators
        Plus, // +
        Minus, // -
        Star, // *
        Slash, // /
        Percent, // %
        StarStar, // **
        PlusPlus, // ++
        MinusMinus, // --

        // Bitwise operators
        Amp, // &
        Pipe, // |
        Caret, // ^
        Tilde, // ~
        LessLess, // <<
        GreaterGreater, // >>

        // Logical operators
        AmpAmp, // &&
        PipePipe, // ||
        Bang, // !

        // Comparison operators
        Equal, // ==
        BangEqual, // !=
        Less, // <
        LessEqual, // <=
        Greater, // >
        GreaterEqual, // >=

        // Assignment operators
        Assign, // =
        PlusAssign, // +=
        MinusAssign, // -=
        StarAssign, // *=
        SlashAssign, // /=
        PercentAssign, // %=
        AmpAssign, // &=
        PipeAssign, // |=
        CaretAssign, // ^=
        LessLessAssign, // <<=
        GreaterGreaterAssign, // >>=

        // Special
        NewLine, // significant newline (if the grammar uses them)
        EndOfFile, // end of file
        Unknown, // unrecognized character — carry it for better errors
    };

    struct SourceLocation
    {
        std::uint32_t line = 1; // 1-based
        std::uint32_t column = 1; // 1-based (UTF-8 byte offset in line)
        std::uint32_t offset = 0; // byte offset from start of file
    };

    struct Token
    {
        TokenKind kind = TokenKind::Unknown;
        std::string text; // original source spelling
        SourceLocation location;

        // Convenience predicates
        [[nodiscard]] bool Is(const TokenKind k) const noexcept { return kind == k; }
        [[nodiscard]] bool IsKeyword() const noexcept;
        [[nodiscard]] bool IsLiteral() const noexcept;
        [[nodiscard]] bool IsOperator() const noexcept;
        [[nodiscard]] bool IsEof() const noexcept { return kind == TokenKind::EndOfFile; }

        // Human-readable description for diagnostics
        [[nodiscard]] std::string Describe() const;
    };

    // Map a keyword string to its TokenKind; returns TokenKind::Ident if not a keyword.
    [[nodiscard]] TokenKind KeywordKind(std::string_view text) noexcept;

    // Name of a TokenKind suitable for error messages.
    [[nodiscard]] std::string_view TokenKindName(TokenKind kind) noexcept;
}
