// Statement and block parsing.

#include "Syntax/Parser/Parser.h"

#include <memory>
#include <optional>
#include <vector>

namespace Rux {
// Block and statements
std::unique_ptr<Block> Parser::ParseBlock() {
    const auto loc = CurrentLocation();
    Expect(TokenKind::LeftBrace, "expected '{'");

    auto block = std::make_unique<Block>();
    block->location = loc;

    while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
        if (auto stmt = ParseStmt()) {
            block->stmts.push_back(std::move(stmt));
        }
        else {
            Recover();
        }
    }

    Expect(TokenKind::RightBrace, "expected '}'");
    return block;
}

StmtPtr Parser::ParseStmt() {
    const auto loc = CurrentLocation();

    if (CheckAny({TokenKind::LetKeyword, TokenKind::VarKeyword})) {
        return ParseLetStmt();
    }
    if (Check(TokenKind::IfKeyword) || Check(TokenKind::WhenKeyword)) {
        return ParseIfStmt();
    }
    // The form `when` replaced.
    if (Check(TokenKind::Hash) && Peek(1).Is(TokenKind::IfKeyword)) {
        EmitError(loc, "'#if' is no longer conditional compilation; write 'when <condition> { ... }'");
        Advance(); // consume '#', so the `if` below parses as the run-time form
        return ParseIfStmt();
    }
    // Optional loop label: `ident ':' loop-keyword`
    std::string loopLabel;
    if (Check(TokenKind::Ident) && Peek(1).kind == TokenKind::Colon) {
        const TokenKind ahead = Peek(2).kind;
        if (ahead == TokenKind::WhileKeyword || ahead == TokenKind::DoKeyword || ahead == TokenKind::LoopKeyword ||
            ahead == TokenKind::ForKeyword) {
            loopLabel = Advance().text; // consume label name
            Advance();                  // consume ':'
        }
    }
    if (Check(TokenKind::WhileKeyword)) {
        auto s = ParseWhileStmt();
        s->label = loopLabel;
        return s;
    }
    if (Check(TokenKind::DoKeyword)) {
        auto s = ParseDoWhileStmt();
        s->label = loopLabel;
        return s;
    }
    if (Check(TokenKind::LoopKeyword)) {
        auto s = ParseLoopStmt();
        s->label = loopLabel;
        return s;
    }
    if (Check(TokenKind::ForKeyword)) {
        auto s = ParseForStmt();
        s->label = loopLabel;
        return s;
    }
    if (Check(TokenKind::MatchKeyword)) {
        return ParseMatchStmt();
    }
    if (Check(TokenKind::ReturnKeyword)) {
        return ParseReturnStmt();
    }

    if (Check(TokenKind::BreakKeyword)) {
        Advance();
        std::string label;
        if (Check(TokenKind::Ident)) {
            label = Advance().text;
        }
        Expect(TokenKind::Semicolon, "expected ';'");
        auto s = std::make_unique<BreakStmt>();
        s->location = loc;
        s->label = label;
        return s;
    }

    if (Check(TokenKind::ContinueKeyword)) {
        Advance();
        std::string label;
        if (Check(TokenKind::Ident)) {
            label = Advance().text;
        }
        Expect(TokenKind::Semicolon, "expected ';'");
        auto s = std::make_unique<ContinueStmt>();
        s->location = loc;
        s->label = label;
        return s;
    }

    // Allow nested declarations inside blocks
    if (CheckAny({TokenKind::PubKeyword, TokenKind::FuncKeyword, TokenKind::StructKeyword, TokenKind::EnumKeyword,
                  TokenKind::UnionKeyword, TokenKind::InterfaceKeyword, TokenKind::ExtendKeyword,
                  TokenKind::ModuleKeyword, TokenKind::ConstKeyword, TokenKind::TypeKeyword,
                  TokenKind::ExternKeyword})) {
        auto ds = std::make_unique<DeclStmt>();
        ds->location = loc;
        ds->decl = ParseDecl();
        return ds;
    }

    // Expression statement
    auto expr = ParseExpr();
    if (!expr) {
        return nullptr;
    }
    // A bare "expected ';'" is confusing when a second token follows a complete
    // expression (e.g. two identifiers `asas asasass`, or a removed keyword used
    // as one): name the token that got in the way instead. The cursor stays put,
    // so the block loop re-parses that token as the next statement.
    if (Check(TokenKind::Semicolon)) {
        Advance();
    }
    else if (!IsAtEnd()) {
        EmitError(CurrentLocation(), "expected ';' after expression, but found '" + Peek().text + "'");
    }
    else {
        EmitError(CurrentLocation(), "expected ';' after expression");
    }
    auto s = std::make_unique<ExprStmt>();
    s->location = loc;
    s->expr = std::move(expr);
    return s;
}

std::unique_ptr<LetStmt> Parser::ParseLetStmt() {
    const auto loc = CurrentLocation();

    auto s = std::make_unique<LetStmt>();
    s->location = loc;

    // `let` introduces an immutable binding; `var` introduces a mutable one.
    s->isMut = Match(TokenKind::VarKeyword);
    if (!s->isMut) {
        Expect(TokenKind::LetKeyword, "expected 'let' or 'var'");
    }
    if (Check(TokenKind::Ident)) {
        s->name = Advance().text;
    }
    else {
        s->pattern = ParsePattern();
        if (!s->pattern) {
            EmitError(CurrentLocation(), "expected variable name or destructuring pattern");
        }
    }

    if (Match(TokenKind::Colon)) {
        s->type = ParseType();
    }

    if (Match(TokenKind::Assign)) {
        s->init = ParseExpr();
    }
    else if (!s->type) {
        EmitError(CurrentLocation(), "expected '='");
    }
    Expect(TokenKind::Semicolon, "expected ';'");
    return s;
}

std::unique_ptr<IfStmt> Parser::ParseIfStmt() {
    const auto loc = CurrentLocation();
    // `when` is the compile-time form: same shape, folded before analysis.
    const bool isCompileTime = Check(TokenKind::WhenKeyword);
    const TokenKind keyword = isCompileTime ? TokenKind::WhenKeyword : TokenKind::IfKeyword;
    Expect(keyword, isCompileTime ? "expected 'when'" : "expected 'if'");

    auto s = std::make_unique<IfStmt>();
    s->location = loc;
    s->isCompileTime = isCompileTime;
    structInitAllowed = false;
    auto subject = ParseExpr();
    structInitAllowed = true;

    // Compile-time match: `when subject { pattern, ... => body, ... else => body }`.
    // An arm names one or more comma-separated patterns before `=>` (it is taken
    // when any matches); its body becomes a block spliced in when selected, and a
    // bare-expression body is wrapped as a one-statement block.
    if (isCompileTime && Check(TokenKind::LeftBrace) && NextBraceIsMatchArms()) {
        s->matchSubject = std::move(subject);

        auto parseArmBody = [&]() -> std::unique_ptr<Block> {
            if (Check(TokenKind::LeftBrace)) {
                return ParseBlock();
            }
            auto block = std::make_unique<Block>();
            block->location = CurrentLocation();
            auto exprStmt = std::make_unique<ExprStmt>();
            exprStmt->location = CurrentLocation();
            exprStmt->expr = ParseExpr();
            block->stmts.push_back(std::move(exprStmt));
            return block;
        };

        Expect(TokenKind::LeftBrace, "expected '{'");
        bool sawElse = false;
        while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
            IfStmt::MatchArm arm;
            arm.location = CurrentLocation();
            if (Match(TokenKind::ElseKeyword)) {
                sawElse = true;
            }
            else {
                structInitAllowed = false;
                arm.patterns.push_back(ParseExpr());
                // Commas before `=>` separate patterns that share this arm's body.
                while (Match(TokenKind::Comma) && !Check(TokenKind::FatArrow)) {
                    arm.patterns.push_back(ParseExpr());
                }
                structInitAllowed = true;
            }
            Expect(TokenKind::FatArrow, "expected '=>' after a 'when' arm pattern");
            arm.block = parseArmBody();
            s->matchArms.push_back(std::move(arm));

            Match(TokenKind::Comma); // optional separator; required only to end a bare-expression arm
            if (sawElse && !Check(TokenKind::RightBrace)) {
                EmitError(CurrentLocation(), "the 'else' arm must be last in a 'when' match");
            }
        }
        Expect(TokenKind::RightBrace, "expected '}' to close the 'when' match");
        return s;
    }

    s->condition = std::move(subject);
    s->thenBlock = ParseBlock();

    // A chain keeps the keyword it opened with: `if`/`else if` throughout, or
    // `when`/`else when` throughout. Mixing them would hide which arms the
    // compiler resolves and which the program tests as it runs.
    const TokenKind wrongKeyword = isCompileTime ? TokenKind::IfKeyword : TokenKind::WhenKeyword;
    while (Check(TokenKind::ElseKeyword) && (Peek(1).Is(keyword) || Peek(1).Is(wrongKeyword))) {
        IfStmt::ElseIf elif;
        elif.location = CurrentLocation();
        Advance(); // consume 'else'
        if (Check(wrongKeyword)) {
            EmitError(CurrentLocation(), isCompileTime ? "expected 'when' after 'else' in a compile-time 'when' chain; "
                                                         "'if' is the run-time conditional"
                                                       : "expected 'if' after 'else' in a run-time 'if' chain; "
                                                         "'when' is the compile-time conditional");
        }
        Advance(); // consume 'if' / 'when'
        structInitAllowed = false;
        elif.condition = ParseExpr();
        structInitAllowed = true;
        elif.block = ParseBlock();
        s->elseIfs.push_back(std::move(elif));
    }

    if (Match(TokenKind::ElseKeyword)) {
        s->elseBlock = ParseBlock();
    }

    return s;
}

std::unique_ptr<WhileStmt> Parser::ParseWhileStmt() {
    const auto loc = CurrentLocation();
    Expect(TokenKind::WhileKeyword, "expected 'while'");

    auto s = std::make_unique<WhileStmt>();
    s->location = loc;
    structInitAllowed = false;
    s->condition = ParseExpr();
    structInitAllowed = true;
    s->body = ParseBlock();
    return s;
}

std::unique_ptr<DoWhileStmt> Parser::ParseDoWhileStmt() {
    const auto loc = CurrentLocation();
    Expect(TokenKind::DoKeyword, "expected 'do'");

    auto s = std::make_unique<DoWhileStmt>();
    s->location = loc;
    s->body = ParseBlock();
    Expect(TokenKind::WhileKeyword, "expected 'while' after do body");
    structInitAllowed = false;
    s->condition = ParseExpr();
    structInitAllowed = true;
    Expect(TokenKind::Semicolon, "expected ';' after do-while condition");
    return s;
}

std::unique_ptr<LoopStmt> Parser::ParseLoopStmt() {
    const auto loc = CurrentLocation();
    Expect(TokenKind::LoopKeyword, "expected 'loop'");

    auto s = std::make_unique<LoopStmt>();
    s->location = loc;
    s->body = ParseBlock();
    return s;
}

std::unique_ptr<ForStmt> Parser::ParseForStmt() {
    const auto loc = CurrentLocation();
    Expect(TokenKind::ForKeyword, "expected 'for'");

    auto s = std::make_unique<ForStmt>();
    s->location = loc;
    s->variable = Expect(TokenKind::Ident, "expected loop variable").text;
    Expect(TokenKind::InKeyword, "expected 'in'");
    structInitAllowed = false;
    s->iterable = ParseExpr();
    structInitAllowed = true;
    s->body = ParseBlock();
    return s;
}

std::unique_ptr<MatchStmt> Parser::ParseMatchStmt() {
    const auto loc = CurrentLocation();
    Expect(TokenKind::MatchKeyword, "expected 'match'");

    auto s = std::make_unique<MatchStmt>();
    s->location = loc;
    structInitAllowed = false;
    s->subject = ParseExpr();
    structInitAllowed = true;

    Expect(TokenKind::LeftBrace, "expected '{'");
    while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
        MatchStmt::Arm arm;
        arm.location = CurrentLocation();
        arm.pattern = ParseMatchArmPattern();
        Expect(TokenKind::FatArrow, "expected '=>'");

        if (Check(TokenKind::LeftBrace)) {
            // Block body
            auto bexpr = std::make_unique<BlockExpr>();
            bexpr->location = CurrentLocation();
            bexpr->block = ParseBlock();
            arm.body = std::move(bexpr);
        }
        else {
            arm.body = ParseExpr();
        }

        s->arms.push_back(std::move(arm));
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
    return s;
}

std::unique_ptr<ReturnStmt> Parser::ParseReturnStmt() {
    const auto loc = CurrentLocation();
    Expect(TokenKind::ReturnKeyword, "expected 'return'");

    auto s = std::make_unique<ReturnStmt>();
    s->location = loc;

    if (!Check(TokenKind::Semicolon)) {
        s->value = ParseExpr();
    }

    Expect(TokenKind::Semicolon, "expected ';'");
    return s;
}
} // namespace Rux
