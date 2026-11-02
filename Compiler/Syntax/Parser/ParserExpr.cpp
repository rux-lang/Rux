// Expression parsing (precedence climbing) and patterns.

#include "Syntax/Parser/Parser.h"

#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Rux {
// Expressions
ExprPtr Parser::ParseExpr() {
    return ParseRequiredExpr();
}

ExprPtr Parser::ParseExprImpl() {
    const bool savedEndsAtCatch = std::exchange(tryOperandEndsAtCatch, false);
    ExprPtr expression = ParseTryPrefix();
    tryOperandEndsAtCatch = savedEndsAtCatch;
    return expression;
}

ExprPtr Parser::ParseTryPrefix() {
    if (Check(TokenKind::TryKeyword)) {
        return ParseTryExpr(false);
    }
    return ParseAssign();
}

// `try` has the lowest precedence of every operator, so it covers the whole expression to its right, assignment
// included: `try a() + b()` marks both calls. It is written only where an expression starts, so one to the right of
// another operator is reported and read as if it were in the right place.
ExprPtr Parser::ParseTryExpr(const bool misplaced) {
    auto expression = std::make_unique<TryMarkExpr>();
    expression->location = CurrentLocation();
    Advance(); // consume 'try'
    if (misplaced) {
        EmitError(expression->location, "'try' cannot appear to the right of an operator",
                  "write 'try' at the start of the whole expression, as in 'try x + F()'");
    }
    if (Check(TokenKind::Question) && !Peek().precededBySpace) {
        Advance();
        expression->form = TryMarkExpr::Form::Optional;
    }
    const bool savedEndsAtCatch = std::exchange(tryOperandEndsAtCatch, true);
    expression->operand = misplaced ? ParseUnary() : ParseAssign();
    tryOperandEndsAtCatch = savedEndsAtCatch;
    if (!expression->operand) {
        EmitMissingExpression(expression->form == TryMarkExpr::Form::Optional ? "after 'try?'" : "after 'try'");
    }
    if (!Check(TokenKind::CatchKeyword)) {
        return expression;
    }
    if (expression->form == TryMarkExpr::Form::Optional) {
        EmitError(CurrentLocation(), "'try?' takes no 'catch' arms",
                  "write 'try ... catch { ... }' to handle the error, or 'try?' alone to turn it into 'none'");
    }
    else {
        expression->form = TryMarkExpr::Form::Catch;
    }
    expression->catchLocation = CurrentLocation();
    Advance(); // consume 'catch'
    if (Match(TokenKind::LeftBrace)) {
        ParseArmList(expression->arms, "catch");
    }
    else {
        EmitExpected(CurrentLocation(), "'{' to start the catch arms");
    }
    if (expression->form == TryMarkExpr::Form::Optional) {
        expression->arms.clear();
    }
    return expression;
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
        TokenKind::Assign,
        TokenKind::MoveArrow,
        TokenKind::PlusAssign,
        TokenKind::MinusAssign,
        TokenKind::StarAssign,
        TokenKind::SlashAssign,
        TokenKind::PercentAssign,
        TokenKind::PlusPercentAssign,
        TokenKind::MinusPercentAssign,
        TokenKind::StarPercentAssign,
        TokenKind::AmpAssign,
        TokenKind::PipeAssign,
        TokenKind::CaretAssign,
        TokenKind::LessLessAssign,
        TokenKind::GreaterGreaterAssign,
        TokenKind::GreaterGreaterGreaterAssign,
    };

    for (auto op : kAssignOps) {
        if (Check(op)) {
            const auto loc = CurrentLocation();
            const std::string opText = Advance().text;
            auto right = ParseTryPrefix(); // right-associative, and `x = try F()` marks the value
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
        RejectEllipsisRange();
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
        RejectEllipsisRange();
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

void Parser::RejectEllipsisRange() {
    if (Check(TokenKind::DotDotDot)) {
        EmitError(CurrentLocation(), "'...' is not a range operator", "use '..='");
    }
}

ExprPtr Parser::ParseTernary() {
    auto cond = ParseCoalesce();
    if (!cond) {
        return nullptr;
    }

    if (Match(TokenKind::Question)) {
        const auto loc = Previous().location;
        const bool savedConditionalThen = std::exchange(inConditionalThen, true);
        auto thenExpr = ParseCoalesce();
        inConditionalThen = savedConditionalThen;
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

// right-associative: a ?? b ?? c  =>  a ?? (b ?? c)
ExprPtr Parser::ParseCoalesce() {
    auto left = ParseOr();
    if (!left) {
        return nullptr;
    }
    if (!Check(TokenKind::QuestionQuestion)) {
        return left;
    }

    const auto loc = CurrentLocation();
    const Token opToken = Advance();
    // The fallback may leave instead of producing a value: `candidate ?? fail NotFound {}`.
    auto right = CheckDivergingKeyword() ? ParseDivergingExpr() : ParseCoalesce();
    if (!right) {
        EmitMissingExpression(std::format("after '{}'", opToken.text));
    }
    auto expression = std::make_unique<BinaryExpr>();
    expression->location = loc;
    expression->op = TokenKind::QuestionQuestion;
    expression->left = std::move(left);
    expression->right = std::move(right);
    return expression;
}

ExprPtr Parser::ParseOr() {
    auto left = ParseAnd();
    if (!left) {
        return nullptr;
    }
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
    if (!left) {
        return nullptr;
    }
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
    if (!left) {
        return nullptr;
    }
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
    if (!left) {
        return nullptr;
    }
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
    if (!left) {
        return nullptr;
    }
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
    if (!left) {
        return nullptr;
    }
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
    if (!left) {
        return nullptr;
    }
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
    if (!left) {
        return nullptr;
    }
    // A cast or test target is a postfix type, so it ends before `..`, `|`, and `!`; a sum or fallible target is
    // grouped, `value is (A | B)`. No expression continues with `!`, so one there is always an ungrouped fallible
    // target.
    const auto rejectUngroupedFallible = [this](const std::string_view keyword) {
        if (!Check(TokenKind::Bang)) {
            return;
        }
        EmitError(CurrentLocation(), std::format("a fallible type after '{}' must be grouped", keyword),
                  std::format("write 'value {} (T ! E)'", keyword));
        Advance();
        ParseSumType();
    };
    while (CheckAny({TokenKind::AsKeyword, TokenKind::IsKeyword})) {
        const auto loc = CurrentLocation();
        if (Match(TokenKind::AsKeyword)) {
            // A cast target ends before a range operator, so `value as int..limit` is still a range of casts.
            auto type = ParsePostfixType();
            rejectUngroupedFallible("as");
            auto e = std::make_unique<CastExpr>();
            e->location = loc;
            e->operand = std::move(left);
            e->type = std::move(type);
            left = std::move(e);
        }
        else {
            Match(TokenKind::IsKeyword);
            auto e = std::make_unique<IsExpr>();
            e->location = loc;
            e->operand = std::move(left);
            if (CheckPatternAfterIs()) {
                e->pattern = ParseIsPattern();
            }
            else {
                e->type = ParsePostfixType();
                rejectUngroupedFallible("is");
            }
            left = std::move(e);
        }
    }
    return left;
}

ExprPtr Parser::ParseShift() {
    auto left = ParseAdd();
    if (!left) {
        return nullptr;
    }
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
    if (!left) {
        return nullptr;
    }
    while (CheckAny({TokenKind::Plus, TokenKind::Minus, TokenKind::PlusPercent, TokenKind::MinusPercent})) {
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
    auto left = ParseCast();
    if (!left) {
        return nullptr;
    }
    while (CheckAny({TokenKind::Star, TokenKind::Slash, TokenKind::Percent, TokenKind::StarPercent})) {
        const auto loc = CurrentLocation();
        const Token opToken = Advance();
        const auto op = opToken.kind;
        auto right = ParseCast();
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

ExprPtr Parser::ParseUnary() {
    if (Check(TokenKind::TryKeyword)) {
        return ParseTryExpr(true);
    }
    // `move value` transfers ownership out of its operand, and binds as tightly as any other prefix operator: `move
    // a.b` moves the field and `move a + b` is `(move a) + b`. The prefix `<-` is the older spelling of the same node.
    if (CheckAny({TokenKind::MoveKeyword, TokenKind::MoveArrow})) {
        const auto loc = CurrentLocation();
        const Token opToken = Advance();
        auto operand = ParseUnary();
        if (!operand) {
            EmitMissingExpression(std::format("after '{}'", opToken.text));
        }
        auto expression = std::make_unique<MoveExpr>();
        expression->location = loc;
        expression->operand = std::move(operand);
        return expression;
    }

    // C spells address-of '&', so report the rewrite where an expression
    // starts and keep parsing as '@'. Recovering here is what stops the
    // operand from cascading into "expected an expression".
    if (Check(TokenKind::Amp)) {
        const auto loc = CurrentLocation();
        EmitError(loc, "'&' does not take an address; write '@' to take the address of a value");
        Advance(); // consume '&', so the operand below parses as address-of
        auto operand = ParseUnary();
        if (!operand) {
            EmitMissingExpression("after unary '&'");
            return nullptr;
        }
        auto e = std::make_unique<UnaryExpr>();
        e->location = loc;
        e->op = TokenKind::At;
        e->operand = std::move(operand);
        return e;
    }

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
            const SourceLocation memberLocation = CurrentLocation();
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
                field->memberLocation = memberLocation;
                field->object = std::move(left);
                field->field = name;
                auto call = std::make_unique<CallExpr>();
                ++callsRead;
                call->location = loc;
                call->callee = std::move(field);
                call->args = std::move(args);
                left = std::move(call);
            }
            else {
                auto e = std::make_unique<FieldExpr>();
                e->location = loc;
                e->memberLocation = memberLocation;
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
            if (auto *path = AstCast<PathExpr>(left.get())) {
                path->segments.push_back(seg);
            }
            else {
                // Wrap existing expression in a path — treat the left side
                // as a segment This handles IDENT::IDENT::... chains
                auto p = std::make_unique<PathExpr>();
                // A path starts where its first segment does, so a diagnostic about the whole name points there.
                p->location = loc;
                if (auto *ident = AstCast<IdentExpr>(left.get())) {
                    p->location = ident->location;
                    p->segments.push_back(ident->name);
                }
                p->segments.push_back(seg);
                left = std::move(p);
            }
            continue;
        }
        // Qualified initializer: Enum::Variant { field: value, ... }
        if (structInitAllowed && Check(TokenKind::LeftBrace)) {
            if (const auto *path = AstCast<const PathExpr>(left.get())) {
                auto e = std::make_unique<StructInitExpr>();
                e->location = loc;
                for (std::size_t i = 0; i < path->segments.size(); ++i) {
                    if (i) {
                        e->typeName += "::";
                    }
                    e->typeName += path->segments[i];
                }
                ParseStructInitFields(*e);
                left = std::move(e);
                continue;
            }
        }
        // Generic function call: expr<T1, T2>(args)
        if (IsGenericCallAhead()) {
            auto typeArgs = ParseTypeArgs();
            auto args = ParseArgList();
            auto e = std::make_unique<CallExpr>();
            ++callsRead;
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
            ++callsRead;
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
            // `value? else (e => mapped)` converts the error before failing with it.
            if (Check(TokenKind::ElseKeyword) && Peek(1).Is(TokenKind::LeftParen)) {
                left = ParseErrorMapper(loc, std::move(left));
                continue;
            }
            auto e = std::make_unique<TryExpr>();
            e->location = loc;
            e->operand = std::move(left);
            left = std::move(e);
            continue;
        }
        // Postfix recovery: expr catch { pattern => body, ... }. It is part of the postfix chain like `?`, so it
        // attaches to the nearest postfix expression and the chain may continue after its closing brace.
        if (!tryOperandEndsAtCatch && Match(TokenKind::CatchKeyword)) {
            auto e = std::make_unique<CatchExpr>();
            e->location = loc;
            e->subject = std::move(left);
            if (Match(TokenKind::LeftBrace)) {
                ParseArmList(e->arms, "catch");
            }
            else {
                EmitExpected(CurrentLocation(), "'{' to start the catch arms");
            }
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

ExprPtr Parser::ParseFunctionLiteral() {
    auto literal = std::make_unique<FuncLiteralExpr>();
    literal->location = CurrentLocation();
    auto function = std::make_unique<FuncDecl>();
    function->location = literal->location;
    // Analysis names the function after the one it is written in; this spelling is what diagnostics show until then.
    function->name = "function literal";
    Advance(); // 'func'
    Advance(); // '('
    bool inferred = false;
    while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
        Param parameter;
        parameter.location = CurrentLocation();
        parameter.name = ExpectBefore(TokenKind::Ident, "a parameter name").text;
        if (Match(TokenKind::Colon)) {
            parameter.type = ParseType("add the parameter type after ':'");
        }
        else {
            parameter.type = std::make_unique<InferredTypeExpr>();
            parameter.type->location = parameter.location;
            inferred = true;
        }
        function->params.push_back(std::move(parameter));
        if (!Match(TokenKind::Comma)) {
            break;
        }
    }
    ExpectBefore(TokenKind::RightParen, "')' to close the function literal's parameters");
    if (Match(TokenKind::Arrow)) {
        function->returnType = ParseType("add the function literal's result type after '->'");
    }
    else if (inferred) {
        // A literal that leaves its parameter types to its context leaves its result type there too.
        auto result = std::make_unique<InferredTypeExpr>();
        result->location = literal->location;
        function->returnType = std::move(result);
    }
    ParseThrowsClause(function->returnType);
    const bool savedStructInitAllowed = std::exchange(structInitAllowed, true);
    function->body = ParseBlock("the function literal's body");
    structInitAllowed = savedStructInitAllowed;
    literal->function = std::move(function);
    return literal;
}

ExprPtr Parser::ParsePrimary() {
    const auto loc = CurrentLocation();
    // `if` in value position; at the start of a statement it is the statement.
    if (Check(TokenKind::IfKeyword)) {
        return ParseIfExpr();
    }
    // `unsafe { ... }` in value position yields its last expression, as any value block does.
    if (Check(TokenKind::UnsafeKeyword)) {
        if (!Peek(1).Is(TokenKind::LeftBrace)) {
            RejectMisplacedUnsafe();
            return nullptr;
        }
        return ParseUnsafeBlock();
    }
    if (Check(TokenKind::FuncKeyword) && Peek(1).Is(TokenKind::LeftParen)) {
        return ParseFunctionLiteral();
    }
    if (Match(TokenKind::MatchKeyword)) {
        auto e = std::make_unique<MatchExpr>();
        e->location = loc;
        const bool savedStructInitAllowed = std::exchange(structInitAllowed, false);
        e->subject = ParseRequiredExpr("after 'match'");
        structInitAllowed = savedStructInitAllowed;

        if (!Match(TokenKind::LeftBrace)) {
            EmitExpected(CurrentLocation(), "'{' to start the match expression arms");
            return e;
        }
        ParseArmList(e->arms, "match");
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
    // none: the absence of an optional
    if (Match(TokenKind::NoneKeyword)) {
        auto e = std::make_unique<NoneExpr>();
        e->location = loc;
        return e;
    }
    // A diverging form in a position that needs a value. It is parsed anyway so the rest of the expression recovers.
    if (CheckDivergingKeyword()) {
        EmitError(loc, std::format("'{}' cannot be used as a value here", Peek().text),
                  "write it as a whole match or catch arm, an error mapping body, or the right side of ?? , or put "
                  "it in a block");
        return ParseDivergingExpr();
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
    // Compile-time layout queries: sizeof(T) and alignof(T)
    if (Check(TokenKind::Ident) && (Peek().text == "sizeof" || Peek().text == "alignof")) {
        const std::string spelling = Advance().text;
        auto e = std::make_unique<TypeQueryExpr>();
        e->location = loc;
        e->query = spelling == "sizeof" ? TypeQueryExpr::Query::Size : TypeQueryExpr::Query::Alignment;
        ExpectBefore(TokenKind::LeftParen, std::format("'(' after '{}'", spelling));
        e->type = ParseType(std::format("add the queried type inside '{}(...)'", spelling));
        ExpectBefore(TokenKind::RightParen, std::format("')' after the '{}' type", spelling));
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
    // Native constructors: .Success(value), .Failure(error), .Some(value). Every other `.Name`, including `.None`,
    // stays the enum shorthand below.
    if (Check(TokenKind::Dot) && Peek(1).Is(TokenKind::Ident) && Peek(2).Is(TokenKind::LeftParen) &&
        (Peek(1).text == "Success" || Peek(1).text == "Failure" || Peek(1).text == "Some")) {
        Advance(); // consume '.'
        const std::string name = Advance().text;
        auto e = std::make_unique<NativeConstructExpr>();
        e->location = loc;
        e->kind = name == "Success" ? NativeConstructExpr::Kind::Success
                : name == "Failure" ? NativeConstructExpr::Kind::Failure
                                    : NativeConstructExpr::Kind::Some;
        auto args = ParseArgList();
        if (args.size() != 1) {
            std::optional<std::string> help;
            if (name == "Some" && args.empty()) {
                help = "write 'none' for an absent optional";
            }
            else if (name == "Success" && args.empty()) {
                help = "write '.Success(())' for a unit success";
            }
            EmitError(loc, std::format("'.{}' takes exactly one value", name), std::move(help));
        }
        if (!args.empty()) {
            e->operand = std::move(args.front());
        }
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
            if (e->elements.size() == 1 && Match(TokenKind::Semicolon)) {
                auto repeated = std::make_unique<ArrayRepeatExpr>();
                repeated->location = loc;
                repeated->value = std::move(e->elements.front());
                repeated->count = ParseRequiredExpr("after ';' in the array repeat expression");
                ExpectBefore(TokenKind::RightBracket, "']' to close the array repeat expression");
                return repeated;
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
        // Empty parentheses are the unit value, the one value of the empty tuple type `()`.
        if (Match(TokenKind::RightParen)) {
            auto unit = std::make_unique<TupleExpr>();
            unit->location = loc;
            return unit;
        }
        const bool savedStructInitAllowed = structInitAllowed;
        structInitAllowed = true;
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
            structInitAllowed = savedStructInitAllowed;
            ExpectBefore(TokenKind::RightParen, "')' to close the tuple expression");
            return t;
        }
        structInitAllowed = savedStructInitAllowed;
        ExpectBefore(TokenKind::RightParen, "')' to close the grouped expression");
        return first;
    }
    // `Self` inside `extend`: the extended type in a literal, a call, or a path.
    if (Check(TokenKind::Ident) && Peek().text == "Self") {
        Advance();
        return ParseSelfExpr(loc);
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
            ParseStructInitFields(*e);
            return e;
        }
        auto e = std::make_unique<IdentExpr>();
        e->location = loc;
        e->name = name;
        return e;
    }
    return nullptr;
}

void Parser::ParseStructInitFields(StructInitExpr &initializer) {
    Advance(); // consume '{'
    while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
        StructInitExpr::Field field;
        field.location = CurrentLocation();
        const bool keywordName = Check(TokenKind::ModuleKeyword);
        field.name =
            keywordName ? Advance().text : ExpectBefore(TokenKind::Ident, "a field name in the initializer").text;
        // `Point { x, y }` is `Point { x: x, y: y }`: a name alone is the binding of that name, read as any other use
        // of it, so construction reads the way the matching structure pattern does.
        if (!keywordName && !field.name.empty() && CheckAny({TokenKind::Comma, TokenKind::RightBrace})) {
            auto value = std::make_unique<IdentExpr>();
            value->location = field.location;
            value->name = field.name;
            field.value = std::move(value);
            field.shorthand = true;
        }
        else {
            ExpectBefore(TokenKind::Colon, "':' after the initializer field name");
            field.value = ParseRequiredExpr("after ':' in the initializer field");
        }
        const bool validField = !field.name.empty() && field.value != nullptr;
        initializer.fields.push_back(std::move(field));
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
        break;
    }
    ExpectBefore(TokenKind::RightBrace, "'}' to close the initializer");
}

bool Parser::CheckDivergingKeyword() const noexcept {
    return CheckAny({TokenKind::FailKeyword, TokenKind::ThrowKeyword, TokenKind::ReturnKeyword, TokenKind::BreakKeyword,
                     TokenKind::ContinueKeyword});
}

ExprPtr Parser::ParseDivergingExpr() {
    auto e = std::make_unique<DivergeExpr>();
    e->location = CurrentLocation();
    const Token keyword = Advance();
    switch (keyword.kind) {
    case TokenKind::FailKeyword:
    case TokenKind::ThrowKeyword:
        // `throw` always names the error; a unit error is written `throw ()`. `fail` is its older spelling.
        e->kind = DivergeExpr::Kind::Fail;
        e->throwSpelling = keyword.kind == TokenKind::ThrowKeyword;
        e->value = ParseRequiredExpr(e->throwSpelling ? "after 'throw'" : "after 'fail'");
        break;
    case TokenKind::ReturnKeyword:
        e->kind = DivergeExpr::Kind::Return;
        if (!CheckAny({TokenKind::Semicolon, TokenKind::Comma, TokenKind::RightParen, TokenKind::RightBrace})) {
            e->value = ParseRequiredExpr("after 'return'");
        }
        break;
    default:
        e->kind = keyword.kind == TokenKind::BreakKeyword ? DivergeExpr::Kind::Break : DivergeExpr::Kind::Continue;
        if (Check(TokenKind::Ident)) {
            e->label = Advance().text;
        }
        break;
    }
    return e;
}

ExprPtr Parser::ParseArmBody(const std::string_view construct) {
    if (Check(TokenKind::LeftBrace)) {
        auto block = std::make_unique<BlockExpr>();
        block->location = CurrentLocation();
        block->block = ParseBlock(std::format("the {} arm body", construct), true);
        return block;
    }
    if (CheckDivergingKeyword()) {
        return ParseDivergingExpr();
    }
    return ParseRequiredExpr(std::format("after '=>' in the {} arm", construct));
}

template <typename Arm>
void Parser::ParseArmList(std::vector<Arm> &arms, const std::string_view construct, const std::string_view form) {
    while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
        Arm arm;
        arm.location = CurrentLocation();
        arm.pattern = ParseMatchArmPattern();
        if (!arm.pattern) {
            while (!CheckAny({TokenKind::FatArrow, TokenKind::Comma, TokenKind::RightBrace}) && !IsAtEnd()) {
                Advance();
            }
        }
        ExpectBefore(TokenKind::FatArrow, std::format("'=>' after the {} arm pattern", construct));
        arm.body = ParseArmBody(construct);
        const bool blockBody = AstCast<const BlockExpr>(arm.body.get()) != nullptr;
        arms.push_back(std::move(arm));
        // A comma separates arms and may follow the last one; a block body ends where its brace closes.
        if (Match(TokenKind::Comma) || blockBody || Check(TokenKind::RightBrace)) {
            continue;
        }
        EmitExpected(CurrentLocation(), std::format("',' between {} arms", construct));
        while (!CheckAny({TokenKind::Comma, TokenKind::RightBrace}) && !IsAtEnd()) {
            Advance();
        }
        Match(TokenKind::Comma);
    }
    ExpectBefore(TokenKind::RightBrace, std::format("'}}' to close the {} {}", construct, form));
    for (std::size_t index = 0; index + 1 < arms.size(); ++index) {
        if (AstCast<const WildcardPattern>(arms[index].pattern.get())) {
            ReportNonfinalDefaultArm(arms[index].location, construct);
        }
    }
}

template void Parser::ParseArmList<MatchExpr::Arm>(std::vector<MatchExpr::Arm> &, std::string_view, std::string_view);
template void Parser::ParseArmList<MatchStmt::Arm>(std::vector<MatchStmt::Arm> &, std::string_view, std::string_view);

void Parser::ReportNonfinalDefaultArm(const SourceLocation location, const std::string_view construct) {
    EmitError(location, std::format("the default arm must be the last {} arm", construct),
              "move the '_' arm after the other arms");
}

ExprPtr Parser::ParseErrorMapper(const SourceLocation location, ExprPtr operand) {
    Advance(); // consume 'else'
    Advance(); // consume '('
    auto e = std::make_unique<MappedTryExpr>();
    e->location = location;
    e->operand = std::move(operand);
    e->bindingLocation = CurrentLocation();
    e->binding = ExpectBefore(TokenKind::Ident, "a name for the error before '=>' in the error mapping").text;
    ExpectBefore(TokenKind::FatArrow, "'=>' after the error binding");
    // The parentheses make a parenthesized context, so a struct literal is allowed even inside a condition.
    const bool savedStructInitAllowed = structInitAllowed;
    structInitAllowed = true;
    e->mapper = CheckDivergingKeyword() ? ParseDivergingExpr() : ParseRequiredExpr("after '=>' in the error mapping");
    if (Check(TokenKind::Comma)) {
        EmitError(CurrentLocation(), "an error mapping has one body",
                  "remove the ','; write '(e => (a, b))' to map the error to a tuple");
        while (!CheckAny({TokenKind::RightParen, TokenKind::Semicolon, TokenKind::RightBrace}) && !IsAtEnd()) {
            Advance();
        }
    }
    structInitAllowed = savedStructInitAllowed;
    ExpectBefore(TokenKind::RightParen, "')' to close the error mapping");
    return e;
}

std::vector<ExprPtr> Parser::ParseArgList() {
    std::vector<ExprPtr> args;
    ExpectBefore(TokenKind::LeftParen, "'(' to start the argument list");
    const bool savedStructInitAllowed = structInitAllowed;
    structInitAllowed = true;
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
    structInitAllowed = savedStructInitAllowed;
    ExpectBefore(TokenKind::RightParen, "')' to close the argument list");
    return args;
}

// Patterns
PatternPtr Parser::ParseMatchArmPattern() {
    const auto loc = CurrentLocation();
    // `_` is the default arm; `else` is its older spelling and is still read the same way.
    if (Match(TokenKind::ElseKeyword)) {
        auto pattern = std::make_unique<WildcardPattern>();
        pattern->location = loc;
        return pattern;
    }

    auto pattern = ParseRequiredPattern("at the start of the match arm");
    if (const auto *guarded = AstCast<const GuardedPattern>(pattern.get());
        guarded && AstCast<const WildcardPattern>(guarded->inner.get())) {
        EmitError(loc, "the default arm '_' cannot have a guard",
                  "remove the guard, or bind the value and test it, as in 'value if ...'");
    }
    return pattern;
}

bool Parser::CheckPatternAfterIs() const noexcept {
    const auto startsPatternElement = [this](const std::size_t at) {
        const Token &token = Peek(at);
        switch (token.kind) {
        case TokenKind::Dot:
        case TokenKind::NoneKeyword:
        case TokenKind::IntLiteral:
        case TokenKind::FloatLiteral:
        case TokenKind::StringLiteral:
        case TokenKind::CharLiteral:
        case TokenKind::BoolLiteral:
        case TokenKind::NullKeyword:
            return true;
        case TokenKind::Minus:
            return Peek(at + 1).Is(TokenKind::IntLiteral);
        case TokenKind::Ident:
            return token.text == "_" || Peek(at + 1).Is(TokenKind::Colon) || Peek(at + 1).Is(TokenKind::LeftParen) ||
                   Peek(at + 1).Is(TokenKind::LeftBrace);
        default:
            return false;
        }
    };
    const Token &next = Peek();
    if (next.Is(TokenKind::Ident) && next.text != "_") {
        // `name: T` binds what it selects, unless the ':' separates the branches of a conditional `c ? a : b`.
        if (Peek(1).Is(TokenKind::Colon)) {
            return !inConditionalThen;
        }
        // `Point { x, y }` is a structure pattern where a brace cannot open a body.
        if (Peek(1).Is(TokenKind::LeftBrace)) {
            return structInitAllowed;
        }
        // `Shape::Circle(r)` and `Shape::Box { width }` name a case with its payload; a bare path is a type.
        std::size_t at = 1;
        while (Peek(at).Is(TokenKind::ColonColon) && Peek(at + 1).Is(TokenKind::Ident)) {
            at += 2;
        }
        return at > 1 &&
               (Peek(at).Is(TokenKind::LeftParen) || (Peek(at).Is(TokenKind::LeftBrace) && structInitAllowed));
    }
    if (next.Is(TokenKind::LeftParen)) {
        // A tuple is a pattern when one of its elements starts as only a pattern can; `(A, B)` and `(A | B)` are types.
        std::size_t depth = 0;
        for (std::size_t at = 0; !Peek(at).IsEof(); ++at) {
            const TokenKind kind = Peek(at).kind;
            if (kind == TokenKind::LeftParen || kind == TokenKind::LeftBracket || kind == TokenKind::LeftBrace) {
                ++depth;
                if (kind == TokenKind::LeftParen && depth == 1 && startsPatternElement(at + 1)) {
                    return true;
                }
                continue;
            }
            if (kind == TokenKind::RightParen || kind == TokenKind::RightBracket || kind == TokenKind::RightBrace) {
                if (--depth == 0) {
                    return false;
                }
                continue;
            }
            if (kind == TokenKind::Comma && depth == 1 && startsPatternElement(at + 1)) {
                return true;
            }
        }
        return false;
    }
    return startsPatternElement(0);
}

PatternPtr Parser::ParseIsPattern() {
    auto first = ParseSinglePattern();
    if (!first) {
        EmitExpected(CurrentLocation(), "a pattern after 'is'");
        return nullptr;
    }
    if (!Check(TokenKind::Pipe)) {
        return first;
    }
    auto alternatives = std::make_unique<AlternativePattern>();
    alternatives->location = first->location;
    alternatives->alternatives.push_back(std::move(first));
    while (Match(TokenKind::Pipe)) {
        auto alternative = ParseSinglePattern();
        if (!alternative) {
            EmitExpected(CurrentLocation(), "a pattern after '|'");
            break;
        }
        alternatives->alternatives.push_back(std::move(alternative));
    }
    return alternatives;
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

PatternPtr Parser::ParsePresenceSuffix(PatternPtr inner) {
    while (Check(TokenKind::Question) || Check(TokenKind::QuestionQuestion)) {
        // Pattern position has no conditional operator, so a separated `?` is still meant as the suffix; it is read as
        // one and reported, so the rest of the arm parses normally.
        if (Peek().precededBySpace) {
            EmitError(CurrentLocation(), "a presence suffix must touch its pattern", "remove the space before '?'");
        }
        if (AstCast<const TypedPattern>(inner.get())) {
            EmitError(CurrentLocation(), "a presence suffix cannot follow a typed pattern",
                      "write '.Some(v: A)' to match a present member by type");
        }
        const int levels = Advance().kind == TokenKind::QuestionQuestion ? 2 : 1;
        for (int level = 0; level < levels; ++level) {
            auto presence = std::make_unique<PresencePattern>();
            presence->location = inner->location;
            presence->inner = std::move(inner);
            inner = std::move(presence);
        }
    }
    return inner;
}

PatternPtr Parser::ParsePatternImpl() {
    auto inner = ParseSinglePattern();
    if (!inner) {
        return nullptr;
    }

    // Alternatives: `.FreeBSD | .Linux` matches what either side matches. A typed pattern's annotation is a whole
    // type, so the `|` of a sum type `v: A | B` never reaches here.
    if (Check(TokenKind::Pipe)) {
        auto alternatives = std::make_unique<AlternativePattern>();
        alternatives->location = inner->location;
        alternatives->alternatives.push_back(std::move(inner));
        while (Match(TokenKind::Pipe)) {
            auto alternative = ParseSinglePattern();
            if (!alternative) {
                EmitExpected(CurrentLocation(), "a pattern after '|'");
                break;
            }
            alternatives->alternatives.push_back(std::move(alternative));
        }
        inner = std::move(alternatives);
    }

    // Guard: pattern if condition, after any range or alternatives so that `1..=9 if flag` guards the whole range.
    if (Match(TokenKind::IfKeyword)) {
        const auto loc = Previous().location;
        auto guard = ParseRequiredExpr("after 'if' in the pattern guard");
        auto p = std::make_unique<GuardedPattern>();
        p->location = loc;
        p->inner = std::move(inner);
        p->guard = std::move(guard);
        return p;
    }

    return inner;
}

PatternPtr Parser::ParseSinglePattern() {
    auto inner = ParsePrimaryPattern();
    if (!inner) {
        return nullptr;
    }
    // Presence suffix: `value?` is `.Some(value)`, and `value??` peels two levels.
    inner = ParsePresenceSuffix(std::move(inner));

    // Range pattern: lo..hi or lo..=hi; `...` is reported and read as `..=`.
    if (Check(TokenKind::DotDot) || Check(TokenKind::DotDotDot) || Check(TokenKind::DotDotEqual)) {
        const bool incl = Peek().kind == TokenKind::DotDotDot || Peek().kind == TokenKind::DotDotEqual;
        const auto loc = CurrentLocation();
        RejectEllipsisRange();
        constexpr std::string_view rangeSuffixHelp = "write '.Some(lo..hi)' to match a present value in a range";
        if (AstCast<const PresencePattern>(inner.get())) {
            EmitError(inner->location, "a range bound cannot take a presence suffix", std::string(rangeSuffixHelp));
        }
        const std::string operatorText = Advance().text;
        auto hi = ParsePrimaryPattern();
        if (!hi) {
            EmitExpected(CurrentLocation(), std::format("a range pattern end after '{}'", operatorText));
        }
        else if (Check(TokenKind::Question) || Check(TokenKind::QuestionQuestion)) {
            EmitError(CurrentLocation(), "a range bound cannot take a presence suffix", std::string(rangeSuffixHelp));
            Advance();
        }
        auto p = std::make_unique<RangePattern>();
        p->location = loc;
        p->inclusive = incl;
        p->lo = std::move(inner);
        p->hi = std::move(hi);
        inner = std::move(p);
    }
    return inner;
}

PatternPtr Parser::ParsePrimaryPattern() {
    const auto loc = CurrentLocation();

    const auto parseEnumPatternSuffix = [this](std::unique_ptr<EnumPattern> pattern) -> PatternPtr {
        // Inside the payload's parentheses a brace opens no body, so a structure pattern is fine even in a condition.
        if (Match(TokenKind::LeftParen)) {
            const bool savedStructInitAllowed = std::exchange(structInitAllowed, true);
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
            structInitAllowed = savedStructInitAllowed;
            ExpectBefore(TokenKind::RightParen, "')' to close the variant pattern");
        }
        // Where a brace opens the body of an `if` or `while`, a case written before it takes no braced payload.
        else if (structInitAllowed && Match(TokenKind::LeftBrace)) {
            pattern->braced = true;
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

    // A typed pattern `name: Type` or `_: Type`. Its annotation is a complete type, so `v: A | B` needs no grouping
    // and a tight `?` belongs to the type: `v: T?` is never a presence suffix.
    const auto parseTypedPattern = [this, loc](std::string name) -> PatternPtr {
        Advance(); // consume ':'
        auto p = std::make_unique<TypedPattern>();
        p->location = loc;
        p->name = std::move(name);
        p->type = ParseType("add the type after ':' in the typed pattern");
        return p;
    };

    // Wildcard: _
    if (Check(TokenKind::Ident) && Peek().text == "_") {
        Advance();
        if (Check(TokenKind::Colon)) {
            return parseTypedPattern("");
        }
        auto p = std::make_unique<WildcardPattern>();
        p->location = loc;
        return p;
    }

    // none: the absence of an optional
    if (Match(TokenKind::NoneKeyword)) {
        auto p = std::make_unique<NonePattern>();
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
        const bool savedStructInitAllowed = std::exchange(structInitAllowed, true);
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
        structInitAllowed = savedStructInitAllowed;
        ExpectBefore(TokenKind::RightParen, "')' to close the tuple pattern");
        return p;
    }

    // Contextual enum pattern: .Variant, .Variant(value), or .Variant { field }
    if (Match(TokenKind::Dot)) {
        auto p = std::make_unique<EnumPattern>();
        p->location = loc;
        p->path.push_back(ExpectBefore(TokenKind::Ident, "an enum member or variant case name after '.'").text);
        return parseEnumPatternSuffix(std::move(p));
    }

    // Identifier-started patterns: ident, EnumName::Variant(args), TypeName
    // { fields }
    if (Check(TokenKind::Ident)) {
        std::string name = Advance().text;
        // `Self { ... }` and `Self::Case` inside `extend` name the extended type, as its own name would.
        std::vector<std::string> selfPath;
        if (name == "Self" && CheckAny({TokenKind::LeftBrace, TokenKind::ColonColon})) {
            if (std::optional<SelfName> self = SelfNameAt(loc)) {
                name = self->Joined();
                selfPath = std::move(self->segments);
            }
        }

        // Enum pattern: Event::Click(x, y)
        if (Check(TokenKind::ColonColon) && Peek(1).Is(TokenKind::Ident)) {
            std::vector<std::string> path = selfPath.empty() ? std::vector<std::string>{name} : std::move(selfPath);
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
                if (Match(TokenKind::Colon)) {
                    f.pattern = ParseRequiredPattern("after ':' in the structure field pattern");
                }
                else if (!f.name.empty()) {
                    // The shorthand `Point { x }` binds the field to a variable of the same name.
                    auto binding = std::make_unique<IdentPattern>();
                    binding->location = f.location;
                    binding->name = f.name;
                    f.pattern = std::move(binding);
                }
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

        if (Check(TokenKind::Colon)) {
            return parseTypedPattern(name);
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
