// Type-expression parsing.

#include <memory>
#include <vector>

#include "Syntax/Parser/Parser.h"

namespace Rux {
// Type expressions
TypeExprPtr Parser::ParseType() {
    const auto loc = CurrentLocation();

    // Pointer: *T  or  *const T
    if (Match(TokenKind::Star)) {
        Match(TokenKind::ConstKeyword); // consume optional 'const' qualifier
        auto p = std::make_unique<PointerTypeExpr>();
        p->location = loc;
        p->pointee = ParseType();
        return p;
    }

    // Tuple: (T, U, ...)
    if (Check(TokenKind::LeftParen)) {
        Advance();
        auto t = std::make_unique<TupleTypeExpr>();
        t->location = loc;
        while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
            t->elements.push_back(ParseType());
            if (!Match(TokenKind::Comma)) {
                break;
            }
        }
        Expect(TokenKind::RightParen, "expected ')'");
        return t;
    }

    TypeExprPtr base = ParseBaseType();
    if (!base) {
        return nullptr;
    }

    // Postfix slice suffix: T[]  or  T[N]
    while (Check(TokenKind::LeftBracket)) {
        Advance();
        auto a = std::make_unique<SliceTypeExpr>();
        a->location = loc;
        a->element = std::move(base);
        if (!Check(TokenKind::RightBracket)) {
            a->size = ParseExpr();
        }
        Expect(TokenKind::RightBracket, "expected ']'");
        base = std::move(a);
    }

    return base;
}

TypeExprPtr Parser::ParseBaseType() {
    const auto loc = CurrentLocation();

    if (Match(TokenKind::SelfKeyword)) {
        auto t = std::make_unique<SelfTypeExpr>();
        t->location = loc;
        return t;
    }

    if (Check(TokenKind::Ident)) {
        const std::string first = Advance().text;

        // Check for path type: A::B::C
        if (Check(TokenKind::ColonColon)) {
            auto p = std::make_unique<PathTypeExpr>();
            p->location = loc;
            p->segments.push_back(first);
            while (Match(TokenKind::ColonColon)) {
                p->segments.push_back(Expect(TokenKind::Ident, "expected type name").text);
            }
            return p;
        }

        auto n = std::make_unique<NamedTypeExpr>();
        n->location = loc;
        n->name = first;
        if (IsTypeArgListAhead()) {
            n->typeArgs = ParseTypeArgs();
        }
        return n;
    }

    EmitError(loc, std::format("expected a type, got '{}'", Peek().text));
    return nullptr;
}
} // namespace Rux
