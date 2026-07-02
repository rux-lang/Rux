// Human-readable semantic-analysis dump (Sema::DumpResult).

#include "Frontend/Sema/Sema.h"

#include <format>
#include <fstream>
#include <string>
#include <string_view>

namespace Rux {

bool Sema::DumpResult(const SemaResult &result, const std::filesystem::path &path) {
    std::ofstream out(path);
    if (!out) {
        return false;
    }

    static constexpr auto kindName = [](SemaSymbol::Kind k) -> std::string_view {
        switch (k) {
        case SemaSymbol::Kind::Var:
            return "var";
        case SemaSymbol::Kind::Func:
            return "func";
        case SemaSymbol::Kind::Type:
            return "type";
        case SemaSymbol::Kind::Const:
            return "const";
        case SemaSymbol::Kind::Module:
            return "module";
        case SemaSymbol::Kind::Interface:
            return "interface";
        }
        return "?";
    };

    out << "=== Semantic Analysis Results ===\n\n";

    // Symbols
    out << std::format("Symbols ({} total)\n", result.symbols.size());
    out << std::string(40, '-') << '\n';

    if (result.symbols.empty()) {
        out << "(none)\n";
    }
    else {
        for (const auto &sym : result.symbols) {
            std::string tag = std::format("{:<10}", kindName(sym.kind));
            std::string qname = sym.name;
            if (sym.isMut) {
                qname += " (var)";
            }
            std::string typeStr = sym.resolvedType.empty() ? "" : "  " + sym.resolvedType;
            out << std::format("{}  {:<28}{}  [{}:{}:{}]\n", tag, qname, typeStr, sym.sourceName, sym.location.line,
                               sym.location.column);
        }
    }

    out << '\n';

    // Diagnostics
    out << std::format("Diagnostics ({} total)\n", result.diagnostics.size());
    out << std::string(40, '-') << '\n';

    if (result.diagnostics.empty()) {
        out << "(none)\n";
    }
    else {
        for (const auto &diag : result.diagnostics) {
            const char *sev = diag.severity == SemaDiagnostic::Severity::Error ? "error" : "warning";
            out << std::format("{}:{}:{}: {}: {}\n", diag.sourceName, diag.location.line, diag.location.column, sev,
                               diag.message);
        }
    }

    return out.good();
}

} // namespace Rux
