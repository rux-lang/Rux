#include "System/Json.h"

#include <cmath>
#include <format>

namespace Rux::System {
namespace {
const std::string emptyString;
const std::vector<JsonValue> emptyElements;
const std::vector<JsonValue::Member> emptyMembers;

[[nodiscard]] bool IsWhitespace(const char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

[[nodiscard]] bool IsDigit(const char c) noexcept {
    return c >= '0' && c <= '9';
}

/// Append one code point to `out` as UTF-8.
void AppendUtf8(const std::uint32_t codePoint, std::string &out) {
    if (codePoint < 0x80) {
        out.push_back(static_cast<char>(codePoint));
    }
    else if (codePoint < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
    else if (codePoint < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
    else {
        out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}
} // namespace

/**
 * @brief Recursive-descent reader over one document.
 *
 * Nesting is bounded explicitly rather than by the call stack, so a document of
 * ten thousand open brackets is a diagnostic instead of a crash.
 */
class JsonParser {
public:
    explicit JsonParser(const std::string_view text)
        : source(text) {
    }

    [[nodiscard]] std::expected<JsonValue, JsonParseError> ParseDocument() {
        SkipWhitespace();
        auto value = ParseValue(0);
        if (!value) {
            return value;
        }
        SkipWhitespace();
        if (pos != source.size()) {
            return Fail("unexpected trailing content");
        }
        return value;
    }

private:
    std::string_view source;
    std::size_t pos = 0;

    [[nodiscard]] std::unexpected<JsonParseError> Fail(std::string message) const {
        return std::unexpected(JsonParseError{.message = std::move(message), .offset = pos});
    }

    void SkipWhitespace() {
        while (pos < source.size() && IsWhitespace(source[pos])) {
            ++pos;
        }
    }

    [[nodiscard]] bool Consume(const char expected) {
        if (pos < source.size() && source[pos] == expected) {
            ++pos;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool ConsumeLiteral(const std::string_view literal) {
        if (source.compare(pos, literal.size(), literal) != 0) {
            return false;
        }
        pos += literal.size();
        return true;
    }

    [[nodiscard]] std::expected<JsonValue, JsonParseError> ParseValue(const std::size_t depth) {
        if (depth > jsonMaxDepth) {
            return Fail(std::format("nesting deeper than {} levels", jsonMaxDepth));
        }
        if (pos >= source.size()) {
            return Fail("unexpected end of document");
        }
        switch (source[pos]) {
        case '{':
            return ParseObject(depth);
        case '[':
            return ParseArray(depth);
        case '"': {
            auto text = ParseString();
            if (!text) {
                return std::unexpected(text.error());
            }
            JsonValue value;
            value.kind = JsonKind::String;
            value.text = std::move(*text);
            return value;
        }
        case 't': {
            if (!ConsumeLiteral("true")) {
                return Fail("invalid literal");
            }
            JsonValue value;
            value.kind = JsonKind::Bool;
            value.boolean = true;
            return value;
        }
        case 'f': {
            if (!ConsumeLiteral("false")) {
                return Fail("invalid literal");
            }
            JsonValue value;
            value.kind = JsonKind::Bool;
            value.boolean = false;
            return value;
        }
        case 'n': {
            if (!ConsumeLiteral("null")) {
                return Fail("invalid literal");
            }
            return JsonValue{};
        }
        default:
            return ParseNumber();
        }
    }

    [[nodiscard]] std::expected<JsonValue, JsonParseError> ParseObject(const std::size_t depth) {
        static_cast<void>(Consume('{'));
        JsonValue value;
        value.kind = JsonKind::Object;
        SkipWhitespace();
        if (Consume('}')) {
            return value;
        }
        while (true) {
            SkipWhitespace();
            if (pos >= source.size() || source[pos] != '"') {
                return Fail("expected a member name");
            }
            auto name = ParseString();
            if (!name) {
                return std::unexpected(name.error());
            }
            SkipWhitespace();
            if (!Consume(':')) {
                return Fail("expected ':' after a member name");
            }
            SkipWhitespace();
            auto member = ParseValue(depth + 1);
            if (!member) {
                return member;
            }
            value.members.emplace_back(std::move(*name), std::move(*member));
            SkipWhitespace();
            if (Consume(',')) {
                continue;
            }
            if (Consume('}')) {
                return value;
            }
            return Fail("expected ',' or '}' in an object");
        }
    }

    [[nodiscard]] std::expected<JsonValue, JsonParseError> ParseArray(const std::size_t depth) {
        static_cast<void>(Consume('['));
        JsonValue value;
        value.kind = JsonKind::Array;
        SkipWhitespace();
        if (Consume(']')) {
            return value;
        }
        while (true) {
            SkipWhitespace();
            auto element = ParseValue(depth + 1);
            if (!element) {
                return element;
            }
            value.elements.push_back(std::move(*element));
            SkipWhitespace();
            if (Consume(',')) {
                continue;
            }
            if (Consume(']')) {
                return value;
            }
            return Fail("expected ',' or ']' in an array");
        }
    }

    /// Read one `\uXXXX` escape body, leaving `pos` after the four digits.
    [[nodiscard]] std::expected<std::uint32_t, JsonParseError> ParseHex4() {
        if (pos + 4 > source.size()) {
            return Fail("truncated '\\u' escape");
        }
        std::uint32_t code = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = source[pos + static_cast<std::size_t>(i)];
            std::uint32_t digit = 0;
            if (IsDigit(c)) {
                digit = static_cast<std::uint32_t>(c - '0');
            }
            else if (c >= 'a' && c <= 'f') {
                digit = static_cast<std::uint32_t>(c - 'a') + 10;
            }
            else if (c >= 'A' && c <= 'F') {
                digit = static_cast<std::uint32_t>(c - 'A') + 10;
            }
            else {
                return Fail("invalid '\\u' escape");
            }
            code = (code << 4) | digit;
        }
        pos += 4;
        return code;
    }

    [[nodiscard]] std::expected<std::string, JsonParseError> ParseString() {
        static_cast<void>(Consume('"'));
        std::string out;
        while (true) {
            if (pos >= source.size()) {
                return Fail("unterminated string");
            }
            const char c = source[pos];
            if (c == '"') {
                ++pos;
                return out;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return Fail("unescaped control character in a string");
            }
            if (c != '\\') {
                out.push_back(c);
                ++pos;
                continue;
            }
            ++pos;
            if (pos >= source.size()) {
                return Fail("unterminated escape");
            }
            const char escape = source[pos++];
            switch (escape) {
            case '"':
                out.push_back('"');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '/':
                out.push_back('/');
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
            case 'u': {
                auto code = ParseHex4();
                if (!code) {
                    return std::unexpected(code.error());
                }
                std::uint32_t codePoint = *code;
                // A high surrogate is only meaningful paired with the low one
                // that follows it; an unpaired half becomes the replacement
                // character rather than invalid UTF-8.
                if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
                    if (pos + 1 < source.size() && source[pos] == '\\' && source[pos + 1] == 'u') {
                        const std::size_t rewind = pos;
                        pos += 2;
                        auto low = ParseHex4();
                        if (!low) {
                            return std::unexpected(low.error());
                        }
                        if (*low >= 0xDC00 && *low <= 0xDFFF) {
                            codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (*low - 0xDC00);
                        }
                        else {
                            pos = rewind;
                            codePoint = 0xFFFD;
                        }
                    }
                    else {
                        codePoint = 0xFFFD;
                    }
                }
                else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF) {
                    codePoint = 0xFFFD;
                }
                AppendUtf8(codePoint, out);
                break;
            }
            default:
                return Fail("unknown escape");
            }
        }
    }

    /**
     * @brief Read one JSON number.
     *
     * The digits are accumulated directly rather than handed to `strtod`, whose
     * decimal separator follows the C locale. No registry field the toolchain
     * reads needs more precision than this gives.
     */
    [[nodiscard]] std::expected<JsonValue, JsonParseError> ParseNumber() {
        const std::size_t start = pos;
        bool negative = false;
        if (Consume('-')) {
            negative = true;
        }
        if (pos >= source.size() || !IsDigit(source[pos])) {
            pos = start;
            return Fail("expected a value");
        }
        // JSON has no leading zeros, so `01` is two tokens rather than one
        // number. Accepting it would silently read half of a malformed
        // document as if it were well-formed.
        if (source[pos] == '0' && pos + 1 < source.size() && IsDigit(source[pos + 1])) {
            return Fail("a number cannot have a leading zero");
        }
        long double mantissa = 0.0L;
        while (pos < source.size() && IsDigit(source[pos])) {
            mantissa = mantissa * 10.0L + static_cast<long double>(source[pos] - '0');
            ++pos;
        }
        int exponent = 0;
        if (Consume('.')) {
            if (pos >= source.size() || !IsDigit(source[pos])) {
                return Fail("expected a digit after '.'");
            }
            while (pos < source.size() && IsDigit(source[pos])) {
                mantissa = mantissa * 10.0L + static_cast<long double>(source[pos] - '0');
                --exponent;
                ++pos;
            }
        }
        if (pos < source.size() && (source[pos] == 'e' || source[pos] == 'E')) {
            ++pos;
            bool negativeExponent = false;
            if (pos < source.size() && (source[pos] == '+' || source[pos] == '-')) {
                negativeExponent = source[pos] == '-';
                ++pos;
            }
            if (pos >= source.size() || !IsDigit(source[pos])) {
                return Fail("expected a digit in an exponent");
            }
            int literal = 0;
            while (pos < source.size() && IsDigit(source[pos])) {
                // Saturate rather than overflow; the result is already infinite
                // or zero long before this point.
                literal = literal > 100000 ? literal : literal * 10 + (source[pos] - '0');
                ++pos;
            }
            exponent += negativeExponent ? -literal : literal;
        }

        JsonValue value;
        value.kind = JsonKind::Number;
        value.number = static_cast<double>(mantissa * std::pow(10.0L, static_cast<long double>(exponent)));
        if (negative) {
            value.number = -value.number;
        }
        return value;
    }
};

const std::string &JsonValue::AsString() const noexcept {
    return kind == JsonKind::String ? text : emptyString;
}

const std::vector<JsonValue> &JsonValue::Elements() const noexcept {
    return kind == JsonKind::Array ? elements : emptyElements;
}

const std::vector<JsonValue::Member> &JsonValue::Members() const noexcept {
    return kind == JsonKind::Object ? members : emptyMembers;
}

const JsonValue *JsonValue::Find(const std::string_view key) const noexcept {
    if (kind != JsonKind::Object) {
        return nullptr;
    }
    for (const auto &member : members) {
        if (member.first == key) {
            return &member.second;
        }
    }
    return nullptr;
}

const std::string &JsonValue::StringAt(const std::string_view key) const noexcept {
    const JsonValue *member = Find(key);
    return member ? member->AsString() : emptyString;
}

bool JsonValue::BoolAt(const std::string_view key, const bool fallback) const noexcept {
    const JsonValue *member = Find(key);
    return member ? member->AsBool(fallback) : fallback;
}

std::expected<JsonValue, JsonParseError> ParseJson(const std::string_view text) {
    if (text.size() > jsonMaxBytes) {
        return std::unexpected(
            JsonParseError{.message = std::format("document larger than {} bytes", jsonMaxBytes), .offset = 0});
    }
    JsonParser parser(text);
    return parser.ParseDocument();
}

namespace {
/// Depth-first search for the first string member named `key`.
const std::string *FindStringMember(const JsonValue &value, const std::string_view key) {
    if (value.IsObject()) {
        for (const auto &member : value.Members()) {
            if (member.first == key && member.second.Kind() == JsonKind::String) {
                return &member.second.AsString();
            }
        }
        for (const auto &member : value.Members()) {
            if (const std::string *found = FindStringMember(member.second, key)) {
                return found;
            }
        }
        return nullptr;
    }
    for (const auto &element : value.Elements()) {
        if (const std::string *found = FindStringMember(element, key)) {
            return found;
        }
    }
    return nullptr;
}
} // namespace

std::string JsonLookupString(const std::string_view json, const std::string_view key) {
    const auto document = ParseJson(json);
    if (!document) {
        return {};
    }
    const std::string *found = FindStringMember(*document, key);
    return found ? *found : std::string{};
}

std::vector<ProblemError> JsonFindProblemErrors(const std::string_view json) {
    std::vector<ProblemError> errors;
    const auto document = ParseJson(json);
    if (!document) {
        return errors;
    }
    const JsonValue *array = document->Find("errors");
    if (array == nullptr || !array->IsArray()) {
        return errors;
    }
    for (const auto &element : array->Elements()) {
        ProblemError entry{.code = element.StringAt("code"), .detail = element.StringAt("detail")};
        if (!entry.code.empty() || !entry.detail.empty()) {
            errors.push_back(std::move(entry));
        }
    }
    return errors;
}
} // namespace Rux::System
