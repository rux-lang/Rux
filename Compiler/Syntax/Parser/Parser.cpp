// Parser core: entry points, token cursor, diagnostics, and error recovery.

#include "Syntax/Parser/Parser.h"

#include <cassert>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Rux {
// ParseResult
bool ParseResult::HasErrors() const noexcept {
    for (const auto &d : diagnostics) {
        if (d.severity == ParserDiagnostic::Severity::Error) {
            return true;
        }
    }
    return false;
}

// Constructor / FromLexResult
Parser::Parser(std::vector<Token> inputTokens, std::string inputSourceName)
    : tokens(std::move(inputTokens))
    , sourceName(std::move(inputSourceName)) {
}

std::optional<ParseResult> Parser::FromLexResult(const LexerResult &lex, const std::string &sourceName) {
    if (lex.HasErrors()) {
        return std::nullopt;
    }
    Parser p(lex.tokens, sourceName);
    return p.Parse();
}

// Parse  –  top-level entry point
ParseResult Parser::Parse() {
    Module mod;
    mod.name = sourceName;

    while (!IsAtEnd()) {
        if (auto decl = ParseDecl()) {
            mod.items.push_back(std::move(decl));
        }
        else {
            Recover();
        }
    }

    return ParseResult{std::move(mod), std::move(diagnostics)};
}

// Token helpers
const Token &Parser::Peek(const std::size_t ahead) const noexcept {
    const std::size_t idx = pos + ahead;
    if (idx < tokens.size()) {
        return tokens[idx];
    }
    return tokens.back(); // EOF sentinel
}

const Token &Parser::Advance() noexcept {
    if (!IsAtEnd()) {
        ++pos;
    }
    return tokens[pos - 1];
}

bool Parser::Check(const TokenKind kind) const noexcept {
    return Peek().kind == kind;
}

bool Parser::CheckAny(const std::initializer_list<TokenKind> kinds) const noexcept {
    for (const auto k : kinds) {
        if (Check(k)) {
            return true;
        }
    }
    return false;
}

bool Parser::Match(const TokenKind kind) noexcept {
    if (!Check(kind)) {
        return false;
    }
    Advance();
    return true;
}

const Token &Parser::Expect(const TokenKind kind, const std::string_view message) {
    if (Check(kind)) {
        return Advance();
    }
    EmitError(CurrentLocation(), std::string(message));
    return Peek(); // return without advancing
}

bool Parser::IsAtEnd() const noexcept {
    return Peek().kind == TokenKind::EndOfFile;
}

const Token &Parser::Previous() const noexcept {
    assert(pos > 0);
    return tokens[pos - 1];
}

SourceLocation Parser::CurrentLocation() const noexcept {
    return Peek().location;
}

bool Parser::IsGenericStructInitAhead() const noexcept {
    if (!Check(TokenKind::Less)) {
        return false;
    }

    int angleDepth = 0;
    for (std::size_t ahead = 0;; ++ahead) {
        const TokenKind kind = Peek(ahead).kind;
        if (kind == TokenKind::EndOfFile || kind == TokenKind::LeftBrace || kind == TokenKind::Semicolon) {
            return false;
        }

        if (kind == TokenKind::Less) {
            ++angleDepth;
            continue;
        }

        if (kind == TokenKind::Greater) {
            --angleDepth;
            if (angleDepth == 0) {
                return Peek(ahead + 1).kind == TokenKind::LeftBrace;
            }
            if (angleDepth < 0) {
                return false;
            }
        }
    }
}

bool Parser::NextBraceIsMatchArms() const noexcept {
    // Peek(0) is '{'. Compile-time match arms have a top-level '=>' (an arm's
    // `pattern => body`); an ordinary `when`/block body reaches a top-level ';'
    // or its closing '}' first.
    int depth = 0;
    for (std::size_t ahead = 1;; ++ahead) {
        switch (Peek(ahead).kind) {
        case TokenKind::EndOfFile:
            return false;
        case TokenKind::LeftParen:
        case TokenKind::LeftBracket:
        case TokenKind::LeftBrace:
            ++depth;
            break;
        case TokenKind::RightParen:
        case TokenKind::RightBracket:
            --depth;
            break;
        case TokenKind::RightBrace:
            if (depth == 0) {
                return false;
            }
            --depth;
            break;
        case TokenKind::FatArrow:
            if (depth == 0) {
                return true;
            }
            break;
        case TokenKind::Semicolon:
            if (depth == 0) {
                return false;
            }
            break;
        default:
            break;
        }
    }
}

bool Parser::IsGenericCallAhead() const noexcept {
    if (!Check(TokenKind::Less)) {
        return false;
    }

    int angleDepth = 0;
    for (std::size_t ahead = 0;; ++ahead) {
        const TokenKind kind = Peek(ahead).kind;
        if (kind == TokenKind::EndOfFile || kind == TokenKind::LeftBrace || kind == TokenKind::Semicolon) {
            return false;
        }

        if (kind == TokenKind::Less) {
            ++angleDepth;
            continue;
        }

        if (kind == TokenKind::Greater) {
            --angleDepth;
            if (angleDepth == 0) {
                return Peek(ahead + 1).kind == TokenKind::LeftParen;
            }
            if (angleDepth < 0) {
                return false;
            }
        }
    }
}

bool Parser::IsTypeArgListAhead() const noexcept {
    if (!Check(TokenKind::Less)) {
        return false;
    }

    int angleDepth = 0;
    for (std::size_t ahead = 0;; ++ahead) {
        switch (Peek(ahead).kind) {
        case TokenKind::Less:
            ++angleDepth;
            continue;
        case TokenKind::Greater:
            --angleDepth;
            if (angleDepth == 0) {
                return true;
            }
            if (angleDepth < 0) {
                return false;
            }
            continue;
        case TokenKind::Ident:
        case TokenKind::Star:
        case TokenKind::LeftParen:
        case TokenKind::RightParen:
        case TokenKind::LeftBracket:
        case TokenKind::RightBracket:
        case TokenKind::Comma:
        case TokenKind::ColonColon:
        case TokenKind::SelfKeyword:
        case TokenKind::MutKeyword:
            continue;
        default:
            return false;
        }
    }
}

// Diagnostics
void Parser::EmitError(const SourceLocation loc, std::string message) {
    diagnostics.push_back(ParserDiagnostic{ParserDiagnostic::Severity::Error, sourceName, loc, std::move(message)});
}

void Parser::EmitWarning(const SourceLocation loc, std::string message) {
    diagnostics.push_back(ParserDiagnostic{ParserDiagnostic::Severity::Warning, sourceName, loc, std::move(message)});
}

void Parser::Synchronize() {
    // Skip until we reach a token that likely starts a new declaration or
    // statement.
    while (!IsAtEnd()) {
        const TokenKind k = Peek().kind;

        // These tokens can safely begin a new item.
        if (k == TokenKind::Semicolon) {
            Advance();
            return;
        }
        if (k == TokenKind::RightBrace) {
            return;
        }

        if (k == TokenKind::FuncKeyword || k == TokenKind::StructKeyword || k == TokenKind::EnumKeyword ||
            k == TokenKind::UnionKeyword || k == TokenKind::InterfaceKeyword || k == TokenKind::ExtendKeyword ||
            k == TokenKind::ModuleKeyword || k == TokenKind::ImportKeyword || k == TokenKind::ConstKeyword ||
            k == TokenKind::TypeKeyword || k == TokenKind::ExternKeyword || k == TokenKind::PubKeyword ||
            k == TokenKind::LetKeyword || k == TokenKind::IfKeyword || k == TokenKind::WhenKeyword ||
            k == TokenKind::WhileKeyword || k == TokenKind::DoKeyword || k == TokenKind::LoopKeyword ||
            k == TokenKind::ForKeyword || k == TokenKind::ReturnKeyword || k == TokenKind::MatchKeyword) {
            return;
        }

        Advance();
    }
}

void Parser::Recover() {
    const std::size_t before = pos;
    Synchronize();
    if (pos == before && !IsAtEnd()) {
        Advance();
    }
}
} // namespace Rux
