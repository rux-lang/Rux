/*
    Rux Compiler
    Copyright © 2026 Ivan Muzyka
    Licensed under the MIT License
*/

#include "Rux/Lexer.h"

#include <cassert>
#include <cctype>
#include <fstream>
#include <print>
#include <sstream>
#include <ostream>

namespace Rux
{
    bool LexerResult::HasErrors() const noexcept
    {
        for (const auto& d : diagnostics)
        {
            if (d.severity == LexerDiagnostic::Severity::Error)
                return true;
        }
        return false;
    }


    Lexer::Lexer(std::string source, std::string sourceName)
        : source(std::move(source))
          , sourceName(std::move(sourceName))
    {
    }

    std::optional<LexerResult> Lexer::FromFile(const std::filesystem::path& path)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f)
        {
            std::print(stderr, "error: cannot open '{}'\n", path.string());
            return std::nullopt;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        if (!f && !f.eof())
        {
            std::print(stderr, "error: failed to read '{}'\n", path.string());
            return std::nullopt;
        }
        Lexer lex(ss.str(), path.string());
        return lex.Tokenize();
    }

    LexerResult Lexer::Tokenize()
    {
        tokens.clear();
        diagnostics.clear();
        pos = 0;
        line = 1;
        col = 1;
        ScanAll();
        // Always append a synthetic EOF token
        tokens.push_back(Token{
            TokenKind::EndOfFile,
            {},
            CurrentLocation()
        });
        return LexerResult{std::move(tokens), std::move(diagnostics)};
    }

    bool Lexer::DumpTokens(const LexerResult& result,
                           const std::filesystem::path& path)
    {
        std::ofstream f(path);
        if (!f) return false;
        for (const auto& tok : result.tokens)
        {
            std::print(f, "{:>4}:{:<4}  {:<16}  {}\n",
                       tok.location.line,
                       tok.location.column,
                       std::string(TokenKindName(tok.kind)),
                       tok.text);
        }
        if (!result.diagnostics.empty())
        {
            std::print(f, "\n--- diagnostics ---\n");
            for (const auto& d : result.diagnostics)
            {
                std::print(f, "{:>4}:{:<4}  {}  {}\n",
                           d.location.line,
                           d.location.column,
                           d.severity == LexerDiagnostic::Severity::Error
                               ? "error  "
                               : "warning",
                           d.message);
            }
        }
        return f.good();
    }

    void Lexer::ScanAll()
    {
        while (!IsAtEnd())
        {
            SkipWhitespace();
            if (IsAtEnd()) break;

            if (Token tok = NextToken(); tok.kind != TokenKind::Unknown || !tok.text.empty())
                tokens.push_back(std::move(tok));
        }
    }

    // NextToken – dispatch on the current character
    Token Lexer::NextToken()
    {
        const SourceLocation start = CurrentLocation();
        const char c = Peek();
        // Prefixed string literals
        if (c == 'c')
        {
            if (Peek(1) == '8' && Peek(2) == '"')
                return ScanString(start, 2);
            if (Peek(1) == '8' && Peek(2) == '\'')
                return ScanChar(start, 2);
            if (Peek(1) == '1' && Peek(2) == '6' && Peek(3) == '"')
                return ScanString(start, 3);
            if (Peek(1) == '1' && Peek(2) == '6' && Peek(3) == '\'')
                return ScanChar(start, 3);
            if (Peek(1) == '3' && Peek(2) == '2' && Peek(3) == '"')
                return ScanString(start, 3);
            if (Peek(1) == '3' && Peek(2) == '2' && Peek(3) == '\'')
                return ScanChar(start, 3);
        }
        // Identifiers / keywords
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_')
            return ScanIdent(start);
        // Numeric literals
        if (std::isdigit(static_cast<unsigned char>(c)))
            return ScanNumber(start);
        // String literals
        if (c == '"')
            return ScanString(start);
        // Char literals
        if (c == '\'')
            return ScanChar(start);
        // Operators, punctuation, and everything else
        return ScanSymbol(start);
    }

    // Character helpers
    bool Lexer::IsAtEnd() const noexcept
    {
        return pos >= source.size();
    }

    char Lexer::Peek(std::size_t ahead) const noexcept
    {
        const std::size_t idx = pos + ahead;
        return (idx < source.size()) ? source[idx] : '\0';
    }

    char Lexer::Advance() noexcept
    {
        const char c = source[pos++];
        if (c == '\n')
        {
            ++line;
            col = 1;
        }
        else
        {
            ++col;
        }
        return c;
    }

    bool Lexer::Match(char expected) noexcept
    {
        if (IsAtEnd() || source[pos] != expected)
            return false;
        Advance();
        return true;
    }

    bool Lexer::MatchStr(std::string_view s) noexcept
    {
        if (pos + s.size() > source.size()) return false;
        for (std::size_t i = 0; i < s.size(); ++i)
            if (source[pos + i] != s[i]) return false;
        for (std::size_t i = 0; i < s.size(); ++i) Advance();
        return true;
    }

    SourceLocation Lexer::CurrentLocation() const noexcept
    {
        return {line, col, static_cast<std::uint32_t>(pos)};
    }

    // Whitespace / comments
    void Lexer::SkipWhitespace()
    {
        while (!IsAtEnd())
        {
            const char c = Peek();
            // Inline whitespace
            if (c == ' ' || c == '\t' || c == '\r')
            {
                Advance();
                continue;
            }
            // Newlines – skip (emit Newline token here if your grammar needs them)
            if (c == '\n')
            {
                Advance();
                continue;
            }
            // Line comment
            if (c == '/' && Peek(1) == '/')
            {
                SkipLineComment();
                continue;
            }
            // Block comment
            if (c == '/' && Peek(1) == '*')
            {
                SkipBlockComment();
                continue;
            }
            break;
        }
    }

    void Lexer::SkipLineComment()
    {
        // Consume everything up to (but not including) the newline
        while (!IsAtEnd() && Peek() != '\n')
            Advance();
    }

    void Lexer::SkipBlockComment()
    {
        // Consume opening  /*
        Advance();
        Advance();
        int depth = 1; // supports nested  /* /* */ */
        while (!IsAtEnd() && depth > 0)
        {
            if (Peek() == '/' && Peek(1) == '*')
            {
                Advance();
                Advance();
                ++depth;
            }
            else if (Peek() == '*' && Peek(1) == '/')
            {
                Advance();
                Advance();
                --depth;
            }
            else
            {
                Advance();
            }
        }
        if (depth > 0)
            EmitError(CurrentLocation(), "unterminated block comment");
    }

    Token Lexer::ScanIdent(SourceLocation start)
    {
        const std::size_t tokenStart = pos;
        while (!IsAtEnd())
        {
            const char c = Peek();
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
                break;
            Advance();
        }
        std::string text = source.substr(tokenStart, pos - tokenStart);
        const TokenKind kind = KeywordKind(text);
        return Token{kind, std::move(text), start};
    }

    Token Lexer::ScanNumber(SourceLocation start)
    {
        const std::size_t tokenStart = pos;
        // Detect base prefix: 0x  0b  0o
        if (Peek() == '0')
        {
            const char next = Peek(1);
            if (next == 'x' || next == 'X')
            {
                Advance();
                Advance(); // consume  0x
                return ScanIntLiteral(start, tokenStart);
            }
            if (next == 'b' || next == 'B')
            {
                Advance();
                Advance(); // consume  0b
                return ScanIntLiteral(start, tokenStart);
            }
            if (next == 'o' || next == 'O')
            {
                Advance();
                Advance(); // consume  0o
                return ScanIntLiteral(start, tokenStart);
            }
        }
        // Decimal integer digits
        while (!IsAtEnd() && std::isdigit(static_cast<unsigned char>(Peek())))
            Advance();
        // Check for floating-point  .  or  e/E
        const bool hasDot = Peek() == '.' && std::isdigit(static_cast<unsigned char>(Peek(1)));
        const bool hasExp = (Peek() == 'e' || Peek() == 'E');
        if (hasDot || hasExp)
            return ScanFloatSuffix(start, tokenStart);
        ConsumeNumberSuffix();
        return MakeToken(TokenKind::IntLiteral, start, tokenStart);
    }

    Token Lexer::ScanIntLiteral(SourceLocation start, std::size_t tokenStart)
    {
        // Consume hex / binary / octal digits (and underscores as separators)
        while (!IsAtEnd())
        {
            const char c = Peek();
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
                Advance();
            else
                break;
        }
        // TODO: validate digits match the declared base
        return MakeToken(TokenKind::IntLiteral, start, tokenStart);
    }

    Token Lexer::ScanFloatSuffix(SourceLocation start, std::size_t tokenStart)
    {
        // Fractional part
        if (Peek() == '.')
        {
            Advance(); // consume  .
            while (!IsAtEnd() && std::isdigit(static_cast<unsigned char>(Peek())))
                Advance();
        }
        // Exponent part  e[+-]digits
        if (Peek() == 'e' || Peek() == 'E')
        {
            Advance();
            if (Peek() == '+' || Peek() == '-') Advance();
            if (!std::isdigit(static_cast<unsigned char>(Peek())))
            {
                EmitError(start, "expected digits after exponent");
            }
            else
            {
                while (!IsAtEnd() && std::isdigit(static_cast<unsigned char>(Peek())))
                    Advance();
            }
        }
        ConsumeNumberSuffix();
        return MakeToken(TokenKind::FloatLiteral, start, tokenStart);
    }

    void Lexer::ConsumeNumberSuffix()
    {
        if (!std::isalpha(static_cast<unsigned char>(Peek())))
            return;
        while (!IsAtEnd())
        {
            const char c = Peek();
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
                break;
            Advance();
        }
    }

    Token Lexer::ScanString(SourceLocation start, std::size_t prefixLen)
    {
        const std::size_t tokenStart = pos;
        for (std::size_t i = 0; i < prefixLen; ++i)
            Advance();
        Advance(); // consume opening  "
        std::string value;
        while (!IsAtEnd() && Peek() != '"')
        {
            if (Peek() == '\n')
            {
                EmitError(start, "unterminated string literal");
                break;
            }
            if (Peek() == '\\')
            {
                value += ScanEscapeSequence();
            }
            else
            {
                value += Advance();
            }
        }

        if (IsAtEnd())
            EmitError(start, "unterminated string literal");
        else
            Advance(); // consume closing  "

        // token text preserves the original source spelling (including quotes)
        return Token{
            TokenKind::StringLiteral,
            source.substr(tokenStart, pos - tokenStart),
            start
        };
    }

    // ScanChar
    Token Lexer::ScanChar(SourceLocation start, std::size_t prefixLen)
    {
        const std::size_t tokenStart = pos;
        for (std::size_t i = 0; i < prefixLen; ++i)
            Advance();
        Advance(); // consume opening  '
        if (IsAtEnd() || Peek() == '\'')
        {
            EmitError(start, "empty character literal");
        }
        else if (Peek() == '\\')
        {
            ScanEscapeSequence();
        }
        else
        {
            Advance();
        }
        if (!Match('\''))
            EmitError(start, "unterminated character literal");
        return Token{
            TokenKind::CharLiteral,
            source.substr(tokenStart, pos - tokenStart),
            start
        };
    }

    // ScanEscapeSequence  (shared by string and char scanners)
    std::string Lexer::ScanEscapeSequence()
    {
        assert(Peek() == '\\');
        const SourceLocation loc = CurrentLocation();
        Advance(); // consume  \
        if (IsAtEnd())
        {
            EmitError(loc, "unexpected end of file in escape sequence");
            return {};
        }
        const char c = Advance();
        switch (c)
        {
        case 'n': return "\n";
        case 't': return "\t";
        case 'r': return "\r";
        case '0': return "\0";
        case '\\': return "\\";
        case '\'': return "'";
        case '"': return "\"";
        case 'u':
            {
                // TODO: Unicode escape  \u{XXXX}
                EmitWarning(loc, "Unicode escape sequences are not yet implemented");
                return {};
            }
        default:
            EmitError(loc, std::string("unknown escape sequence '\\") + c + "'");
            return {};
        }
    }

    // ScanSymbol  –  operators and punctuation
    Token Lexer::ScanSymbol(const SourceLocation start)
    {
        const std::size_t tokenStart = pos;
        switch (const char c = Advance())
        {
        // Single-character unambiguous tokens
        case '(': return MakeToken(TokenKind::LeftParen, start, tokenStart);
        case ')': return MakeToken(TokenKind::RightParen, start, tokenStart);
        case '{': return MakeToken(TokenKind::LeftBrace, start, tokenStart);
        case '}': return MakeToken(TokenKind::RightBrace, start, tokenStart);
        case '[': return MakeToken(TokenKind::LeftBracket, start, tokenStart);
        case ']': return MakeToken(TokenKind::RightBracket, start, tokenStart);
        case ',': return MakeToken(TokenKind::Comma, start, tokenStart);
        case ';': return MakeToken(TokenKind::Semicolon, start, tokenStart);
        case '@': return MakeToken(TokenKind::At, start, tokenStart);
        case '#': return MakeToken(TokenKind::Hash, start, tokenStart);
        case '?': return MakeToken(TokenKind::Question, start, tokenStart);
        case '~': return MakeToken(TokenKind::Tilde, start, tokenStart);
        // :  or  ::
        case ':':
            return MakeToken(Match(':')
                                 ? TokenKind::ColonColon
                                 : TokenKind::Colon,
                             start, tokenStart);
        // .  or  ..  or  ...
        case '.':
            if (Match('.'))
            {
                return MakeToken(Match('.')
                                     ? TokenKind::DotDotDot
                                     : TokenKind::DotDot,
                                 start, tokenStart);
            }
            return MakeToken(TokenKind::Dot, start, tokenStart);

        // +  or  +=
        case '+':
            return MakeToken(Match('=')
                                 ? TokenKind::PlusAssign
                                 : TokenKind::Plus,
                             start, tokenStart);

        // -  or  -=  or  ->
        case '-':
            if (Match('>')) return MakeToken(TokenKind::Arrow, start, tokenStart);
            if (Match('=')) return MakeToken(TokenKind::MinusAssign, start, tokenStart);
            return MakeToken(TokenKind::Minus, start, tokenStart);

        // *  or  *=  or  **
        case '*':
            if (Match('*')) return MakeToken(TokenKind::StarStar, start, tokenStart);
            if (Match('=')) return MakeToken(TokenKind::StarAssign, start, tokenStart);
            return MakeToken(TokenKind::Star, start, tokenStart);

        // /  or  /=   (comments already consumed in SkipWhitespace)
        case '/':
            return MakeToken(Match('=')
                                 ? TokenKind::SlashAssign
                                 : TokenKind::Slash,
                             start, tokenStart);

        // %  or  %=
        case '%':
            return MakeToken(Match('=')
                                 ? TokenKind::PercentAssign
                                 : TokenKind::Percent,
                             start, tokenStart);

        // &  or  &=  or  &&
        case '&':
            if (Match('&')) return MakeToken(TokenKind::AmpAmp, start, tokenStart);
            if (Match('=')) return MakeToken(TokenKind::AmpAssign, start, tokenStart);
            return MakeToken(TokenKind::Amp, start, tokenStart);

        // |  or  |=  or  ||
        case '|':
            if (Match('|')) return MakeToken(TokenKind::PipePipe, start, tokenStart);
            if (Match('=')) return MakeToken(TokenKind::PipeAssign, start, tokenStart);
            return MakeToken(TokenKind::Pipe, start, tokenStart);

        // ^  or  ^=
        case '^':
            return MakeToken(Match('=')
                                 ? TokenKind::CaretAssign
                                 : TokenKind::Caret,
                             start, tokenStart);

        // !  or  !=
        case '!':
            return MakeToken(Match('=')
                                 ? TokenKind::BangEqual
                                 : TokenKind::Bang,
                             start, tokenStart);

        // =  or  ==  or  =>
        case '=':
            if (Match('=')) return MakeToken(TokenKind::Equal, start, tokenStart);
            if (Match('>')) return MakeToken(TokenKind::FatArrow, start, tokenStart);
            return MakeToken(TokenKind::Assign, start, tokenStart);

        // <  or  <=  or  <<=  or  <<
        case '<':
            if (Match('<'))
            {
                return MakeToken(Match('=')
                                     ? TokenKind::LessLessAssign
                                     : TokenKind::LessLess,
                                 start, tokenStart);
            }
            return MakeToken(Match('=')
                                 ? TokenKind::LessEqual
                                 : TokenKind::Less,
                             start, tokenStart);

        // >  or  >=  or  >>=  or  >>
        case '>':
            if (Match('>'))
            {
                return MakeToken(Match('=')
                                     ? TokenKind::GreaterGreaterAssign
                                     : TokenKind::GreaterGreater,
                                 start, tokenStart);
            }
            return MakeToken(Match('=')
                                 ? TokenKind::GreaterEqual
                                 : TokenKind::Greater,
                             start, tokenStart);

        default:
            return ScanUnknown(start);
        }
    }

    Token Lexer::ScanUnknown(const SourceLocation start)
    {
        const std::size_t tokenStart = pos - 1; // already advanced past the char
        EmitError(start,
                  std::string("unexpected character '") + source[tokenStart] + "'");
        return Token{
            TokenKind::Unknown,
            source.substr(tokenStart, 1),
            start
        };
    }

    Token Lexer::MakeToken(const TokenKind kind,
                           const SourceLocation start,
                           const std::size_t tokenStart) const
    {
        return Token{
            kind,
            source.substr(tokenStart, pos - tokenStart),
            start
        };
    }

    void Lexer::EmitError(const SourceLocation loc, std::string message)
    {
        diagnostics.push_back(LexerDiagnostic{
            LexerDiagnostic::Severity::Error,
            loc,
            std::move(message)
        });
    }

    void Lexer::EmitWarning(const SourceLocation loc, std::string message)
    {
        diagnostics.push_back(LexerDiagnostic{
            LexerDiagnostic::Severity::Warning,
            loc,
            std::move(message)
        });
    }
}
