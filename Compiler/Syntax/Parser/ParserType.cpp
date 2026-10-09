// Type-expression parsing.

#include "Syntax/Parser/Parser.h"

#include <memory>
#include <utility>
#include <vector>

namespace Rux {
// Type expressions

bool Parser::CanStartType() const noexcept {
    return CheckAny({TokenKind::Ident, TokenKind::Star, TokenKind::Amp, TokenKind::LeftParen, TokenKind::FuncKeyword,
                     TokenKind::SelfKeyword, TokenKind::VarKeyword, TokenKind::Bang});
}

/// A complete type: a sum, optionally followed by one `!` and the error sum, or a leading `!` and the error sum for a
/// unit success. `|` binds more tightly than `!`, so `A | B ! E | F` is `(A | B) ! (E | F)`, and a second
/// unparenthesized `!` is reported rather than read as a nested fallible.
TypeExprPtr Parser::ParseType(std::optional<std::string> help) {
    const auto loc = CurrentLocation();
    TypeExprPtr success;
    if (!Check(TokenKind::Bang)) {
        success = ParseSumType(std::move(help));
        if (!success || !Check(TokenKind::Bang)) {
            return success;
        }
    }
    Advance(); // consume '!'
    auto fallible = std::make_unique<FallibleTypeExpr>();
    fallible->location = loc;
    fallible->success = std::move(success);
    fallible->error = ParseSumType("add the error type after '!'");
    if (const auto *optional = dynamic_cast<const OptionalTypeExpr *>(fallible->error.get());
        optional && !optional->parenthesized) {
        EmitError(optional->location, "an optional error type must be grouped",
                  "write 'T ! (E?)' for an optional error, or '(T ! E)?' for an optional result");
    }
    if (Check(TokenKind::Bang)) {
        EmitError(CurrentLocation(), "a type contains at most one unparenthesized '!'",
                  "group the nested fallible, as in '(T ! E) ! F' or 'T ! (E ! F)'");
        Advance();
        ParseSumType();
    }
    return fallible;
}

/// A sum of range-level types joined by `|`. A member that is an optional, or a function type after the first member,
/// must be grouped: `A | B?` reads as either `A | (B?)` or `(A | B)?`, and a function's return type would otherwise
/// extend over the members that follow it.
TypeExprPtr Parser::ParseSumType(std::optional<std::string> help) {
    const auto loc = CurrentLocation();
    TypeExprPtr first = ParseRangeType(std::move(help));
    if (!first || !Check(TokenKind::Pipe)) {
        return first;
    }
    auto sum = std::make_unique<SumTypeExpr>();
    sum->location = loc;
    sum->members.push_back(std::move(first));
    while (Match(TokenKind::Pipe)) {
        sum->members.push_back(ParseRangeType("add a member type after '|'"));
    }
    for (std::size_t index = 0; index < sum->members.size(); ++index) {
        const TypeExpr *member = sum->members[index].get();
        if (!member || member->parenthesized) {
            continue;
        }
        if (dynamic_cast<const OptionalTypeExpr *>(member)) {
            EmitError(member->location, "an optional sum member must be grouped",
                      "write 'A | (B?)' for an optional member, or '(A | B)?' for an optional sum");
        }
        else if (index > 0 && dynamic_cast<const FunctionTypeExpr *>(member)) {
            EmitError(member->location, "a function type in a sum must be grouped",
                      "write '(func() -> T) | U' so the return type ends before the next member");
        }
    }
    return sum;
}

/// A type with the two spellings that reach past a postfix type: `var T[..]`, which qualifies a slice's elements, and
/// the range spellings `T..T`, `T..=T`, `T..`, `..T`, `..=T` and `..`, whose bounds hold the element type. A range
/// does not chain, so `int..int..int` is reported at the second operator, and a range whose element is itself a range
/// is written in parentheses. It stops before `|` and `!`, which belong to the sum and fallible levels above it.
TypeExprPtr Parser::ParseRangeType(std::optional<std::string> help) {
    const auto loc = CurrentLocation();

    // A range with no lower bound: `..T`, `..=T` and the full range `..`.
    if (Check(TokenKind::DotDot) || Check(TokenKind::DotDotEqual)) {
        auto range = std::make_unique<RangeTypeExpr>();
        range->location = loc;
        range->inclusive = Advance().kind == TokenKind::DotDotEqual;
        if (CanStartType()) {
            range->end = ParsePostfixType("add the end bound's type after the range operator");
        }
        else if (range->inclusive) {
            EmitExpected(CurrentLocation(), "a type after '..='", "an inclusive range type names its end bound");
        }
        return range;
    }

    if (Match(TokenKind::VarKeyword)) {
        auto element = ParsePostfixType("add the slice type after 'var'");
        if (auto *slice = dynamic_cast<SliceTypeExpr *>(element.get())) {
            slice->elementMut = true;
        }
        else if (const auto *optional = dynamic_cast<const OptionalTypeExpr *>(element.get());
                 optional && dynamic_cast<const SliceTypeExpr *>(optional->payload.get())) {
            EmitError(loc, "'var' in a type qualifies only a slice's elements",
                      "write '(var T[..])?' for an optional writable slice");
        }
        else if (element) {
            EmitError(loc, "'var' in a type qualifies only a slice's elements",
                      "write 'var T[..]' for a writable slice, or '*var T' for a writable pointee");
        }
        return element;
    }

    TypeExprPtr start = ParsePostfixType(std::move(help));
    if (!start || !(Check(TokenKind::DotDot) || Check(TokenKind::DotDotEqual))) {
        return start;
    }
    auto range = std::make_unique<RangeTypeExpr>();
    range->location = loc;
    range->inclusive = Advance().kind == TokenKind::DotDotEqual;
    range->start = std::move(start);
    if (CanStartType()) {
        range->end = ParsePostfixType("add the end bound's type after the range operator");
    }
    else if (range->inclusive) {
        EmitExpected(CurrentLocation(), "a type after '..='", "an inclusive range type names its end bound");
    }
    return range;
}

TypeExprPtr Parser::ParsePostfixType(std::optional<std::string> help) {
    const auto loc = CurrentLocation();
    TypeExprPtr base;

    // Function type: func(params) -> T
    if (Check(TokenKind::FuncKeyword)) {
        base = ParseFunctionType();
    }
    // Pointer: *T  or  *var T
    else if (Match(TokenKind::Star)) {
        const bool pointeeMut = Match(TokenKind::VarKeyword); // optional pointee mutability qualifier
        auto p = std::make_unique<PointerTypeExpr>();
        p->location = loc;
        p->pointeeMut = pointeeMut;
        p->pointee = ParseRangeType("add the pointee type after '*'");
        base = std::move(p);
    }
    // Reference: &T  or  &var T
    else if (Match(TokenKind::Amp)) {
        const bool pointeeMut = Match(TokenKind::VarKeyword);
        auto reference = std::make_unique<ReferenceTypeExpr>();
        reference->location = loc;
        reference->pointeeMut = pointeeMut;
        reference->pointee = ParseRangeType("add the referent type after '&'");
        base = std::move(reference);
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
            auto first = ParseType("add a type after '('");
            if (Match(TokenKind::Comma)) {
                auto tuple = std::make_unique<TupleTypeExpr>();
                tuple->location = loc;
                tuple->elements.push_back(std::move(first));
                while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
                    tuple->elements.push_back(ParseType("add a tuple element type after ','"));
                    if (!Match(TokenKind::Comma)) {
                        break;
                    }
                }
                base = std::move(tuple);
            }
            else {
                base = std::move(first);
                if (base) {
                    base->parenthesized = true;
                }
            }
        }
        ExpectBefore(TokenKind::RightParen, "')' to close the tuple type");
    }
    else {
        base = ParseBaseType(std::move(help));
    }
    if (!base) {
        return nullptr;
    }

    // Postfix suffixes apply left to right, so `int32?[..]` is a slice of optionals and `int32[..]?` an optional slice.
    // A bracket suffix is T[] (flexible tail), T[N] (fixed array) or T[..] (slice); a size is a full expression, and
    // `..` is one too, so the slice spelling is settled by the one token after '[' before any expression is read. An
    // optional suffix `?` must touch the type, as postfix propagation does, so `flag as bool ? a : b` stays a
    // conditional; the lexer's `??` is two optional suffixes here.
    while (true) {
        if ((Check(TokenKind::Question) || Check(TokenKind::QuestionQuestion)) && !Peek().precededBySpace) {
            const int levels = Advance().kind == TokenKind::QuestionQuestion ? 2 : 1;
            for (int level = 0; level < levels; ++level) {
                auto optional = std::make_unique<OptionalTypeExpr>();
                optional->location = loc;
                optional->payload = std::move(base);
                base = std::move(optional);
            }
            continue;
        }
        if (!Check(TokenKind::LeftBracket)) {
            break;
        }
        Advance();
        if (Check(TokenKind::DotDot) && Peek(1).kind == TokenKind::RightBracket) {
            Advance();
            Advance();
            auto slice = std::make_unique<SliceTypeExpr>();
            slice->location = loc;
            slice->element = std::move(base);
            base = std::move(slice);
            continue;
        }
        auto a = std::make_unique<ArrayTypeExpr>();
        a->location = loc;
        a->element = std::move(base);
        if (!Check(TokenKind::RightBracket)) {
            a->size = ParseRequiredExpr("after '[' in the array type");
        }
        ExpectBefore(TokenKind::RightBracket, "']' to close the array type");
        base = std::move(a);
    }

    return base;
}

TypeExprPtr Parser::ParseBaseType(std::optional<std::string> help) {
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
                p->segments.push_back(
                    ExpectBefore(TokenKind::Ident, "a type name after '::'", "add the next path segment").text);
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

    EmitExpected(loc, "a type", std::move(help));
    return nullptr;
}

/// func(x: int, y: int) -> bool
///
/// Parameter names are optional; only the types and the return type matter.
TypeExprPtr Parser::ParseFunctionType() {
    const auto loc = CurrentLocation();
    ExpectBefore(TokenKind::FuncKeyword, "'func' to start the function type");

    auto t = std::make_unique<FunctionTypeExpr>();
    t->location = loc;

    if (!Match(TokenKind::LeftParen)) {
        EmitExpected(CurrentLocation(), "'(' after 'func'", "write function type parameters inside parentheses");
        while (!CheckAny({TokenKind::Arrow, TokenKind::Semicolon, TokenKind::RightBrace}) && !IsAtEnd()) {
            Advance();
        }
        if (Match(TokenKind::Arrow)) {
            t->returnType = ParseType("add the function type's return type after '->'");
        }
        return t;
    }
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
        auto parameterType = ParseType("add a parameter type to the function type");
        if (!parameterType) {
            while (!CheckAny({TokenKind::Comma, TokenKind::RightParen, TokenKind::Semicolon, TokenKind::RightBrace}) &&
                   !IsAtEnd()) {
                Advance();
            }
            if (Match(TokenKind::Comma)) {
                continue;
            }
            break;
        }
        t->params.push_back(std::move(parameterType));
        if (Match(TokenKind::Comma)) {
            continue;
        }
        if (Check(TokenKind::RightParen) || IsAtEnd()) {
            break;
        }
        EmitExpected(CurrentLocation(), "',' between function type parameters",
                     "separate adjacent parameter types with ','");
    }
    ExpectBefore(TokenKind::RightParen, "')' to close the function type parameter list");

    // An omitted return arrow means the function yields no value.
    if (Match(TokenKind::Arrow)) {
        t->returnType = ParseType("add the function type's return type after '->'");
    }

    return t;
}

std::vector<TypeParameter> Parser::ParseTypeParams() {
    std::vector<TypeParameter> parameters;
    ExpectBefore(TokenKind::Less, "'<' to start the type parameter list");
    while (!CheckCloseAngle() && !IsAtEnd()) {
        if (!Check(TokenKind::Ident)) {
            EmitExpected(CurrentLocation(), "a type parameter name");
            while (!CheckAny({TokenKind::Comma, TokenKind::Greater, TokenKind::GreaterGreater,
                              TokenKind::GreaterGreaterGreater, TokenKind::LeftBrace, TokenKind::Semicolon}) &&
                   !IsAtEnd()) {
                Advance();
            }
            if (Match(TokenKind::Comma)) {
                continue;
            }
            break;
        }

        const Token parameterToken = Advance();
        TypeParameter parameter;
        parameter.location = parameterToken.location;
        parameter.name = parameterToken.text;
        if (Match(TokenKind::Colon)) {
            bool needsBound = true;
            bool attemptedBound = false;
            while (!CheckAny({TokenKind::Comma, TokenKind::Greater, TokenKind::GreaterGreater,
                              TokenKind::GreaterGreaterGreater}) &&
                   !IsAtEnd()) {
                if (!needsBound) {
                    EmitExpected(CurrentLocation(), "'+' between interface bounds",
                                 "separate multiple bounds with '+'");
                }

                attemptedBound = true;
                bool parsedBound = false;
                if (TypeExprPtr bound = ParseInterfaceBound()) {
                    parameter.bounds.push_back(std::move(bound));
                    parsedBound = true;
                }
                else {
                    while (!CheckAny({TokenKind::Plus, TokenKind::Comma, TokenKind::Greater, TokenKind::GreaterGreater,
                                      TokenKind::GreaterGreaterGreater}) &&
                           !IsAtEnd()) {
                        Advance();
                    }
                }

                needsBound = false;
                if (Match(TokenKind::Plus)) {
                    needsBound = true;
                    if (parsedBound && CheckAny({TokenKind::Comma, TokenKind::Greater, TokenKind::GreaterGreater,
                                                 TokenKind::GreaterGreaterGreater})) {
                        EmitExpected(CurrentLocation(), "an interface bound after '+'",
                                     "add the next interface name or remove the trailing '+'");
                        needsBound = false;
                    }
                    continue;
                }
                if (!Check(TokenKind::Ident)) {
                    break;
                }
            }
            if (!attemptedBound && !IsAtEnd() &&
                CheckAny({TokenKind::Comma, TokenKind::Greater, TokenKind::GreaterGreater,
                          TokenKind::GreaterGreaterGreater})) {
                EmitExpected(CurrentLocation(), "an interface bound after ':'",
                             "add an interface name after the type parameter constraint");
            }
        }
        parameters.push_back(std::move(parameter));

        if (Match(TokenKind::Comma)) {
            continue;
        }
        if (CheckCloseAngle() || IsAtEnd()) {
            break;
        }
        EmitExpected(CurrentLocation(), "',' between type parameters",
                     "separate adjacent type parameter names with ','");
    }
    if (CheckCloseAngle()) {
        ConsumeCloseAngle();
    }
    else {
        ExpectBefore(TokenKind::Greater, "'>' to close the type parameter list");
    }
    return parameters;
}

TypeExprPtr Parser::ParseInterfaceBound() {
    if (!Check(TokenKind::Ident)) {
        EmitExpected(CurrentLocation(), "an interface name in the type parameter bound",
                     "bounds name interfaces, for example 'T: Display + Debug'");
        return nullptr;
    }
    return ParseBaseType("add the interface name after ':' or '+'");
}

std::vector<TypeExprPtr> Parser::ParseTypeArgs() {
    std::vector<TypeExprPtr> args;
    ExpectBefore(TokenKind::Less, "'<' to start the type argument list");
    while (!CheckCloseAngle() && !IsAtEnd()) {
        auto argument = ParseType("add a type argument after '<' or ','");
        if (!argument) {
            while (!CheckAny({TokenKind::Comma, TokenKind::Greater, TokenKind::GreaterGreater,
                              TokenKind::GreaterGreaterGreater, TokenKind::RightParen, TokenKind::RightBrace,
                              TokenKind::Semicolon}) &&
                   !IsAtEnd()) {
                Advance();
            }
            if (Match(TokenKind::Comma)) {
                continue;
            }
            break;
        }
        args.push_back(std::move(argument));
        if (Match(TokenKind::Comma)) {
            continue;
        }
        if (CheckCloseAngle() || IsAtEnd()) {
            break;
        }
        EmitExpected(CurrentLocation(), "',' between type arguments", "separate adjacent type arguments with ','");
    }
    if (CheckCloseAngle()) {
        ConsumeCloseAngle();
    }
    else {
        ExpectBefore(TokenKind::Greater, "'>' to close the type argument list");
    }
    return args;
}

/// Language aliases are normalized so extension keys match their resolved receiver types (for
/// example, `bool[]` and `bool8[]`). The parser sits below the semantic type system, so this repeats rather than
/// reads the primitive catalog's alias table; `PrimitiveCatalogTests` fails if the two ever disagree.
static std::string NormalizePrimitiveName(const std::string &name) {
    if (name == "bool") {
        return "bool8";
    }
    if (name == "byte") {
        return "uint8";
    }
    if (name == "char") {
        return "char32";
    }
    if (name == "float") {
        return "float64";
    }
    return name;
}

std::string ImplTypeName(const TypeExpr &type) {
    if (const auto *named = dynamic_cast<const NamedTypeExpr *>(&type)) {
        std::string result = NormalizePrimitiveName(named->name);
        if (!named->typeArgs.empty()) {
            result += "<";
            for (std::size_t index = 0; index < named->typeArgs.size(); ++index) {
                if (index != 0) {
                    result += ", ";
                }
                result += ImplTypeName(*named->typeArgs[index]);
            }
            result += ">";
        }
        return result;
    }
    if (const auto *array = dynamic_cast<const ArrayTypeExpr *>(&type)) {
        return ImplTypeName(*array->element) + (array->size ? "[N]" : "[]");
    }
    if (const auto *slice = dynamic_cast<const SliceTypeExpr *>(&type)) {
        return (slice->elementMut ? "var " : "") + ImplTypeName(*slice->element) + "[..]";
    }
    if (const auto *range = dynamic_cast<const RangeTypeExpr *>(&type)) {
        return (range->start ? ImplTypeName(*range->start) : "") + (range->inclusive ? "..=" : "..") +
               (range->end ? ImplTypeName(*range->end) : "");
    }
    if (const auto *pointer = dynamic_cast<const PointerTypeExpr *>(&type)) {
        return (pointer->pointeeMut ? "*var " : "*") + ImplTypeName(*pointer->pointee);
    }
    if (const auto *reference = dynamic_cast<const ReferenceTypeExpr *>(&type)) {
        return (reference->pointeeMut ? "&var " : "&") + ImplTypeName(*reference->pointee);
    }
    if (const auto *path = dynamic_cast<const PathTypeExpr *>(&type)) {
        std::string result;
        for (std::size_t index = 0; index < path->segments.size(); ++index) {
            if (index != 0) {
                result += "::";
            }
            result += path->segments[index];
        }
        return result;
    }
    // The native forms and the unit are never extension targets, but they are named so that rejection reads well.
    const auto grouped = [](const TypeExpr *child) {
        return child->parenthesized ? "(" + ImplTypeName(*child) + ")" : ImplTypeName(*child);
    };
    if (const auto *sum = dynamic_cast<const SumTypeExpr *>(&type)) {
        std::string result;
        for (std::size_t index = 0; index < sum->members.size(); ++index) {
            result += (index != 0 ? " | " : "") + grouped(sum->members[index].get());
        }
        return result;
    }
    if (const auto *optional = dynamic_cast<const OptionalTypeExpr *>(&type)) {
        return grouped(optional->payload.get()) + "?";
    }
    if (const auto *fallible = dynamic_cast<const FallibleTypeExpr *>(&type)) {
        return (fallible->success ? grouped(fallible->success.get()) + " ! " : std::string("! ")) +
               grouped(fallible->error.get());
    }
    if (const auto *tuple = dynamic_cast<const TupleTypeExpr *>(&type); tuple && tuple->elements.empty()) {
        return "()";
    }
    return "?";
}
} // namespace Rux
