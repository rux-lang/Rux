#pragma once

#include "Diagnostics/Diagnostics.h"
#include "Lexer/Token.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Rux {
using LexerDiagnostic = Diagnostic;

/// The four comment spellings recognized by the lexer. Only the two exact documentation forms become parser tokens;
/// every form remains available as lossless trivia for formatting and tooling.
enum class CommentKind : std::uint8_t {
    Line,
    Block,
    DocumentationLine,
    DocumentationBlock,
};

[[nodiscard]] constexpr bool IsDocumentationComment(const CommentKind kind) noexcept {
    return kind == CommentKind::DocumentationLine || kind == CommentKind::DocumentationBlock;
}

/// One source comment with its exact spelling. The range excludes a line comment's line terminator and includes both
/// delimiters of a block comment. An unterminated block reaches EOF and is accompanied by the usual lexer diagnostic.
struct CommentTrivia {
    CommentKind kind = CommentKind::Line;
    std::string raw;
    SourceRange range;
    bool lineLeading = false;
    bool terminated = true;
};

/// The token stream and everything that went wrong producing it. A result with errors still carries a usable stream,
/// since scanning recovers rather than stopping.
struct LexerResult {
    std::vector<Token> tokens;
    std::vector<CommentTrivia> comments;
    std::vector<LexerDiagnostic> diagnostics;
    [[nodiscard]] bool HasErrors() const noexcept;
};

/**
 * @brief Turns UTF-8 source text into the token stream the parser reads.
 *
 * Scanning never stops at the first bad character: an unrecognized byte or an unterminated literal becomes a diagnostic
 * and a token, so the parser still receives a complete stream and one run can report many problems.
 *
 * Exact line and block documentation comments are tokens because Rux attaches them to the declaration that follows.
 * Every comment is also retained as lossless trivia for formatters and other source tools, and block comments nest.
 */
class Lexer {
public:
    /// Construct from in-memory source text. `sourceName` is used only for diagnostic messages (e.g. file path).
    explicit Lexer(std::string inputSource, std::string inputSourceName = "<input>");

    /// Tokenize borrowed text synchronously. Tokens and diagnostics own their strings; no input view escapes.
    [[nodiscard]] static LexerResult TokenizeSource(std::string_view source, std::string sourceName = "<input>");

    /// Convenience: read a file and lex it. Open and read failures are returned as diagnostics; reusable compiler code
    /// never prints them directly.
    [[nodiscard]] static LexerResult FromFile(const std::filesystem::path &path);

    /// Run the full lexer pass and return all tokens + diagnostics.
    [[nodiscard]] LexerResult Tokenize();

    /// Dump a token list to a file for debugging. Path defaults to sourceName + ".tokens" if not specified.
    static bool DumpTokens(const LexerResult &result, const std::filesystem::path &path = {});

    /// The single code point a character literal denotes, with any escape already resolved.
    ///
    /// @return nullopt when the text is not exactly one code point
    [[nodiscard]] static std::optional<std::uint32_t> DecodeCharLiteralCodePoint(std::string_view text);

private:
    // Source buffer
    struct BorrowedSource {};

    Lexer(BorrowedSource, std::string_view source, std::string sourceName);

    std::shared_ptr<const std::string> ownedSource;
    std::string_view source;
    std::string sourceName;

    // Cursor state
    std::size_t pos = 0; ///< current byte position
    std::uint32_t line = 1;
    std::uint32_t col = 1;

    // Output accumulators
    std::vector<Token> tokens;
    std::vector<CommentTrivia> comments;
    std::vector<LexerDiagnostic> diagnostics;

    // Core scanning loop
    void ScanAll();
    Token NextToken();

    // Character helpers
    [[nodiscard]] bool IsAtEnd() const noexcept;
    [[nodiscard]] char Peek(std::size_t ahead = 0) const noexcept;
    char Advance() noexcept;
    void AdvanceUtf8CodePoint() noexcept;
    bool Match(char expected) noexcept;
    bool MatchStr(std::string_view s) noexcept;

    // Location tracking
    [[nodiscard]] SourceLocation CurrentLocation() const noexcept;

    // Whitespace / comments
    [[nodiscard]] bool SkipWhitespace();
    [[nodiscard]] bool IsLineLeading(std::size_t offset) const noexcept;
    [[nodiscard]] bool IsDocumentationLineStart() const noexcept;
    [[nodiscard]] bool IsDocumentationBlockStart() const noexcept;
    SourceRange ScanLineComment(CommentKind kind);
    SourceRange ScanBlockComment(CommentKind kind); // supports nesting
    Token ScanDocumentationLine(SourceLocation start);
    Token ScanDocumentationBlock(SourceLocation start);

    // Scanners for each token family
    Token ScanIdent(SourceLocation start);
    Token ScanNumber(SourceLocation start); // int and float
    Token ScanString(SourceLocation start,
                     std::size_t prefixLen = 0); // "…" / c8"…" / c16"…" / c32"…"
    Token ScanChar(SourceLocation start,
                   std::size_t prefixLen = 0); // '…' / c8'…' / c16'…' / c32'…'
    Token ScanSymbol(SourceLocation start);    // operators & punctuation
    Token ScanUnknown(SourceLocation start);   // fallback for bad chars

    // Literal helpers
    Token ScanIntLiteral(SourceLocation start, std::size_t tokenStart);
    Token ScanFloatSuffix(SourceLocation start, std::size_t tokenStart);
    void ConsumeNumberSuffix(SourceLocation start);
    std::string ScanEscapeSequence(); // inside string / char

    // Emit helpers
    [[nodiscard]] Token MakeToken(TokenKind kind, SourceLocation start, std::size_t tokenStart) const;
    void EmitError(SourceLocation loc, std::string message, std::optional<std::string> help = {},
                   std::optional<std::string> documentationUrl = {});
    void EmitWarning(SourceLocation loc, std::string message);
};
} // namespace Rux
