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
    return ParseRequiredExpr();
}

ExprPtr Parser::ParseExprImpl() {
    return ParseAssign();
}

ExprPtr Parser::ParseRequiredExpr(const std::string_view context) {
    auto expression = ParseExprImpl();
    if (!expression) {
        EmitMissingExpression(context);
    }
    return expression;
}

void Parser::EmitMissingExpression(const std::string_view context) {
    if (context.empty()) {
        EmitExpected(CurrentLocation(), "an expression");
    }
    else {
        EmitExpected(CurrentLocation(), std::format("an expression {}", context));
    }
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
        TokenKind::LessLessAssign, TokenKind::GreaterGreaterAssign, TokenKind::GreaterGreaterGreaterAssign,
    };

    for (auto op : kAssignOps) {
        if (Check(op)) {
            const auto loc = CurrentLocation();
            const std::string opText = Advance().text;
            auto right = ParseAssign(); // right-associative
            if (!right) {
                EmitMissingExpression(std::format("after '{}'", opText));
            }
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
    const auto parseUpperBound = [&]() -> ExprPtr {
        if (Check(TokenKind::RightBracket) || Check(TokenKind::RightParen) || Check(TokenKind::LeftBrace) ||
            Check(TokenKind::RightBrace) || Check(TokenKind::Comma) || Check(TokenKind::Semicolon) || IsAtEnd()) {
            return nullptr;
        }
        return ParseTernary();
    };

    // Prefix and full ranges: `..end`, `..=end`, and `..`.
    if (Check(TokenKind::DotDot) || Check(TokenKind::DotDotDot) || Check(TokenKind::DotDotEqual)) {
        const auto loc = CurrentLocation();
        const bool incl = Peek().kind == TokenKind::DotDotDot || Peek().kind == TokenKind::DotDotEqual;
        Advance();
        auto right = parseUpperBound();
        if (incl && !right) {
            EmitError(loc, "inclusive range requires an end bound");
        }
        auto e = std::make_unique<RangeExpr>();
        e->location = loc;
        e->inclusive = incl;
        e->hi = std::move(right);
        return e;
    }

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
        auto right = parseUpperBound();
        if (incl && !right) {
            EmitError(loc, "inclusive range requires an end bound");
        }
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
        if (!thenExpr) {
            EmitMissingExpression("after '?' in the conditional expression");
        }
        ExpectBefore(TokenKind::Colon, "':' between the conditional expression branches");
        auto elseExpr = ParseTernary(); // right-associative
        if (!elseExpr) {
            EmitMissingExpression("after ':' in the conditional expression");
        }
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
        const Token opToken = Advance();
        const auto op = opToken.kind;
        auto right = ParseAnd();
        if (!right) {
            EmitMissingExpression(std::format("after '{}'", opToken.text));
        }
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
        const Token opToken = Advance();
        const auto op = opToken.kind;
        auto right = ParseBitOr();
        if (!right) {
            EmitMissingExpression(std::format("after '{}'", opToken.text));
        }
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
        const Token opToken = Advance();
        const auto op = opToken.kind;
        auto right = ParseBitXor();
        if (!right) {
            EmitMissingExpression(std::format("after '{}'", opToken.text));
        }
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
        const Token opToken = Advance();
        const auto op = opToken.kind;
        auto right = ParseBitAnd();
        if (!right) {
            EmitMissingExpression(std::format("after '{}'", opToken.text));
        }
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
        const Token opToken = Advance();
        const auto op = opToken.kind;
        auto right = ParseEquality();
        if (!right) {
            EmitMissingExpression(std::format("after '{}'", opToken.text));
        }
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
        const Token opToken = Advance();
        const auto op = opToken.kind;
        auto right = ParseComparison();
        if (!right) {
            EmitMissingExpression(std::format("after '{}'", opToken.text));
        }
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
        const Token opToken = Advance();
        const auto op = opToken.kind;
        auto right = ParseShift();
        if (!right) {
            EmitMissingExpression(std::format("after '{}'", opToken.text));
        }
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
    auto left = ParseUnary();
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
    while (CheckAny({TokenKind::LessLess, TokenKind::GreaterGreater, TokenKind::GreaterGreaterGreater})) {
        const auto loc = CurrentLocation();
        const Token opToken = Advance();
        const auto op = opToken.kind;
        auto right = ParseAdd();
        if (!right) {
            EmitMissingExpression(std::format("after '{}'", opToken.text));
        }
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
        const Token opToken = Advance();
        const auto op = opToken.kind;
        auto right = ParseMul();
        if (!right) {
            EmitMissingExpression(std::format("after '{}'", opToken.text));
        }
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
        const Token opToken = Advance();
        const auto op = opToken.kind;
        auto right = ParseExp();
        if (!right) {
            EmitMissingExpression(std::format("after '{}'", opToken.text));
        }
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
    auto left = ParseCast();

    if (Check(TokenKind::Star) && Peek(1).kind == TokenKind::Star) {
        const auto loc = CurrentLocation();

        Advance(); // first *
        Advance(); // second *

        auto right = ParseExp(); // right-associative
        if (!right) {
            EmitMissingExpression("after '**'");
        }

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
        const Token opToken = Advance();
        const auto op = opToken.kind;
        auto operand = ParseUnary();
        if (!operand) {
            EmitMissingExpression(std::format("after unary '{}'", opToken.text));
        }
        auto e = std::make_unique<UnaryExpr>();
        e->location = loc;
        e->op = op;
        e->operand = std::move(operand);
        return e;
    }
    return ParsePostfix();
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
                name = ExpectBefore(TokenKind::Ident, "a field name or tuple index after '.'").text;
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
            const std::string seg = ExpectBefore(TokenKind::Ident, "a path segment after '::'").text;
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
                    field.name = ExpectBefore(TokenKind::Ident, "a field name in the initializer").text;
                    ExpectBefore(TokenKind::Colon, "':' after the initializer field name");
                    field.value = ParseRequiredExpr("after ':' in the initializer field");
                    const bool validField = !field.name.empty() && field.value != nullptr;
                    e->fields.push_back(std::move(field));
                    if (!validField) {
                        if (RecoverDelimitedList(TokenKind::RightBrace)) {
                            continue;
                        }
                        break;
                    }
                    if (Match(TokenKind::Comma)) {
                        continue;
                    }
                    if (!Check(TokenKind::RightBrace)) {
                        EmitExpected(CurrentLocation(), "',' between initializer fields");
                        continue;
                    }
                    else {
                        break;
                    }
                }
                ExpectBefore(TokenKind::RightBrace, "'}' to close the initializer");
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
            auto idx = ParseRequiredExpr("after '[' in the index expression");
            ExpectBefore(TokenKind::RightBracket, "']' to close the index expression");
            auto e = std::make_unique<IndexExpr>();
            e->location = loc;
            e->object = std::move(left);
            e->index = std::move(idx);
            left = std::move(e);
            continue;
        }
        // Failure propagation: expr?
        //
        // The same token opens a conditional expression, so the spelling decides which one this is: `Read()?` is a
        // propagation and `ready ? a : b` is a conditional. Only the tight form is postfix, so the conditional keeps
        // its own parse below at its own precedence.
        if (Check(TokenKind::Question) && !Peek().precededBySpace) {
            Advance();
            auto e = std::make_unique<TryExpr>();
            e->location = loc;
            e->operand = std::move(left);
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
        e->subject = ParseRequiredExpr("after 'match'");
        structInitAllowed = true;

        if (!Match(TokenKind::LeftBrace)) {
            EmitExpected(CurrentLocation(), "'{' to start the match expression arms");
            return e;
        }
        while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
            MatchExpr::Arm arm;
            arm.location = CurrentLocation();
            arm.pattern = ParseMatchArmPattern();
            if (!arm.pattern) {
                while (!CheckAny({TokenKind::FatArrow, TokenKind::Comma, TokenKind::RightBrace}) && !IsAtEnd()) {
                    Advance();
                }
            }
            ExpectBefore(TokenKind::FatArrow, "'=>' after the match arm pattern");

            if (Check(TokenKind::LeftBrace)) {
                auto bexpr = std::make_unique<BlockExpr>();
                bexpr->location = CurrentLocation();
                bexpr->block = ParseBlock("the match arm body");
                arm.body = std::move(bexpr);
            }
            else {
                arm.body = ParseRequiredExpr("after '=>' in the match arm");
            }

            e->arms.push_back(std::move(arm));
            if (Match(TokenKind::Comma)) {
                if (Check(TokenKind::RightBrace)) {
                    EmitError(Previous().location, "trailing comma is not allowed in match blocks");
                }
                continue;
            }
            if (!Check(TokenKind::RightBrace)) {
                EmitExpected(CurrentLocation(), "',' between match arms");
                while (!CheckAny({TokenKind::Comma, TokenKind::RightBrace}) && !IsAtEnd()) {
                    Advance();
                }
                Match(TokenKind::Comma);
            }
        }
        ExpectBefore(TokenKind::RightBrace, "'}' to close the match expression");
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
        ExpectBefore(TokenKind::LeftParen, "'(' after 'sizeof'");
        e->type = ParseType("add the queried type inside 'sizeof(...)'");
        ExpectBefore(TokenKind::RightParen, "')' after the 'sizeof' type");
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
        auto e = std::make_unique<ArrayExpr>();
        e->location = loc;
        while (!Check(TokenKind::RightBracket) && !IsAtEnd()) {
            auto element = ParseRequiredExpr(e->elements.empty() ? "after '[' in the slice literal"
                                                                 : "after ',' in the slice literal");
            const bool validElement = element != nullptr;
            e->elements.push_back(std::move(element));
            if (!validElement) {
                if (RecoverDelimitedList(TokenKind::RightBracket)) {
                    continue;
                }
                break;
            }
            if (Match(TokenKind::Comma)) {
                continue;
            }
            if (!Check(TokenKind::RightBracket)) {
                EmitExpected(CurrentLocation(), "',' between slice elements");
                continue;
            }
            else {
                break;
            }
        }
        ExpectBefore(TokenKind::RightBracket, "']' to close the slice literal");
        return e;
    }
    // Grouped expression or tuple: (expr)  or  (expr, expr, ...)
    if (Match(TokenKind::LeftParen)) {
        auto first = ParseRequiredExpr("after '('");
        if (Match(TokenKind::Comma)) {
            auto t = std::make_unique<TupleExpr>();
            t->location = loc;
            t->elements.push_back(std::move(first));
            while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
                auto element = ParseRequiredExpr("after ',' in the tuple expression");
                const bool validElement = element != nullptr;
                t->elements.push_back(std::move(element));
                if (!validElement) {
                    if (RecoverDelimitedList(TokenKind::RightParen)) {
                        continue;
                    }
                    break;
                }
                if (Match(TokenKind::Comma)) {
                    continue;
                }
                if (!Check(TokenKind::RightParen)) {
                    EmitExpected(CurrentLocation(), "',' between tuple elements");
                    continue;
                }
                else {
                    break;
                }
            }
            ExpectBefore(TokenKind::RightParen, "')' to close the tuple expression");
            return t;
        }
        ExpectBefore(TokenKind::RightParen, "')' to close the grouped expression");
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
                field.name = Check(TokenKind::ModuleKeyword)
                               ? Advance().text
                               : ExpectBefore(TokenKind::Ident, "a field name in the initializer").text;
                ExpectBefore(TokenKind::Colon, "':' after the initializer field name");
                field.value = ParseRequiredExpr("after ':' in the initializer field");
                const bool validField = !field.name.empty() && field.value != nullptr;
                e->fields.push_back(std::move(field));
                if (!validField) {
                    if (RecoverDelimitedList(TokenKind::RightBrace)) {
                        continue;
                    }
                    break;
                }
                if (Match(TokenKind::Comma)) {
                    continue;
                }
                if (!Check(TokenKind::RightBrace)) {
                    EmitExpected(CurrentLocation(), "',' between initializer fields");
                    continue;
                }
                else {
                    break;
                }
            }
            ExpectBefore(TokenKind::RightBrace, "'}' to close the initializer");
            return e;
        }
        auto e = std::make_unique<IdentExpr>();
        e->location = loc;
        e->name = name;
        return e;
    }
    return nullptr;
}

std::vector<ExprPtr> Parser::ParseArgList() {
    std::vector<ExprPtr> args;
    ExpectBefore(TokenKind::LeftParen, "'(' to start the argument list");
    while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
        auto e = ParseRequiredExpr(args.empty() ? "after '(' in the argument list" : "after ',' in the argument list");
        if (e && Match(TokenKind::DotDotDot)) {
            const auto loc = e->location;
            auto spread = std::make_unique<SpreadExpr>();
            spread->location = loc;
            spread->operand = std::move(e);
            args.push_back(std::move(spread));
        }
        else if (e) {
            args.push_back(std::move(e));
        }
        if (Match(TokenKind::Comma)) {
            continue;
        }
        if (!Check(TokenKind::RightParen)) {
            EmitExpected(CurrentLocation(), "',' between arguments");
            while (!CheckAny({TokenKind::Comma, TokenKind::RightParen, TokenKind::Semicolon, TokenKind::RightBrace}) &&
                   !IsAtEnd()) {
                Advance();
            }
            if (Match(TokenKind::Comma)) {
                continue;
            }
            break;
        }
        else {
            break;
        }
    }
    ExpectBefore(TokenKind::RightParen, "')' to close the argument list");
    return args;
}

// Patterns
PatternPtr Parser::ParseMatchArmPattern() {
    const auto loc = CurrentLocation();
    if (Match(TokenKind::ElseKeyword)) {
        auto pattern = std::make_unique<WildcardPattern>();
        pattern->location = loc;
        return pattern;
    }

    auto pattern = ParseRequiredPattern("at the start of the match arm");
    if (dynamic_cast<const WildcardPattern *>(pattern.get())) {
        EmitError(loc, "use 'else' for the default match arm");
    }
    return pattern;
}

PatternPtr Parser::ParsePattern() {
    return ParseRequiredPattern();
}

PatternPtr Parser::ParseRequiredPattern(const std::string_view context) {
    auto pattern = ParsePatternImpl();
    if (!pattern) {
        if (context.empty()) {
            EmitExpected(CurrentLocation(), "a pattern");
        }
        else {
            EmitExpected(CurrentLocation(), std::format("a pattern {}", context));
        }
    }
    return pattern;
}

PatternPtr Parser::ParsePatternImpl() {
    auto inner = ParsePrimaryPattern();
    if (!inner) {
        return nullptr;
    }

    // Guard: pattern if condition
    if (Match(TokenKind::IfKeyword)) {
        const auto loc = Previous().location;
        auto guard = ParseRequiredExpr("after 'if' in the pattern guard");
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
        const std::string operatorText = Advance().text;
        auto hi = ParsePrimaryPattern();
        if (!hi) {
            EmitExpected(CurrentLocation(), std::format("a range pattern end after '{}'", operatorText));
        }
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

    const auto parseEnumPatternSuffix = [this](std::unique_ptr<EnumPattern> pattern) -> PatternPtr {
        if (Match(TokenKind::LeftParen)) {
            while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
                auto argument = ParseRequiredPattern(pattern->args.empty() ? "after '(' in the variant pattern"
                                                                           : "after ',' in the variant pattern");
                const bool validArgument = argument != nullptr;
                pattern->args.push_back(std::move(argument));
                if (!validArgument) {
                    if (RecoverDelimitedList(TokenKind::RightParen)) {
                        continue;
                    }
                    break;
                }
                if (Match(TokenKind::Comma)) {
                    continue;
                }
                if (!Check(TokenKind::RightParen)) {
                    EmitExpected(CurrentLocation(), "',' between variant pattern elements");
                    continue;
                }
                else {
                    break;
                }
            }
            ExpectBefore(TokenKind::RightParen, "')' to close the variant pattern");
        }
        else if (Match(TokenKind::LeftBrace)) {
            while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
                EnumPattern::NamedArg arg;
                arg.location = CurrentLocation();
                arg.name = ExpectBefore(TokenKind::Ident, "a variant field name").text;
                if (Match(TokenKind::Colon)) {
                    arg.pattern = ParseRequiredPattern("after ':' in the variant field pattern");
                }
                else {
                    auto binding = std::make_unique<IdentPattern>();
                    binding->location = arg.location;
                    binding->name = arg.name;
                    arg.pattern = std::move(binding);
                }
                pattern->namedArgs.push_back(std::move(arg));
                if (pattern->namedArgs.back().name.empty() || pattern->namedArgs.back().pattern == nullptr) {
                    if (RecoverDelimitedList(TokenKind::RightBrace)) {
                        continue;
                    }
                    break;
                }
                if (Match(TokenKind::Comma)) {
                    continue;
                }
                if (!Check(TokenKind::RightBrace)) {
                    EmitExpected(CurrentLocation(), "',' between variant pattern fields");
                    continue;
                }
                else {
                    break;
                }
            }
            ExpectBefore(TokenKind::RightBrace, "'}' to close the variant pattern");
        }
        return pattern;
    };

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
            auto element = ParseRequiredPattern(p->elements.empty() ? "after '(' in the tuple pattern"
                                                                    : "after ',' in the tuple pattern");
            const bool validElement = element != nullptr;
            p->elements.push_back(std::move(element));
            if (!validElement) {
                if (RecoverDelimitedList(TokenKind::RightParen)) {
                    continue;
                }
                break;
            }
            if (Match(TokenKind::Comma)) {
                continue;
            }
            if (!Check(TokenKind::RightParen)) {
                EmitExpected(CurrentLocation(), "',' between tuple pattern elements");
                continue;
            }
            else {
                break;
            }
        }
        ExpectBefore(TokenKind::RightParen, "')' to close the tuple pattern");
        return p;
    }

    // Contextual enum pattern: .Variant, .Variant(value), or .Variant { field }
    if (Match(TokenKind::Dot)) {
        auto p = std::make_unique<EnumPattern>();
        p->location = loc;
        p->path.push_back(ExpectBefore(TokenKind::Ident, "an enum variant name after '.'").text);
        return parseEnumPatternSuffix(std::move(p));
    }

    // Identifier-started patterns: ident, EnumName::Variant(args), TypeName
    // { fields }
    if (Check(TokenKind::Ident)) {
        const std::string name = Advance().text;

        // Enum pattern: Event::Click(x, y)
        if (Check(TokenKind::ColonColon) && Peek(1).Is(TokenKind::Ident)) {
            std::vector<std::string> path = {name};
            while (Match(TokenKind::ColonColon)) {
                path.push_back(ExpectBefore(TokenKind::Ident, "a variant name after '::'").text);
            }
            auto p = std::make_unique<EnumPattern>();
            p->location = loc;
            p->path = std::move(path);
            return parseEnumPatternSuffix(std::move(p));
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
                f.name = ExpectBefore(TokenKind::Ident, "a field name in the structure pattern").text;
                ExpectBefore(TokenKind::Colon, "':' after the structure pattern field name");
                f.pattern = ParseRequiredPattern("after ':' in the structure field pattern");
                const bool validField = !f.name.empty() && f.pattern != nullptr;
                p->fields.push_back(std::move(f));
                if (!validField) {
                    if (RecoverDelimitedList(TokenKind::RightBrace)) {
                        continue;
                    }
                    break;
                }
                if (Match(TokenKind::Comma)) {
                    continue;
                }
                if (!Check(TokenKind::RightBrace)) {
                    EmitExpected(CurrentLocation(), "',' between structure pattern fields");
                    continue;
                }
                else {
                    break;
                }
            }
            ExpectBefore(TokenKind::RightBrace, "'}' to close the structure pattern");
            return p;
        }

        // Simple identifier binding
        auto p = std::make_unique<IdentPattern>();
        p->location = loc;
        p->name = name;
        return p;
    }

    return nullptr;
}
} // namespace Rux
