#include "Diagnostics/Diagnostics.h"

#include "Reporting/Reporting.h"

#include <cstdio>
#include <format>
#include <limits>
#include <utility>
#include <vector>

namespace Rux {
namespace {
/// Append text with control bytes escaped.
///
/// A diagnostic can quote source or a file path, and neither is trusted to be printable: an embedded escape sequence
/// could otherwise move the cursor and forge lines the compiler never wrote.
void AppendHumanText(std::string &out, const std::string_view text) {
    for (const char ch : text) {
        const auto value = static_cast<unsigned char>(ch);
        switch (value) {
        case '\0':
            out += "\\0";
            break;
        case '\a':
            out += "\\a";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\v':
            out += "\\v";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\r':
            out += "\\r";
            break;
        default:
            if (value < 0x20 || value == 0x7f) {
                out += std::format("\\x{:02x}", value);
            }
            else {
                out += ch;
            }
            break;
        }
    }
}

void AppendLabel(std::string &out, const std::string_view label, const std::string_view prefix,
                 const Reporting::Style &style) {
    out += prefix;
    out += style.Bold();
    out += label;
    out += ':';
    out += style.Reset();
}

constexpr std::size_t tabWidth = 4;
constexpr std::size_t sourceFrameWidth = 120;
constexpr std::size_t clippingMarkerWidth = 3;

struct ExpandedSourceLine {
    std::vector<std::string> cells;
    std::size_t caret = 0;
};

/// The byte length of the UTF-8 code point starting at `offset`, so the caret can be positioned by character rather
/// than by byte.
std::size_t Utf8CodePointSize(const std::string_view text, const std::size_t offset) {
    const auto lead = static_cast<unsigned char>(text[offset]);
    std::size_t size = 1;
    if ((lead & 0xe0U) == 0xc0U) {
        size = 2;
    }
    else if ((lead & 0xf0U) == 0xe0U) {
        size = 3;
    }
    else if ((lead & 0xf8U) == 0xf0U) {
        size = 4;
    }
    if (offset + size > text.size()) {
        return 1;
    }
    for (std::size_t index = 1; index < size; ++index) {
        if ((static_cast<unsigned char>(text[offset + index]) & 0xc0U) != 0x80U) {
            return 1;
        }
    }
    return size;
}

void AppendEscapedSourceByte(std::vector<std::string> &cells, const unsigned char value) {
    const auto escaped = std::format("\\x{:02x}", value);
    for (const char ch : escaped) {
        cells.emplace_back(1, ch);
    }
}

/// Prepare one source line for display, expanding tabs and escaping unprintable bytes, and reporting where the caret
/// lands once those substitutions have shifted the columns.
ExpandedSourceLine ExpandSourceLine(const std::string_view line, const std::size_t column) {
    ExpandedSourceLine expanded;
    const std::size_t caretByte = std::min(column > 0 ? column - 1 : 0, line.size());
    std::size_t offset = 0;
    while (offset < line.size()) {
        const auto value = static_cast<unsigned char>(line[offset]);
        if (value == '\t') {
            const auto spaces = tabWidth - (expanded.cells.size() % tabWidth);
            for (std::size_t index = 0; index < spaces; ++index) {
                expanded.cells.emplace_back(" ");
            }
            ++offset;
            if (offset <= caretByte) {
                expanded.caret = expanded.cells.size();
            }
            continue;
        }
        if (value < 0x20 || value == 0x7f) {
            AppendEscapedSourceByte(expanded.cells, value);
            ++offset;
            if (offset <= caretByte) {
                expanded.caret = expanded.cells.size();
            }
            continue;
        }
        const auto codePointSize = Utf8CodePointSize(line, offset);
        expanded.cells.emplace_back(line.substr(offset, codePointSize));
        offset += codePointSize;
        if (offset <= caretByte) {
            expanded.caret = expanded.cells.size();
        }
    }
    if (caretByte >= offset) {
        expanded.caret = expanded.cells.size();
    }
    return expanded;
}

void AppendSourceFrame(std::string &out, const Diagnostic &diag, const std::string_view line,
                       const Reporting::Style &style) {
    auto expanded = ExpandSourceLine(line, diag.location.column);
    const std::size_t contentCapacity = sourceFrameWidth - (2 * clippingMarkerWidth);
    std::size_t begin = expanded.caret > contentCapacity / 2 ? expanded.caret - contentCapacity / 2 : 0;
    std::size_t end = std::min(expanded.cells.size(), begin + contentCapacity);
    if (end == expanded.cells.size() && end > contentCapacity) {
        begin = end - contentCapacity;
    }
    const bool clippedLeft = begin > 0;
    const bool clippedRight = end < expanded.cells.size();
    const std::size_t renderedCaret = (clippedLeft ? clippingMarkerWidth : 0) + expanded.caret - begin;
    const auto gutterWidth = std::to_string(diag.location.line).size();

    out += style.Dim();
    out += "  ";
    out += std::to_string(diag.location.line);
    out += " |";
    out += style.Reset();
    out += ' ';
    if (clippedLeft) {
        out += "...";
    }
    for (std::size_t index = begin; index < end; ++index) {
        out += expanded.cells[index];
    }
    if (clippedRight) {
        out += "...";
    }
    out += '\n';

    out += style.Dim();
    out.append(gutterWidth + 2, ' ');
    out += " |";
    out += style.Reset();
    out += ' ';
    out.append(renderedCaret, ' ');
    out += diag.severity == Diagnostic::Severity::Error ? style.Red() : style.Yellow();
    out += '^';
    out += style.Reset();
    out += '\n';
}
} // namespace

std::string EscapeJson(std::string_view s) {
    std::string out;
    if (s.size() < ((std::numeric_limits<size_t>::max)() - 128)) {
        out.reserve(s.size() + (s.size() / 10) + 16);
    }
    for (char ch : s) {
        unsigned char u_ch = static_cast<unsigned char>(ch);
        switch (u_ch) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default: {
            if (u_ch < 0x20) {
                char buf[7];
                std::snprintf(buf, sizeof(buf), "\\u%04x", u_ch);
                out += buf;
            }
            else {
                out += ch;
            }
            break;
        }
        }
    }
    return out;
}

std::optional<std::string_view> FindSourceLine(const std::string_view source, const std::size_t lineNumber) {
    if (lineNumber == 0) {
        return std::nullopt;
    }
    std::size_t currentLine = 1;
    std::size_t begin = 0;
    while (currentLine < lineNumber) {
        const auto newline = source.find('\n', begin);
        if (newline == std::string_view::npos) {
            return std::nullopt;
        }
        begin = newline + 1;
        ++currentLine;
    }
    auto end = source.find('\n', begin);
    if (end == std::string_view::npos) {
        end = source.size();
    }
    if (end > begin && source[end - 1] == '\r') {
        --end;
    }
    return source.substr(begin, end - begin);
}

Diagnostic ErrorDiagnostic(std::string message, std::vector<std::string> notes, std::optional<std::string> help,
                           std::optional<std::string> documentationUrl) {
    return {Diagnostic::Severity::Error,
            {},
            {.line = 0, .column = 0, .offset = 0},
            std::move(message),
            std::move(notes),
            std::move(help),
            std::move(documentationUrl)};
}

std::string_view SeverityName(const Diagnostic::Severity severity) noexcept {
    return severity == Diagnostic::Severity::Error ? "error" : "warning";
}

std::string RenderDiagnostic(const Diagnostic &diag, const bool color, const SourceLineLookup &sourceLineLookup) {
    const Reporting::Style style{color};
    std::string out;
    out.reserve(diag.sourceName.size() + diag.message.size() + 64);

    if (!diag.sourceName.empty()) {
        AppendHumanText(out, diag.sourceName);
        out += std::format(":{}:{}: ", diag.location.line, diag.location.column);
    }

    const auto severityColor = diag.severity == Diagnostic::Severity::Error ? style.Red() : style.Yellow();
    AppendLabel(out, SeverityName(diag.severity), severityColor, style);
    out += ' ';
    AppendHumanText(out, diag.message);
    out += '\n';

    if (sourceLineLookup && !diag.sourceName.empty() && diag.location.line > 0 && diag.location.column > 0) {
        if (const auto sourceLine = sourceLineLookup(diag.sourceName, diag.location.line)) {
            AppendSourceFrame(out, diag, *sourceLine, style);
        }
    }

    for (const auto &note : diag.notes) {
        if (note.empty()) {
            continue;
        }
        out += Reporting::indentation;
        AppendLabel(out, "note", style.Dim(), style);
        out += ' ';
        AppendHumanText(out, note);
        out += '\n';
    }
    if (diag.help && !diag.help->empty()) {
        out += Reporting::indentation;
        AppendLabel(out, "help", {}, style);
        out += ' ';
        AppendHumanText(out, *diag.help);
        out += '\n';
    }
    if (diag.documentationUrl && !diag.documentationUrl->empty()) {
        out += Reporting::indentation;
        AppendLabel(out, "docs", style.Cyan(), style);
        out += ' ';
        out += style.Cyan();
        AppendHumanText(out, *diag.documentationUrl);
        out += style.Reset();
        out += '\n';
    }
    return out;
}

std::string RenderDiagnosticsJson(const std::span<const Diagnostic> diags, const bool success) {
    std::string out = std::format("{{\n  \"success\": {},\n  \"diagnostics\": [\n", success ? "true" : "false");
    for (std::size_t i = 0; i < diags.size(); ++i) {
        const auto &d = diags[i];
        out +=
            std::format("    {{\"file\":\"{}\",\"line\":{},\"column\":{},\"severity\":\"{}\","
                        "\"message\":\"{}\"}}{}\n",
                        EscapeJson(d.sourceName), d.location.line, d.location.column,
                        EscapeJson(SeverityName(d.severity)), EscapeJson(d.message), (i + 1 < diags.size()) ? "," : "");
    }
    out += "  ]\n}\n";
    return out;
}

} // namespace Rux
