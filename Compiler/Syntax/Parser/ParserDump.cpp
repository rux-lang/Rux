// Human-readable AST dump (Parser::DumpAst).

#include "Syntax/Parser/Detail/AstDumpWriter.h"
#include "Syntax/Parser/Parser.h"

#include <format>
#include <fstream>
#include <ostream>

namespace Rux::ParserDumpDetail {
AstDumpWriter::AstDumpWriter(std::ostream &output)
    : out(output) {
}

void AstDumpWriter::Pad() const {
    for (int index = 0; index < indent; ++index) {
        out << "  ";
    }
}
} // namespace Rux::ParserDumpDetail

namespace Rux {
namespace {
class AstPrinter final : private ParserDumpDetail::AstDumpWriter {
public:
    explicit AstPrinter(std::ostream &output)
        : AstDumpWriter(output)
        , declarations(
              *this, [this](const Expr &expression) { expressions.Print(expression); },
              [this](const Block &block) { statements.PrintBlock(block); })
        , statements(
              *this, [this](const Expr &expression) { expressions.Print(expression); },
              [this](const Decl &declaration) { declarations.Print(declaration); })
        , expressions(
              *this, [this](const Block &block) { statements.PrintBlock(block); },
              [this](const Pattern &pattern) { statements.PrintPattern(pattern); }) {
    }

    void Print(const Module &module) {
        out << "Module \"" << module.name << "\"\n";
        ++indent;
        for (const auto &item : module.items) {
            if (item) {
                declarations.Print(*item);
            }
        }
        --indent;
    }

private:
    ParserDumpDetail::DeclarationPrinter declarations;
    ParserDumpDetail::StatementPrinter statements;
    ParserDumpDetail::ExpressionPrinter expressions;
};
} // namespace

bool Parser::DumpAst(const ParseResult &result, const std::filesystem::path &path) {
    std::ofstream file(path);
    if (!file) {
        return false;
    }
    AstPrinter printer(file);
    printer.Print(result.module);
    if (!result.diagnostics.empty()) {
        file << "\n--- diagnostics ---\n";
        for (const auto &diagnostic : result.diagnostics) {
            file << std::format("{:>4}:{:<4}  {}  {}\n", diagnostic.location.line, diagnostic.location.column,
                                diagnostic.severity == ParserDiagnostic::Severity::Error ? "error  " : "warning",
                                diagnostic.message);
        }
    }
    return file.good();
}
} // namespace Rux
