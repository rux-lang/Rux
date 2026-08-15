#include "Diagnostics/Diagnostics.h"

#include "Reporting/Reporting.h"

#include <cstdio>
#include <format>
#include <limits>
#include <print>
#include <utility>

namespace Rux {
namespace {
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

std::string RenderDiagnostic(const Diagnostic &diag, const bool color) {
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

void PrintDiagnostic(const Diagnostic &diag) {
    std::print(stderr, "{}", RenderDiagnostic(diag));
}

bool PrintDiagnostics(std::span<const Diagnostic> diags) {
    bool hasErrors = false;
    for (const auto &diag : diags) {
        PrintDiagnostic(diag);
        hasErrors |= diag.IsError();
    }
    return hasErrors;
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

void PrintDiagnosticsJson(const std::span<const Diagnostic> diags, const bool success) {
    std::print("{}", RenderDiagnosticsJson(diags, success));
}
} // namespace Rux
