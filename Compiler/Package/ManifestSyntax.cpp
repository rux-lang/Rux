#include "Package/ManifestSyntax.h"

#include <charconv>
#include <format>
#include <utility>

namespace Rux::ManifestDetail {
std::string_view Value::KindName() const noexcept {
    switch (kind) {
    case Kind::String:
        return "string";
    case Kind::Integer:
        return "integer";
    case Kind::Boolean:
        return "boolean";
    case Kind::Array:
        return "array";
    case Kind::InlineTable:
        return "inline table";
    }
    return "value";
}

namespace {
// Thrown only to unwind the recursive-descent parser. ParseManifestSyntax
// catches it at the translation-unit boundary and returns an error value.
struct ParseFailure {
    Location location;
    std::string message;
};

class Scanner {
public:
    explicit Scanner(const std::string_view input)
        : text(input) {
    }

    Document ScanDocument() {
        Document document;
        SkipTrivia(true);
        // Keys before the first table header have no home in this syntax tree.
        if (!AtEnd() && Peek() != '[') {
            Fail("expected a table header such as '[Manifest]'");
        }
        while (!AtEnd()) {
            document.tables.push_back(ScanTable());
        }
        return document;
    }

private:
    std::string_view text;
    std::size_t pos = 0;
    std::uint32_t line = 1;
    std::uint32_t column = 1;

    [[nodiscard]] bool AtEnd() const noexcept {
        return pos >= text.size();
    }

    [[nodiscard]] char Peek() const noexcept {
        return pos < text.size() ? text[pos] : '\0';
    }

    [[nodiscard]] Location Here() const noexcept {
        return {line, column};
    }

    [[noreturn]] void Fail(std::string message) const {
        throw ParseFailure{Here(), std::move(message)};
    }

    [[noreturn]] static void FailAt(const Location location, std::string message) {
        throw ParseFailure{location, std::move(message)};
    }

    char Advance() {
        const char c = text[pos++];
        if (c == '\n') {
            ++line;
            column = 1;
        }
        else {
            ++column;
        }
        return c;
    }

    bool Match(const char expected) {
        if (Peek() == expected) {
            Advance();
            return true;
        }
        return false;
    }

    void Expect(const char expected, const std::string_view what) {
        if (!Match(expected)) {
            Fail(std::format("expected '{}' {}", expected, what));
        }
    }

    // Skip spaces, tabs and comments. Newlines are skipped only when the caller
    // is between entries rather than inside one.
    void SkipTrivia(const bool acrossLines) {
        while (!AtEnd()) {
            const char c = Peek();
            if (c == ' ' || c == '\t' || c == '\r') {
                Advance();
            }
            else if (c == '#') {
                while (!AtEnd() && Peek() != '\n') {
                    Advance();
                }
            }
            else if (c == '\n' && acrossLines) {
                Advance();
            }
            else {
                break;
            }
        }
    }

    void ExpectEndOfLine() {
        SkipTrivia(false);
        if (AtEnd()) {
            return;
        }
        if (Peek() != '\n') {
            Fail("unexpected text after the value");
        }
        Advance();
    }

    static bool IsBareKeyChar(const char c) noexcept {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    }

    std::string ScanKey() {
        if (Peek() == '"') {
            return ScanBasicString();
        }
        std::string key;
        while (!AtEnd() && IsBareKeyChar(Peek())) {
            key.push_back(Advance());
        }
        if (key.empty()) {
            Fail("expected a key");
        }
        return key;
    }

    void AppendUtf8(std::string &out, const std::uint32_t codepoint, const Location location) {
        if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
            FailAt(location, "escape is not a Unicode scalar value");
        }
        if (codepoint < 0x80) {
            out.push_back(static_cast<char>(codepoint));
        }
        else if (codepoint < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
        else if (codepoint < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
        else {
            out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
    }

    std::uint32_t ScanHexDigits(const int count, const Location location) {
        std::uint32_t codepoint = 0;
        for (int i = 0; i < count; ++i) {
            if (AtEnd()) {
                FailAt(location, "unterminated Unicode escape");
            }
            const char c = Advance();
            std::uint32_t digit = 0;
            if (c >= '0' && c <= '9') {
                digit = static_cast<std::uint32_t>(c - '0');
            }
            else if (c >= 'a' && c <= 'f') {
                digit = static_cast<std::uint32_t>(c - 'a' + 10);
            }
            else if (c >= 'A' && c <= 'F') {
                digit = static_cast<std::uint32_t>(c - 'A' + 10);
            }
            else {
                FailAt(location, "Unicode escape needs hexadecimal digits");
            }
            codepoint = codepoint * 16 + digit;
        }
        return codepoint;
    }

    std::string ScanBasicString() {
        const Location start = Here();
        Expect('"', "to open a string");
        std::string out;
        while (true) {
            if (AtEnd() || Peek() == '\n') {
                FailAt(start, "unterminated string");
            }
            const char c = Advance();
            if (c == '"') {
                break;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            const Location escape = Here();
            if (AtEnd()) {
                FailAt(start, "unterminated string");
            }
            switch (const char escaped = Advance()) {
            case '"':
                out.push_back('"');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u':
                AppendUtf8(out, ScanHexDigits(4, escape), escape);
                break;
            case 'U':
                AppendUtf8(out, ScanHexDigits(8, escape), escape);
                break;
            default:
                FailAt(escape, std::format("unknown string escape '\\{}'", escaped));
            }
        }
        return out;
    }

    std::unique_ptr<Value> ScanValue() {
        SkipTrivia(false);
        auto value = std::make_unique<Value>();
        value->location = Here();

        const char c = Peek();
        if (c == '"') {
            value->kind = Value::Kind::String;
            value->text = ScanBasicString();
            return value;
        }
        if (c == '[') {
            value->kind = Value::Kind::Array;
            Advance();
            SkipTrivia(true);
            while (Peek() != ']') {
                if (AtEnd()) {
                    FailAt(value->location, "unterminated array");
                }
                value->array.push_back(ScanValue());
                SkipTrivia(true);
                if (!Match(',')) {
                    break;
                }
                SkipTrivia(true);
            }
            SkipTrivia(true);
            Expect(']', "to close the array");
            return value;
        }
        if (c == '{') {
            value->kind = Value::Kind::InlineTable;
            Advance();
            SkipTrivia(false);
            while (Peek() != '}') {
                if (AtEnd() || Peek() == '\n') {
                    FailAt(value->location, "unterminated inline table");
                }
                KeyValue entry;
                entry.keyLocation = Here();
                entry.key = ScanKey();
                SkipTrivia(false);
                Expect('=', "after the key");
                entry.value = ScanValue();
                for (const auto &existing : value->table) {
                    if (existing.key == entry.key) {
                        FailAt(entry.keyLocation, std::format("duplicate key '{}'", entry.key));
                    }
                }
                value->table.push_back(std::move(entry));
                SkipTrivia(false);
                if (!Match(',')) {
                    break;
                }
                SkipTrivia(false);
            }
            SkipTrivia(false);
            Expect('}', "to close the inline table");
            return value;
        }
        if (c == 't' || c == 'f') {
            std::string word;
            while (!AtEnd() && Peek() >= 'a' && Peek() <= 'z') {
                word.push_back(Advance());
            }
            if (word != "true" && word != "false") {
                FailAt(value->location, std::format("expected a value, found '{}'", word));
            }
            value->kind = Value::Kind::Boolean;
            value->boolean = word == "true";
            return value;
        }
        if (c == '-' || c == '+' || (c >= '0' && c <= '9')) {
            std::string digits;
            if (c == '-' || c == '+') {
                digits.push_back(Advance());
            }
            while (!AtEnd() && Peek() >= '0' && Peek() <= '9') {
                digits.push_back(Advance());
            }
            // Anything else glued to the number is a value this syntax does not
            // accept, such as a float or a date.
            if (!AtEnd() && (IsBareKeyChar(Peek()) || Peek() == '.')) {
                FailAt(value->location, "expected an integer");
            }
            const std::string_view body = digits.front() == '+' ? std::string_view(digits).substr(1) : digits;
            std::int64_t number = 0;
            if (const auto [end, error] = std::from_chars(body.data(), body.data() + body.size(), number);
                error != std::errc{} || end != body.data() + body.size()) {
                FailAt(value->location, "integer is out of range");
            }
            value->kind = Value::Kind::Integer;
            value->integer = number;
            return value;
        }
        Fail("expected a value");
    }

    Table ScanTable() {
        Table table;
        table.location = Here();
        Expect('[', "to open a table header");
        SkipTrivia(false);
        table.name = ScanKey();
        while (Match('.')) {
            table.name.push_back('.');
            table.name += ScanKey();
        }
        SkipTrivia(false);
        Expect(']', "to close the table header");
        ExpectEndOfLine();

        while (true) {
            SkipTrivia(true);
            if (AtEnd() || Peek() == '[') {
                break;
            }
            KeyValue entry;
            entry.keyLocation = Here();
            entry.key = ScanKey();
            SkipTrivia(false);
            Expect('=', "after the key");
            entry.value = ScanValue();
            ExpectEndOfLine();
            for (const auto &existing : table.entries) {
                if (existing.key == entry.key) {
                    FailAt(entry.keyLocation, std::format("duplicate key '{}' in table '[{}]'", entry.key, table.name));
                }
            }
            table.entries.push_back(std::move(entry));
        }
        return table;
    }
};
} // namespace

std::expected<Document, SyntaxError> ParseManifestSyntax(const std::string_view text) {
    try {
        return Scanner(text).ScanDocument();
    }
    catch (const ParseFailure &failure) {
        return std::unexpected(SyntaxError{failure.location, failure.message});
    }
}
} // namespace Rux::ManifestDetail
