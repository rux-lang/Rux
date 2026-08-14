// Top-level declaration dispatch and function parsing.

#include "Syntax/Parser/Parser.h"

#include <format>
#include <memory>
#include <string>
#include <utility>

namespace Rux {

DeclPtr Parser::ParseDecl() {
    std::string documentation = ParseDocumentation();
    const auto loc = CurrentLocation();
    auto AttachDocumentation = [&](DeclPtr declaration) -> DeclPtr {
        if (declaration)
            declaration->documentation = documentation;
        return declaration;
    };

    // Conditional compilation.
    if (Check(TokenKind::WhenKeyword)) {
        return AttachDocumentation(ParseWhenDecl());
    }
    // The forms `when` replaced. Both are diagnosed here rather than left to the
    // attribute parser, which would only report that '#' wants a name.
    if (Check(TokenKind::Hash) && Peek(1).Is(TokenKind::IfKeyword)) {
        EmitError(loc, "'#if' is no longer conditional compilation; write 'when <condition> { ... }'");
        Advance(); // '#'
        Advance(); // 'if'
        // Parse it as the `when` it meant, so the chain reports only its spelling.
        return AttachDocumentation(ParseWhenBody(loc));
    }
    if (Check(TokenKind::Hash) && Peek(1).Is(TokenKind::Ident) && Peek(1).text == "When") {
        EmitError(loc, "the '#When' attribute has been removed; wrap the declaration in "
                       "'when <condition> { ... }' instead");
        Advance(); // '#'
        Advance(); // 'When'
        // Drop the condition and keep the declaration it guarded: reporting the
        // rewrite once beats burying it under errors from the tokens that follow.
        if (Match(TokenKind::LeftParen)) {
            for (int depth = 1; depth > 0 && !IsAtEnd();) {
                if (Check(TokenKind::LeftParen)) {
                    ++depth;
                }
                else if (Check(TokenKind::RightParen)) {
                    --depth;
                }
                Advance();
            }
        }
        return AttachDocumentation(ParseDecl());
    }

    // The form `intrinsic` replaced. Caught before ParseAttrs, which would
    // otherwise consume it as an attribute and leave the constant that follows
    // looking like an ordinary one that forgot its value. Recovered as the
    // keyword it meant, so it reports only its spelling.
    if (Check(TokenKind::Hash) && Peek(1).Is(TokenKind::Ident) && Peek(1).text == "Intrinsic") {
        EmitError(loc, "the '#Intrinsic' attribute has been removed; write 'intrinsic #name: Type;' "
                       "or 'intrinsic func Name(...);'");
        Advance(); // '#'
        Advance(); // 'Intrinsic'
        if (Match(TokenKind::LeftParen)) {
            for (int depth = 1; depth > 0 && !IsAtEnd();) {
                if (Check(TokenKind::LeftParen)) {
                    ++depth;
                }
                else if (Check(TokenKind::RightParen)) {
                    --depth;
                }
                Advance();
            }
        }
        ParsedAttrs rest = ParseAttrs();
        const bool pub = Match(TokenKind::PubKeyword);
        return AttachDocumentation(ParseIntrinsicDecl(pub, rest, loc));
    }

    ParsedAttrs attrs = ParseAttrs();

    bool isPublic = false;
    if (Match(TokenKind::PubKeyword)) {
        isPublic = true;
    }

    // intrinsic value/func: the compiler supplies the value or the body. The
    // declaration itself names the intrinsic, so there is nothing to write twice.
    if (Match(TokenKind::IntrinsicKeyword)) {
        return AttachDocumentation(ParseIntrinsicDecl(isPublic, attrs, Previous().location));
    }

    // asm func
    if (Check(TokenKind::Ident) && Peek().text == "asm" && Peek(1).Is(TokenKind::FuncKeyword)) {
        Advance(); // consume 'asm'
        return AttachDocumentation(ApplyAttrs(ParseFuncDecl(isPublic, true, attrs.callConv), attrs));
    }

    if (Check(TokenKind::FuncKeyword)) {
        return AttachDocumentation(ApplyAttrs(ParseFuncDecl(isPublic, false, attrs.callConv), attrs));
    }
    if (Check(TokenKind::StructKeyword)) {
        return AttachDocumentation(ApplyAttrs(ParseStructDecl(isPublic), attrs));
    }
    if (Check(TokenKind::EnumKeyword)) {
        return AttachDocumentation(ApplyAttrs(ParseEnumDecl(isPublic), attrs));
    }
    if (Check(TokenKind::UnionKeyword)) {
        return AttachDocumentation(ApplyAttrs(ParseUnionDecl(isPublic), attrs));
    }
    if (Check(TokenKind::InterfaceKeyword)) {
        return AttachDocumentation(ApplyAttrs(ParseInterfaceDecl(isPublic), attrs));
    }
    if (Check(TokenKind::ExtendKeyword)) {
        return AttachDocumentation(ApplyAttrs(ParseImplDecl(), attrs));
    }
    if (Check(TokenKind::ModuleKeyword)) {
        return AttachDocumentation(ApplyAttrs(ParseModuleDecl(isPublic), attrs));
    }
    if (Check(TokenKind::ImportKeyword)) {
        return AttachDocumentation(ApplyAttrs(ParseUseDecl(), attrs));
    }
    if (Check(TokenKind::ConstKeyword)) {
        return AttachDocumentation(ApplyAttrs(ParseConstDecl(isPublic), attrs));
    }
    if (Check(TokenKind::TypeKeyword)) {
        return AttachDocumentation(ApplyAttrs(ParseTypeAliasDecl(isPublic), attrs));
    }
    if (Check(TokenKind::ExternKeyword)) {
        return AttachDocumentation(ApplyAttrs(ParseExternDecl(isPublic, attrs), attrs));
    }

    EmitError(loc, std::format("unexpected token '{}', expected a declaration", Peek().text));
    return nullptr;
}

Param Parser::ParseParam(const bool allowVariadic) {
    Param parameter;
    parameter.location = CurrentLocation();

    if (allowVariadic && Check(TokenKind::DotDotDot)) {
        Advance();
        parameter.isVariadic = true;
        parameter.name = "...";
        parameter.type = std::make_unique<NamedTypeExpr>();
        dynamic_cast<NamedTypeExpr *>(parameter.type.get())->name = "...";
        return parameter;
    }

    if (Match(TokenKind::SelfKeyword)) {
        parameter.name = "self";
        parameter.type = std::make_unique<SelfTypeExpr>();
        return parameter;
    }

    parameter.isMut = Match(TokenKind::VarKeyword);
    parameter.name = Expect(TokenKind::Ident, "expected parameter name").text;
    Expect(TokenKind::Colon, "expected ':'");
    parameter.type = ParseType();
    if (allowVariadic && Match(TokenKind::DotDotDot)) {
        parameter.isVariadic = true;
    }
    if (!parameter.isVariadic && Match(TokenKind::Assign)) {
        parameter.defaultValue = ParseExpr();
    }
    return parameter;
}

std::vector<Param> Parser::ParseParamList(const bool allowVariadic) {
    std::vector<Param> params;
    while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
        params.push_back(ParseParam(allowVariadic));
        if (!Match(TokenKind::Comma)) {
            break;
        }
    }
    return params;
}

std::unique_ptr<FuncDecl> Parser::ParseFuncDecl(const bool isPublic, const bool isAsm,
                                                const CallingConvention callConv) {
    const auto loc = CurrentLocation();
    Expect(TokenKind::FuncKeyword, "expected 'func'");

    auto decl = std::make_unique<FuncDecl>();
    decl->location = loc;
    decl->isPublic = isPublic;
    decl->isAsm = isAsm;
    decl->callConv = callConv;
    if (Check(TokenKind::Hash) && Peek(1).Is(TokenKind::Ident)) {
        // A compile-time intrinsic function: `intrinsic func #Error(...)`. The
        // '#' is part of the name.
        Advance(); // consume '#'
        decl->name = "#" + Advance().text;
    }
    else if (Peek().IsOperator()) {
        decl->name = Advance().text;
    }
    else {
        decl->name = Expect(TokenKind::Ident, "expected function name").text;
    }

    if (Check(TokenKind::Less)) {
        decl->typeParams = ParseTypeParams();
    }

    Expect(TokenKind::LeftParen, "expected '('");
    decl->params = ParseParamList(true);
    Expect(TokenKind::RightParen, "expected ')'");

    if (Match(TokenKind::Arrow)) {
        decl->returnType = ParseType();
    }

    if (isAsm) {
        Expect(TokenKind::LeftBrace, "expected '{'");
        decl->asmBody = ParseAsmBody();
        Expect(TokenKind::RightBrace, "expected '}'");
    }
    else if (Check(TokenKind::LeftBrace)) {
        decl->body = ParseBlock();
    }
    else {
        Expect(TokenKind::Semicolon, "expected '{' or ';'");
    }

    return decl;
}

} // namespace Rux
