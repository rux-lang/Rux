#include "Driver/BuildStats.h"

#include "Lexer/Token.h"

namespace Rux::Driver {
std::size_t CountLines(std::string_view source) {
    if (source.empty()) {
        return 0;
    }

    std::size_t lines = 0;
    for (const char ch : source) {
        if (ch == '\n') {
            ++lines;
        }
    }
    if (source.back() != '\n') {
        ++lines;
    }
    return lines;
}

std::size_t CountTokens(std::span<const Token> tokens) {
    if (tokens.empty()) {
        return 0;
    }
    return tokens.back().IsEof() ? tokens.size() - 1 : tokens.size();
}

} // namespace Rux::Driver
