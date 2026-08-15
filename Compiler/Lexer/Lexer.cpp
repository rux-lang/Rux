#include "Lexer/Lexer.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <format>
#include <fstream>
#include <ostream>
#include <print>
#include <sstream>

namespace Rux {
namespace {
struct DecodedUtf8 {
    std::uint32_t codePoint;
    std::size_t width;
};

constexpr std::string_view LiteralDocumentation = "https://rux-lang.dev/docs/";

std::optional<DecodedUtf8> DecodeUtf8(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }

    const auto continuation = [&](std::size_t index) -> std::optional<std::uint32_t> {
        if (index >= text.size()) {
            return std::nullopt;
        }
        const auto byte = static_cast<unsigned char>(text[index]);
        if ((byte & 0xC0) != 0x80) {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(byte & 0x3F);
    };

    const auto b0 = static_cast<unsigned char>(text[0]);
    std::uint32_t codePoint = 0;
    std::size_t width = 0;
    std::uint32_t minValue = 0;
    if (b0 <= 0x7F) {
        codePoint = b0;
        width = 1;
        minValue = 0;
    }
    else if ((b0 & 0xE0) == 0xC0) {
        codePoint = b0 & 0x1F;
        width = 2;
        minValue = 0x80;
    }
    else if ((b0 & 0xF0) == 0xE0) {
        codePoint = b0 & 0x0F;
        width = 3;
        minValue = 0x800;
    }
    else if ((b0 & 0xF8) == 0xF0) {
        codePoint = b0 & 0x07;
        width = 4;
        minValue = 0x10000;
    }
    else {
        return std::nullopt;
    }

    if (text.size() < width) {
        return std::nullopt;
    }
    for (std::size_t i = 1; i < width; ++i) {
        const auto byte = continuation(i);
        if (!byte) {
            return std::nullopt;
        }
        codePoint = (codePoint << 6) | *byte;
    }
    if (codePoint < minValue || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF)) {
        return std::nullopt;
    }
    return DecodedUtf8{codePoint, width};
}

std::optional<std::uint32_t> DecodeUtf8CodePoint(const std::string_view text) {
    const auto decoded = DecodeUtf8(text);
    if (!decoded || decoded->width != text.size()) {
        return std::nullopt;
    }
    return decoded->codePoint;
}

bool IsDigitForBase(const char c, const int base) {
    if (c >= '0' && c <= '9') {
        return c - '0' < base;
    }
    return base == 16 && ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'));
}

std::string_view BaseName(const int base) {
    switch (base) {
    case 2:
        return "binary";
    case 8:
        return "octal";
    case 16:
        return "hexadecimal";
    default:
        return "decimal";
    }
}
} // namespace

bool LexerResult::HasErrors() const noexcept {
    for (const auto &d : diagnostics) {
        if (d.severity == LexerDiagnostic::Severity::Error) {
            return true;
        }
    }
    return false;
}

Lexer::Lexer(std::string inputSource, std::string inputSourceName)
    : source(std::move(inputSource))
    , sourceName(std::move(inputSourceName)) {
}

LexerResult Lexer::FromFile(const std::filesystem::path &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return {{},
                {ErrorDiagnostic(std::format("cannot open source file '{}'", path.string()), {},
                                 "check that the path exists and the file is readable")}};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    if (!f && !f.eof()) {
        return {{},
                {ErrorDiagnostic(std::format("failed to read source file '{}'", path.string()), {},
                                 "check the file and retry")}};
    }
    Lexer lex(ss.str(), path.string());
    return lex.Tokenize();
}

LexerResult Lexer::Tokenize() {
    tokens.clear();
    diagnostics.clear();
    pos = 0;
    line = 1;
    col = 1;
    // Reject malformed encoding before tokenization. This prevents one bad
    // byte from becoming an unexpected-character error followed by parser
    // recovery noise.
    std::uint32_t utf8Line = 1;
    std::uint32_t utf8Column = 1;
    for (std::size_t offset = 0; offset < source.size();) {
        const auto byte = static_cast<unsigned char>(source[offset]);
        if (byte < 0x80) {
            if (byte == '\n') {
                ++utf8Line;
                utf8Column = 1;
            }
            else {
                ++utf8Column;
            }
            ++offset;
            continue;
        }
        const auto decoded = DecodeUtf8(std::string_view(source).substr(offset));
        if (!decoded) {
            EmitError({utf8Line, utf8Column, static_cast<std::uint32_t>(offset)},
                      std::format("source contains invalid UTF-8 byte 0x{:02X}", byte),
                      "save the source file as valid UTF-8");
            tokens.push_back(Token{TokenKind::EndOfFile, {}, CurrentLocation()});
            return LexerResult{std::move(tokens), std::move(diagnostics)};
        }
        offset += decoded->width;
        utf8Column += static_cast<std::uint32_t>(decoded->width);
    }
    ScanAll();
    // Always append a synthetic EOF token
    tokens.push_back(Token{TokenKind::EndOfFile, {}, CurrentLocation()});
    return LexerResult{std::move(tokens), std::move(diagnostics)};
}

bool Lexer::DumpTokens(const LexerResult &result, const std::filesystem::path &path) {
    std::ofstream f(path);
    if (!f) {
        return false;
    }
    for (const auto &tok : result.tokens) {
        std::print(f, "{:>4}:{:<4}  {:<16}  {}\n", tok.location.line, tok.location.column,
                   std::string(TokenKindName(tok.kind)), tok.text);
    }
    if (!result.diagnostics.empty()) {
        std::print(f, "\n--- diagnostics ---\n");
        for (const auto &d : result.diagnostics) {
            std::print(f, "{:>4}:{:<4}  {}  {}\n", d.location.line, d.location.column,
                       d.severity == LexerDiagnostic::Severity::Error ? "error  " : "warning", d.message);
        }
    }
    return f.good();
}

std::optional<std::uint32_t> Lexer::DecodeCharLiteralCodePoint(std::string_view text) {
    const std::size_t quote = text.find('\'');
    if (quote == std::string::npos || text.size() < quote + 3) {
        return std::nullopt;
    }

    std::string_view body(text);
    body.remove_prefix(quote + 1);
    body.remove_suffix(1);
    if (body.empty()) {
        return std::nullopt;
    }

    if (body[0] == '\\') {
        if (body == "\\n") {
            return static_cast<std::uint32_t>('\n');
        }
        if (body == "\\t") {
            return static_cast<std::uint32_t>('\t');
        }
        if (body == "\\r") {
            return static_cast<std::uint32_t>('\r');
        }
        if (body == "\\a") {
            return static_cast<std::uint32_t>('\a');
        }
        if (body == "\\b") {
            return static_cast<std::uint32_t>('\b');
        }
        if (body == "\\f") {
            return static_cast<std::uint32_t>('\f');
        }
        if (body == "\\v") {
            return static_cast<std::uint32_t>('\v');
        }
        if (body == "\\0") {
            return static_cast<std::uint32_t>('\0');
        }
        if (body == "\\\\") {
            return static_cast<std::uint32_t>('\\');
        }
        if (body == "\\'") {
            return static_cast<std::uint32_t>('\'');
        }
        if (body == "\\\"") {
            return static_cast<std::uint32_t>('\"');
        }
        if (body.starts_with("\\u{") && body.ends_with("}")) {
            const std::string_view digits = body.substr(3, body.size() - 4);
            if (digits.empty() || digits.size() > 8) {
                return std::nullopt;
            }
            std::uint32_t codePoint = 0;
            for (const char digit : digits) {
                codePoint <<= 4;
                if (digit >= '0' && digit <= '9') {
                    codePoint |= static_cast<std::uint32_t>(digit - '0');
                }
                else if (digit >= 'a' && digit <= 'f') {
                    codePoint |= static_cast<std::uint32_t>(digit - 'a' + 10);
                }
                else if (digit >= 'A' && digit <= 'F') {
                    codePoint |= static_cast<std::uint32_t>(digit - 'A' + 10);
                }
                else {
                    return std::nullopt;
                }
            }
            if (codePoint > 0x10FFFF) {
                return std::nullopt;
            }
            return codePoint;
        }
        return std::nullopt;
    }

    return DecodeUtf8CodePoint(body);
}

void Lexer::ScanAll() {
    while (!IsAtEnd()) {
        SkipWhitespace();
        if (IsAtEnd()) {
            break;
        }

        if (Token tok = NextToken(); tok.kind != TokenKind::Unknown || !tok.text.empty()) {
            tokens.push_back(std::move(tok));
        }
    }
}

// NextToken – dispatch on the current character
Token Lexer::NextToken() {
    const SourceLocation start = CurrentLocation();
    const char c = Peek();
    if (c == '/' && Peek(1) == '/' && Peek(2) == '/') {
        return ScanDocComment(start);
    }
    // Prefixed string literals
    if (c == 'c') {
        if (Peek(1) == '8' && Peek(2) == '"') {
            return ScanString(start, 2);
        }
        if (Peek(1) == '8' && Peek(2) == '\'') {
            return ScanChar(start, 2);
        }
        if (Peek(1) == '1' && Peek(2) == '6' && Peek(3) == '"') {
            return ScanString(start, 3);
        }
        if (Peek(1) == '1' && Peek(2) == '6' && Peek(3) == '\'') {
            return ScanChar(start, 3);
        }
        if (Peek(1) == '3' && Peek(2) == '2' && Peek(3) == '"') {
            return ScanString(start, 3);
        }
        if (Peek(1) == '3' && Peek(2) == '2' && Peek(3) == '\'') {
            return ScanChar(start, 3);
        }
    }
    // Identifiers / keywords
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
        return ScanIdent(start);
    }
    // Numeric literals
    if (std::isdigit(static_cast<unsigned char>(c))) {
        return ScanNumber(start);
    }
    // String literals
    if (c == '"') {
        return ScanString(start);
    }
    // Char literals
    if (c == '\'') {
        return ScanChar(start);
    }
    // Operators, punctuation, and everything else
    return ScanSymbol(start);
}

// Character helpers
bool Lexer::IsAtEnd() const noexcept {
    return pos >= source.size();
}

char Lexer::Peek(std::size_t ahead) const noexcept {
    const std::size_t idx = pos + ahead;
    return (idx < source.size()) ? source[idx] : '\0';
}

char Lexer::Advance() noexcept {
    const char c = source[pos++];
    if (c == '\n') {
        ++line;
        col = 1;
    }
    else {
        ++col;
    }
    return c;
}

void Lexer::AdvanceUtf8CodePoint() noexcept {
    if (IsAtEnd()) {
        return;
    }

    const auto lead = static_cast<unsigned char>(Peek());
    std::size_t byteCount = 1;
    if ((lead & 0x80u) == 0) {
        byteCount = 1;
    }
    else if ((lead & 0xE0u) == 0xC0u) {
        byteCount = 2;
    }
    else if ((lead & 0xF0u) == 0xE0u) {
        byteCount = 3;
    }
    else if ((lead & 0xF8u) == 0xF0u) {
        byteCount = 4;
    }

    for (std::size_t i = 0; i < byteCount && !IsAtEnd(); ++i) {
        Advance();
    }
}

bool Lexer::Match(char expected) noexcept {
    if (IsAtEnd() || source[pos] != expected) {
        return false;
    }
    Advance();
    return true;
}

bool Lexer::MatchStr(std::string_view s) noexcept {
    if (pos + s.size() > source.size()) {
        return false;
    }
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (source[pos + i] != s[i]) {
            return false;
        }
    }
    for (std::size_t i = 0; i < s.size(); ++i) {
        Advance();
    }
    return true;
}

SourceLocation Lexer::CurrentLocation() const noexcept {
    return {line, col, static_cast<std::uint32_t>(pos)};
}

// Whitespace / comments
void Lexer::SkipWhitespace() {
    while (!IsAtEnd()) {
        const char c = Peek();
        // Inline whitespace
        if (c == ' ' || c == '\t' || c == '\r') {
            Advance();
            continue;
        }
        // Newlines – skip (emit Newline token here if your grammar needs
        // them)
        if (c == '\n') {
            Advance();
            continue;
        }
        // Line comment
        if (c == '/' && Peek(1) == '/' && Peek(2) != '/') {
            SkipLineComment();
            continue;
        }
        // Block comment
        if (c == '/' && Peek(1) == '*') {
            SkipBlockComment();
            continue;
        }
        break;
    }
}

Token Lexer::ScanDocComment(const SourceLocation start) {
    Advance();
    Advance();
    Advance();
    if (Peek() == ' ')
        Advance();
    const std::size_t contentStart = pos;
    while (!IsAtEnd() && Peek() != '\n')
        Advance();
    return Token{
        .kind = TokenKind::DocComment, .text = source.substr(contentStart, pos - contentStart), .location = start};
}

void Lexer::SkipLineComment() {
    while (!IsAtEnd() && Peek() != '\n') {
        Advance();
    }
    if (!IsAtEnd()) {
        Advance(); // consume the newline so the line counter advances
    }
}

void Lexer::SkipBlockComment() {
    const SourceLocation start = CurrentLocation();
    // Consume opening  /*
    Advance();
    Advance();
    int depth = 1; // supports nested  /* /* */ */
    while (!IsAtEnd() && depth > 0) {
        if (Peek() == '/' && Peek(1) == '*') {
            Advance();
            Advance();
            ++depth;
        }
        else if (Peek() == '*' && Peek(1) == '/') {
            Advance();
            Advance();
            --depth;
        }
        else {
            Advance();
        }
    }
    if (depth > 0) {
        EmitError(start, "block comment is not terminated", "close the comment with '*/'");
    }
}

Token Lexer::ScanIdent(SourceLocation start) {
    const std::size_t tokenStart = pos;
    while (!IsAtEnd()) {
        const char c = Peek();
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
            break;
        }
        Advance();
    }
    std::string text = source.substr(tokenStart, pos - tokenStart);
    const TokenKind kind = KeywordKind(text);
    return Token{kind, std::move(text), start};
}

Token Lexer::ScanNumber(SourceLocation start) {
    const std::size_t tokenStart = pos;
    // Detect base prefix: 0x  0b  0o
    if (Peek() == '0') {
        const char next = Peek(1);
        if (next == 'x' || next == 'X') {
            Advance();
            Advance(); // consume  0x
            return ScanIntLiteral(start, tokenStart);
        }
        if (next == 'b' || next == 'B') {
            Advance();
            Advance(); // consume  0b
            return ScanIntLiteral(start, tokenStart);
        }
        if (next == 'o' || next == 'O') {
            Advance();
            Advance(); // consume  0o
            return ScanIntLiteral(start, tokenStart);
        }
    }
    bool invalidSeparator = false;
    bool previousWasDigit = false;
    while (!IsAtEnd() && (std::isdigit(static_cast<unsigned char>(Peek())) || Peek() == '_')) {
        if (Peek() == '_') {
            invalidSeparator |= !previousWasDigit || !std::isdigit(static_cast<unsigned char>(Peek(1)));
            previousWasDigit = false;
        }
        else {
            previousWasDigit = true;
        }
        Advance();
    }
    if (invalidSeparator) {
        EmitError(start, "numeric separator '_' must appear between digits", "write digit separators like '1_000'",
                  std::string(LiteralDocumentation));
    }
    // Check for floating-point  .  or  e/E
    const bool hasDot = Peek() == '.' && std::isdigit(static_cast<unsigned char>(Peek(1)));
    const bool hasExp = (Peek() == 'e' || Peek() == 'E');
    if (hasDot || hasExp) {
        return ScanFloatSuffix(start, tokenStart);
    }
    ConsumeNumberSuffix(start);
    return MakeToken(TokenKind::IntLiteral, start, tokenStart);
}

Token Lexer::ScanIntLiteral(SourceLocation start, std::size_t tokenStart) {
    // Detect base from prefix (set in ScanNumber before calling this).
    // 0x/0X -> 16, 0b/0B -> 2, 0o/0O -> 8, else 10.
    const std::string_view text(source.data() + tokenStart, pos - tokenStart);
    int base = 10;
    if (text.size() >= 2 && text[0] == '0') {
        const char prefix = text[1];
        if (prefix == 'x' || prefix == 'X') {
            base = 16;
        }
        else if (prefix == 'b' || prefix == 'B') {
            base = 2;
        }
        else if (prefix == 'o' || prefix == 'O') {
            base = 8;
        }
    }

    // Consume the valid base digits, stopping at the
    // first character that is not a digit for this base so a type suffix
    // (e.g. 0xFFu) can follow, exactly like the decimal literal path in
    // ScanNumber.
    std::size_t digitCount = 0;
    bool invalidSeparator = false;
    bool previousWasDigit = false;
    while (!IsAtEnd()) {
        const char c = Peek();
        if (!IsDigitForBase(c, base) && c != '_') {
            break;
        }
        if (c == '_') {
            invalidSeparator |= !previousWasDigit || !IsDigitForBase(Peek(1), base);
            previousWasDigit = false;
        }
        else {
            ++digitCount;
            previousWasDigit = true;
        }
        Advance();
    }
    if (digitCount == 0) {
        while (!IsAtEnd() && (std::isalnum(static_cast<unsigned char>(Peek())) || Peek() == '_')) {
            Advance();
        }
        const std::string_view prefix(source.data() + tokenStart, 2);
        EmitError(start, std::format("{} literal requires at least one digit after '{}'", BaseName(base), prefix),
                  base == 16  ? "write a hexadecimal literal like '0x2A'"
                  : base == 8 ? "write an octal literal like '0o52'"
                              : "write a binary literal like '0b101010'",
                  std::string(LiteralDocumentation));
        return MakeToken(TokenKind::IntLiteral, start, tokenStart);
    }
    if (invalidSeparator) {
        EmitError(start, "numeric separator '_' must appear between digits", "write separators like '0xFF_FF'",
                  std::string(LiteralDocumentation));
    }
    if (std::isdigit(static_cast<unsigned char>(Peek()))) {
        const char invalidDigit = Peek();
        while (!IsAtEnd() && (std::isalnum(static_cast<unsigned char>(Peek())) || Peek() == '_')) {
            Advance();
        }
        EmitError(
            start,
            std::format("digit '{}' is not valid in {} {} literal", invalidDigit, base == 8 ? "an" : "a",
                        BaseName(base)),
            std::format("use only {} digits after the base prefix", base == 2 ? "'0' and '1'" : "'0' through '7'"),
            std::string(LiteralDocumentation));
        return MakeToken(TokenKind::IntLiteral, start, tokenStart);
    }
    ConsumeNumberSuffix(start);
    return MakeToken(TokenKind::IntLiteral, start, tokenStart);
}

Token Lexer::ScanFloatSuffix(SourceLocation start, std::size_t tokenStart) {
    // Fractional part
    if (Peek() == '.') {
        Advance(); // consume  .
        bool previousWasDigit = false;
        bool invalidSeparator = false;
        while (!IsAtEnd() && (std::isdigit(static_cast<unsigned char>(Peek())) || Peek() == '_')) {
            if (Peek() == '_') {
                invalidSeparator |= !previousWasDigit || !std::isdigit(static_cast<unsigned char>(Peek(1)));
                previousWasDigit = false;
            }
            else {
                previousWasDigit = true;
            }
            Advance();
        }
        if (invalidSeparator) {
            EmitError(start, "numeric separator '_' must appear between digits", "write a fraction like '1.25_00'",
                      std::string(LiteralDocumentation));
        }
    }
    // Exponent part  e[+-]digits
    if (Peek() == 'e' || Peek() == 'E') {
        const SourceLocation exponent = CurrentLocation();
        const char marker = Advance();
        if (Peek() == '+' || Peek() == '-') {
            Advance();
        }
        if (!std::isdigit(static_cast<unsigned char>(Peek()))) {
            EmitError(exponent, std::format("exponent requires at least one digit after '{}'", marker),
                      "write an exponent like '1.5e+2'", std::string(LiteralDocumentation));
        }
        else {
            bool invalidSeparator = false;
            bool previousWasDigit = false;
            while (!IsAtEnd() && (std::isdigit(static_cast<unsigned char>(Peek())) || Peek() == '_')) {
                if (Peek() == '_') {
                    invalidSeparator |= !previousWasDigit || !std::isdigit(static_cast<unsigned char>(Peek(1)));
                    previousWasDigit = false;
                }
                else {
                    previousWasDigit = true;
                }
                Advance();
            }
            if (invalidSeparator) {
                EmitError(exponent, "numeric separator '_' must appear between exponent digits",
                          "write an exponent like '1e1_000'", std::string(LiteralDocumentation));
            }
        }
    }
    ConsumeNumberSuffix(start);
    return MakeToken(TokenKind::FloatLiteral, start, tokenStart);
}

void Lexer::ConsumeNumberSuffix(const SourceLocation start) {
    if (!std::isalpha(static_cast<unsigned char>(Peek()))) {
        return;
    }
    const std::size_t suffixStart = pos;
    while (!IsAtEnd()) {
        if (const char c = Peek(); !std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
            break;
        }
        Advance();
    }

    const std::string_view suffix(source.data() + suffixStart, pos - suffixStart);
    static constexpr std::string_view validSuffixes[] = {
        "i", "i8", "i16", "i32", "i64", "u", "u8", "u16", "u32", "u64", "f32", "f64",
    };
    if (std::find(std::begin(validSuffixes), std::end(validSuffixes), suffix) == std::end(validSuffixes)) {
        EmitError(start, std::format("numeric literal suffix '{}' is not recognized", suffix),
                  "use 'i', 'i8', 'i16', 'i32', 'i64', 'u', 'u8', 'u16', 'u32', 'u64', 'f32', or 'f64'",
                  std::string(LiteralDocumentation));
    }
}

Token Lexer::ScanString(SourceLocation start, std::size_t prefixLen) {
    const std::size_t tokenStart = pos;
    for (std::size_t i = 0; i < prefixLen; ++i) {
        Advance();
    }
    Advance(); // consume opening  "
    std::string value;
    while (!IsAtEnd() && Peek() != '"') {
        if (Peek() == '\n') {
            EmitError(start, "string literal is not terminated before the end of the line",
                      "add a closing '\"' before the line ends", std::string(LiteralDocumentation));
            return Token{TokenKind::StringLiteral, source.substr(tokenStart, pos - tokenStart), start};
        }
        if (Peek() == '\\') {
            const bool incomplete = Peek(1) == '\0' || Peek(1) == '\n';
            const std::size_t diagnosticCount = diagnostics.size();
            value += ScanEscapeSequence();
            if (incomplete || (diagnostics.size() != diagnosticCount && (IsAtEnd() || Peek() == '\n'))) {
                return Token{TokenKind::StringLiteral, source.substr(tokenStart, pos - tokenStart), start};
            }
        }
        else if (static_cast<unsigned char>(Peek()) < 0x20) {
            const auto control = static_cast<unsigned char>(Advance());
            EmitError(start, std::format("string literal contains unescaped control byte 0x{:02X}", control),
                      "use a supported escape sequence instead", std::string(LiteralDocumentation));
        }
        else {
            value += Advance();
        }
    }

    if (IsAtEnd()) {
        EmitError(start, "string literal is not terminated before the end of the file", "add a closing '\"'",
                  std::string(LiteralDocumentation));
    }
    else {
        Advance(); // consume closing  "
    }

    // token text preserves the original source spelling (including quotes)
    return Token{TokenKind::StringLiteral, source.substr(tokenStart, pos - tokenStart), start};
}

// ScanChar
Token Lexer::ScanChar(SourceLocation start, std::size_t prefixLen) {
    const std::size_t tokenStart = pos;
    for (std::size_t i = 0; i < prefixLen; ++i) {
        Advance();
    }
    Advance(); // consume opening  '
    if (IsAtEnd()) {
        EmitError(start, "character literal is not terminated before the end of the file", "add a character and '\''",
                  std::string(LiteralDocumentation));
        return Token{TokenKind::CharLiteral, source.substr(tokenStart, pos - tokenStart), start};
    }
    if (Peek() == '\'') {
        Advance();
        EmitError(start, "character literal is empty", "put exactly one character between the quotes",
                  std::string(LiteralDocumentation));
        return Token{TokenKind::CharLiteral, source.substr(tokenStart, pos - tokenStart), start};
    }
    if (Peek() == '\n') {
        EmitError(start, "character literal is not terminated before the end of the line", "add a closing '\''",
                  std::string(LiteralDocumentation));
        return Token{TokenKind::CharLiteral, source.substr(tokenStart, pos - tokenStart), start};
    }
    if (Peek() == '\\') {
        const std::size_t diagnosticCount = diagnostics.size();
        ScanEscapeSequence();
        if (diagnostics.size() != diagnosticCount) {
            while (!IsAtEnd() && Peek() != '\n' && Peek() != '\'') {
                AdvanceUtf8CodePoint();
            }
            Match('\'');
            return Token{TokenKind::CharLiteral, source.substr(tokenStart, pos - tokenStart), start};
        }
    }
    else {
        AdvanceUtf8CodePoint();
    }
    if (Match('\'')) {
        return Token{TokenKind::CharLiteral, source.substr(tokenStart, pos - tokenStart), start};
    }

    while (!IsAtEnd() && Peek() != '\n' && Peek() != '\'') {
        AdvanceUtf8CodePoint();
    }
    if (Match('\'')) {
        EmitError(start, "character literal contains more than one character",
                  "use a string literal for text or keep exactly one character between the quotes",
                  std::string(LiteralDocumentation));
    }
    else {
        EmitError(start,
                  IsAtEnd() ? "character literal is not terminated before the end of the file"
                            : "character literal is not terminated before the end of the line",
                  "add a closing '\''", std::string(LiteralDocumentation));
    }
    return Token{TokenKind::CharLiteral, source.substr(tokenStart, pos - tokenStart), start};
}

// ScanEscapeSequence (shared by string and char scanners)
std::string Lexer::ScanEscapeSequence() {
    assert(Peek() == '\\');
    const SourceLocation loc = CurrentLocation();
    Advance(); // consume backslash
    if (IsAtEnd()) {
        EmitError(loc, "escape sequence is not complete before the end of the file",
                  "write a supported escape such as '\\n' or '\\u{1F600}'", std::string(LiteralDocumentation));
        return {};
    }
    if (Peek() == '\n') {
        EmitError(loc, "escape sequence is not complete before the end of the line",
                  "write a supported escape such as '\\n'", std::string(LiteralDocumentation));
        return {};
    }
    const char c = Advance();
    switch (c) {
    case 'n':
        return "\n";
    case 't':
        return "\t";
    case 'r':
        return "\r";
    case 'a':
        return "\a";
    case 'b':
        return "\b";
    case 'f':
        return "\f";
    case 'v':
        return "\v";
    case '0':
        return "\0";
    case '\\':
        return "\\";
    case '\'':
        return "'";
    case '"':
        return "\"";
    case 'u': {
        // Unicode escape  \u{XXXX}  or  \u{XXXXXXXX}
        if (!Match('{')) {
            EmitError(loc, "Unicode escape requires '{' after '\\u'", "write the escape like '\\u{1F600}'",
                      std::string(LiteralDocumentation));
            return {};
        }
        std::uint32_t codepoint = 0;
        int digits = 0;
        bool invalidDigit = false;
        bool tooManyDigits = false;
        while (!IsAtEnd() && Peek() != '}') {
            if (Peek() == '\n') {
                EmitError(loc, "Unicode escape is not terminated before the end of the line",
                          "close the escape with '}'", std::string(LiteralDocumentation));
                return {};
            }
            const char h = Advance();
            int val = -1;
            if (h >= '0' && h <= '9') {
                val = h - '0';
            }
            else if (h >= 'a' && h <= 'f') {
                val = h - 'a' + 10;
            }
            else if (h >= 'A' && h <= 'F') {
                val = h - 'A' + 10;
            }
            else {
                invalidDigit = true;
                continue;
            }
            if (digits >= 8) {
                tooManyDigits = true;
                continue;
            }
            codepoint = (codepoint << 4) | val;
            ++digits;
        }
        if (!Match('}')) {
            EmitError(loc, "Unicode escape is not terminated before the end of the file", "close the escape with '}'",
                      std::string(LiteralDocumentation));
            return {};
        }
        if (invalidDigit) {
            EmitError(loc, "Unicode escape contains a non-hexadecimal digit",
                      "use only digits '0'-'9', 'a'-'f', or 'A'-'F'", std::string(LiteralDocumentation));
            return {};
        }
        if (digits == 0) {
            EmitError(loc, "Unicode escape requires at least one hexadecimal digit", "write the escape like '\\u{41}'",
                      std::string(LiteralDocumentation));
            return {};
        }
        if (tooManyDigits) {
            EmitError(loc, "Unicode escape contains more than eight hexadecimal digits",
                      "use a Unicode scalar value from '\\u{0}' through '\\u{10FFFF}'",
                      std::string(LiteralDocumentation));
            return {};
        }
        if (codepoint > 0x10FFFF) {
            EmitError(loc, std::format("Unicode escape U+{:X} is above the maximum scalar value U+10FFFF", codepoint),
                      "use a value from '\\u{0}' through '\\u{10FFFF}'", std::string(LiteralDocumentation));
            return {};
        }
        if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
            EmitError(loc, std::format("Unicode escape U+{:04X} is a surrogate, not a scalar value", codepoint),
                      "use the scalar value for the character instead of a UTF-16 surrogate",
                      std::string(LiteralDocumentation));
            return {};
        }
        // Encode as UTF-8
        std::string result;
        if (codepoint <= 0x7F) {
            result += static_cast<char>(codepoint);
        }
        else if (codepoint <= 0x7FF) {
            result += static_cast<char>(0xC0 | (codepoint >> 6));
            result += static_cast<char>(0x80 | (codepoint & 0x3F));
        }
        else if (codepoint <= 0xFFFF) {
            result += static_cast<char>(0xE0 | (codepoint >> 12));
            result += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (codepoint & 0x3F));
        }
        else {
            result += static_cast<char>(0xF0 | (codepoint >> 18));
            result += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
            result += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (codepoint & 0x3F));
        }
        return result;
    }
    default:
        EmitError(loc, std::format("escape sequence '\\{}' is not recognized", c),
                  R"(use '\n', '\t', '\r', '\a', '\b', '\f', '\v', '\0', '\\', '\'', '\"', or '\u{...}')",
                  std::string(LiteralDocumentation));
        return {};
    }
}

// ScanSymbol  –  operators and punctuation
Token Lexer::ScanSymbol(const SourceLocation start) {
    const std::size_t tokenStart = pos;
    switch (Advance()) {
    // Single-character unambiguous tokens
    case '(':
        return MakeToken(TokenKind::LeftParen, start, tokenStart);
    case ')':
        return MakeToken(TokenKind::RightParen, start, tokenStart);
    case '{':
        return MakeToken(TokenKind::LeftBrace, start, tokenStart);
    case '}':
        return MakeToken(TokenKind::RightBrace, start, tokenStart);
    case '[':
        return MakeToken(TokenKind::LeftBracket, start, tokenStart);
    case ']':
        return MakeToken(TokenKind::RightBracket, start, tokenStart);
    case ',':
        return MakeToken(TokenKind::Comma, start, tokenStart);
    case ';':
        return MakeToken(TokenKind::Semicolon, start, tokenStart);
    case '@':
        return MakeToken(TokenKind::At, start, tokenStart);
    case '#':
        return MakeToken(TokenKind::Hash, start, tokenStart);
    case '?':
        return MakeToken(TokenKind::Question, start, tokenStart);
    case '~':
        return MakeToken(TokenKind::Tilde, start, tokenStart);
    // :  or  ::
    case ':':
        return MakeToken(Match(':') ? TokenKind::ColonColon : TokenKind::Colon, start, tokenStart);
    // .  or  ..  or  ...  or  ..=
    case '.':
        if (Match('.')) {
            if (Match('.')) {
                return MakeToken(TokenKind::DotDotDot, start, tokenStart);
            }
            if (Match('=')) {
                return MakeToken(TokenKind::DotDotEqual, start, tokenStart);
            }
            return MakeToken(TokenKind::DotDot, start, tokenStart);
        }
        return MakeToken(TokenKind::Dot, start, tokenStart);

    // +  or  ++  or  +=
    case '+':
        if (Match('+')) {
            return MakeToken(TokenKind::PlusPlus, start, tokenStart);
        }
        return MakeToken(Match('=') ? TokenKind::PlusAssign : TokenKind::Plus, start, tokenStart);

    // -  or  --  or  -=  or  ->
    case '-':
        if (Match('-')) {
            return MakeToken(TokenKind::MinusMinus, start, tokenStart);
        }
        if (Match('>')) {
            return MakeToken(TokenKind::Arrow, start, tokenStart);
        }
        if (Match('=')) {
            return MakeToken(TokenKind::MinusAssign, start, tokenStart);
        }
        return MakeToken(TokenKind::Minus, start, tokenStart);

    // *  or  *=  or  **
    case '*':
        if (Match('=')) {
            return MakeToken(TokenKind::StarAssign, start, tokenStart);
        }
        return MakeToken(TokenKind::Star, start, tokenStart);

    // /  or  /=   (comments already consumed in SkipWhitespace)
    case '/':
        return MakeToken(Match('=') ? TokenKind::SlashAssign : TokenKind::Slash, start, tokenStart);

    // %  or  %=
    case '%':
        return MakeToken(Match('=') ? TokenKind::PercentAssign : TokenKind::Percent, start, tokenStart);

    // &  or  &=  or  &&
    case '&':
        if (Match('&')) {
            return MakeToken(TokenKind::AmpAmp, start, tokenStart);
        }
        if (Match('=')) {
            return MakeToken(TokenKind::AmpAssign, start, tokenStart);
        }
        return MakeToken(TokenKind::Amp, start, tokenStart);

    // |  or  |=  or  ||
    case '|':
        if (Match('|')) {
            return MakeToken(TokenKind::PipePipe, start, tokenStart);
        }
        if (Match('=')) {
            return MakeToken(TokenKind::PipeAssign, start, tokenStart);
        }
        return MakeToken(TokenKind::Pipe, start, tokenStart);

    // ^  or  ^=
    case '^':
        return MakeToken(Match('=') ? TokenKind::CaretAssign : TokenKind::Caret, start, tokenStart);

    // !  or  !=
    case '!':
        return MakeToken(Match('=') ? TokenKind::BangEqual : TokenKind::Bang, start, tokenStart);

    // =  or  ==  or  =>
    case '=':
        if (Match('=')) {
            return MakeToken(TokenKind::Equal, start, tokenStart);
        }
        if (Match('>')) {
            return MakeToken(TokenKind::FatArrow, start, tokenStart);
        }
        return MakeToken(TokenKind::Assign, start, tokenStart);

    // <  or  <=  or  <<=  or  <<
    case '<':
        if (Match('<')) {
            return MakeToken(Match('=') ? TokenKind::LessLessAssign : TokenKind::LessLess, start, tokenStart);
        }
        return MakeToken(Match('=') ? TokenKind::LessEqual : TokenKind::Less, start, tokenStart);

    // >  or  >=  or  >>  or  >>=  or  >>>  or  >>>=
    case '>':
        if (Match('>')) {
            if (Match('>')) {
                return MakeToken(Match('=') ? TokenKind::GreaterGreaterGreaterAssign : TokenKind::GreaterGreaterGreater,
                                 start, tokenStart);
            }
            return MakeToken(Match('=') ? TokenKind::GreaterGreaterAssign : TokenKind::GreaterGreater, start,
                             tokenStart);
        }
        return MakeToken(Match('=') ? TokenKind::GreaterEqual : TokenKind::Greater, start, tokenStart);

    default:
        return ScanUnknown(start);
    }
}

Token Lexer::ScanUnknown(const SourceLocation start) {
    const std::size_t tokenStart = pos - 1; // already advanced past the char
    // '$' used to mark a compiler-initialized constant, so point at its
    // replacement rather than report it as line noise.
    if (source[tokenStart] == '$') {
        EmitError(start, "'$' declarations are no longer supported",
                  "replace 'const $name: Type;' with 'intrinsic #name: Type;'");
    }
    else {
        const auto byte = static_cast<unsigned char>(source[tokenStart]);
        if (byte >= 0x80) {
            const auto decoded = DecodeUtf8(std::string_view(source).substr(tokenStart));
            assert(decoded.has_value());
            for (std::size_t i = 1; i < decoded->width; ++i) {
                Advance();
            }
            const std::string fragment = source.substr(tokenStart, decoded->width);
            EmitError(start, std::format("unexpected character '{}' (U+{:04X})", fragment, decoded->codePoint),
                      "remove the character or replace it with a valid Rux token");
        }
        else if (std::iscntrl(byte)) {
            EmitError(start, std::format("unexpected control byte 0x{:02X}", byte),
                      "remove the control byte or use whitespace accepted by Rux");
        }
        else {
            EmitError(start, std::format("unexpected character '{}'", source[tokenStart]),
                      "remove the character or replace it with a valid Rux token");
        }
    }
    return Token{TokenKind::Unknown, source.substr(tokenStart, pos - tokenStart), start};
}

Token Lexer::MakeToken(const TokenKind kind, const SourceLocation start, const std::size_t tokenStart) const {
    return Token{kind, source.substr(tokenStart, pos - tokenStart), start};
}

void Lexer::EmitError(const SourceLocation loc, std::string message, std::optional<std::string> help,
                      std::optional<std::string> documentationUrl) {
    diagnostics.push_back(LexerDiagnostic{LexerDiagnostic::Severity::Error,
                                          sourceName,
                                          loc,
                                          std::move(message),
                                          {},
                                          std::move(help),
                                          std::move(documentationUrl)});
}

void Lexer::EmitWarning(const SourceLocation loc, std::string message) {
    diagnostics.push_back(
        LexerDiagnostic{LexerDiagnostic::Severity::Warning, sourceName, loc, std::move(message), {}, {}, {}});
}
} // namespace Rux
