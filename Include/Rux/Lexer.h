/*
    Rux Compiler
    Copyright © 2026 Ivan Muzyka
    Licensed under the MIT License
*/

#pragma once

#include "Rux/Token.h"

#include <filesystem>
#include <string>
#include <vector>
#include <optional>

namespace Rux {
    struct LexerDiagnostic {
        enum class Severity { Warning, Error };

        Severity severity = Severity::Error;
        SourceLocation location;
        std::string message;
    };

    struct LexerResult {
        std::vector<Token> tokens;
        std::vector<LexerDiagnostic> diagnostics;

        [[nodiscard]] bool HasErrors() const noexcept;
    };

    class Lexer {
    public:
        // Construct from in-memory source text.
        // `sourceName` is used only for diagnostic messages (e.g. file path).
        explicit Lexer(std::string source, std::string sourceName = "<input>");

        // Convenience: read file from disk and lex it.
        // Returns std::nullopt if the file cannot be read.
        [[nodiscard]] static std::optional<LexerResult>
        FromFile(const std::filesystem::path &path);

        // Run the full lexer pass and return all tokens + diagnostics.
        [[nodiscard]] LexerResult Tokenize();

        // Dump a token list to a file for debugging.
        // Path defaults to sourceName + ".tokens" if not specified.
        static bool DumpTokens(const LexerResult &result,
                               const std::filesystem::path &path = {});

    private:
        // Source buffer
        std::string source;
        std::string sourceName;

        // Cursor state
        std::size_t pos = 0; // current byte position
        std::uint32_t line = 1;
        std::uint32_t col = 1;

        // Output accumulators
        std::vector<Token> tokens;
        std::vector<LexerDiagnostic> diagnostics;

        // Core scanning loop
        void ScanAll();

        Token NextToken();

        // Character helpers
        [[nodiscard]] bool IsAtEnd() const noexcept;

        [[nodiscard]] char Peek(std::size_t ahead = 0) const noexcept;

        char Advance() noexcept;

        bool Match(char expected) noexcept;

        bool MatchStr(std::string_view s) noexcept;

        // Location tracking
        [[nodiscard]] SourceLocation CurrentLocation() const noexcept;

        // Whitespace / comments
        void SkipWhitespace();

        void SkipLineComment(); // // …
        void SkipBlockComment(); // /* … */  (supports nesting)

        // Scanners for each token family
        Token ScanIdent(SourceLocation start);

        Token ScanNumber(SourceLocation start); // int and float
        Token ScanString(SourceLocation start, std::size_t prefixLen = 0); // "…" / c8"…" / c16"…" / c32"…"
        Token ScanChar(SourceLocation start); // '…'
        Token ScanSymbol(SourceLocation start); // operators & punctuation
        Token ScanUnknown(SourceLocation start); // fallback for bad chars

        // Literal helpers
        Token ScanIntLiteral(SourceLocation start, std::size_t tokenStart);

        Token ScanFloatSuffix(SourceLocation start, std::size_t tokenStart);

        std::string ScanEscapeSequence(); // inside string / char

        // Emit helpers
        [[nodiscard]] Token MakeToken(TokenKind kind,
                                      SourceLocation start,
                                      std::size_t tokenStart) const;

        void EmitError(SourceLocation loc, std::string message);

        void EmitWarning(SourceLocation loc, std::string message);
    };
}
