// Statement and block parsing.

#include <memory>
#include <optional>
#include <vector>

#include "Syntax/Parser/Parser.h"

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

    if (Check(TokenKind::LetKeyword) || Check(TokenKind::VarKeyword)) {
        return ParseLetStmt();
    }
    if (Check(TokenKind::IfKeyword)) {
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
    Expect(TokenKind::Semicolon, "expected ';'");
    auto s = std::make_unique<ExprStmt>();
    s->location = loc;
    s->expr = std::move(expr);
    return s;
}

std::unique_ptr<LetStmt> Parser::ParseLetStmt() {
    const auto loc = CurrentLocation();

    auto s = std::make_unique<LetStmt>();
    s->location = loc;

    if (Match(TokenKind::VarKeyword)) {
        s->isMut = true; // var is always mutable
    }
    else {
        Expect(TokenKind::LetKeyword, "expected 'let' or 'var'");
        s->isMut = false;
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
    Expect(TokenKind::IfKeyword, "expected 'if'");

    auto s = std::make_unique<IfStmt>();
    s->location = loc;
    structInitAllowed = false;
    s->condition = ParseExpr();
    structInitAllowed = true;
    s->thenBlock = ParseBlock();

    while (Check(TokenKind::ElseKeyword) && Peek(1).Is(TokenKind::IfKeyword)) {
        IfStmt::ElseIf elif;
        elif.location = CurrentLocation();
        Advance(); // consume 'else'
        Advance(); // consume 'if'
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
        arm.pattern = ParsePattern();
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
