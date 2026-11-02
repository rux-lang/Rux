// Counting a literal format string's placeholders against the arguments a call passes after it.
// A parameter written `#Format()` is a format string, and the variadic parameter that ends the same list collects the
// values its placeholders take. When a call passes a string literal there, the count is known while compiling, so a
// mismatch is reported here rather than left to fail at run time. The grammar counted is the one every formatter in
// the first-party packages implements: `{}` and `{:spec}` are placeholders, `{{` and `}}` are literal braces. The
// compiler knows only the attribute, never which package declared the function.

#include "Semantic/Analysis/AnalysisContext.h"

#include <algorithm>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

/// A placeholder as the source spells it: where its braces are in the literal's token text, and what is between them.
struct RawPlaceholder {
    std::size_t open = 0;
    std::size_t close = 0;
    std::string body;
};

/// The placeholders of a string literal's token text, read from its source spelling: an escape is skipped, so the
/// braces of a `\u{...}` are no placeholder's, and `{{` and `}}` are literal braces. Nothing when a brace is unmatched.
[[nodiscard]] std::optional<std::vector<RawPlaceholder>> RawPlaceholders(const std::string &token) {
    const std::size_t start = token.find('"');
    const std::size_t end = token.rfind('"');
    if (start == std::string::npos || end == std::string::npos || end <= start) {
        return std::nullopt;
    }
    std::vector<RawPlaceholder> placeholders;
    for (std::size_t index = start + 1; index < end; ++index) {
        const char character = token[index];
        if (character == '\\') {
            if (index + 2 < end && token[index + 1] == 'u' && token[index + 2] == '{') {
                const std::size_t closing = token.find('}', index + 2);
                index = closing == std::string::npos ? end : closing;
            }
            else {
                ++index;
            }
            continue;
        }
        if ((character == '{' || character == '}') && index + 1 < end && token[index + 1] == character) {
            ++index;
            continue;
        }
        if (character == '}') {
            return std::nullopt;
        }
        if (character != '{') {
            continue;
        }
        const std::size_t closing = token.find('}', index + 1);
        if (closing == std::string::npos || closing >= end) {
            return std::nullopt;
        }
        RawPlaceholder placeholder{index, closing, token.substr(index + 1, closing - index - 1)};
        if (placeholder.body.find_first_of("{\\\"") != std::string::npos) {
            return std::nullopt;
        }
        placeholders.push_back(std::move(placeholder));
        index = closing;
    }
    return placeholders;
}

/// The name part of a placeholder's body, before any `:spec`.
[[nodiscard]] std::string PlaceholderName(const std::string &body) {
    return body.substr(0, body.find(':'));
}

/// Whether a placeholder starts with a name, as `{key}` and `{Twice(key)}` do; `{0}` and `{+}` are only malformed.
[[nodiscard]] bool StartsWithName(const std::string &body) {
    const std::string name = PlaceholderName(body);
    return !name.empty() && ((name.front() >= 'a' && name.front() <= 'z') ||
                             (name.front() >= 'A' && name.front() <= 'Z') || name.front() == '_');
}

/// Whether `name` is a binding or a field path: identifiers joined by '.'.
[[nodiscard]] bool IsNamePath(const std::string_view name) {
    bool segmentStart = true;
    for (const char character : name) {
        const bool letter =
            (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || character == '_';
        const bool digit = character >= '0' && character <= '9';
        if (character == '.') {
            if (segmentStart) {
                return false;
            }
            segmentStart = true;
            continue;
        }
        if (segmentStart ? !letter : !(letter || digit)) {
            return false;
        }
        segmentStart = false;
    }
    return !name.empty() && !segmentStart;
}

/// The expression a placeholder's name path stands for, located inside the literal.
[[nodiscard]] std::shared_ptr<const Expr> NamePathExpression(const std::string &name, const SourceLocation location) {
    ExprPtr expression;
    std::size_t start = 0;
    for (;;) {
        const std::size_t dot = name.find('.', start);
        const std::string segment = name.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        SourceLocation at = location;
        at.column += static_cast<std::uint32_t>(start);
        at.offset += static_cast<std::uint32_t>(start);
        if (!expression) {
            if (segment == "self") {
                expression = std::make_unique<SelfExpr>();
            }
            else {
                auto identifier = std::make_unique<IdentExpr>();
                identifier->name = segment;
                expression = std::move(identifier);
            }
            expression->location = at;
        }
        else {
            auto field = std::make_unique<FieldExpr>();
            field->location = at;
            field->memberLocation = at;
            field->object = std::move(expression);
            field->field = segment;
            expression = std::move(field);
        }
        if (dot == std::string::npos) {
            break;
        }
        start = dot + 1;
    }
    return std::shared_ptr<const Expr>(std::move(expression));
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
        if (AstCast<const SpreadExpr>(call.args[index].get())) {
            return;
        }
    }
    const auto *literal = AstCast<const LiteralExpr>(call.args[formatIndex].get());
    if (!literal || literal->token.kind != TokenKind::StringLiteral) {
        return;
    }
    const std::size_t provided = call.args.size() - fixedCount;
    if (CheckNamedFormat(call, *literal, declaration, provided)) {
        return;
    }
    const auto placeholders = CountPlaceholders(PatternOfLiteral(literal->token.text));
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

bool AnalysisContext::CheckNamedFormat(const CallExpr &call, const LiteralExpr &literal, const FuncDecl &declaration,
                                       const std::size_t providedCount) {
    const auto placeholders = RawPlaceholders(literal.token.text);
    if (!placeholders) {
        return false;
    }
    const auto isNamed = [](const RawPlaceholder &placeholder) { return StartsWithName(placeholder.body); };
    if (std::ranges::none_of(*placeholders, isNamed)) {
        return false;
    }
    const auto nameLocation = [&](const RawPlaceholder &placeholder) {
        SourceLocation location = literal.location;
        location.column += static_cast<std::uint32_t>(placeholder.open + 1);
        location.offset += static_cast<std::uint32_t>(placeholder.open + 1);
        return location;
    };
    if (std::ranges::any_of(*placeholders, [](const RawPlaceholder &placeholder) {
            return PlaceholderName(placeholder.body).empty();
        })) {
        EmitError(literal.location, "format string mixes named and positional placeholders",
                  {"a '{}' placeholder takes the next argument, which a named placeholder does not leave"},
                  "name every placeholder or none");
        return true;
    }
    if (providedCount != 0) {
        EmitError(call.args[call.args.size() - providedCount]->location,
                  "a format string with named placeholders takes no arguments after it",
                  {"each named placeholder already names the value it writes"}, "name every placeholder or none");
        return true;
    }
    for (const RawPlaceholder &placeholder : *placeholders) {
        const std::string name = PlaceholderName(placeholder.body);
        if (!IsNamePath(name)) {
            EmitError(nameLocation(placeholder),
                      std::format("format placeholder '{{{}}}' must name a binding or a field path", name),
                      {"a placeholder names a value; it does not evaluate an expression"},
                      "pass the expression as an argument and write '{}' in its place");
            return true;
        }
    }
    // A body checked again, as a generic body is for an instantiation, checks the same value expressions.
    NamedFormatArguments rewritten;
    const auto previous = namedFormats.find(&literal);
    const bool reuse = previous != namedFormats.end() && previous->second.values.size() == placeholders->size();
    std::string positional = literal.token.text;
    for (std::size_t index = placeholders->size(); index-- > 0;) {
        const RawPlaceholder &placeholder = (*placeholders)[index];
        positional.erase(placeholder.open + 1, PlaceholderName(placeholder.body).size());
    }
    rewritten.positional = std::move(positional);
    for (std::size_t index = 0; index < placeholders->size(); ++index) {
        const RawPlaceholder &placeholder = (*placeholders)[index];
        rewritten.values.push_back(
            reuse ? previous->second.values[index]
                  : NamePathExpression(PlaceholderName(placeholder.body), nameLocation(placeholder)));
    }
    // Each value is an argument of the variadic parameter, as if it had been written after the literal.
    const std::optional<TypeRef> element = ResolveVariadicElementType(declaration);
    for (std::size_t index = 0; index < rewritten.values.size(); ++index) {
        const Expr &value = *rewritten.values[index];
        const TypeRef type = CheckExpr(value);
        if (type.IsUnknown()) {
            continue;
        }
        if (element && !element->IsUnknown() && !CanAssignExprTo(value, type, *element)) {
            EmitError(value.location,
                      std::format("format placeholder '{{{}}}' has type '{}', but the format arguments take '{}'",
                                  PlaceholderName((*placeholders)[index].body), type.DisplayString(),
                                  element->ToString()));
            continue;
        }
        // A borrowed format argument lends its value for the call and consumes nothing.
        if (!element || element->IsUnknown() || (*element)->kind != TypeRef::Kind::Reference) {
            ConsumeValue(value, type, ValueConsumptionKind::Argument, value.location);
        }
    }
    namedFormats.insert_or_assign(&literal, std::move(rewritten));
    return true;
}

} // namespace Rux::SemanticDetail
