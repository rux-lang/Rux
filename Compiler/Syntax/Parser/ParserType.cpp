// Type-expression parsing.

#include "Syntax/Parser/Parser.h"

#include <memory>
#include <utility>
#include <vector>

namespace Rux {
// Type expressions
TypeExprPtr Parser::ParseType(std::optional<std::string> help) {
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
        p->pointee = ParseType("add the pointee type after '*'");
        base = std::move(p);
    }
    // Reference: &T  or  &var T
    else if (Match(TokenKind::Amp)) {
        const bool pointeeMut = Match(TokenKind::VarKeyword);
        auto reference = std::make_unique<ReferenceTypeExpr>();
        reference->location = loc;
        reference->pointeeMut = pointeeMut;
        reference->pointee = ParseType("add the referent type after '&'");
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

    // Postfix inline-array suffix: T[] (flexible tail) or T[N] (fixed)
    while (Check(TokenKind::LeftBracket)) {
        Advance();
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

/// Language aliases are normalized so extension and intrinsic binding keys match their resolved receiver types (for
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
    return "?";
}
} // namespace Rux
