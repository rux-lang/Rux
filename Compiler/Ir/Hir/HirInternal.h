#pragma once

// Shared between HIR lowering (Hir.cpp) and the HIR text dump (HirDump.cpp).

#include "Lexer/Token.h"

#include <string_view>

namespace Rux {
// Source spelling of a binary/unary operator token, e.g. TokenKind::Plus -> "+".
[[nodiscard]] std::string_view OpStr(TokenKind op);
} // namespace Rux
