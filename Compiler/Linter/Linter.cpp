#include "Linter/Linter.h"

#include <algorithm>
#include <iterator>

#include "Lexer/Lexer.h"
#include "Syntax/Parser/Parser.h"

namespace Rux::Linting {

bool LintResult::HasErrors() const noexcept {
    return std::ranges::any_of(diagnostics, [](const Diagnostic &diagnostic) { return diagnostic.IsError(); });
}

LintResult Lint(std::string source, std::string sourceName) {
    Lexer lexer(std::move(source), sourceName);
    auto lexed = lexer.Tokenize();
    LintResult result{std::move(lexed.diagnostics)};
    if (result.HasErrors()) {
        return result;
    }

    Parser parser(std::move(lexed.tokens), std::move(sourceName));
    auto parsed = parser.Parse();
    result.diagnostics.insert(result.diagnostics.end(), std::make_move_iterator(parsed.diagnostics.begin()),
                              std::make_move_iterator(parsed.diagnostics.end()));
    return result;
}

} // namespace Rux::Linting
