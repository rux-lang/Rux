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

TEST_CASE("arm rules are unchanged around the new patterns") {
    const ParseResult trailing = ParseSource("func F() { let a = match s { value? => 0, none => 1, }; }");
    bool reported = false;
    for (const auto &diagnostic : trailing.diagnostics) {
        reported = reported || diagnostic.message == "trailing comma is not allowed in match blocks";
    }
    CHECK(reported);
    CHECK(ParseSource("func F() { match s { value? => {}, none => {} } }").diagnostics.empty());
}

TEST_CASE("formatting leaves native pattern spellings unchanged") {
    const std::string source = "func F() {\n    match s {\n        v: A | B => {},\n        value?? => {},\n"
                               "        none => {},\n        else => {}\n    }\n}\n";
    const auto formatted = Formatting::Format(source);
    CHECK_FALSE(formatted.changed);
    CHECK_EQ(formatted.text, source);
}
