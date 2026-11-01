// Statement and block parsing.

#include "Syntax/Parser/Parser.h"

#include <algorithm>
#include <format>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace Rux {
// Block and statements
std::unique_ptr<Block> Parser::ParseBlock(const std::string_view role, const bool valueBlock) {
    const auto loc = CurrentLocation();
    const bool savedYieldsValue = blockYieldsValue;
    if (!Match(TokenKind::LeftBrace)) {
        EmitExpected(CurrentLocation(), std::format("'{{' to start {}", role));
    }

    auto block = std::make_unique<Block>();
    block->location = loc;

    while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
        // Documentation attaches only to declarations, so a '///' run inside a block is reported once and skipped
        // rather than misread as the start of an expression.
        if (Check(TokenKind::DocComment)) {
            EmitError(CurrentLocation(), "documentation comment inside a block is not attached to a declaration",
                      "write an ordinary '//' comment here; '///' documents the declaration that follows it");
            while (Check(TokenKind::DocComment)) {
                Advance();
            }
            continue;
        }
        // A statement nested inside this one reads its own block, which sets this again for its statements.
        blockYieldsValue = valueBlock;
        if (auto stmt = ParseStmt()) {
            block->stmts.push_back(std::move(stmt));
        }
        else {
            Recover();
        }
    }
    blockYieldsValue = savedYieldsValue;

    ExpectBefore(TokenKind::RightBrace, std::format("'}}' to close {}", role));
    block->end = CurrentLocation();
    return block;
}

StmtPtr Parser::ParseStmt() {
    const auto loc = CurrentLocation();
    // Only the last expression of a value block may leave out its ';', so the flag is read before anything nested.
    const bool tailAllowed = std::exchange(blockYieldsValue, false);

    if (CheckAny({TokenKind::LetKeyword, TokenKind::VarKeyword})) {
        return ParseLetStmt();
    }
    if (Check(TokenKind::IfKeyword) || Check(TokenKind::WhenKeyword)) {
        auto conditional = ParseIfStmt();
        // An `if` that ends a value block and yields a value is that block's value, as an `if` expression would be.
        if (tailAllowed && Check(TokenKind::RightBrace) && IfYieldsValue(*conditional)) {
            auto tail = std::make_unique<ExprStmt>();
            tail->location = loc;
            tail->expr = IfAsExpression(*conditional);
            tail->yieldsValue = true;
            return tail;
        }
        return conditional;
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
        auto s = ParseMatchStmt();
        RejectPostfixAfterMatchStatement();
        return s;
    }
    if (Check(TokenKind::ReturnKeyword)) {
        return ParseReturnStmt();
    }
    if (Check(TokenKind::FailKeyword)) {
        return ParseFailStmt();
    }
    if (Check(TokenKind::DeferKeyword)) {
        return ParseDeferStmt();
    }

    if (Check(TokenKind::BreakKeyword)) {
        Advance();
        std::string label;
        if (Check(TokenKind::Ident)) {
            label = Advance().text;
        }
        ExpectBefore(TokenKind::Semicolon, "';' after the 'break' statement");
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
        ExpectBefore(TokenKind::Semicolon, "';' after the 'continue' statement");
        auto s = std::make_unique<ContinueStmt>();
        s->location = loc;
        s->label = label;
        return s;
    }

    // Allow nested declarations inside blocks
    if (CheckAny({TokenKind::PubKeyword, TokenKind::FuncKeyword, TokenKind::StructKeyword, TokenKind::EnumKeyword,
                  TokenKind::VariantKeyword, TokenKind::UnionKeyword, TokenKind::InterfaceKeyword,
                  TokenKind::ExtendKeyword, TokenKind::ModuleKeyword, TokenKind::ConstKeyword, TokenKind::TypeKeyword,
                  TokenKind::ExternKeyword})) {
        auto ds = std::make_unique<DeclStmt>();
        ds->location = loc;
        bodiesNeedConditionalFolding = true;
        // A declaration nested in a method body is not a member of the type being extended, so `Self` does not name
        // that type inside it.
        SelfScope enclosing = std::exchange(selfScope, SelfScope{});
        ds->decl = ParseDecl();
        selfScope = std::move(enclosing);
        bodiesDeclareItems = bodiesDeclareItems || (ds->decl && !AstCast<const ConstDecl>(ds->decl.get()));
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
    auto s = std::make_unique<ExprStmt>();
    s->location = loc;
    if (Check(TokenKind::Semicolon)) {
        s->semicolon = CurrentLocation();
        Advance();
    }
    else if (tailAllowed && Check(TokenKind::RightBrace)) {
        // The last expression of a value block, written without its ';', is the value the block yields.
        s->yieldsValue = true;
    }
    else if (!IsAtEnd()) {
        EmitError(CurrentLocation(), "expected ';' after expression, but found '" + Peek().text + "'");
    }
    else {
        EmitError(CurrentLocation(), "expected ';' after expression");
    }
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
    // `_` names nothing: it is the wildcard pattern, which discards the value, even with a type annotation after it.
    if (Check(TokenKind::Ident) && Peek().text == "_") {
        auto wildcard = std::make_unique<WildcardPattern>();
        wildcard->location = Advance().location;
        s->pattern = std::move(wildcard);
    }
    else if (Check(TokenKind::Ident) && !Peek(1).Is(TokenKind::LeftBrace) && !Peek(1).Is(TokenKind::ColonColon)) {
        s->name = Advance().text;
    }
    else if (CheckAny({TokenKind::NoneKeyword, TokenKind::CatchKeyword, TokenKind::FailKeyword})) {
        // `none` is the absence pattern, which could never bind, and `catch` and `fail` are reserved for recovery and
        // failure; a binding spelled as one of them is a rename to make.
        const std::string keyword = Peek().text;
        const std::string_view suggestion = Check(TokenKind::NoneKeyword)  ? "absent"
                                          : Check(TokenKind::CatchKeyword) ? "caught"
                                                                           : "failed";
        EmitError(CurrentLocation(), std::format("'{}' is a reserved keyword and cannot name a binding", keyword),
                  std::format("choose another name, such as '{}'", suggestion));
        auto recovered = std::make_unique<WildcardPattern>();
        recovered->location = Advance().location;
        s->pattern = std::move(recovered);
    }
    else {
        s->pattern = ParseRequiredPattern("after the binding keyword");
        if (!s->pattern) {
            while (!CheckAny({TokenKind::Colon, TokenKind::Assign, TokenKind::MoveArrow, TokenKind::Semicolon}) &&
                   !IsAtEnd()) {
                Advance();
            }
        }
    }

    if (Match(TokenKind::Colon)) {
        s->type = ParseType("add the binding type after ':'");
    }

    if (Match(TokenKind::Assign)) {
        s->init = ParseRequiredExpr("after '=' in the binding");
    }
    else if (Match(TokenKind::MoveArrow)) {
        const SourceLocation moveLocation = Previous().location;
        auto move = std::make_unique<MoveExpr>();
        move->location = moveLocation;
        move->operand = ParseRequiredExpr("after '<-' in the binding");
        s->init = std::move(move);
    }
    else if (!s->type) {
        EmitExpected(CurrentLocation(), "'=' or '<-' before the binding initializer",
                     "add a type annotation or initialize the binding");
    }
    ExpectBefore(TokenKind::Semicolon, "';' after the binding declaration");
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
    bodiesNeedConditionalFolding = bodiesNeedConditionalFolding || isCompileTime;
    const bool savedStructInitAllowed = std::exchange(structInitAllowed, false);
    auto subject = ParseRequiredExpr(isCompileTime ? "after 'when'" : "after 'if'");
    structInitAllowed = savedStructInitAllowed;

    // Compile-time match: `when subject { pattern, ... => body, ... else => body }`.
    // An arm names one or more comma-separated patterns before `=>` (it is taken
    // when any matches); its body becomes a block spliced in when selected, and a
    // bare-expression body is wrapped as a one-statement block.
    if (isCompileTime && Check(TokenKind::LeftBrace) && NextBraceIsMatchArms()) {
        s->matchSubject = std::move(subject);

        // An arm body is a block or a single expression. A statement written bare is the mistake worth naming: it
        // reads like it should work, and every token of it would otherwise be reported one at a time as the arm
        // failed to parse and the next arm started in the middle of it.
        const auto startsStatementNotExpression = [&]() {
            return CheckAny({TokenKind::ReturnKeyword, TokenKind::LetKeyword, TokenKind::VarKeyword,
                             TokenKind::IfKeyword, TokenKind::WhileKeyword, TokenKind::ForKeyword,
                             TokenKind::LoopKeyword, TokenKind::BreakKeyword, TokenKind::ContinueKeyword,
                             TokenKind::WhenKeyword, TokenKind::FailKeyword});
        };
        auto parseArmBody = [&]() -> std::unique_ptr<Block> {
            if (Check(TokenKind::LeftBrace)) {
                return ParseBlock("the 'when' arm body");
            }
            auto block = std::make_unique<Block>();
            block->location = CurrentLocation();
            if (startsStatementNotExpression()) {
                EmitError(CurrentLocation(), "a 'when' arm body that is a statement must be written as a block, as in "
                                             "'.Windows => { return 1; }'");
                // Skip the rest of the statement so the next arm starts where it should, taking the terminator with
                // it: left behind, a ';' would be read as the start of another arm and reported all over again.
                while (!CheckAny({TokenKind::Comma, TokenKind::Semicolon, TokenKind::RightBrace}) && !IsAtEnd()) {
                    Advance();
                }
                Match(TokenKind::Semicolon);
                return block;
            }
            auto exprStmt = std::make_unique<ExprStmt>();
            exprStmt->location = CurrentLocation();
            exprStmt->expr = ParseRequiredExpr("after '=>' in the 'when' arm");
            block->stmts.push_back(std::move(exprStmt));
            return block;
        };

        ExpectBefore(TokenKind::LeftBrace, "'{' to start the 'when' match arms");
        std::optional<SourceLocation> defaultArm;
        while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
            // Where this arm started. An arm whose body does not parse consumes nothing, and without this the loop
            // would run forever building empty arms -- which is what a statement written as a bare arm body used to
            // do, since a `return` is not an expression and left the cursor where it was.
            const std::size_t armStart = pos;
            IfStmt::MatchArm arm;
            arm.location = CurrentLocation();
            if (defaultArm) {
                ReportNonfinalDefaultArm(*defaultArm, "'when'");
                defaultArm.reset();
            }
            if (CheckWhenDefaultArm()) {
                Advance();
                defaultArm = arm.location;
            }
            else {
                ParseWhenArmPatterns(arm.patterns);
            }
            ExpectBefore(TokenKind::FatArrow, "'=>' after a 'when' arm pattern");
            arm.block = parseArmBody();
            const bool blockBody =
                arm.block && (Previous().kind == TokenKind::RightBrace || Previous().kind == TokenKind::Semicolon);
            s->matchArms.push_back(std::move(arm));

            // Arms are separated as a match separates them: a comma, which may follow the last arm and may be left
            // out after a block.
            const bool separated = Match(TokenKind::Comma);
            if (pos == armStart) {
                EmitError(CurrentLocation(),
                          "expected a 'when' match arm, or '}' to close the match; an arm body that is a statement "
                          "rather than an expression needs braces, as in '.Windows => { return 1; }'");
                break;
            }
            if (!separated && !blockBody && !Check(TokenKind::RightBrace)) {
                EmitExpected(CurrentLocation(), "',' between 'when' arms");
            }
        }
        ExpectBefore(TokenKind::RightBrace, "'}' to close the 'when' match");
        return s;
    }

    // A run-time branch is a value block, whether the `if` is a statement or a value; a compile-time branch is spliced
    // in place of the `when`.
    const bool valueBranches = !isCompileTime;
    s->condition = std::move(subject);
    s->thenBlock = ParseBlock(isCompileTime ? "the 'when' body" : "the 'if' body", valueBranches);

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
        elif.condition = ParseRequiredExpr(isCompileTime ? "after 'else when'" : "after 'else if'");
        structInitAllowed = savedStructInitAllowed;
        elif.block = ParseBlock(isCompileTime ? "the 'else when' body" : "the 'else if' body", valueBranches);
        s->elseIfs.push_back(std::move(elif));
    }

    if (Match(TokenKind::ElseKeyword)) {
        s->elseBlock = ParseBlock("the 'else' body", valueBranches);
    }

    return s;
}

ExprPtr Parser::ParseIfExpr() {
    const SourceLocation location = CurrentLocation();
    auto statement = ParseIfStmt();
    if (!statement->elseBlock) {
        EmitError(location, "an 'if' used as a value needs an 'else' branch",
                  "add 'else { ... }' with the value for when the condition is false");
    }
    return IfAsExpression(*statement);
}

namespace {
ExprPtr BranchExpression(std::unique_ptr<Block> block, const SourceLocation location) {
    auto branch = std::make_unique<BlockExpr>();
    if (!block) {
        block = std::make_unique<Block>();
        block->location = location;
        block->end = location;
    }
    branch->location = block->location;
    branch->block = std::move(block);
    return branch;
}

ExprPtr ConditionalMatch(const SourceLocation location, ExprPtr condition, ExprPtr thenBranch, ExprPtr elseBranch,
                         const SourceLocation elseLocation) {
    auto match = std::make_unique<MatchExpr>();
    match->location = location;
    match->form = MatchExpr::Form::If;
    match->subject = std::move(condition);

    MatchExpr::Arm taken;
    taken.location = thenBranch->location;
    auto truth = std::make_unique<LiteralPattern>();
    truth->location = thenBranch->location;
    truth->value.kind = TokenKind::BoolLiteral;
    truth->value.text = "true";
    truth->value.location = thenBranch->location;
    taken.pattern = std::move(truth);
    taken.body = std::move(thenBranch);
    match->arms.push_back(std::move(taken));

    MatchExpr::Arm otherwise;
    otherwise.location = elseLocation;
    auto wildcard = std::make_unique<WildcardPattern>();
    wildcard->location = elseLocation;
    otherwise.pattern = std::move(wildcard);
    otherwise.body = elseBranch ? std::move(elseBranch) : BranchExpression(nullptr, elseLocation);
    match->arms.push_back(std::move(otherwise));
    return match;
}

bool BlockYieldsValue(const Block *block) noexcept {
    if (!block || block->stmts.empty()) {
        return false;
    }
    const auto *last = AstCast<const ExprStmt>(block->stmts.back().get());
    return last && last->yieldsValue;
}
} // namespace

bool Parser::IfYieldsValue(const IfStmt &statement) noexcept {
    if (statement.isCompileTime || !statement.elseBlock) {
        return false;
    }
    return BlockYieldsValue(statement.thenBlock.get()) || BlockYieldsValue(statement.elseBlock.get()) ||
           std::ranges::any_of(statement.elseIfs,
                               [](const IfStmt::ElseIf &branch) { return BlockYieldsValue(branch.block.get()); });
}

ExprPtr Parser::IfAsExpression(IfStmt &statement) {
    ExprPtr elseBranch;
    SourceLocation elseLocation = statement.elseBlock ? statement.elseBlock->location : statement.location;
    if (statement.elseBlock) {
        elseBranch = BranchExpression(std::move(statement.elseBlock), elseLocation);
    }
    for (auto branch = statement.elseIfs.rbegin(); branch != statement.elseIfs.rend(); ++branch) {
        elseBranch = ConditionalMatch(branch->location, std::move(branch->condition),
                                      BranchExpression(std::move(branch->block), branch->location),
                                      std::move(elseBranch), elseLocation);
        elseLocation = branch->location;
    }
    return ConditionalMatch(statement.location, std::move(statement.condition),
                            BranchExpression(std::move(statement.thenBlock), statement.location), std::move(elseBranch),
                            elseLocation);
}

std::unique_ptr<WhileStmt> Parser::ParseWhileStmt() {
    const auto loc = CurrentLocation();
    Expect(TokenKind::WhileKeyword, "expected 'while'");

    auto s = std::make_unique<WhileStmt>();
    s->location = loc;
    const bool savedStructInitAllowed = std::exchange(structInitAllowed, false);
    s->condition = ParseRequiredExpr("after 'while'");
    structInitAllowed = savedStructInitAllowed;
    s->body = ParseBlock("the 'while' body");
    return s;
}

std::unique_ptr<DoWhileStmt> Parser::ParseDoWhileStmt() {
    const auto loc = CurrentLocation();
    Expect(TokenKind::DoKeyword, "expected 'do'");

    auto s = std::make_unique<DoWhileStmt>();
    s->location = loc;
    s->body = ParseBlock("the 'do' body");
    ExpectBefore(TokenKind::WhileKeyword, "'while' after the 'do' body");
    const bool savedStructInitAllowed = std::exchange(structInitAllowed, false);
    s->condition = ParseRequiredExpr("after 'while' in the 'do' statement");
    structInitAllowed = savedStructInitAllowed;
    ExpectBefore(TokenKind::Semicolon, "';' after the 'do while' condition");
    return s;
}

std::unique_ptr<LoopStmt> Parser::ParseLoopStmt() {
    const auto loc = CurrentLocation();
    Expect(TokenKind::LoopKeyword, "expected 'loop'");

    auto s = std::make_unique<LoopStmt>();
    s->location = loc;
    s->body = ParseBlock("the 'loop' body");
    return s;
}

std::unique_ptr<ForStmt> Parser::ParseForStmt() {
    const auto loc = CurrentLocation();
    Expect(TokenKind::ForKeyword, "expected 'for'");

    auto s = std::make_unique<ForStmt>();
    s->location = loc;
    // A pattern other than a name binds a hidden loop variable and takes it apart at the top of the body.
    PatternPtr pattern;
    if (Check(TokenKind::Ident) && Peek(1).Is(TokenKind::InKeyword)) {
        s->variable = Advance().text;
    }
    else {
        pattern = ParseRequiredPattern("after 'for'");
        if (pattern && !PatternMatchesEveryElement(*pattern)) {
            EmitError(pattern->location, "a 'for' pattern must match every element",
                      "bind the element and test it in the body, as in 'for item in items { if item is ... { ... } }'");
        }
        s->variable = std::format("<for item {}:{}>", loc.line, loc.column);
    }
    ExpectBefore(TokenKind::InKeyword, "'in' after the loop variable");
    const bool savedStructInitAllowed = std::exchange(structInitAllowed, false);
    s->iterable = ParseRequiredExpr("after 'in' in the 'for' statement");
    structInitAllowed = savedStructInitAllowed;
    s->body = ParseBlock("the 'for' body");
    if (pattern && s->body) {
        auto item = std::make_unique<IdentExpr>();
        item->location = pattern->location;
        item->name = s->variable;
        auto transfer = std::make_unique<MoveExpr>();
        transfer->location = pattern->location;
        transfer->operand = std::move(item);
        auto destructure = std::make_unique<LetStmt>();
        destructure->location = pattern->location;
        destructure->pattern = std::move(pattern);
        destructure->init = std::move(transfer);
        s->body->stmts.insert(s->body->stmts.begin(), std::move(destructure));
    }
    return s;
}

bool Parser::PatternMatchesEveryElement(const Pattern &pattern) noexcept {
    if (AstCast<const IdentPattern>(&pattern) || AstCast<const WildcardPattern>(&pattern)) {
        return true;
    }
    if (const auto *tuple = AstCast<const TuplePattern>(&pattern)) {
        return std::ranges::all_of(
            tuple->elements, [](const PatternPtr &element) { return element && PatternMatchesEveryElement(*element); });
    }
    if (const auto *structure = AstCast<const StructPattern>(&pattern)) {
        return std::ranges::all_of(structure->fields, [](const StructPattern::Field &field) {
            return field.pattern && PatternMatchesEveryElement(*field.pattern);
        });
    }
    return false;
}

std::unique_ptr<MatchStmt> Parser::ParseMatchStmt() {
    const auto loc = CurrentLocation();
    Expect(TokenKind::MatchKeyword, "expected 'match'");

    auto s = std::make_unique<MatchStmt>();
    s->location = loc;
    const bool savedStructInitAllowed = std::exchange(structInitAllowed, false);
    s->subject = ParseRequiredExpr("after 'match'");
    structInitAllowed = savedStructInitAllowed;

    if (!Match(TokenKind::LeftBrace)) {
        EmitExpected(CurrentLocation(), "'{' to start the match statement arms");
        return s;
    }
    ParseArmList(s->arms, "match", "statement");
    return s;
}

std::unique_ptr<ReturnStmt> Parser::ParseReturnStmt() {
    const auto loc = CurrentLocation();
    Expect(TokenKind::ReturnKeyword, "expected 'return'");

    auto s = std::make_unique<ReturnStmt>();
    s->location = loc;

    if (!Check(TokenKind::Semicolon)) {
        s->value = ParseRequiredExpr("after 'return'");
    }

    ExpectBefore(TokenKind::Semicolon, "';' after the 'return' statement");
    return s;
}

std::unique_ptr<FailStmt> Parser::ParseFailStmt() {
    auto s = std::make_unique<FailStmt>();
    s->location = CurrentLocation();
    Expect(TokenKind::FailKeyword, "expected 'fail'");
    s->value = ParseRequiredExpr("after 'fail'");
    ExpectBefore(TokenKind::Semicolon, "';' after the 'fail' statement");
    return s;
}

void Parser::RejectPostfixAfterMatchStatement() {
    const bool tightQuestion = Check(TokenKind::Question) && !Peek().precededBySpace;
    if (!Check(TokenKind::CatchKeyword) && !tightQuestion && !Check(TokenKind::QuestionQuestion)) {
        return;
    }
    // A match that begins a statement ends at its closing brace and yields no value, so nothing postfix applies to it.
    EmitError(CurrentLocation(), std::format("'{}' cannot follow a match statement", Peek().text),
              "write 'let value = match ... { ... } catch { ... };' or '(match ... { ... }) catch { ... };' to use "
              "the match's result");
    if (Match(TokenKind::CatchKeyword)) {
        if (Match(TokenKind::LeftBrace)) {
            std::vector<MatchExpr::Arm> discarded;
            ParseArmList(discarded, "catch");
        }
    }
    else if (Match(TokenKind::QuestionQuestion)) {
        ParseExpr();
    }
    else {
        Advance();
    }
    Match(TokenKind::Semicolon);
}

std::unique_ptr<DeferStmt> Parser::ParseDeferStmt() {
    const auto loc = CurrentLocation();
    Expect(TokenKind::DeferKeyword, "expected 'defer'");

    auto s = std::make_unique<DeferStmt>();
    s->location = loc;
    // A deferred expression is never the value of its block, so it keeps its ';'.
    blockYieldsValue = false;
    s->deferredStmt = ParseStmt();
    return s;
}

bool Parser::CheckWhenDefaultArm() const noexcept {
    return Check(TokenKind::ElseKeyword) ||
           (Check(TokenKind::Ident) && Peek().text == "_" && Peek(1).Is(TokenKind::FatArrow));
}

void Parser::ParseWhenArmPatterns(std::vector<ExprPtr> &patterns) {
    // A `when` pattern is a compile-time value, so each alternative is read above `|`, which separates them as it does
    // in a match. A comma separates them too, and may end the list before `=>`.
    const bool savedStructInitAllowed = structInitAllowed;
    structInitAllowed = false;
    for (bool first = true;; first = false) {
        ExprPtr pattern = ParseBitXor();
        if (!pattern) {
            EmitMissingExpression(first ? "at the start of the 'when' arm" : "after '|' or ',' in the 'when' arm");
        }
        patterns.push_back(std::move(pattern));
        if (Match(TokenKind::Pipe)) {
            continue;
        }
        if (Match(TokenKind::Comma) && !Check(TokenKind::FatArrow)) {
            continue;
        }
        break;
    }
    structInitAllowed = savedStructInitAllowed;
}
} // namespace Rux
