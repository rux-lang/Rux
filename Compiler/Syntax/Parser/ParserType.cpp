// Type-expression parsing.

#include "Syntax/Parser/Parser.h"

#include <format>
#include <memory>
#include <vector>

namespace Rux {
// Type expressions
TypeExprPtr Parser::ParseType() {
    const auto loc = CurrentLocation();
    TypeExprPtr base;

    // Function type: func(params) -> T
    if (Check(TokenKind::FuncKeyword)) {
        base = ParseFunctionType();
    }
    // Pointer: *T  or  *mut T
    else if (Match(TokenKind::Star)) {
        const bool pointeeMut = Match(TokenKind::MutKeyword); // optional 'mut' qualifier
        auto p = std::make_unique<PointerTypeExpr>();
        p->location = loc;
        p->pointeeMut = pointeeMut;
        p->pointee = ParseType();
        base = std::move(p);
    }
    // Grouped type or tuple: (T) or (T, U, ...)
    else if (Check(TokenKind::LeftParen)) {
        Advance();
        if (Check(TokenKind::RightParen)) {
            auto tuple = std::make_unique<TupleTypeExpr>();
            tuple->location = loc;
            base = std::move(tuple);
        }
        else {
            auto first = ParseType();
            if (Match(TokenKind::Comma)) {
                auto tuple = std::make_unique<TupleTypeExpr>();
                tuple->location = loc;
                tuple->elements.push_back(std::move(first));
                while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
                    tuple->elements.push_back(ParseType());
                    if (!Match(TokenKind::Comma)) {
                        break;
                    }
                }
                base = std::move(tuple);
            }
            else {
                base = std::move(first);
            }
        }
        Expect(TokenKind::RightParen, "expected ')'");
    }
    else {
        base = ParseBaseType();
    }
    if (!base) {
        return nullptr;
    }

    // Postfix inline-array suffix: T[] (flexible tail) or T[N] (fixed)
    while (Check(TokenKind::LeftBracket)) {
        Advance();
        auto a = std::make_unique<ArrayTypeExpr>();
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

// func(x: int, y: int) -> bool
// Parameter names are optional; only the types and the return type matter.
TypeExprPtr Parser::ParseFunctionType() {
    const auto loc = CurrentLocation();
    Expect(TokenKind::FuncKeyword, "expected 'func'");

    auto t = std::make_unique<FunctionTypeExpr>();
    t->location = loc;

    Expect(TokenKind::LeftParen, "expected '(' after 'func'");
    while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
        if (Match(TokenKind::DotDotDot)) {
            t->isVariadic = true;
            break;
        }
        // Optional parameter name for readability: `name: Type`.
        if (Check(TokenKind::Ident) && Peek(1).kind == TokenKind::Colon) {
            Advance(); // name
            Advance(); // ':'
        }
        t->params.push_back(ParseType());
        if (!Match(TokenKind::Comma)) {
            break;
        }
    }
    Expect(TokenKind::RightParen, "expected ')'");

    // An omitted return arrow means the function yields no value.
    if (Match(TokenKind::Arrow)) {
        t->returnType = ParseType();
    }

    return t;
}
} // namespace Rux
