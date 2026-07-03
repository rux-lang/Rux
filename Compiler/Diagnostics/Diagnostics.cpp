#include "Diagnostics/Diagnostics.h"

#include <cstdio>
#include <limits>
#include <print>

namespace Rux {

namespace {

std::string JsonEscape(std::string_view s) {
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

} // namespace

Diagnostic ErrorDiagnostic(std::string message) {
    return {Diagnostic::Severity::Error, {}, {.line = 0, .column = 0, .offset = 0}, std::move(message)};
}

std::string_view SeverityName(const Diagnostic::Severity severity) noexcept {
    return severity == Diagnostic::Severity::Error ? "error" : "warning";
}

void PrintDiagnostic(const Diagnostic &diag) {
    if (diag.sourceName.empty()) {
        std::print(stderr, "{}: {}\n", SeverityName(diag.severity), diag.message);
    }
    else {
        std::print(stderr, "{}:{}:{}: {}: {}\n", diag.sourceName, diag.location.line, diag.location.column,
                   SeverityName(diag.severity), diag.message);
    }
}

bool PrintDiagnostics(std::span<const Diagnostic> diags) {
    bool hasErrors = false;
    for (const auto &diag : diags) {
        PrintDiagnostic(diag);
        hasErrors |= diag.IsError();
    }
    return hasErrors;
}

void PrintDiagnosticsJson(std::span<const Diagnostic> diags, const bool success) {
    std::print("{{\n");
    std::print("  \"success\": {},\n", success ? "true" : "false");
    std::print("  \"diagnostics\": [\n");
    for (std::size_t i = 0; i < diags.size(); ++i) {
        const auto &d = diags[i];
        std::print("    {{");
        std::print("\"file\":\"{}\",", JsonEscape(d.sourceName));
        std::print("\"line\":{},", d.location.line);
        std::print("\"column\":{},", d.location.column);
        std::print("\"severity\":\"{}\",", JsonEscape(SeverityName(d.severity)));
        std::print("\"message\":\"{}\"", JsonEscape(d.message));
        std::print("}}{}\n", (i + 1 < diags.size()) ? "," : "");
    }
    std::print("  ]\n");
    std::print("}}\n");
}

} // namespace Rux
