// What `Self` and a shorthand receiver name: the type an `extend` block extends, or an interface's implementing type.
//
// Both are spellings of a type the reader already has in view, so the parser writes that type in their place. Inside
// `extend Vector<T>`, `&self` is `self: &Vector<T>`, `Self` is `Vector<T>`, `Self { ... }` is `Vector<T> { ... }` and
// `Self::WithCapacity(...)` is `Vector::WithCapacity<T>(...)`. Every later stage therefore sees the type written out
// and decides nothing about `Self` again: it is an alias, and it changes neither dispatch nor layout. Inside an
// interface the implementing type is not known until a bound supplies it, so there `Self` stays the interface's own
// type parameter.

#include "Syntax/Parser/Parser.h"

#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Rux {
namespace {
constexpr std::string_view kSelfOutsideHelp = "write the type's own name, or move the declaration into 'extend Type'";
} // namespace

std::string Parser::SelfName::Joined() const {
    std::string text;
    for (const std::string &segment : segments) {
        if (!text.empty()) {
            text += "::";
        }
        text += segment;
    }
    return text;
}

std::vector<Token> Parser::ParseExtendedType(TypeExprPtr &type, const std::string_view help) {
    // A run of closing angles such as `>>` is narrowed in place while the type is read, so the tokens are copied
    // before it is. An extended type never holds a '{', which is where its body starts.
    const std::size_t begin = pos;
    std::size_t end = begin;
    while (end < tokens.size() && tokens[end].kind != TokenKind::LeftBrace &&
           tokens[end].kind != TokenKind::EndOfFile) {
        ++end;
    }
    std::vector<Token> typeTokens(tokens.begin() + static_cast<std::ptrdiff_t>(begin),
                                  tokens.begin() + static_cast<std::ptrdiff_t>(end));
    const std::size_t diagnosticsBefore = diagnostics.size();
    type = ParseType(std::string(help));
    if (!type || diagnostics.size() != diagnosticsBefore || pos <= begin || pos > end) {
        return {};
    }
    typeTokens.resize(pos - begin);
    typeTokens.push_back(tokens.back()); // the end-of-file sentinel
    return typeTokens;
}

TypeExprPtr Parser::SelfTypeAt(const SourceLocation location) {
    if (selfScope.kind == SelfScope::Kind::Extend && !selfScope.typeTokens.empty()) {
        // The type is read again from its own tokens, each moved to where `Self` is written, so a diagnostic about
        // the type points at the use rather than at the header.
        std::vector<Token> typeTokens = selfScope.typeTokens;
        for (Token &token : typeTokens) {
            token.location = location;
        }
        Parser reader(std::move(typeTokens), sourceName, arch);
        if (TypeExprPtr type = reader.ParseType(); type && reader.diagnostics.empty()) {
            return type;
        }
    }
    if (selfScope.kind == SelfScope::Kind::None) {
        EmitError(location, "'Self' is only available inside an 'extend' block or an interface",
                  std::string(kSelfOutsideHelp));
    }
    // In an interface `Self` is the implementing type, which semantic analysis binds per bound. After an error it is
    // kept as written so the rest of the declaration still parses.
    auto named = std::make_unique<NamedTypeExpr>();
    named->location = location;
    named->name = "Self";
    return named;
}

std::optional<Parser::SelfName> Parser::SelfNameAt(const SourceLocation location) {
    if (selfScope.kind != SelfScope::Kind::Extend) {
        EmitError(location,
                  selfScope.kind == SelfScope::Kind::Interface
                      ? "'Self' in an interface names the implementing type, which an expression cannot name"
                      : "'Self' is only available inside an 'extend' block or an interface",
                  selfScope.kind == SelfScope::Kind::Interface ? std::optional<std::string>{}
                                                               : std::string(kSelfOutsideHelp));
        return std::nullopt;
    }
    if (selfScope.typeTokens.empty()) {
        return std::nullopt; // the header's own error has been reported
    }
    TypeExprPtr type = SelfTypeAt(location);
    if (auto *named = AstCast<NamedTypeExpr>(type.get())) {
        SelfName name;
        name.segments.push_back(named->name);
        name.typeArgs = std::move(named->typeArgs);
        return name;
    }
    if (const auto *path = AstCast<const PathTypeExpr>(type.get())) {
        SelfName name;
        name.segments = path->segments;
        return name;
    }
    std::string spelled;
    for (std::size_t index = 0; index + 1 < selfScope.typeTokens.size(); ++index) {
        spelled += selfScope.typeTokens[index].text;
    }
    EmitError(location, std::format("'Self' names '{}', which has no name to construct or call through", spelled),
              "use 'Self' as a type here, or write the value without it");
    return std::nullopt;
}

ExprPtr Parser::ParseSelfExpr(const SourceLocation location) {
    std::optional<SelfName> self = SelfNameAt(location);
    if (!self) {
        // Reported already; the rest is read as if `Self` were a type's name, so one mistake reports once.
        self.emplace();
        self->segments.emplace_back("Self");
    }
    // A struct literal: `Self { ... }` is `Vector<T> { ... }`.
    if (structInitAllowed && Check(TokenKind::LeftBrace)) {
        auto initializer = std::make_unique<StructInitExpr>();
        initializer->location = location;
        initializer->typeName = self->Joined();
        initializer->typeArgs = std::move(self->typeArgs);
        ParseStructInitFields(*initializer);
        return initializer;
    }

    // The type's name, as an identifier or, for a type named by a path, as that path.
    ExprPtr callee;
    if (self->segments.size() == 1) {
        auto identifier = std::make_unique<IdentExpr>();
        identifier->location = location;
        identifier->name = self->segments.front();
        callee = std::move(identifier);
    }
    else {
        auto path = std::make_unique<PathExpr>();
        path->location = location;
        path->segments = self->segments;
        callee = std::move(path);
    }
    // A member: `Self::Name`. The type's own arguments come first in a call, before any the member adds, which is
    // how `Vector::FromSlice<T>(...)` and `Option::Some<T>(...)` are written out.
    if (Check(TokenKind::ColonColon) && Peek(1).Is(TokenKind::Ident)) {
        auto path = std::make_unique<PathExpr>();
        path->location = location;
        path->segments = self->segments;
        while (Match(TokenKind::ColonColon)) {
            path->segments.push_back(ExpectBefore(TokenKind::Ident, "a path segment after '::'").text);
        }
        callee = std::move(path);
    }
    if (!IsGenericCallAhead() && !Check(TokenKind::LeftParen)) {
        return callee;
    }
    std::vector<TypeExprPtr> typeArgs = std::move(self->typeArgs);
    if (IsGenericCallAhead()) {
        for (TypeExprPtr &argument : ParseTypeArgs()) {
            typeArgs.push_back(std::move(argument));
        }
    }
    auto call = std::make_unique<CallExpr>();
    ++callsRead;
    call->location = location;
    call->callee = std::move(callee);
    call->typeArgs = std::move(typeArgs);
    call->args = ParseArgList(&call->labels);
    return call;
}

bool Parser::CheckShorthandReceiver() const noexcept {
    std::size_t ahead = 0;
    if (Check(TokenKind::Amp) || Check(TokenKind::Star)) {
        ahead = Peek(1).Is(TokenKind::VarKeyword) ? 2 : 1;
    }
    return Peek(ahead).Is(TokenKind::SelfKeyword) &&
           (Peek(ahead + 1).Is(TokenKind::Comma) || Peek(ahead + 1).Is(TokenKind::RightParen));
}

void Parser::ParseShorthandReceiver(Param &parameter) {
    const SourceLocation location = CurrentLocation();
    const bool reference = Match(TokenKind::Amp);
    const bool pointer = !reference && Match(TokenKind::Star);
    const bool writable = (reference || pointer) && Match(TokenKind::VarKeyword);
    Advance(); // 'self'
    parameter.name = "self";
    parameter.location = location;
    parameter.shorthandReceiver = true;

    if (selfScope.kind == SelfScope::Kind::None) {
        const std::string spelled = std::format("{}{}self",
                                                reference ? "&"
                                                : pointer ? "*"
                                                          : "",
                                                writable ? "var " : "");
        EmitError(
            location,
            std::format("receiver '{}' is only allowed in an 'extend' block or an interface requirement", spelled),
            "write the parameter as 'name: Type', or declare the function inside 'extend Type'");
        parameter.type = std::make_unique<SelfTypeExpr>();
        parameter.type->location = location;
        return;
    }
    TypeExprPtr self = SelfTypeAt(location);
    if (reference) {
        auto type = std::make_unique<ReferenceTypeExpr>();
        type->location = location;
        type->pointeeMut = writable;
        type->pointee = std::move(self);
        parameter.type = std::move(type);
    }
    else if (pointer) {
        auto type = std::make_unique<PointerTypeExpr>();
        type->location = location;
        type->pointeeMut = writable;
        type->pointee = std::move(self);
        parameter.type = std::move(type);
    }
    else {
        parameter.type = std::move(self);
    }
}
} // namespace Rux
