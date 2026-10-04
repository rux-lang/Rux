// Counting a literal format string's placeholders against the arguments a call passes after it.
// A parameter written `#Format()` is a format string, and the variadic parameter that ends the same list collects the
// values its placeholders take. When a call passes a string literal there, the count is known while compiling, so a
// mismatch is reported here rather than left to fail at run time. The grammar counted is the one every formatter in
// the first-party packages implements: `{}` and `{:spec}` are placeholders, `{{` and `}}` are literal braces. The
// compiler knows only the attribute, never which package declared the function.

#include "Semantic/Analysis/AnalysisContext.h"

#include <format>
#include <optional>
#include <string>

namespace Rux::SemanticDetail {
namespace {

// One hexadecimal digit's value, or nothing for a byte that is not one.
[[nodiscard]] std::optional<std::uint32_t> HexValue(const char digit) {
    if (digit >= '0' && digit <= '9') {
        return static_cast<std::uint32_t>(digit - '0');
    }
    if (digit >= 'a' && digit <= 'f') {
        return static_cast<std::uint32_t>(digit - 'a' + 10);
    }
    if (digit >= 'A' && digit <= 'F') {
        return static_cast<std::uint32_t>(digit - 'A' + 10);
    }
    return std::nullopt;
}

// The value of a string literal token, as far as a format pattern can tell. A character written as itself is kept, and
// an escape becomes a byte that is not a brace unless it decodes to one: a `\u{7B}` is a brace in the value although
// none is written in the source, and the `{` of a `\u{...}` escape is the reverse.
[[nodiscard]] std::string PatternOfLiteral(const std::string &token) {
    std::string value;
    const std::size_t open = token.find('"');
    const std::size_t close = token.rfind('"');
    if (open == std::string::npos || close == std::string::npos || close <= open) {
        return value;
    }
    for (std::size_t index = open + 1; index < close; ++index) {
        const char character = token[index];
        if (character != '\\') {
            value += character;
            continue;
        }
        ++index;
        if (index >= close) {
            break;
        }
        if (token[index] != 'u' || index + 1 >= close || token[index + 1] != '{') {
            value += '.';
            continue;
        }
        std::uint32_t codePoint = 0;
        index += 2;
        while (index < close && token[index] != '}') {
            if (const auto digit = HexValue(token[index])) {
                codePoint = (codePoint << 4) | *digit;
            }
            ++index;
        }
        value += codePoint == 0x7B ? '{' : codePoint == 0x7D ? '}' : '.';
    }
    return value;
}

// How many placeholders a pattern holds, or nothing when the pattern is not well-formed: an unmatched brace, or a
// placeholder whose body does not open with ':'. A malformed pattern is the formatter's own failure to report when it
// runs, and a count taken from one would only guess at what was meant.
[[nodiscard]] std::optional<std::size_t> CountPlaceholders(const std::string &pattern) {
    std::size_t placeholders = 0;
    std::size_t index = 0;
    while (index < pattern.size()) {
        const char character = pattern[index];
        const bool paired = index + 1 < pattern.size() && pattern[index + 1] == character;
        if ((character == '{' || character == '}') && paired) {
            index += 2;
            continue;
        }
        if (character == '}') {
            return std::nullopt;
        }
        if (character != '{') {
            ++index;
            continue;
        }
        const std::size_t end = pattern.find('}', index + 1);
        if (end == std::string::npos) {
            return std::nullopt;
        }
        const std::string_view body(pattern.data() + index + 1, end - index - 1);
        if ((!body.empty() && body.front() != ':') || body.find('{') != std::string_view::npos) {
            return std::nullopt;
        }
        ++placeholders;
        index = end + 1;
    }
    return placeholders;
}

[[nodiscard]] std::string Counted(const std::size_t count, const std::string_view singular) {
    return std::format("{} {}{}", count, singular, count == 1 ? "" : "s");
}

} // namespace

void AnalysisContext::CheckFormatArguments(const CallExpr &call, const FuncDecl &declaration, const bool isMethod) {
    std::vector<const Param *> parameters;
    for (const Param &parameter : declaration.params) {
        if (!(isMethod && parameter.IsReceiver())) {
            parameters.push_back(&parameter);
        }
    }
    if (parameters.empty() || !parameters.back()->isVariadic) {
        return;
    }
    std::size_t formatIndex = parameters.size();
    for (std::size_t index = 0; index + 1 < parameters.size(); ++index) {
        if (parameters[index]->isFormat) {
            formatIndex = index;
            break;
        }
    }
    // Too few arguments is an arity error reported where the call was resolved; nothing is counted against it.
    const std::size_t fixedCount = parameters.size() - 1;
    if (formatIndex == parameters.size() || call.args.size() < fixedCount) {
        return;
    }
    // A spread hands over a slice whose length is a run-time value.
    for (std::size_t index = fixedCount; index < call.args.size(); ++index) {
        if (dynamic_cast<const SpreadExpr *>(call.args[index].get())) {
            return;
        }
    }
    const auto *literal = dynamic_cast<const LiteralExpr *>(call.args[formatIndex].get());
    if (!literal || literal->token.kind != TokenKind::StringLiteral) {
        return;
    }
    const auto placeholders = CountPlaceholders(PatternOfLiteral(literal->token.text));
    const std::size_t provided = call.args.size() - fixedCount;
    if (!placeholders || *placeholders == provided) {
        return;
    }
    const Param &format = *parameters[formatIndex];
    std::string file = currentFile;
    if (const auto source = functionDeclFiles.find(&declaration); source != functionDeclFiles.end()) {
        file = source->second;
    }
    EmitError(literal->location,
              std::format("format string has {}, but {} provided", Counted(*placeholders, "placeholder"),
                          provided == 1 ? "1 argument was" : std::format("{} arguments were", provided)),
              {std::format("format parameter '{}' of '{}' declared at '{}':{}:{}", format.name, declaration.name, file,
                           format.location.line, format.location.column)},
              *placeholders > provided ? "pass one argument for each '{}' placeholder"
                                       : "remove the extra arguments, or add a placeholder for each; write '{{' and "
                                         "'}}' for a literal brace");
}

} // namespace Rux::SemanticDetail
