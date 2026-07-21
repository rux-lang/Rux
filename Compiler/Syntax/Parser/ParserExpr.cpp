// Expression parsing (precedence climbing) and patterns.

#include "Syntax/Parser/Parser.h"

#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Rux {
// Expressions
ExprPtr Parser::ParseExpr() {
    return ParseAssign();
}

// right-associative: a = b = c  =>  a = (b = c)
ExprPtr Parser::ParseAssign() {
    auto left = ParseRange();
    if (!left) {
        return nullptr;
    }

    static constexpr TokenKind kAssignOps[] = {
        TokenKind::Assign,         TokenKind::PlusAssign,           TokenKind::MinusAssign,
        TokenKind::StarAssign,     TokenKind::SlashAssign,          TokenKind::PercentAssign,
        TokenKind::AmpAssign,      TokenKind::PipeAssign,           TokenKind::CaretAssign,
        TokenKind::LessLessAssign, TokenKind::GreaterGreaterAssign,
    };

    for (auto op : kAssignOps) {
        if (Check(op)) {
            const auto loc = CurrentLocation();
            Advance();
            auto right = ParseAssign(); // right-associative
            auto e = std::make_unique<AssignExpr>();
            e->location = loc;
            e->op = op;
            e->target = std::move(left);
            e->value = std::move(right);
            return e;
        }
    }
    return left;
}

ExprPtr Parser::ParseRange() {
    auto left = ParseTernary();
    if (!left) {
        return nullptr;
    }

    if (Check(TokenKind::DotDot) || Check(TokenKind::DotDotDot) || Check(TokenKind::DotDotEqual)) {
        // Leave bare `expr...` for ParseArgList to handle as a spread
        if (Peek().kind == TokenKind::DotDotDot) {
            const TokenKind next = Peek(1).kind;
            if (next == TokenKind::RightParen || next == TokenKind::Comma) {
                return left;
            }
        }
        const bool incl = Peek().kind == TokenKind::DotDotDot || Peek().kind == TokenKind::DotDotEqual;
        const auto loc = CurrentLocation();
        Advance();
        auto right = ParseTernary();
        auto e = std::make_unique<RangeExpr>();
        e->location = loc;
        e->inclusive = incl;
        e->lo = std::move(left);
        e->hi = std::move(right);
        return e;
    }
    return left;
}

ExprPtr Parser::ParseTernary() {
    auto cond = ParseOr();
    if (!cond) {
        return nullptr;
    }

    if (Match(TokenKind::Question)) {
        const auto loc = Previous().location;
        auto thenExpr = ParseOr();
        Expect(TokenKind::Colon, "expected ':' in ternary");
        auto elseExpr = ParseTernary(); // right-associative
        auto e = std::make_unique<TernaryExpr>();
        e->location = loc;
        e->condition = std::move(cond);
        e->thenExpr = std::move(thenExpr);
        e->elseExpr = std::move(elseExpr);
        return e;
    }
    return cond;
}

ExprPtr Parser::ParseOr() {
    auto left = ParseAnd();
    while (Check(TokenKind::PipePipe)) {
        const auto loc = CurrentLocation();
        const auto op = Advance().kind;
        auto right = ParseAnd();
        auto e = std::make_unique<BinaryExpr>();
        e->location = loc;
        e->op = op;
        e->left = std::move(left);
        e->right = std::move(right);
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::ParseAnd() {
    auto left = ParseBitOr();
    while (Check(TokenKind::AmpAmp)) {
        const auto loc = CurrentLocation();
        const auto op = Advance().kind;
        auto right = ParseBitOr();
        auto e = std::make_unique<BinaryExpr>();
        e->location = loc;
        e->op = op;
        e->left = std::move(left);
        e->right = std::move(right);
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::ParseBitOr() {
    auto left = ParseBitXor();
    while (Check(TokenKind::Pipe)) {
        const auto loc = CurrentLocation();
        const auto op = Advance().kind;
        auto right = ParseBitXor();
        auto e = std::make_unique<BinaryExpr>();
        e->location = loc;
        e->op = op;
        e->left = std::move(left);
        e->right = std::move(right);
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::ParseBitXor() {
    auto left = ParseBitAnd();
    while (Check(TokenKind::Caret)) {
        const auto loc = CurrentLocation();
        const auto op = Advance().kind;
        auto right = ParseBitAnd();
        auto e = std::make_unique<BinaryExpr>();
        e->location = loc;
        e->op = op;
        e->left = std::move(left);
        e->right = std::move(right);
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::ParseBitAnd() {
    auto left = ParseEquality();
    while (Check(TokenKind::Amp)) {
        const auto loc = CurrentLocation();
        const auto op = Advance().kind;
        auto right = ParseEquality();
        auto e = std::make_unique<BinaryExpr>();
        e->location = loc;
        e->op = op;
        e->left = std::move(left);
        e->right = std::move(right);
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::ParseEquality() {
    auto left = ParseComparison();
    while (CheckAny({TokenKind::Equal, TokenKind::BangEqual})) {
        const auto loc = CurrentLocation();
        const auto op = Advance().kind;
        auto right = ParseComparison();
        auto e = std::make_unique<BinaryExpr>();
        e->location = loc;
        e->op = op;
        e->left = std::move(left);
        e->right = std::move(right);
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::ParseComparison() {
    auto left = ParseShift();
    while (CheckAny({TokenKind::Less, TokenKind::LessEqual, TokenKind::Greater, TokenKind::GreaterEqual})) {
        const auto loc = CurrentLocation();
        const auto op = Advance().kind;
        auto right = ParseShift();
        auto e = std::make_unique<BinaryExpr>();
        e->location = loc;
        e->op = op;
        e->left = std::move(left);
        e->right = std::move(right);
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::ParseCast() {
    auto left = ParsePostfix();
    while (CheckAny({TokenKind::AsKeyword, TokenKind::IsKeyword})) {
        const auto loc = CurrentLocation();
        if (Match(TokenKind::AsKeyword)) {
            auto type = ParseType();
            auto e = std::make_unique<CastExpr>();
            e->location = loc;
            e->operand = std::move(left);
            e->type = std::move(type);
            left = std::move(e);
        }
        else {
            Match(TokenKind::IsKeyword);
            auto type = ParseType();
            auto e = std::make_unique<IsExpr>();
            e->location = loc;
            e->operand = std::move(left);
            e->type = std::move(type);
            left = std::move(e);
        }
    }
    return left;
}

ExprPtr Parser::ParseShift() {
    auto left = ParseAdd();
    while (CheckAny({TokenKind::LessLess, TokenKind::GreaterGreater})) {
        const auto loc = CurrentLocation();
        const auto op = Advance().kind;
        auto right = ParseAdd();
        auto e = std::make_unique<BinaryExpr>();
        e->location = loc;
        e->op = op;
        e->left = std::move(left);
        e->right = std::move(right);
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::ParseAdd() {
    auto left = ParseMul();
    while (CheckAny({TokenKind::Plus, TokenKind::Minus})) {
        const auto loc = CurrentLocation();
        const auto op = Advance().kind;
        auto right = ParseMul();
        auto e = std::make_unique<BinaryExpr>();
        e->location = loc;
        e->op = op;
        e->left = std::move(left);
        e->right = std::move(right);
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::ParseMul() {
    auto left = ParseExp();
    while (CheckAny({TokenKind::Star, TokenKind::Slash, TokenKind::Percent})) {
        const auto loc = CurrentLocation();
        const auto op = Advance().kind;
        auto right = ParseExp();
        auto e = std::make_unique<BinaryExpr>();
        e->location = loc;
        e->op = op;
        e->left = std::move(left);
        e->right = std::move(right);
        left = std::move(e);
    }
    return left;
}

// ** is right-associative (exponentiation)
ExprPtr Parser::ParseExp() {
    auto left = ParseUnary();

    if (Check(TokenKind::Star) && Peek(1).kind == TokenKind::Star) {
        const auto loc = CurrentLocation();

        Advance(); // first *
        Advance(); // second *

        auto right = ParseExp(); // right-associative

        auto e = std::make_unique<BinaryExpr>();
        e->location = loc;
        e->op = TokenKind::StarStar; // keep AST/LIR compatibility
        e->left = std::move(left);
        e->right = std::move(right);

        return e;
    }

    return left;
}

ExprPtr Parser::ParseUnary() {
    // '@' is address-of. It is unambiguous against the '@[Attr]' attribute
    // sigil: attributes are only parsed in declaration position, and there
    // the '@' is always followed by '['.
    if (CheckAny({TokenKind::Bang, TokenKind::Minus, TokenKind::Tilde, TokenKind::Star, TokenKind::At,
                  TokenKind::PlusPlus, TokenKind::MinusMinus})) {
        const auto loc = CurrentLocation();
        const auto op = Advance().kind;
        auto operand = ParseUnary();
        auto e = std::make_unique<UnaryExpr>();
        e->location = loc;
        e->op = op;
        e->operand = std::move(operand);
        return e;
    }
    return ParseCast();
}

ExprPtr Parser::ParsePostfix() {
    auto left = ParsePrimary();
    if (!left) {
        return nullptr;
    }
    while (true) {
        const auto loc = CurrentLocation();
        // Method/field/tuple-index: expr.field  expr.method(args)  expr.0
        if (Match(TokenKind::Dot)) {
            std::string name;
            if (Check(TokenKind::IntLiteral)) {
                name = Advance().text;
            }
            else if (Check(TokenKind::ModuleKeyword)) {
                name = Advance().text;
            }
            else {
                name = Expect(TokenKind::Ident, "expected field name or tuple index").text;
            }

            if (Check(TokenKind::LeftParen) && !name.empty() && !std::isdigit(name[0])) {
                // Method call: expr.method(args)
                auto args = ParseArgList();
                // Desugar to CallExpr with FieldExpr callee
                auto field = std::make_unique<FieldExpr>();
                field->location = loc;
                field->object = std::move(left);
                field->field = name;
                auto call = std::make_unique<CallExpr>();
                call->location = loc;
                call->callee = std::move(field);
                call->args = std::move(args);
                left = std::move(call);
            }
            else {
                auto e = std::make_unique<FieldExpr>();
                e->location = loc;
                e->object = std::move(left);
                e->field = name;
                left = std::move(e);
            }
            continue;
        }
        // Qualified path: expr::member
        if (Match(TokenKind::ColonColon)) {
            const std::string seg = Expect(TokenKind::Ident, "expected identifier").text;
            // Build or extend a PathExpr
            if (auto *path = dynamic_cast<PathExpr *>(left.get())) {
                path->segments.push_back(seg);
            }
            else {
                // Wrap existing expression in a path — treat the left side
                // as a segment This handles IDENT::IDENT::... chains
                auto p = std::make_unique<PathExpr>();
                p->location = loc;
                if (auto *ident = dynamic_cast<IdentExpr *>(left.get())) {
                    p->segments.push_back(ident->name);
                }
                p->segments.push_back(seg);
                left = std::move(p);
            }
            continue;
        }
        // Qualified initializer: Enum::Variant { field: value, ... }
        if (structInitAllowed && Check(TokenKind::LeftBrace)) {
            if (const auto *path = dynamic_cast<const PathExpr *>(left.get())) {
                auto e = std::make_unique<StructInitExpr>();
                e->location = loc;
                for (std::size_t i = 0; i < path->segments.size(); ++i) {
                    if (i) {
                        e->typeName += "::";
                    }
                    e->typeName += path->segments[i];
                }
                Advance(); // consume '{'
                while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
                    StructInitExpr::Field field;
                    field.location = CurrentLocation();
                    field.name = Expect(TokenKind::Ident, "expected field name").text;
                    Expect(TokenKind::Colon, "expected ':'");
                    field.value = ParseExpr();
                    e->fields.push_back(std::move(field));
                    if (!Match(TokenKind::Comma)) {
                        break;
                    }
                }
                Expect(TokenKind::RightBrace, "expected '}'");
                left = std::move(e);
                continue;
            }
        }
        // Generic function call: expr<T1, T2>(args)
        if (IsGenericCallAhead()) {
            auto typeArgs = ParseTypeArgs();
            auto args = ParseArgList();
            auto e = std::make_unique<CallExpr>();
            e->location = loc;
            e->callee = std::move(left);
            e->typeArgs = std::move(typeArgs);
            e->args = std::move(args);
            left = std::move(e);
            continue;
        }
        // Function/direct call: expr(args)
        if (Check(TokenKind::LeftParen)) {
            auto args = ParseArgList();
            auto e = std::make_unique<CallExpr>();
            e->location = loc;
            e->callee = std::move(left);
            e->args = std::move(args);
            left = std::move(e);
            continue;
        }
        // Index: expr[idx]
        if (Match(TokenKind::LeftBracket)) {
            auto idx = ParseExpr();
            Expect(TokenKind::RightBracket, "expected ']'");
            auto e = std::make_unique<IndexExpr>();
            e->location = loc;
            e->object = std::move(left);
            e->index = std::move(idx);
            left = std::move(e);
            continue;
        }
        // Post-increment / post-decrement: expr++ or expr--
        if (Check(TokenKind::PlusPlus) || Check(TokenKind::MinusMinus)) {
            const TokenKind op = Advance().kind;
            auto e = std::make_unique<PostfixExpr>();
            e->location = loc;
            e->op = op;
            e->operand = std::move(left);
            left = std::move(e);
            continue;
        }

        break;
    }
    return left;
}

ExprPtr Parser::ParsePrimary() {
    const auto loc = CurrentLocation();
    if (Match(TokenKind::MatchKeyword)) {
        auto e = std::make_unique<MatchExpr>();
        e->location = loc;
        structInitAllowed = false;
        e->subject = ParseExpr();
        structInitAllowed = true;

        Expect(TokenKind::LeftBrace, "expected '{'");
        while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
            MatchExpr::Arm arm;
            arm.location = CurrentLocation();
            arm.pattern = ParsePattern();
            Expect(TokenKind::FatArrow, "expected '=>'");

            if (Check(TokenKind::LeftBrace)) {
                auto bexpr = std::make_unique<BlockExpr>();
                bexpr->location = CurrentLocation();
                bexpr->block = ParseBlock();
                arm.body = std::move(bexpr);
            }
            else {
                arm.body = ParseExpr();
            }

            e->arms.push_back(std::move(arm));
            if (Match(TokenKind::Comma)) {
                if (Check(TokenKind::RightBrace)) {
                    EmitError(Previous().location, "trailing comma is not allowed in match blocks");
                }
            }
            else {
                break;
            }
        }
        Expect(TokenKind::RightBrace, "expected '}'");
        return e;
    }

    // Literals
    if (Check(TokenKind::IntLiteral) || Check(TokenKind::FloatLiteral) || Check(TokenKind::StringLiteral) ||
        Check(TokenKind::CharLiteral) || Check(TokenKind::BoolLiteral)) {
        auto e = std::make_unique<LiteralExpr>();
        e->location = loc;
        e->token = Advance();
        return e;
    }
    // null literal
    if (Match(TokenKind::NullKeyword)) {
        auto e = std::make_unique<LiteralExpr>();
        e->location = loc;
        e->token = Previous();
        return e;
    }
    // self
    if (Match(TokenKind::SelfKeyword)) {
        auto e = std::make_unique<SelfExpr>();
        e->location = loc;
        return e;
    }
    // Compile-time size query: sizeof(T)
    if (Check(TokenKind::Ident) && Peek().text == "sizeof") {
        Advance();
        auto e = std::make_unique<SizeOfExpr>();
        e->location = loc;
        Expect(TokenKind::LeftParen, "expected '(' after 'sizeof'");
        e->type = ParseType();
        Expect(TokenKind::RightParen, "expected ')' after sizeof type");
        return e;
    }
    // Compiler-injected intrinsic value: #target, #build, #source, ... The
    // leading '#' is part of the name, so it resolves like any other symbol
    // once the intrinsic has been imported.
    if (Check(TokenKind::Hash) && Peek(1).Is(TokenKind::Ident)) {
        Advance(); // consume '#'
        auto e = std::make_unique<IdentExpr>();
        e->location = loc;
        e->name = "#" + Advance().text;
        return e;
    }
    // Enum variant without its type: .Windows
    if (Check(TokenKind::Dot) && Peek(1).Is(TokenKind::Ident)) {
        Advance(); // consume '.'
        auto e = std::make_unique<EnumShorthandExpr>();
        e->location = loc;
        e->variant = Advance().text;
        return e;
    }
    // Slice literal: [a, b, c]
    if (Match(TokenKind::LeftBracket)) {
        auto e = std::make_unique<SliceExpr>();
        e->location = loc;
        while (!Check(TokenKind::RightBracket) && !IsAtEnd()) {
            e->elements.push_back(ParseExpr());
            if (!Match(TokenKind::Comma)) {
                break;
            }
        }
        Expect(TokenKind::RightBracket, "expected ']'");
        return e;
    }
    // Grouped expression or tuple: (expr)  or  (expr, expr, ...)
    if (Match(TokenKind::LeftParen)) {
        auto first = ParseExpr();
        if (Match(TokenKind::Comma)) {
            auto t = std::make_unique<TupleExpr>();
            t->location = loc;
            t->elements.push_back(std::move(first));
            while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
                t->elements.push_back(ParseExpr());
                if (!Match(TokenKind::Comma)) {
                    break;
                }
            }
            Expect(TokenKind::RightParen, "expected ')'");
            return t;
        }
        Expect(TokenKind::RightParen, "expected ')'");
        return first;
    }
    // Identifier, possible struct init, or path expression
    if (Check(TokenKind::Ident)) {
        const std::string name = Advance().text;
        std::vector<TypeExprPtr> typeArgs;
        if (IsGenericStructInitAhead()) {
            typeArgs = ParseTypeArgs();
        }
        // Struct initialization: Name { field: value, ... }
        // Disabled in control-flow condition contexts to avoid ambiguity.
        if (structInitAllowed && Check(TokenKind::LeftBrace)) {
            auto e = std::make_unique<StructInitExpr>();
            e->location = loc;
            e->typeName = name;
            e->typeArgs = std::move(typeArgs);
            Advance(); // consume '{'
            while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
                StructInitExpr::Field field;
                field.location = CurrentLocation();
                field.name = Check(TokenKind::ModuleKeyword) ? Advance().text
                                                             : Expect(TokenKind::Ident, "expected field name").text;
                Expect(TokenKind::Colon, "expected ':'");
                field.value = ParseExpr();
                e->fields.push_back(std::move(field));
                if (!Match(TokenKind::Comma)) {
                    break;
                }
            }
            Expect(TokenKind::RightBrace, "expected '}'");
            return e;
        }
        auto e = std::make_unique<IdentExpr>();
        e->location = loc;
        e->name = name;
        return e;
    }
    EmitError(loc, std::format("unexpected token '{}' in expression", Peek().text));
    return nullptr;
}

std::vector<ExprPtr> Parser::ParseArgList() {
    std::vector<ExprPtr> args;
    Expect(TokenKind::LeftParen, "expected '('");
    while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
        auto e = ParseExpr();
        if (Match(TokenKind::DotDotDot)) {
            const auto loc = e->location;
            auto spread = std::make_unique<SpreadExpr>();
            spread->location = loc;
            spread->operand = std::move(e);
            args.push_back(std::move(spread));
        }
        else {
            args.push_back(std::move(e));
        }
        if (!Match(TokenKind::Comma)) {
            break;
        }
    }
    Expect(TokenKind::RightParen, "expected ')'");
    return args;
}

// Patterns
PatternPtr Parser::ParsePattern() {
    auto inner = ParsePrimaryPattern();
    if (!inner) {
        return nullptr;
    }

    // Guard: pattern if condition
    if (Match(TokenKind::IfKeyword)) {
        const auto loc = Previous().location;
        auto guard = ParseExpr();
        auto p = std::make_unique<GuardedPattern>();
        p->location = loc;
        p->inner = std::move(inner);
        p->guard = std::move(guard);
        return p;
    }

    // Range pattern: lo..hi or lo...hi or lo..=hi
    if (Check(TokenKind::DotDot) || Check(TokenKind::DotDotDot) || Check(TokenKind::DotDotEqual)) {
        const bool incl = Peek().kind == TokenKind::DotDotDot || Peek().kind == TokenKind::DotDotEqual;
        const auto loc = CurrentLocation();
        Advance();
        auto hi = ParsePrimaryPattern();
        auto p = std::make_unique<RangePattern>();
        p->location = loc;
        p->inclusive = incl;
        p->lo = std::move(inner);
        p->hi = std::move(hi);
        return p;
    }

    return inner;
}

PatternPtr Parser::ParsePrimaryPattern() {
    const auto loc = CurrentLocation();

    // Wildcard: _
    if (Check(TokenKind::Ident) && Peek().text == "_") {
        Advance();
        auto p = std::make_unique<WildcardPattern>();
        p->location = loc;
        return p;
    }

    // Literals
    if (Check(TokenKind::IntLiteral) || Check(TokenKind::FloatLiteral) || Check(TokenKind::StringLiteral) ||
        Check(TokenKind::CharLiteral) || Check(TokenKind::BoolLiteral) || Check(TokenKind::NullKeyword)) {
        auto p = std::make_unique<LiteralPattern>();
        p->location = loc;
        p->value = Advance();
        return p;
    }

    // Negative literal: -42
    if (Check(TokenKind::Minus) && Peek(1).Is(TokenKind::IntLiteral)) {
        Advance(); // consume '-'
        auto p = std::make_unique<LiteralPattern>();
        p->location = loc;
        p->value = Advance();
        // The negative sign is implicit; store it in text for diagnostics.
        p->value.text = "-" + p->value.text;
        return p;
    }

    // Tuple pattern: (a, b, ...)
    if (Match(TokenKind::LeftParen)) {
        auto p = std::make_unique<TuplePattern>();
        p->location = loc;
        while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
            p->elements.push_back(ParsePattern());
            if (!Match(TokenKind::Comma)) {
                break;
            }
        }
        Expect(TokenKind::RightParen, "expected ')'");
        return p;
    }

    // Identifier-started patterns: ident, EnumName::Variant(args), TypeName
    // { fields }
    if (Check(TokenKind::Ident)) {
        const std::string name = Advance().text;

        // Enum pattern: Event::Click(x, y)
        if (Check(TokenKind::ColonColon) && Peek(1).Is(TokenKind::Ident)) {
            std::vector<std::string> path = {name};
            while (Match(TokenKind::ColonColon)) {
                path.push_back(Expect(TokenKind::Ident, "expected variant name").text);
            }
            auto p = std::make_unique<EnumPattern>();
            p->location = loc;
            p->path = std::move(path);
            if (Match(TokenKind::LeftParen)) {
                while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
                    p->args.push_back(ParsePattern());
                    if (!Match(TokenKind::Comma)) {
                        break;
                    }
                }
                Expect(TokenKind::RightParen, "expected ')'");
            }
            else if (Match(TokenKind::LeftBrace)) {
                while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
                    EnumPattern::NamedArg arg;
                    arg.location = CurrentLocation();
                    arg.name = Expect(TokenKind::Ident, "expected variant field name").text;
                    if (Match(TokenKind::Colon)) {
                        arg.pattern = ParsePattern();
                    }
                    else {
                        auto binding = std::make_unique<IdentPattern>();
                        binding->location = arg.location;
                        binding->name = arg.name;
                        arg.pattern = std::move(binding);
                    }
                    p->namedArgs.push_back(std::move(arg));
                    if (!Match(TokenKind::Comma)) {
                        break;
                    }
                }
                Expect(TokenKind::RightBrace, "expected '}'");
            }
            return p;
        }

        // Struct pattern: TypeName { field: pat, ... }
        if (Check(TokenKind::LeftBrace)) {
            Advance(); // consume '{'
            auto p = std::make_unique<StructPattern>();
            p->location = loc;
            p->typeName = name;
            while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
                StructPattern::Field f;
                f.location = CurrentLocation();
                f.name = Expect(TokenKind::Ident, "expected field name").text;
                Expect(TokenKind::Colon, "expected ':'");
                f.pattern = ParsePattern();
                p->fields.push_back(std::move(f));
                if (!Match(TokenKind::Comma)) {
                    break;
                }
            }
            Expect(TokenKind::RightBrace, "expected '}'");
            return p;
        }

        // Simple identifier binding
        auto p = std::make_unique<IdentPattern>();
        p->location = loc;
        p->name = name;
        return p;
    }

    EmitError(loc, std::format("expected a pattern, got '{}'", Peek().text));
    return nullptr;
}
} // namespace Rux
