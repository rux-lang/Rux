// Compile-time evaluation of constants whose initializers call functions: blocks, statements, loops and patterns.

#include "Lexer/Lexer.h"
#include "Numeric/IntegerLiteral.h"
#include "Semantic/Analysis/ConstantCallEvaluator.h"

#include <algorithm>
#include <format>

namespace Rux::SemanticDetail {
using Evaluation::TypedConstant;

void ConstantCallEvaluator::PushScope() {
    frames.back().scopes.emplace_back();
}

void ConstantCallEvaluator::PopScope() {
    frames.back().scopes.pop_back();
}

ConstantCallEvaluator::Completion ConstantCallEvaluator::Abrupt() {
    if (failure)
        return Completion::Failed;
    if (unwinding != Completion::Normal)
        return unwinding;
    static_cast<void>(Fail(SourceLocation{}, "an expression produced no value"));
    return Completion::Failed;
}

std::optional<EvaluatedValue> ConstantCallEvaluator::EvaluateBlock(const Block &block) {
    std::optional<EvaluatedValue> yielded;
    if (ExecuteBlock(block, &yielded) != Completion::Normal)
        return std::nullopt;
    return yielded ? std::move(yielded) : std::optional{MakeUnit()};
}

ConstantCallEvaluator::Completion ConstantCallEvaluator::ExecuteBlock(const Block &block,
                                                                      std::optional<EvaluatedValue> *yielded) {
    PushScope();
    Completion completion = Completion::Normal;
    for (const StmtPtr &statement : block.stmts) {
        completion = ExecuteStatement(*statement, yielded);
        if (completion != Completion::Normal)
            break;
    }
    PopScope();
    return completion;
}

ConstantCallEvaluator::Completion ConstantCallEvaluator::ExecuteLoopBody(const Block &body, const std::string &label,
                                                                         bool &leave) {
    const Completion completion = ExecuteBlock(body);
    if ((completion == Completion::Break || completion == Completion::Continue) &&
        (unwindingLabel.empty() || unwindingLabel == label)) {
        leave = completion == Completion::Break;
        unwinding = Completion::Normal;
        unwindingLabel.clear();
        return Completion::Normal;
    }
    return completion;
}

ConstantCallEvaluator::Completion ConstantCallEvaluator::ExecuteStatement(const Stmt &statement,
                                                                          std::optional<EvaluatedValue> *yielded) {
    if (!Step(statement.location))
        return Completion::Failed;
    if (const auto *expression = AstCast<const ExprStmt>(&statement)) {
        auto value = EvaluateExpression(*expression->expr);
        if (!value)
            return Abrupt();
        if (expression->yieldsValue && yielded)
            *yielded = std::move(value);
        return Completion::Normal;
    }
    if (const auto *let = AstCast<const LetStmt>(&statement))
        return ExecuteLet(*let);
    if (const auto *conditional = AstCast<const IfStmt>(&statement))
        return ExecuteIf(*conditional);
    if (const auto *result = AstCast<const ReturnStmt>(&statement)) {
        std::optional<EvaluatedValue> value = result->value ? EvaluateExpression(**result->value) : MakeUnit();
        if (!value)
            return Abrupt();
        returned = std::move(value);
        unwinding = Completion::Return;
        return Completion::Return;
    }
    return ExecuteCompound(statement);
}

ConstantCallEvaluator::Completion ConstantCallEvaluator::ExecuteCompound(const Stmt &statement) {
    if (const auto *loop = AstCast<const WhileStmt>(&statement)) {
        for (;;) {
            if (!Step(loop->location))
                return Completion::Failed;
            PushScope();
            const auto condition = EvaluateExpression(*loop->condition);
            const auto truth = condition ? Truth(*condition, loop->condition->location) : std::nullopt;
            if (!truth || !*truth) {
                PopScope();
                return truth ? Completion::Normal : Abrupt();
            }
            bool leave = false;
            const Completion completion = ExecuteLoopBody(*loop->body, loop->label, leave);
            PopScope();
            if (completion != Completion::Normal)
                return completion;
            if (leave)
                return Completion::Normal;
        }
    }
    if (const auto *loop = AstCast<const DoWhileStmt>(&statement)) {
        for (;;) {
            if (!Step(loop->location))
                return Completion::Failed;
            bool leave = false;
            const Completion completion = ExecuteLoopBody(*loop->body, loop->label, leave);
            if (completion != Completion::Normal)
                return completion;
            if (leave)
                return Completion::Normal;
            const auto condition = EvaluateExpression(*loop->condition);
            const auto truth = condition ? Truth(*condition, loop->condition->location) : std::nullopt;
            if (!truth)
                return Abrupt();
            if (!*truth)
                return Completion::Normal;
        }
    }
    if (const auto *loop = AstCast<const LoopStmt>(&statement)) {
        for (;;) {
            if (!Step(loop->location))
                return Completion::Failed;
            bool leave = false;
            const Completion completion = ExecuteLoopBody(*loop->body, loop->label, leave);
            if (completion != Completion::Normal)
                return completion;
            if (leave)
                return Completion::Normal;
        }
    }
    if (const auto *loop = AstCast<const ForStmt>(&statement))
        return ExecuteFor(*loop);
    if (const auto *match = AstCast<const MatchStmt>(&statement)) {
        auto subject = EvaluateExpression(*match->subject);
        if (!subject)
            return Abrupt();
        for (const auto &arm : match->arms) {
            PushScope();
            const auto matched = Matches(*arm.pattern, *subject);
            if (!matched) {
                PopScope();
                return Abrupt();
            }
            if (*matched) {
                const auto value = EvaluateExpression(*arm.body);
                PopScope();
                return value ? Completion::Normal : Abrupt();
            }
            PopScope();
        }
        static_cast<void>(Fail(match->location, "no arm of the 'match' takes its value"));
        return Completion::Failed;
    }
    if (const auto *exit = AstCast<const BreakStmt>(&statement)) {
        unwinding = Completion::Break;
        unwindingLabel = exit->label;
        return Completion::Break;
    }
    if (const auto *next = AstCast<const ContinueStmt>(&statement)) {
        unwinding = Completion::Continue;
        unwindingLabel = next->label;
        return Completion::Continue;
    }
    if (const auto *declaration = AstCast<const DeclStmt>(&statement)) {
        // A local constant is computed where it is read, like any other; a local type names nothing to run.
        if (AstCast<const ConstDecl>(declaration->decl.get()) || AstCast<const TypeAliasDecl>(declaration->decl.get()))
            return Completion::Normal;
        static_cast<void>(Unsupported(statement.location, "this declaration"));
        return Completion::Failed;
    }
    if (const auto *thrown = AstCast<const FailStmt>(&statement)) {
        static_cast<void>(Fail(thrown->location, std::format("{} throws an error of type '{}'", Actor(),
                                                             ValueType(TypeOf(*thrown->value)).ToString())));
        return Completion::Failed;
    }
    if (AstCast<const DeferStmt>(&statement)) {
        static_cast<void>(Unsupported(statement.location, "'defer'"));
        return Completion::Failed;
    }
    if (AstCast<const TryStmt>(&statement)) {
        static_cast<void>(Unsupported(statement.location, "a 'try' block"));
        return Completion::Failed;
    }
    static_cast<void>(Unsupported(statement.location, "this statement"));
    return Completion::Failed;
}

ConstantCallEvaluator::Completion ConstantCallEvaluator::ExecuteLet(const LetStmt &let) {
    const TypeRef declared = let.type ? ValueType(DeclaredType(**let.type)) : TypeRef::MakeUnknown();
    const bool alias = let.type && DeclaredType(**let.type)->kind == TypeRef::Kind::Reference;
    std::optional<EvaluatedValue> value;
    if (let.init && alias) {
        value = Argument(*let.init, DeclaredType(**let.type));
    }
    else if (let.init) {
        value = EvaluateAs(*let.init, declared.IsUnknown() ? ValueType(TypeOf(*let.init)) : declared);
    }
    else if (const auto found = context.defaultConstructors.find(&let); found != context.defaultConstructors.end()) {
        value = Invoke(*found->second.declaration, {}, nullptr, {}, let.location);
    }
    else {
        value = DefaultValue(declared, let.location);
    }
    if (!value)
        return Abrupt();
    if (let.pattern && !AstCast<const IdentPattern>(let.pattern.get())) {
        const auto matched = Matches(*let.pattern, *value);
        if (!matched)
            return Abrupt();
        if (!*matched) {
            static_cast<void>(Fail(let.location, "the value does not match the pattern"));
            return Completion::Failed;
        }
        return Completion::Normal;
    }
    const auto *named = AstCast<const IdentPattern>(let.pattern.get());
    Declare(named ? named->name : let.name, std::move(*value));
    return Completion::Normal;
}

ConstantCallEvaluator::Completion ConstantCallEvaluator::ExecuteIf(const IfStmt &statement) {
    if (statement.isCompileTime) {
        static_cast<void>(Unsupported(statement.location, "a 'when' statement left unfolded"));
        return Completion::Failed;
    }
    // Names an `is` test binds are seen by the branch it guards, so each condition opens a scope.
    const auto branch = [&](const Expr &condition, const Block &block, bool &taken) {
        PushScope();
        const auto value = EvaluateExpression(condition);
        const auto truth = value ? Truth(*value, condition.location) : std::nullopt;
        Completion completion = Completion::Normal;
        if (!truth)
            completion = Abrupt();
        else if (*truth) {
            taken = true;
            completion = ExecuteBlock(block);
        }
        PopScope();
        return completion;
    };
    bool taken = false;
    Completion completion = branch(*statement.condition, *statement.thenBlock, taken);
    for (const auto &alternative : statement.elseIfs) {
        if (taken || completion != Completion::Normal)
            return completion;
        completion = branch(*alternative.condition, *alternative.block, taken);
    }
    if (!taken && completion == Completion::Normal && statement.elseBlock)
        return ExecuteBlock(*statement.elseBlock);
    return completion;
}

ConstantCallEvaluator::Completion ConstantCallEvaluator::ExecuteFor(const ForStmt &statement) {
    const auto iteration = context.iterations.find(&statement);
    if (iteration == context.iterations.end()) {
        static_cast<void>(Unsupported(statement.location, "this 'for' loop"));
        return Completion::Failed;
    }
    const TypeRef itemType = Substitute(iteration->second.itemType);
    // Runs the body once with the loop variable bound to `item`; `stop` is set when the loop is left.
    const auto pass = [&](EvaluatedValue item, bool &stop) {
        if (!Step(statement.location))
            return Completion::Failed;
        PushScope();
        Declare(statement.variable, std::move(item));
        const Completion completion = ExecuteLoopBody(*statement.body, statement.label, stop);
        PopScope();
        return completion;
    };
    if (iteration->second.kind == ResolvedIteration::Kind::Range) {
        const auto *range = AstCast<const RangeExpr>(statement.iterable.get());
        if (!range || !range->lo || !range->hi) {
            static_cast<void>(Unsupported(statement.location, "a 'for' loop over this range"));
            return Completion::Failed;
        }
        const TypeRef type = ValueType(itemType);
        auto current = EvaluateAs(*range->lo, type);
        if (!current)
            return Abrupt();
        const auto end = EvaluateAs(*range->hi, type);
        if (!end)
            return Abrupt();
        if (current->kind != EvaluatedValue::Kind::Scalar || end->kind != EvaluatedValue::Kind::Scalar) {
            static_cast<void>(Unsupported(statement.location, "a 'for' loop over this range"));
            return Completion::Failed;
        }
        const auto one = MakeInteger("1", type, statement.location);
        if (!one)
            return Completion::Failed;
        for (;;) {
            const auto more = Evaluation::EvaluateBinary(range->inclusive ? TokenKind::LessEqual : TokenKind::Less,
                                                         *current->scalar, *end->scalar);
            if (!more || !more->BooleanValue().value_or(false))
                return Completion::Normal;
            bool stop = false;
            const Completion completion = pass(*current, stop);
            if (completion != Completion::Normal)
                return completion;
            // An inclusive range ending at the type's maximum ends without stepping past it.
            const auto last = Evaluation::EvaluateBinary(TokenKind::Equal, *current->scalar, *end->scalar);
            if (stop || (last && last->BooleanValue().value_or(false)))
                return Completion::Normal;
            const auto next = Evaluation::EvaluateBinary(TokenKind::Plus, *current->scalar, *one->scalar);
            if (!next)
                return Completion::Normal;
            current->scalar = std::make_shared<const TypedConstant>(*next);
        }
    }
    if (iteration->second.kind == ResolvedIteration::Kind::Indexed) {
        auto subject = EvaluateExpression(*statement.iterable);
        if (!subject)
            return Abrupt();
        if (subject->kind != EvaluatedValue::Kind::Array) {
            static_cast<void>(Unsupported(statement.location, "a 'for' loop over this value"));
            return Completion::Failed;
        }
        for (EvaluatedValue &element : subject->elements) {
            bool stop = false;
            const Completion completion = pass(std::move(element), stop);
            if (completion != Completion::Normal)
                return completion;
            if (stop)
                return Completion::Normal;
        }
        return Completion::Normal;
    }
    static_cast<void>(Unsupported(statement.location, "a 'for' loop driven by an iterator"));
    return Completion::Failed;
}

std::optional<EvaluatedValue> ConstantCallEvaluator::EvaluateMatch(const MatchExpr &match) {
    PushScope();
    auto subject = EvaluateExpression(*match.subject);
    if (!subject) {
        PopScope();
        return std::nullopt;
    }
    const TypeRef type = ValueType(TypeOf(match));
    for (const auto &arm : match.arms) {
        PushScope();
        const auto matched = Matches(*arm.pattern, *subject);
        if (!matched || *matched) {
            std::optional<EvaluatedValue> value = matched ? EvaluateExpression(*arm.body) : std::nullopt;
            PopScope();
            PopScope();
            if (!value || value->kind == EvaluatedValue::Kind::Unit || type.IsUnit())
                return value;
            return Coerce(std::move(*value), type, arm.body->location);
        }
        PopScope();
    }
    PopScope();
    return Fail(match.location, "no arm of the 'match' takes its value");
}

std::optional<bool> ConstantCallEvaluator::MatchLiteral(const Token &token, const EvaluatedValue &value,
                                                        const SourceLocation location) {
    if (value.kind != EvaluatedValue::Kind::Scalar)
        return Unsupported(location, std::format("matching a literal against '{}'", value.type.ToString()));
    std::optional<TypedConstant> literal;
    const TypeRef model = ScalarModelType(value.type);
    if (token.kind == TokenKind::CharLiteral) {
        if (const auto codePoint = Lexer::DecodeCharLiteralCodePoint(token.text))
            literal = Evaluation::ParseConstant(std::to_string(*codePoint), model);
    }
    else if (token.kind == TokenKind::IntLiteral || token.kind == TokenKind::FloatLiteral ||
             token.kind == TokenKind::BoolLiteral) {
        std::string_view text = token.text;
        text.remove_suffix(NumericLiteralSuffixOf(text).size());
        literal = Evaluation::ParseConstant(text, model);
    }
    if (!literal)
        return Unsupported(location, std::format("the pattern '{}'", token.text));
    const auto equal = Evaluation::EvaluateBinary(TokenKind::Equal, *value.scalar, *literal);
    if (!equal)
        return Unsupported(location, std::format("the pattern '{}'", token.text));
    return equal->BooleanValue();
}

std::optional<bool> ConstantCallEvaluator::Matches(const Pattern &pattern, const EvaluatedValue &value) {
    if (failure)
        return std::nullopt;
    if (AstCast<const WildcardPattern>(&pattern))
        return true;
    if (const auto *binding = AstCast<const IdentPattern>(&pattern)) {
        Declare(binding->name, value);
        return true;
    }
    if (const auto *literal = AstCast<const LiteralPattern>(&pattern))
        return MatchLiteral(literal->value, value, pattern.location);
    if (const auto *range = AstCast<const RangePattern>(&pattern)) {
        const auto *low = AstCast<const LiteralPattern>(range->lo.get());
        const auto *high = AstCast<const LiteralPattern>(range->hi.get());
        if (value.kind != EvaluatedValue::Kind::Scalar || (range->lo && !low) || (range->hi && !high))
            return Unsupported(pattern.location, "this range pattern");
        const TypeRef model = ScalarModelType(value.type);
        const auto bound = [&](const LiteralPattern &end) -> std::optional<TypedConstant> {
            if (end.value.kind == TokenKind::CharLiteral) {
                const auto codePoint = Lexer::DecodeCharLiteralCodePoint(end.value.text);
                return codePoint ? Evaluation::ParseConstant(std::to_string(*codePoint), model) : std::nullopt;
            }
            std::string_view text = end.value.text;
            text.remove_suffix(NumericLiteralSuffixOf(text).size());
            return Evaluation::ParseConstant(text, model);
        };
        if (low) {
            const auto lower = bound(*low);
            const auto above =
                lower ? Evaluation::EvaluateBinary(TokenKind::GreaterEqual, *value.scalar, *lower) : std::nullopt;
            if (!above)
                return Unsupported(pattern.location, "this range pattern");
            if (!above->BooleanValue().value_or(false))
                return false;
        }
        if (high) {
            const auto upper = bound(*high);
            const auto below = upper
                                 ? Evaluation::EvaluateBinary(range->inclusive ? TokenKind::LessEqual : TokenKind::Less,
                                                              *value.scalar, *upper)
                                 : std::nullopt;
            if (!below)
                return Unsupported(pattern.location, "this range pattern");
            return below->BooleanValue();
        }
        return true;
    }
    if (const auto *selected = AstCast<const EnumPattern>(&pattern)) {
        const auto resolved = context.casePatterns.find(selected);
        if (resolved == context.casePatterns.end() || value.kind != EvaluatedValue::Kind::Case)
            return Unsupported(pattern.location, "this case pattern");
        if (resolved->second.selectedCase != value.selectedCase)
            return false;
        for (std::size_t index = 0; index < selected->args.size(); ++index) {
            if (index >= value.elements.size())
                return Unsupported(pattern.location, "this case pattern");
            const auto matched = Matches(*selected->args[index], value.elements[index]);
            if (!matched || !*matched)
                return matched;
        }
        for (const auto &argument : selected->namedArgs) {
            const auto &fields = value.selectedCase->namedFields;
            const auto field = std::ranges::find(fields, argument.name, &EnumDecl::Variant::NamedField::name);
            const auto position = static_cast<std::size_t>(field - fields.begin());
            if (field == fields.end() || position >= value.elements.size())
                return Unsupported(pattern.location, "this case pattern");
            if (!argument.pattern) {
                Declare(argument.name, value.elements[position]);
                continue;
            }
            const auto matched = Matches(*argument.pattern, value.elements[position]);
            if (!matched || !*matched)
                return matched;
        }
        return true;
    }
    if (const auto *structure = AstCast<const StructPattern>(&pattern)) {
        if (value.kind != EvaluatedValue::Kind::Struct)
            return Unsupported(pattern.location, "this struct pattern");
        for (const auto &field : structure->fields) {
            const auto position = value.FieldIndex(field.name);
            if (!position)
                return Unsupported(pattern.location, "this struct pattern");
            const EvaluatedValue &part = value.elements[*position];
            if (!field.pattern) {
                Declare(field.name, part);
                continue;
            }
            const auto matched = Matches(*field.pattern, part);
            if (!matched || !*matched)
                return matched;
        }
        return true;
    }
    if (const auto *tuple = AstCast<const TuplePattern>(&pattern)) {
        if (value.kind != EvaluatedValue::Kind::Tuple || value.elements.size() != tuple->elements.size())
            return Unsupported(pattern.location, "this tuple pattern");
        for (std::size_t index = 0; index < tuple->elements.size(); ++index) {
            const auto matched = Matches(*tuple->elements[index], value.elements[index]);
            if (!matched || !*matched)
                return matched;
        }
        return true;
    }
    if (const auto *alternatives = AstCast<const AlternativePattern>(&pattern)) {
        for (const PatternPtr &alternative : alternatives->alternatives) {
            const auto matched = Matches(*alternative, value);
            if (!matched || *matched)
                return matched;
        }
        return false;
    }
    if (const auto *guarded = AstCast<const GuardedPattern>(&pattern)) {
        const auto matched = Matches(*guarded->inner, value);
        if (!matched || !*matched)
            return matched;
        const auto guard = EvaluateExpression(*guarded->guard);
        return guard ? Truth(*guard, guarded->guard->location) : std::nullopt;
    }
    return Unsupported(pattern.location, "a pattern over a native optional, fallible or sum");
}
} // namespace Rux::SemanticDetail
