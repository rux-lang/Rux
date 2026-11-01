// Parsing of the native patterns: typed member bindings, the `none` pattern, unit, and the presence suffix `p?`, with
// the existing case, struct, tuple, guard, and default-arm patterns they compose with.

#include "Formatter/Formatter.h"
#include "Lexer/Lexer.h"
#include "Syntax/Ast/Ast.h"
#include "Syntax/Parser/Parser.h"

#include <doctest.h>
#include <string>
#include <string_view>
#include <utility>

using namespace Rux;

namespace {
ParseResult ParseSource(const std::string_view source) {
    Lexer lexer(std::string(source), "native-pattern-syntax.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "native-pattern-syntax.rux", Target::HostArch);
    return parser.Parse();
}

std::string TypeShape(const TypeExpr *type) {
    if (const auto *path = dynamic_cast<const PathTypeExpr *>(type)) {
        std::string text;
        for (std::size_t index = 0; index < path->segments.size(); ++index) {
            text += (index ? "::" : "") + path->segments[index];
        }
        return text;
    }
    if (const auto *named = dynamic_cast<const NamedTypeExpr *>(type)) {
        std::string text = named->name;
        if (!named->typeArgs.empty()) {
            text += "<";
            for (std::size_t index = 0; index < named->typeArgs.size(); ++index) {
                text += (index ? ", " : "") + TypeShape(named->typeArgs[index].get());
            }
            text += ">";
        }
        return text;
    }
    if (const auto *sum = dynamic_cast<const SumTypeExpr *>(type)) {
        std::string text = "sum(";
        for (std::size_t index = 0; index < sum->members.size(); ++index) {
            text += (index ? ", " : "") + TypeShape(sum->members[index].get());
        }
        return text + ")";
    }
    if (const auto *optional = dynamic_cast<const OptionalTypeExpr *>(type)) {
        return "opt(" + TypeShape(optional->payload.get()) + ")";
    }
    if (const auto *slice = dynamic_cast<const SliceTypeExpr *>(type)) {
        return "slice(" + TypeShape(slice->element.get()) + ")";
    }
    return "?";
}

/// A bracketed description of a parsed pattern.
std::string Shape(const Pattern *pattern) {
    if (!pattern) {
        return "<null>";
    }
    const auto list = [](const auto &children) {
        std::string text;
        for (std::size_t index = 0; index < children.size(); ++index) {
            text += (index ? ", " : "") + Shape(children[index].get());
        }
        return text;
    };
    if (const auto *ident = dynamic_cast<const IdentPattern *>(pattern)) {
        return ident->name;
    }
    if (dynamic_cast<const WildcardPattern *>(pattern)) {
        return "_";
    }
    if (const auto *literal = dynamic_cast<const LiteralPattern *>(pattern)) {
        return literal->value.text;
    }
    if (const auto *typed = dynamic_cast<const TypedPattern *>(pattern)) {
        return (typed->name.empty() ? "_" : typed->name) + ": " + TypeShape(typed->type.get());
    }
    if (const auto *presence = dynamic_cast<const PresencePattern *>(pattern)) {
        return "present(" + Shape(presence->inner.get()) + ")";
    }
    if (dynamic_cast<const NonePattern *>(pattern)) {
        return "none";
    }
    if (const auto *tuple = dynamic_cast<const TuplePattern *>(pattern)) {
        return "(" + list(tuple->elements) + ")";
    }
    if (const auto *enumerator = dynamic_cast<const EnumPattern *>(pattern)) {
        std::string text;
        for (std::size_t index = 0; index < enumerator->path.size(); ++index) {
            text += (index ? "::" : enumerator->path.size() == 1 ? "." : "") + enumerator->path[index];
        }
        return enumerator->args.empty() ? text : text + "(" + list(enumerator->args) + ")";
    }
    if (const auto *structure = dynamic_cast<const StructPattern *>(pattern)) {
        return structure->typeName + "{..}";
    }
    if (const auto *guarded = dynamic_cast<const GuardedPattern *>(pattern)) {
        return Shape(guarded->inner.get()) + " if ..";
    }
    if (const auto *range = dynamic_cast<const RangePattern *>(pattern)) {
        return Shape(range->lo.get()) + ".." + Shape(range->hi.get());
    }
    if (const auto *alternatives = dynamic_cast<const AlternativePattern *>(pattern)) {
        std::string text;
        for (std::size_t index = 0; index < alternatives->alternatives.size(); ++index) {
            text += (index ? " | " : "") + Shape(alternatives->alternatives[index].get());
        }
        return "alt(" + text + ")";
    }
    return "?";
}

/// The pattern of the first arm of `match subject { <arm> => 0, else => 1 }`, which must parse cleanly.
std::string ArmShape(const std::string_view arm) {
    const ParseResult parsed =
        ParseSource("func F() { let a = match subject { " + std::string(arm) + " => 0, else => 1 }; }");
    for (const auto &diagnostic : parsed.diagnostics) {
        FAIL_CHECK("unexpected diagnostic for '", arm, "': ", diagnostic.message);
    }
    const auto *function = dynamic_cast<const FuncDecl *>(parsed.module.items.front().get());
    REQUIRE(function != nullptr);
    const auto *let = dynamic_cast<const LetStmt *>(function->body->stmts.front().get());
    REQUIRE(let != nullptr);
    const auto *match = dynamic_cast<const MatchExpr *>(let->init.get());
    REQUIRE(match != nullptr);
    REQUIRE_EQ(match->arms.size(), 2);
    return Shape(match->arms.front().pattern.get());
}

bool ArmReports(const std::string_view arm, const std::string_view message) {
    const ParseResult parsed =
        ParseSource("func F() { let a = match subject { " + std::string(arm) + " => 0, else => 1 }; }");
    for (const auto &diagnostic : parsed.diagnostics) {
        if (diagnostic.message == message) {
            return true;
        }
    }
    return false;
}
} // namespace

TEST_CASE("typed patterns bind a name or select without binding") {
    CHECK_EQ(ArmShape("options: Options"), "options: Options");
    CHECK_EQ(ArmShape("_: Options"), "_: Options");
    CHECK_EQ(ArmShape("v: Options if v.verbose"), "v: Options if ..");
    CHECK_EQ(ArmShape("v: Vector<A | B>"), "v: Vector<sum(A, B)>");
    CHECK_EQ(ArmShape("v: Text::View"), "v: Text::View");
}

TEST_CASE("a typed annotation is a complete type") {
    // `|` needs no grouping, and a tight `?` belongs to the type rather than being a presence suffix.
    CHECK_EQ(ArmShape("any: Options | Defaults"), "any: sum(Options, Defaults)");
    CHECK_EQ(ArmShape("stored: int32?"), "stored: opt(int32)");
    CHECK_EQ(ArmShape("view: int32[..]?"), "view: opt(slice(int32))");
    CHECK_EQ(ArmShape(".Success(v: A | B)"), ".Success(v: sum(A, B))");
    CHECK_EQ(ArmShape("(left: A, right)"), "(left: A, right)");
}

TEST_CASE("native case patterns keep the existing case pattern shape") {
    CHECK_EQ(ArmShape(".Success(value)"), ".Success(value)");
    CHECK_EQ(ArmShape(".Failure(e: ParseError)"), ".Failure(e: ParseError)");
    CHECK_EQ(ArmShape(".Some(.Some(value))"), ".Some(.Some(value))");
    CHECK_EQ(ArmShape(".Success(())"), ".Success(())");
    CHECK_EQ(ArmShape("DecodeError::Missing"), "DecodeError::Missing");
    CHECK_EQ(ArmShape("DecodeError::InvalidDigit(position)"), "DecodeError::InvalidDigit(position)");
}

TEST_CASE("none is a pattern and never a binding") {
    CHECK_EQ(ArmShape("none"), "none");
    CHECK_EQ(ArmShape(".Some(none)"), ".Some(none)");
    CHECK_EQ(ArmShape("()"), "()");
}

TEST_CASE("the presence suffix wraps one level per question mark") {
    CHECK_EQ(ArmShape("value?"), "present(value)");
    CHECK_EQ(ArmShape("_?"), "present(_)");
    CHECK_EQ(ArmShape("none?"), "present(none)");
    CHECK_EQ(ArmShape("value??"), "present(present(value))");
    CHECK_EQ(ArmShape("7i32?"), "present(7i32)");
    CHECK_EQ(ArmShape(".Ok(x)?"), "present(.Ok(x))");
    CHECK_EQ(ArmShape("Point { x: a }?"), "present(Point{..})");
    CHECK_EQ(ArmShape("(a, b)?"), "present((a, b))");
    CHECK_EQ(ArmShape("value? if value > 0"), "present(value) if ..");
    CHECK_EQ(ArmShape(".Success(options?)"), ".Success(present(options))");
}

TEST_CASE("the presence suffix is rejected where its meaning would be unclear") {
    CHECK(ArmReports("value ?", "a presence suffix must touch its pattern"));
    CHECK(ArmReports("v: A ?", "a presence suffix cannot follow a typed pattern"));
    CHECK(ArmReports("1..10?", "a range bound cannot take a presence suffix"));
    CHECK(ArmReports("1?..10", "a range bound cannot take a presence suffix"));
}

TEST_CASE("arm lists take a trailing comma, and a block arm needs no comma") {
    CHECK(ParseSource("func F() { let a = match s { value? => 0, none => 1, }; }").diagnostics.empty());
    CHECK(ParseSource("func F() { let a = match s { .Some(v) => 0, none => 1, }; }").diagnostics.empty());
    CHECK(ParseSource("func F() { match s { value? => {}, none => {} } }").diagnostics.empty());
    CHECK(ParseSource("func F() { match s { .Some(v) => {} none => {} } }").diagnostics.empty());
    bool reported = false;
    for (const auto &diagnostic : ParseSource("func F() { let a = match s { none => 1 _ => 2 }; }").diagnostics) {
        reported = reported || diagnostic.message.starts_with("expected ',' between match arms");
    }
    CHECK(reported);
}

TEST_CASE("alternatives join whole patterns with '|' and a guard covers all of them") {
    CHECK_EQ(ArmShape(".FreeBSD | .Linux"), "alt(.FreeBSD | .Linux)");
    CHECK_EQ(ArmShape("1 | 2 | 3"), "alt(1 | 2 | 3)");
    CHECK_EQ(ArmShape("none | .Some(0)"), "alt(none | .Some(0))");
    CHECK_EQ(ArmShape(".Circle(1 | 2)"), ".Circle(alt(1 | 2))");
    CHECK_EQ(ArmShape("0..=9 | 20..=29"), "alt(0..9 | 20..29)");
    CHECK_EQ(ArmShape(".A | .B if ready"), "alt(.A | .B) if ..");
    // A typed annotation keeps reading its `|` as the sum type.
    CHECK_EQ(ArmShape("v: A | B"), "v: sum(A, B)");
    bool reported = false;
    for (const auto &diagnostic : ParseSource("func F() { let a = match s { .A | => 0, _ => 1 }; }").diagnostics) {
        reported = reported || diagnostic.message.starts_with("expected a pattern after '|'");
    }
    CHECK(reported);
}

TEST_CASE("'_' is the default arm, and it is last and unguarded") {
    CHECK(ParseSource("func F() { let a = match s { 1 => 0, _ => 1 }; }").diagnostics.empty());
    CHECK(ParseSource("func F() { let a = match s { 1 => 0, else => 1 }; }").diagnostics.empty());
    CHECK(ArmReports("_ if ready", "the default arm '_' cannot have a guard"));
    const auto reports = [](const std::string_view source, const std::string_view message) {
        for (const auto &diagnostic : ParseSource(source).diagnostics) {
            if (diagnostic.message == message) {
                return true;
            }
        }
        return false;
    };
    CHECK(reports("func F() { let a = match s { _ => 0, 1 => 1 }; }", "the default arm must be the last match arm"));
    CHECK(reports("func F() { match s { else => {} 1 => {} } }", "the default arm must be the last match arm"));
    CHECK(
        reports("func F() { let a = Read() catch { _ => 0, e => 1 }; }", "the default arm must be the last catch arm"));
}

TEST_CASE("formatting leaves native pattern spellings unchanged") {
    const std::string source = "func F() {\n    match s {\n        v: A | B => {},\n        value?? => {},\n"
                               "        none => {},\n        else => {}\n    }\n}\n";
    const auto formatted = Formatting::Format(source);
    CHECK_FALSE(formatted.changed);
    CHECK_EQ(formatted.text, source);
}

namespace {
/// The initializer of `let probe = <expression>;` in a function body, which must parse cleanly.
const Expr *ParsedInitializer(const ParseResult &parsed) {
    for (const auto &diagnostic : parsed.diagnostics) {
        FAIL_CHECK("unexpected diagnostic: ", diagnostic.message);
    }
    const auto *function = dynamic_cast<const FuncDecl *>(parsed.module.items.front().get());
    REQUIRE(function != nullptr);
    const auto *let = dynamic_cast<const LetStmt *>(function->body->stmts.front().get());
    REQUIRE(let != nullptr);
    return let->init.get();
}

/// What follows `is` in `let probe = subject is <test>;`: a pattern's shape, or `type` for the type form.
std::string IsShape(const std::string_view test) {
    const ParseResult parsed = ParseSource("func F() { let probe = subject is " + std::string(test) + "; }");
    const auto *is = dynamic_cast<const IsExpr *>(ParsedInitializer(parsed));
    REQUIRE(is != nullptr);
    return is->pattern ? Shape(is->pattern.get()) : "type " + TypeShape(is->type.get());
}

bool Reports(const std::string_view source, const std::string_view message) {
    return std::ranges::any_of(ParseSource(source).diagnostics,
                               [&](const ParserDiagnostic &diagnostic) { return diagnostic.message == message; });
}
} // namespace

TEST_CASE("'is' takes a pattern, and a type keeps the type test") {
    CHECK_EQ(IsShape(".Some(value)"), ".Some(value)");
    CHECK_EQ(IsShape("none"), "none");
    CHECK_EQ(IsShape("Shape::Circle(radius)"), "Shape::Circle(radius)");
    CHECK_EQ(IsShape("circle: Circle"), "circle: Circle");
    CHECK_EQ(IsShape("_: A | B"), "_: sum(A, B)");
    CHECK_EQ(IsShape("'a'..='z'"), "'a'..'z'");
    CHECK_EQ(IsShape(".Red | .Green"), "alt(.Red | .Green)");
    CHECK_EQ(IsShape("(0, y)"), "(0, y)");
    CHECK_EQ(IsShape("Point { x, y }"), "Point{..}");
    CHECK_EQ(IsShape("Circle"), "type Circle");
    CHECK_EQ(IsShape("Shapes::Circle"), "type Shapes::Circle");
    CHECK_EQ(IsShape("int32?"), "type opt(int32)");
}

TEST_CASE("an 'is' pattern before a body leaves the brace to the body") {
    const ParseResult parsed =
        ParseSource("func F() { if s is .Done { return; } while q.Pop() is .Some(job) { Run(job); } }");
    CHECK(parsed.diagnostics.empty());
    const auto *function = dynamic_cast<const FuncDecl *>(parsed.module.items.front().get());
    REQUIRE(function != nullptr);
    REQUIRE_EQ(function->body->stmts.size(), 2);
    const auto *statement = dynamic_cast<const IfStmt *>(function->body->stmts[0].get());
    REQUIRE(statement != nullptr);
    const auto *test = dynamic_cast<const IsExpr *>(statement->condition.get());
    REQUIRE(test != nullptr);
    CHECK_EQ(Shape(test->pattern.get()), ".Done");
    // The conditional operator's ':' is not a typed pattern's.
    const ParseResult conditional = ParseSource("func F() { let probe = flag ? value is Circle : false; }");
    const auto *ternary = dynamic_cast<const TernaryExpr *>(ParsedInitializer(conditional));
    REQUIRE(ternary != nullptr);
    const auto *typeTest = dynamic_cast<const IsExpr *>(ternary->thenExpr.get());
    REQUIRE(typeTest != nullptr);
    CHECK(typeTest->type != nullptr);
}

TEST_CASE("an 'if' in value position is a match on its condition") {
    const ParseResult parsed = ParseSource("func F() { let probe = if a { 1 } else if b { 2 } else { 3 }; }");
    const auto *value = dynamic_cast<const MatchExpr *>(ParsedInitializer(parsed));
    REQUIRE(value != nullptr);
    CHECK(value->form == MatchExpr::Form::If);
    REQUIRE_EQ(value->arms.size(), 2);
    CHECK_EQ(Shape(value->arms[0].pattern.get()), "true");
    CHECK_EQ(Shape(value->arms[1].pattern.get()), "_");
    const auto *then = dynamic_cast<const BlockExpr *>(value->arms[0].body.get());
    REQUIRE(then != nullptr);
    const auto *yielded = dynamic_cast<const ExprStmt *>(then->block->stmts.back().get());
    REQUIRE(yielded != nullptr);
    CHECK(yielded->yieldsValue);
    const auto *elseIf = dynamic_cast<const MatchExpr *>(value->arms[1].body.get());
    REQUIRE(elseIf != nullptr);
    CHECK(elseIf->form == MatchExpr::Form::If);

    CHECK(Reports("func F() { let probe = if a { 1 }; }", "an 'if' used as a value needs an 'else' branch"));
    // Only a value block may leave out the last ';'; a function body and a loop body may not.
    CHECK(Reports("func F() -> int { 1 }", "expected ';' after expression, but found '}'"));
    CHECK(Reports("func F() { while a { Run() } }", "expected ';' after expression, but found '}'"));
    CHECK_FALSE(Reports("func F() { if a { Run() } else { Stop() } }", "expected ';' after expression, but found '}'"));
}

TEST_CASE("a 'for' pattern takes the element apart in the body, and refutable patterns are refused") {
    const ParseResult parsed = ParseSource("func F() { for (key, value) in pairs { Use(key); } }");
    CHECK(parsed.diagnostics.empty());
    const auto *function = dynamic_cast<const FuncDecl *>(parsed.module.items.front().get());
    REQUIRE(function != nullptr);
    const auto *loop = dynamic_cast<const ForStmt *>(function->body->stmts.front().get());
    REQUIRE(loop != nullptr);
    REQUIRE_EQ(loop->body->stmts.size(), 2);
    const auto *destructure = dynamic_cast<const LetStmt *>(loop->body->stmts.front().get());
    REQUIRE(destructure != nullptr);
    CHECK_EQ(Shape(destructure->pattern.get()), "(key, value)");
    const auto *transfer = dynamic_cast<const MoveExpr *>(destructure->init.get());
    REQUIRE(transfer != nullptr);
    const auto *item = dynamic_cast<const IdentExpr *>(transfer->operand.get());
    REQUIRE(item != nullptr);
    CHECK_EQ(item->name, loop->variable);

    const auto refused = [](const std::string_view pattern) {
        return Reports("func F() { for " + std::string(pattern) + " in items {} }",
                       "a 'for' pattern must match every element");
    };
    CHECK_FALSE(refused("Point { x, y }"));
    CHECK_FALSE(refused("(_, (a, b))"));
    CHECK(refused(".Some(value)"));
    CHECK(refused("(1, value)"));
    CHECK(refused("value: int32"));
}

TEST_CASE("'...' is no range operator, while the variadic marker and spread keep it") {
    const auto messages = [](const std::string_view source) {
        std::vector<std::string> found;
        for (const auto &diagnostic : ParseSource(source).diagnostics) {
            found.push_back(diagnostic.message + " / " + diagnostic.help.value_or(""));
        }
        return found;
    };
    CHECK(messages("func F(values: int32...) { G(values...); let r = 1..=5; }").empty());
    const auto ranges = messages("func F() { let a = 1...5; let b = ...5; match x { 1...4 => {} _ => {} } }");
    CHECK_EQ(ranges.size(), 3);
    CHECK(std::ranges::all_of(
        ranges, [](const std::string &text) { return text == "'...' is not a range operator / use '..='"; }));
}
