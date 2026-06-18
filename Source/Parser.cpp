// Copyright (c) Rux contributors.
// SPDX-License-Identifier: MIT

#include "Rux/Parser.h"

#include <cassert>
#include <format>
#include <fstream>

namespace Rux {
// ParseResult
bool ParseResult::HasErrors() const noexcept {
    for (auto const &d : diagnostics) {
        if (d.severity == ParserDiagnostic::Severity::Error) {
            return true;
        }
    }
    return false;
}

// Constructor / FromLexResult
Parser::Parser(std::vector<Token> tokens, std::string sourceName)
    : tokens(std::move(tokens))
    , sourceName(std::move(sourceName)) {
}

std::optional<ParseResult> Parser::FromLexResult(LexerResult const &lex,
                                                 std::string const &sourceName) {
    if (lex.HasErrors()) {
        return std::nullopt;
    }
    Parser p(lex.tokens, sourceName);
    return p.Parse();
}

// Parse  –  top-level entry point
ParseResult Parser::Parse() {
    Module mod;
    mod.name = sourceName;

    while (!IsAtEnd()) {
        if (auto decl = ParseDecl()) {
            mod.items.push_back(std::move(decl));
        }
        else {
            Recover();
        }
    }

    return ParseResult{std::move(mod), std::move(diagnostics)};
}

// Token helpers
Token const &Parser::Peek(std::size_t const ahead) const noexcept {
    std::size_t const idx = pos + ahead;
    if (idx < tokens.size()) {
        return tokens[idx];
    }
    return tokens.back(); // EOF sentinel
}

Token const &Parser::Advance() noexcept {
    if (!IsAtEnd()) {
        ++pos;
    }
    return tokens[pos - 1];
}

bool Parser::Check(TokenKind const kind) const noexcept {
    return Peek().kind == kind;
}

bool Parser::CheckAny(std::initializer_list<TokenKind> const kinds) const noexcept {
    for (auto const k : kinds) {
        if (Check(k)) {
            return true;
        }
    }
    return false;
}

bool Parser::Match(TokenKind const kind) noexcept {
    if (!Check(kind)) {
        return false;
    }
    Advance();
    return true;
}

Token const &Parser::Expect(TokenKind const kind, std::string_view const message) {
    if (Check(kind)) {
        return Advance();
    }
    EmitError(CurrentLocation(), std::string(message));
    return Peek(); // return without advancing
}

bool Parser::IsAtEnd() const noexcept {
    return Peek().kind == TokenKind::EndOfFile;
}

Token const &Parser::Previous() const noexcept {
    assert(pos > 0);
    return tokens[pos - 1];
}

SourceLocation Parser::CurrentLocation() const noexcept {
    return Peek().location;
}

bool Parser::IsGenericStructInitAhead() const noexcept {
    if (!Check(TokenKind::Less)) {
        return false;
    }

    int angleDepth = 0;
    for (std::size_t ahead = 0;; ++ahead) {
        TokenKind const kind = Peek(ahead).kind;
        if (kind == TokenKind::EndOfFile || kind == TokenKind::LeftBrace ||
            kind == TokenKind::Semicolon) {
            return false;
        }

        if (kind == TokenKind::Less) {
            ++angleDepth;
            continue;
        }

        if (kind == TokenKind::Greater) {
            --angleDepth;
            if (angleDepth == 0) {
                return Peek(ahead + 1).kind == TokenKind::LeftBrace;
            }
            if (angleDepth < 0) {
                return false;
            }
        }
    }
}

bool Parser::IsTypeArgListAhead() const noexcept {
    if (!Check(TokenKind::Less)) {
        return false;
    }

    int angleDepth = 0;
    for (std::size_t ahead = 0;; ++ahead) {
        switch (Peek(ahead).kind) {
        case TokenKind::Less:
            ++angleDepth;
            continue;
        case TokenKind::Greater:
            --angleDepth;
            if (angleDepth == 0) {
                return true;
            }
            if (angleDepth < 0) {
                return false;
            }
            continue;
        case TokenKind::Ident:
        case TokenKind::Star:
        case TokenKind::LeftParen:
        case TokenKind::RightParen:
        case TokenKind::LeftBracket:
        case TokenKind::RightBracket:
        case TokenKind::Comma:
        case TokenKind::ColonColon:
        case TokenKind::SelfKeyword:
        case TokenKind::ConstKeyword:
            continue;
        default:
            return false;
        }
    }
}

// Diagnostics
void Parser::EmitError(SourceLocation const loc, std::string message) {
    diagnostics.push_back(
        ParserDiagnostic{ParserDiagnostic::Severity::Error, loc, std::move(message)});
}

void Parser::EmitWarning(SourceLocation const loc, std::string message) {
    diagnostics.push_back(
        ParserDiagnostic{ParserDiagnostic::Severity::Warning, loc, std::move(message)});
}

void Parser::Synchronize() {
    // Skip until we reach a token that likely starts a new declaration or
    // statement.
    while (!IsAtEnd()) {
        TokenKind const k = Peek().kind;

        // These tokens can safely begin a new item.
        if (k == TokenKind::Semicolon) {
            Advance();
            return;
        }
        if (k == TokenKind::RightBrace) {
            return;
        }

        if (k == TokenKind::FuncKeyword || k == TokenKind::StructKeyword ||
            k == TokenKind::EnumKeyword || k == TokenKind::UnionKeyword ||
            k == TokenKind::InterfaceKeyword || k == TokenKind::ExtendKeyword ||
            k == TokenKind::ModuleKeyword || k == TokenKind::ImportKeyword ||
            k == TokenKind::ConstKeyword || k == TokenKind::TypeKeyword ||
            k == TokenKind::ExternKeyword || k == TokenKind::PubKeyword ||
            k == TokenKind::LetKeyword || k == TokenKind::VarKeyword || k == TokenKind::IfKeyword ||
            k == TokenKind::WhileKeyword || k == TokenKind::DoKeyword ||
            k == TokenKind::LoopKeyword || k == TokenKind::ForKeyword ||
            k == TokenKind::ReturnKeyword || k == TokenKind::MatchKeyword) {
            return;
        }

        Advance();
    }
}

void Parser::Recover() {
    std::size_t const before = pos;
    Synchronize();
    if (pos == before && !IsAtEnd()) {
        Advance();
    }
}

// Attribute parsing
static std::string DecodeStringLiteralText(std::string const &text) {
    std::string out;
    std::size_t const quote = text.find('"');
    if (quote == std::string::npos) {
        return out;
    }

    for (std::size_t i = quote + 1; i + 1 < text.size(); ++i) {
        if (text[i] != '\\') {
            out += text[i];
            continue;
        }

        if (++i + 1 > text.size()) {
            break;
        }
        switch (text[i]) {
        case 'n':
            out += '\n';
            break;
        case 't':
            out += '\t';
            break;
        case 'r':
            out += '\r';
            break;
        case '0':
            out += '\0';
            break;
        case '\\':
            out += '\\';
            break;
        case '\'':
            out += '\'';
            break;
        case '"':
            out += '"';
            break;
        default:
            out += text[i];
            break;
        }
    }

    return out;
}

// Parses zero or more @[AttrName(...)] attributes that precede a
// declaration. Unknown attribute names are a compilation error.
Parser::ParsedAttrs Parser::ParseAttrs() {
    ParsedAttrs attrs;
    while (Check(TokenKind::At)) {
        Advance(); // consume '@'
        Expect(TokenKind::LeftBracket, "expected '[' after '@'");

        SourceLocation const attrLoc = CurrentLocation();
        std::string attrName;
        if (Check(TokenKind::Ident)) {
            attrName = Advance().text;
        }

        if (Check(TokenKind::LeftParen)) {
            Advance(); // consume '('

            if (attrName == "Call" && Check(TokenKind::Dot)) {
                // @[Call(.Win64)] — positional enum variant
                Advance(); // consume '.'
                std::string variant;
                if (Check(TokenKind::Ident)) {
                    variant = Advance().text;
                }
                if (variant == "Win64") {
                    attrs.callConv = CallingConvention::Win64;
                }
                else {
                    EmitWarning(CurrentLocation(),
                                std::format("unknown calling convention '.{}'", variant));
                }
            }
            else if (attrName == "Target" && Check(TokenKind::StringLiteral)) {
                // @[Target("Windows")] — positional OS string
                Token const tok = Advance();
                std::string os = DecodeStringLiteralText(tok.text);
                if (os == "MacOS" || os == "Macos" || os == "macos") {
                    os = "macOS";
                }
                if (os != "BSD" && os != "Illumos" && os != "Linux" && os != "macOS" &&
                    os != "Windows") {
                    EmitError(tok.location, std::format("unsupported target '{}'; valid "
                                                        "targets are: BSD, "
                                                        "Illumos, Linux, macOS, Windows",
                                                        os));
                }
                else {
                    attrs.targetOs = std::move(os);
                }
            }
            else if (attrName == "Warn" && Check(TokenKind::StringLiteral)) {
                // @[Warn("message")] — emit a compiler warning at each call
                // site
                attrs.warnMessage = DecodeStringLiteralText(Advance().text);
            }
            else if (attrName == "Error" && Check(TokenKind::StringLiteral)) {
                // @[Error("message")] — emit a compiler error at each call
                // site
                attrs.errorMessage = DecodeStringLiteralText(Advance().text);
            }
            else if (attrName == "Import") {
                // @[Import(lib: "...")] — DLL import library
                while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
                    if (!Check(TokenKind::Ident)) {
                        Advance();
                        continue;
                    }
                    std::string key = Advance().text;
                    if (!Match(TokenKind::Colon)) {
                        continue;
                    }
                    if (key == "lib" && Check(TokenKind::StringLiteral)) {
                        attrs.importLib = DecodeStringLiteralText(Advance().text);
                    }
                    else {
                        Advance(); // skip unknown value
                    }
                    if (!Match(TokenKind::Comma)) {
                        break;
                    }
                }
            }
            else {
                EmitError(attrLoc, std::format("unknown attribute '{}'", attrName));
                while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
                    Advance();
                }
            }

            Expect(TokenKind::RightParen, "expected ')' to close attribute");
        }
        else {
            EmitError(attrLoc, std::format("unknown attribute '{}'", attrName));
        }

        Expect(TokenKind::RightBracket, "expected ']' to close attribute");
    }
    return attrs;
}

// Top-level declarations
DeclPtr Parser::ParseDecl() {
    auto const loc = CurrentLocation();

    ParsedAttrs attrs = ParseAttrs();

    bool isPublic = false;
    if (Match(TokenKind::PubKeyword)) {
        isPublic = true;
    }

    auto withTarget = [&](DeclPtr decl) -> DeclPtr {
        if (decl) {
            if (decl->targetOs.empty()) {
                decl->targetOs = attrs.targetOs;
            }
            if (decl->warnMessage.empty()) {
                decl->warnMessage = attrs.warnMessage;
            }
            if (decl->errorMessage.empty()) {
                decl->errorMessage = attrs.errorMessage;
            }
        }
        return decl;
    };

    // asm func
    if (Check(TokenKind::Ident) && Peek().text == "asm" && Peek(1).Is(TokenKind::FuncKeyword)) {
        Advance(); // consume 'asm'
        return withTarget(ParseFuncDecl(isPublic, true, attrs.callConv));
    }

    if (Check(TokenKind::FuncKeyword)) {
        return withTarget(ParseFuncDecl(isPublic, false, attrs.callConv));
    }
    if (Check(TokenKind::StructKeyword)) {
        return withTarget(ParseStructDecl(isPublic));
    }
    if (Check(TokenKind::EnumKeyword)) {
        return withTarget(ParseEnumDecl(isPublic));
    }
    if (Check(TokenKind::UnionKeyword)) {
        return withTarget(ParseUnionDecl(isPublic));
    }
    if (Check(TokenKind::InterfaceKeyword)) {
        return withTarget(ParseInterfaceDecl(isPublic));
    }
    if (Check(TokenKind::ExtendKeyword)) {
        return withTarget(ParseImplDecl());
    }
    if (Check(TokenKind::ModuleKeyword)) {
        return withTarget(ParseModuleDecl(isPublic));
    }
    if (Check(TokenKind::ImportKeyword)) {
        return ParseUseDecl(std::move(attrs));
    }
    if (Check(TokenKind::ConstKeyword)) {
        return withTarget(ParseConstDecl(isPublic));
    }
    if (Check(TokenKind::TypeKeyword)) {
        return withTarget(ParseTypeAliasDecl(isPublic));
    }
    if (Check(TokenKind::ExternKeyword)) {
        return withTarget(ParseExternDecl(isPublic, attrs));
    }

    EmitError(loc, std::format("unexpected token '{}', expected a declaration", Peek().text));
    return nullptr;
}

// Shared declaration helpers
std::vector<std::string> Parser::ParseTypeParams() {
    // <T, U, ...>
    std::vector<std::string> params;
    Expect(TokenKind::Less, "expected '<'");
    while (!Check(TokenKind::Greater) && !IsAtEnd()) {
        auto &t = Expect(TokenKind::Ident, "expected type parameter name");
        params.push_back(t.text);
        if (!Match(TokenKind::Comma)) {
            break;
        }
    }
    Expect(TokenKind::Greater, "expected '>'");
    return params;
}

std::vector<TypeExprPtr> Parser::ParseTypeArgs() {
    // <int32, T[], ...>
    std::vector<TypeExprPtr> args;
    Expect(TokenKind::Less, "expected '<'");
    while (!Check(TokenKind::Greater) && !IsAtEnd()) {
        args.push_back(ParseType());
        if (!Match(TokenKind::Comma)) {
            break;
        }
    }
    Expect(TokenKind::Greater, "expected '>'");
    return args;
}

Param Parser::ParseParam(bool allowVariadic) {
    Param p;
    p.location = CurrentLocation();

    if (allowVariadic && Check(TokenKind::DotDotDot)) {
        Advance();
        p.isVariadic = true;
        p.name = "...";
        p.type = std::make_unique<NamedTypeExpr>();
        dynamic_cast<NamedTypeExpr *>(p.type.get())->name = "...";
        return p;
    }

    if (Match(TokenKind::SelfKeyword)) {
        p.name = "self";
        p.type = std::make_unique<SelfTypeExpr>();
        return p;
    }

    p.name = Expect(TokenKind::Ident, "expected parameter name").text;
    Expect(TokenKind::Colon, "expected ':'");
    p.type = ParseType();
    if (allowVariadic && Match(TokenKind::DotDotDot)) {
        p.isVariadic = true;
    }
    if (!p.isVariadic && Match(TokenKind::Assign)) {
        p.defaultValue = ParseExpr();
    }
    return p;
}

std::vector<Param> Parser::ParseParamList(bool allowVariadic) {
    std::vector<Param> params;
    while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
        params.push_back(ParseParam(allowVariadic));
        if (!Match(TokenKind::Comma)) {
            break;
        }
    }
    return params;
}

// func
std::unique_ptr<FuncDecl> Parser::ParseFuncDecl(bool isPublic, bool isAsm,
                                                CallingConvention callConv) {
    auto const loc = CurrentLocation();
    Expect(TokenKind::FuncKeyword, "expected 'func'");

    auto decl = std::make_unique<FuncDecl>();
    decl->location = loc;
    decl->isPublic = isPublic;
    decl->isAsm = isAsm;
    decl->callConv = callConv;
    if (Peek().IsOperator()) {
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

    if (Check(TokenKind::LeftBrace)) {
        decl->body = ParseBlock();
    }
    else {
        Expect(TokenKind::Semicolon, "expected '{' or ';'");
    }

    return decl;
}

// struct
std::unique_ptr<StructDecl> Parser::ParseStructDecl(bool isPublic) {
    auto const loc = CurrentLocation();
    Expect(TokenKind::StructKeyword, "expected 'struct'");

    auto decl = std::make_unique<StructDecl>();
    decl->location = loc;
    decl->isPublic = isPublic;
    decl->name = Expect(TokenKind::Ident, "expected struct name").text;

    if (Check(TokenKind::Less)) {
        decl->typeParams = ParseTypeParams();
    }

    Expect(TokenKind::LeftBrace, "expected '{'");
    while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
        StructDecl::Field field;
        field.location = CurrentLocation();

        if (Match(TokenKind::PubKeyword)) {
            field.isPublic = true;
        }

        field.name = Expect(TokenKind::Ident, "expected field name").text;
        Expect(TokenKind::Colon, "expected ':'");
        field.type = ParseType();
        Expect(TokenKind::Semicolon, "expected ';' after field");
        decl->fields.push_back(std::move(field));
    }
    Expect(TokenKind::RightBrace, "expected '}'");
    return decl;
}

// enum
std::unique_ptr<EnumDecl> Parser::ParseEnumDecl(bool const isPublic) {
    auto const loc = CurrentLocation();
    Expect(TokenKind::EnumKeyword, "expected 'enum'");

    auto decl = std::make_unique<EnumDecl>();
    decl->location = loc;
    decl->isPublic = isPublic;
    decl->name = Expect(TokenKind::Ident, "expected enum name").text;
    if (Match(TokenKind::Colon)) {
        decl->baseType = ParseType();
    }

    Expect(TokenKind::LeftBrace, "expected '{'");
    while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
        EnumDecl::Variant variant;
        variant.location = CurrentLocation();
        variant.name = Expect(TokenKind::Ident, "expected variant name").text;

        if (Match(TokenKind::LeftParen)) {
            while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
                variant.fields.push_back(ParseType());
                if (!Match(TokenKind::Comma)) {
                    break;
                }
            }
            Expect(TokenKind::RightParen, "expected ')'");
        }
        else if (Match(TokenKind::LeftBrace)) {
            while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
                EnumDecl::Variant::NamedField field;
                field.location = CurrentLocation();
                field.name = Expect(TokenKind::Ident, "expected variant field name").text;
                Expect(TokenKind::Colon, "expected ':'");
                field.type = ParseType();
                Expect(TokenKind::Semicolon, "expected ';' after variant field");
                variant.namedFields.push_back(std::move(field));
            }
            Expect(TokenKind::RightBrace, "expected '}'");
        }

        if (Match(TokenKind::Assign)) {
            std::string value;
            if (Match(TokenKind::Minus)) {
                value = "-";
            }
            value += Expect(TokenKind::IntLiteral, "expected integer enum discriminant").text;
            variant.discriminant = std::move(value);
        }

        decl->variants.push_back(std::move(variant));
        if (Match(TokenKind::Comma)) {
            if (Check(TokenKind::RightBrace)) {
                EmitError(Previous().location,
                          "trailing comma is not allowed in enum declarations");
            }
        }
        else {
            break;
        }
    }
    Expect(TokenKind::RightBrace, "expected '}'");
    return decl;
}

// union
std::unique_ptr<UnionDecl> Parser::ParseUnionDecl(bool isPublic) {
    auto const loc = CurrentLocation();
    Expect(TokenKind::UnionKeyword, "expected 'union'");

    auto decl = std::make_unique<UnionDecl>();
    decl->location = loc;
    decl->isPublic = isPublic;
    decl->name = Expect(TokenKind::Ident, "expected union name").text;

    Expect(TokenKind::LeftBrace, "expected '{'");
    while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
        UnionDecl::Field field;
        field.location = CurrentLocation();
        field.name = Expect(TokenKind::Ident, "expected field name").text;
        Expect(TokenKind::Colon, "expected ':'");
        field.type = ParseType();
        decl->fields.push_back(std::move(field));
        if (!Match(TokenKind::Comma)) {
            break;
        }
    }
    Expect(TokenKind::RightBrace, "expected '}'");
    return decl;
}

// interface
std::unique_ptr<InterfaceDecl> Parser::ParseInterfaceDecl(bool isPublic) {
    auto const loc = CurrentLocation();
    Expect(TokenKind::InterfaceKeyword, "expected 'interface'");

    auto decl = std::make_unique<InterfaceDecl>();
    decl->location = loc;
    decl->isPublic = isPublic;
    decl->name = Expect(TokenKind::Ident, "expected interface name").text;

    Expect(TokenKind::LeftBrace, "expected '{'");
    while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
        if (!Check(TokenKind::FuncKeyword)) {
            EmitError(CurrentLocation(), "expected 'func' in interface body");
            Recover();
            continue;
        }
        if (auto method = ParseFuncDecl(false, false)) {
            decl->methods.push_back(std::move(method));
        }
    }
    Expect(TokenKind::RightBrace, "expected '}'");
    return decl;
}

// extend
std::unique_ptr<ImplDecl> Parser::ParseImplDecl() {
    auto const loc = CurrentLocation();
    Expect(TokenKind::ExtendKeyword, "expected 'extend'");

    auto decl = std::make_unique<ImplDecl>();
    decl->location = loc;

    // extend TypeName  or  extend TypeName : InterfaceName  or  extend
    // InterfaceName for TypeName
    std::string const firstName = Expect(TokenKind::Ident, "expected type name").text;
    if (Match(TokenKind::Colon)) {
        decl->typeName = firstName;
        decl->interfaceName = Expect(TokenKind::Ident, "expected interface name after ':'").text;
    }
    else if (Match(TokenKind::ForKeyword)) {
        decl->interfaceName = firstName;
        decl->typeName = Expect(TokenKind::Ident, "expected type name after 'for'").text;
    }
    else {
        decl->typeName = firstName;
    }

    Expect(TokenKind::LeftBrace, "expected '{'");
    while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
        bool pub = Match(TokenKind::PubKeyword);
        if (!Check(TokenKind::FuncKeyword)) {
            EmitError(CurrentLocation(), "expected 'func' in extend body");
            Recover();
            continue;
        }
        if (auto method = ParseFuncDecl(pub, false)) {
            decl->methods.push_back(std::move(method));
        }
    }
    Expect(TokenKind::RightBrace, "expected '}'");
    return decl;
}

// module
std::unique_ptr<ModuleDecl> Parser::ParseModuleDecl(bool isPublic) {
    auto const loc = CurrentLocation();
    Expect(TokenKind::ModuleKeyword, "expected 'module'");

    std::vector<std::string> path;
    path.push_back(Expect(TokenKind::Ident, "expected module name").text);
    while (Match(TokenKind::ColonColon)) {
        path.push_back(Expect(TokenKind::Ident, "expected module name").text);
    }

    Expect(TokenKind::LeftBrace, "expected '{'");
    std::vector<DeclPtr> items;
    while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
        if (auto item = ParseDecl()) {
            items.push_back(std::move(item));
        }
        else {
            Recover();
        }
    }
    Expect(TokenKind::RightBrace, "expected '}'");

    std::unique_ptr<ModuleDecl> nested;
    for (std::size_t i = path.size(); i-- > 0;) {
        auto decl = std::make_unique<ModuleDecl>();
        decl->location = loc;
        decl->isPublic = (i == 0) ? isPublic : false;
        decl->name = std::move(path[i]);
        if (nested) {
            decl->items.push_back(std::move(nested));
        }
        else {
            decl->items = std::move(items);
        }
        nested = std::move(decl);
    }
    return nested;
}

// import
std::unique_ptr<UseDecl> Parser::ParseUseDecl(ParsedAttrs attrs) {
    auto const loc = CurrentLocation();
    Expect(TokenKind::ImportKeyword, "expected 'import'");

    auto decl = std::make_unique<UseDecl>();
    decl->location = loc;
    decl->targetOs = std::move(attrs.targetOs);

    // Parse path segments separated by '.' or '::'
    decl->path.push_back(Expect(TokenKind::Ident, "expected module path").text);

    while (!IsAtEnd()) {
        if (Match(TokenKind::Dot)) {
            if (Match(TokenKind::Star)) {
                // import Std.Io.*;
                decl->kind = UseDecl::Kind::Glob;
                break;
            }
            decl->path.push_back(Expect(TokenKind::Ident, "expected identifier").text);
        }
        else if (Match(TokenKind::ColonColon)) {
            if (Check(TokenKind::LeftBrace)) {
                // import Http::{ Request, Response };
                Advance(); // consume '{'
                decl->kind = UseDecl::Kind::Multi;
                while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
                    decl->names.push_back(Expect(TokenKind::Ident, "expected name").text);
                    if (!Match(TokenKind::Comma)) {
                        break;
                    }
                }
                Expect(TokenKind::RightBrace, "expected '}'");
                break;
            }
            if (Match(TokenKind::Star)) {
                // import Std::Io::*
                decl->kind = UseDecl::Kind::Glob;
                break;
            }
            decl->path.push_back(Expect(TokenKind::Ident, "expected identifier").text);
        }
        else {
            break;
        }
    }

    Expect(TokenKind::Semicolon, "expected ';'");
    return decl;
}

// const
std::unique_ptr<ConstDecl> Parser::ParseConstDecl(bool isPublic) {
    auto const loc = CurrentLocation();
    Expect(TokenKind::ConstKeyword, "expected 'const'");

    auto decl = std::make_unique<ConstDecl>();
    decl->location = loc;
    decl->isPublic = isPublic;
    decl->name = Expect(TokenKind::Ident, "expected constant name").text;

    if (Match(TokenKind::Colon)) {
        decl->type = ParseType();
    }

    Expect(TokenKind::Assign, "expected '='");
    decl->value = ParseExpr();

    Expect(TokenKind::Semicolon, "expected ';'");
    return decl;
}

// type alias
std::unique_ptr<TypeAliasDecl> Parser::ParseTypeAliasDecl(bool isPublic) {
    auto const loc = CurrentLocation();
    Expect(TokenKind::TypeKeyword, "expected 'type'");

    auto decl = std::make_unique<TypeAliasDecl>();
    decl->location = loc;
    decl->isPublic = isPublic;
    decl->name = Expect(TokenKind::Ident, "expected type alias name").text;

    Expect(TokenKind::Assign, "expected '='");
    decl->type = ParseType();

    Expect(TokenKind::Semicolon, "expected ';'");
    return decl;
}

// extern
DeclPtr Parser::ParseExternDecl(bool isPublic, ParsedAttrs attrs) {
    auto const loc = CurrentLocation();
    Expect(TokenKind::ExternKeyword, "expected 'extern'");

    if (Check(TokenKind::LeftBrace)) {
        // @[...] extern { func ...; ... }
        Advance(); // consume '{'
        auto block = std::make_unique<ExternBlockDecl>();
        block->location = loc;
        block->dll = attrs.importLib;
        block->callConv = attrs.callConv;
        while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
            if (Check(TokenKind::ExternKeyword)) {
                EmitError(CurrentLocation(), "'extern' is not allowed inside an extern block");
                while (!IsAtEnd() && !Check(TokenKind::Semicolon) &&
                       !Check(TokenKind::RightBrace)) {
                    Advance();
                }
                Match(TokenKind::Semicolon);
                continue;
            }
            if (Check(TokenKind::FuncKeyword)) {
                Advance(); // consume 'func'
                auto fd = std::make_unique<ExternFuncDecl>();
                fd->location = CurrentLocation();
                fd->isPublic = isPublic;
                fd->dll = attrs.importLib;
                fd->callConv = attrs.callConv;
                fd->name = Expect(TokenKind::Ident, "expected function name").text;
                Expect(TokenKind::LeftParen, "expected '('");
                while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
                    if (Check(TokenKind::DotDotDot)) {
                        Advance();
                        fd->isVariadic = true;
                        break;
                    }
                    fd->params.push_back(ParseParam(true));
                    if (!Match(TokenKind::Comma)) {
                        break;
                    }
                }
                Expect(TokenKind::RightParen, "expected ')'");
                if (Match(TokenKind::Arrow)) {
                    fd->returnType = ParseType();
                }
                Expect(TokenKind::Semicolon, "expected ';'");
                block->items.push_back(std::move(fd));
            }
            else if (Check(TokenKind::Ident)) {
                auto vd = std::make_unique<ExternVarDecl>();
                vd->location = CurrentLocation();
                vd->isPublic = isPublic;
                vd->name = Advance().text;
                Expect(TokenKind::Colon, "expected ':'");
                vd->type = ParseType();
                Expect(TokenKind::Semicolon, "expected ';'");
                block->items.push_back(std::move(vd));
            }
            else {
                EmitError(CurrentLocation(), "expected 'func' or variable declaration in "
                                             "extern block");
                Recover();
            }
        }
        Expect(TokenKind::RightBrace, "expected '}'");
        return block;
    }

    if (Check(TokenKind::FuncKeyword)) {
        // @[Import(lib: "...")] extern func Name(params) -> Type;
        Advance(); // consume 'func'

        auto decl = std::make_unique<ExternFuncDecl>();
        decl->location = loc;
        decl->isPublic = isPublic;
        decl->dll = std::move(attrs.importLib);
        decl->callConv = attrs.callConv;
        decl->name = Expect(TokenKind::Ident, "expected function name").text;

        Expect(TokenKind::LeftParen, "expected '('");
        while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
            if (Check(TokenKind::DotDotDot)) {
                Advance();
                decl->isVariadic = true;
                break;
            }
            decl->params.push_back(ParseParam(true));
            if (!Match(TokenKind::Comma)) {
                break;
            }
        }
        Expect(TokenKind::RightParen, "expected ')'");

        if (Match(TokenKind::Arrow)) {
            decl->returnType = ParseType();
        }

        Expect(TokenKind::Semicolon, "expected ';'");
        return decl;
    }

    // extern Name: Type;
    auto decl = std::make_unique<ExternVarDecl>();
    decl->location = loc;
    decl->isPublic = isPublic;
    decl->name = Expect(TokenKind::Ident, "expected variable name").text;

    Expect(TokenKind::Colon, "expected ':'");
    decl->type = ParseType();

    Expect(TokenKind::Semicolon, "expected ';'");
    return decl;
}

// Type expressions
TypeExprPtr Parser::ParseType() {
    auto const loc = CurrentLocation();

    // Pointer: *T  or  *const T
    if (Match(TokenKind::Star)) {
        Match(TokenKind::ConstKeyword); // consume optional 'const' qualifier
        auto p = std::make_unique<PointerTypeExpr>();
        p->location = loc;
        p->pointee = ParseType();
        return p;
    }

    // Tuple: (T, U, ...)
    if (Check(TokenKind::LeftParen)) {
        Advance();
        auto t = std::make_unique<TupleTypeExpr>();
        t->location = loc;
        while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
            t->elements.push_back(ParseType());
            if (!Match(TokenKind::Comma)) {
                break;
            }
        }
        Expect(TokenKind::RightParen, "expected ')'");
        return t;
    }

    TypeExprPtr base = ParseBaseType();
    if (!base) {
        return nullptr;
    }

    // Postfix slice suffix: T[]  or  T[N]
    while (Check(TokenKind::LeftBracket)) {
        Advance();
        auto a = std::make_unique<SliceTypeExpr>();
        a->location = loc;
        a->element = std::move(base);
        if (!Check(TokenKind::RightBracket)) {
            a->size = ParseExpr();
        }
        Expect(TokenKind::RightBracket, "expected ']'");
        base = std::move(a);
    }

    return base;
}

TypeExprPtr Parser::ParseBaseType() {
    auto const loc = CurrentLocation();

    if (Match(TokenKind::SelfKeyword)) {
        auto t = std::make_unique<SelfTypeExpr>();
        t->location = loc;
        return t;
    }

    if (Check(TokenKind::Ident)) {
        std::string const first = Advance().text;

        // Check for path type: A::B::C
        if (Check(TokenKind::ColonColon)) {
            auto p = std::make_unique<PathTypeExpr>();
            p->location = loc;
            p->segments.push_back(first);
            while (Match(TokenKind::ColonColon)) {
                p->segments.push_back(Expect(TokenKind::Ident, "expected type name").text);
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

    EmitError(loc, std::format("expected a type, got '{}'", Peek().text));
    return nullptr;
}

// Block and statements
std::unique_ptr<Block> Parser::ParseBlock() {
    auto const loc = CurrentLocation();
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
    auto const loc = CurrentLocation();

    if (Check(TokenKind::LetKeyword) || Check(TokenKind::VarKeyword)) {
        return ParseLetStmt();
    }
    if (Check(TokenKind::IfKeyword)) {
        return ParseIfStmt();
    }
    // Optional loop label: `ident ':' loop-keyword`
    std::string loopLabel;
    if (Check(TokenKind::Ident) && Peek(1).kind == TokenKind::Colon) {
        TokenKind const ahead = Peek(2).kind;
        if (ahead == TokenKind::WhileKeyword || ahead == TokenKind::DoKeyword ||
            ahead == TokenKind::LoopKeyword || ahead == TokenKind::ForKeyword) {
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
    if (CheckAny({TokenKind::PubKeyword, TokenKind::FuncKeyword, TokenKind::StructKeyword,
                  TokenKind::EnumKeyword, TokenKind::UnionKeyword, TokenKind::InterfaceKeyword,
                  TokenKind::ExtendKeyword, TokenKind::ModuleKeyword, TokenKind::ConstKeyword,
                  TokenKind::TypeKeyword, TokenKind::ExternKeyword})) {
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
    auto const loc = CurrentLocation();

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
    auto const loc = CurrentLocation();
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
    auto const loc = CurrentLocation();
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
    auto const loc = CurrentLocation();
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
    auto const loc = CurrentLocation();
    Expect(TokenKind::LoopKeyword, "expected 'loop'");

    auto s = std::make_unique<LoopStmt>();
    s->location = loc;
    s->body = ParseBlock();
    return s;
}

std::unique_ptr<ForStmt> Parser::ParseForStmt() {
    auto const loc = CurrentLocation();
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
    auto const loc = CurrentLocation();
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
    auto const loc = CurrentLocation();
    Expect(TokenKind::ReturnKeyword, "expected 'return'");

    auto s = std::make_unique<ReturnStmt>();
    s->location = loc;

    if (!Check(TokenKind::Semicolon)) {
        s->value = ParseExpr();
    }

    Expect(TokenKind::Semicolon, "expected ';'");
    return s;
}

// Expressions
ExprPtr Parser::ParseExpr() {
    return ParseAssign();
}

// right-associative: a = b = c  =>  a = (b = c)
ExprPtr Parser::ParseAssign() {
    auto left = ParseRange();
    if (!left) {
        return nullptr;
    }

    static constexpr TokenKind kAssignOps[] = {
        TokenKind::Assign,         TokenKind::PlusAssign,           TokenKind::MinusAssign,
        TokenKind::StarAssign,     TokenKind::SlashAssign,          TokenKind::PercentAssign,
        TokenKind::AmpAssign,      TokenKind::PipeAssign,           TokenKind::CaretAssign,
        TokenKind::LessLessAssign, TokenKind::GreaterGreaterAssign,
    };

    for (auto op : kAssignOps) {
        if (Check(op)) {
            auto const loc = CurrentLocation();
            Advance();
            auto right = ParseAssign(); // right-associative
            auto e = std::make_unique<AssignExpr>();
            e->location = loc;
            e->op = op;
            e->target = std::move(left);
            e->value = std::move(right);
            return e;
        }
    }
    return left;
}

ExprPtr Parser::ParseRange() {
    auto left = ParseTernary();
    if (!left) {
        return nullptr;
    }

    if (Check(TokenKind::DotDot) || Check(TokenKind::DotDotDot) || Check(TokenKind::DotDotEqual)) {
        // Leave bare `expr...` for ParseArgList to handle as a spread
        if (Peek().kind == TokenKind::DotDotDot) {
            TokenKind const next = Peek(1).kind;
            if (next == TokenKind::RightParen || next == TokenKind::Comma) {
                return left;
            }
        }
        bool const incl =
            Peek().kind == TokenKind::DotDotDot || Peek().kind == TokenKind::DotDotEqual;
        auto const loc = CurrentLocation();
        Advance();
        auto right = ParseTernary();
        auto e = std::make_unique<RangeExpr>();
        e->location = loc;
        e->inclusive = incl;
        e->lo = std::move(left);
        e->hi = std::move(right);
        return e;
    }
    return left;
}

ExprPtr Parser::ParseTernary() {
    auto cond = ParseOr();
    if (!cond) {
        return nullptr;
    }

    if (Match(TokenKind::Question)) {
        auto const loc = Previous().location;
        auto thenExpr = ParseOr();
        Expect(TokenKind::Colon, "expected ':' in ternary");
        auto elseExpr = ParseTernary(); // right-associative
        auto e = std::make_unique<TernaryExpr>();
        e->location = loc;
        e->condition = std::move(cond);
        e->thenExpr = std::move(thenExpr);
        e->elseExpr = std::move(elseExpr);
        return e;
    }
    return cond;
}

ExprPtr Parser::ParseOr() {
    auto left = ParseAnd();
    while (Check(TokenKind::PipePipe)) {
        auto const loc = CurrentLocation();
        auto const op = Advance().kind;
        auto right = ParseAnd();
        auto e = std::make_unique<BinaryExpr>();
        e->location = loc;
        e->op = op;
        e->left = std::move(left);
        e->right = std::move(right);
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::ParseAnd() {
    auto left = ParseBitOr();
    while (Check(TokenKind::AmpAmp)) {
        auto const loc = CurrentLocation();
        auto const op = Advance().kind;
        auto right = ParseBitOr();
        auto e = std::make_unique<BinaryExpr>();
        e->location = loc;
        e->op = op;
        e->left = std::move(left);
        e->right = std::move(right);
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::ParseBitOr() {
    auto left = ParseBitXor();
    while (Check(TokenKind::Pipe)) {
        auto const loc = CurrentLocation();
        auto const op = Advance().kind;
        auto right = ParseBitXor();
        auto e = std::make_unique<BinaryExpr>();
        e->location = loc;
        e->op = op;
        e->left = std::move(left);
        e->right = std::move(right);
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::ParseBitXor() {
    auto left = ParseBitAnd();
    while (Check(TokenKind::Caret)) {
        auto const loc = CurrentLocation();
        auto const op = Advance().kind;
        auto right = ParseBitAnd();
        auto e = std::make_unique<BinaryExpr>();
        e->location = loc;
        e->op = op;
        e->left = std::move(left);
        e->right = std::move(right);
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::ParseBitAnd() {
    auto left = ParseEquality();
    while (Check(TokenKind::Amp)) {
        auto const loc = CurrentLocation();
        auto const op = Advance().kind;
        auto right = ParseEquality();
        auto e = std::make_unique<BinaryExpr>();
        e->location = loc;
        e->op = op;
        e->left = std::move(left);
        e->right = std::move(right);
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::ParseEquality() {
    auto left = ParseComparison();
    while (CheckAny({TokenKind::Equal, TokenKind::BangEqual})) {
        auto const loc = CurrentLocation();
        auto const op = Advance().kind;
        auto right = ParseComparison();
        auto e = std::make_unique<BinaryExpr>();
        e->location = loc;
        e->op = op;
        e->left = std::move(left);
        e->right = std::move(right);
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::ParseComparison() {
    auto left = ParseShift();
    while (CheckAny(
        {TokenKind::Less, TokenKind::LessEqual, TokenKind::Greater, TokenKind::GreaterEqual})) {
        auto const loc = CurrentLocation();
        auto const op = Advance().kind;
        auto right = ParseShift();
        auto e = std::make_unique<BinaryExpr>();
        e->location = loc;
        e->op = op;
        e->left = std::move(left);
        e->right = std::move(right);
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::ParseCast() {
    auto left = ParsePostfix();
    while (CheckAny({TokenKind::AsKeyword, TokenKind::IsKeyword})) {
        auto const loc = CurrentLocation();
        if (Match(TokenKind::AsKeyword)) {
            auto type = ParseType();
            auto e = std::make_unique<CastExpr>();
            e->location = loc;
            e->operand = std::move(left);
            e->type = std::move(type);
            left = std::move(e);
        }
        else {
            Match(TokenKind::IsKeyword);
            auto type = ParseType();
            auto e = std::make_unique<IsExpr>();
            e->location = loc;
            e->operand = std::move(left);
            e->type = std::move(type);
            left = std::move(e);
        }
    }
    return left;
}

ExprPtr Parser::ParseShift() {
    auto left = ParseAdd();
    while (CheckAny({TokenKind::LessLess, TokenKind::GreaterGreater})) {
        auto const loc = CurrentLocation();
        auto const op = Advance().kind;
        auto right = ParseAdd();
        auto e = std::make_unique<BinaryExpr>();
        e->location = loc;
        e->op = op;
        e->left = std::move(left);
        e->right = std::move(right);
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::ParseAdd() {
    auto left = ParseMul();
    while (CheckAny({TokenKind::Plus, TokenKind::Minus})) {
        auto const loc = CurrentLocation();
        auto const op = Advance().kind;
        auto right = ParseMul();
        auto e = std::make_unique<BinaryExpr>();
        e->location = loc;
        e->op = op;
        e->left = std::move(left);
        e->right = std::move(right);
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::ParseMul() {
    auto left = ParseExp();
    while (CheckAny({TokenKind::Star, TokenKind::Slash, TokenKind::Percent})) {
        auto const loc = CurrentLocation();
        auto const op = Advance().kind;
        auto right = ParseExp();
        auto e = std::make_unique<BinaryExpr>();
        e->location = loc;
        e->op = op;
        e->left = std::move(left);
        e->right = std::move(right);
        left = std::move(e);
    }
    return left;
}

// ** is right-associative (exponentiation)
ExprPtr Parser::ParseExp() {
    auto left = ParseUnary();

    if (Check(TokenKind::Star) && Peek(1).kind == TokenKind::Star) {
        auto const loc = CurrentLocation();

        Advance(); // first *
        Advance(); // second *

        auto right = ParseExp(); // right-associative

        auto e = std::make_unique<BinaryExpr>();
        e->location = loc;
        e->op = TokenKind::StarStar; // keep AST/LIR compatibility
        e->left = std::move(left);
        e->right = std::move(right);

        return e;
    }

    return left;
}

ExprPtr Parser::ParseUnary() {
    if (CheckAny({TokenKind::Bang, TokenKind::Minus, TokenKind::Tilde, TokenKind::Star,
                  TokenKind::Amp, TokenKind::PlusPlus, TokenKind::MinusMinus})) {
        auto const loc = CurrentLocation();
        auto const op = Advance().kind;
        auto operand = ParseUnary();
        auto e = std::make_unique<UnaryExpr>();
        e->location = loc;
        e->op = op;
        e->operand = std::move(operand);
        return e;
    }
    return ParseCast();
}

ExprPtr Parser::ParsePostfix() {
    auto left = ParsePrimary();
    if (!left) {
        return nullptr;
    }
    while (true) {
        auto const loc = CurrentLocation();
        // Method/field/tuple-index: expr.field  expr.method(args)  expr.0
        if (Match(TokenKind::Dot)) {
            std::string name;
            if (Check(TokenKind::IntLiteral)) {
                name = Advance().text;
            }
            else {
                name = Expect(TokenKind::Ident, "expected field name or tuple index").text;
            }

            if (Check(TokenKind::LeftParen) && !name.empty() && !std::isdigit(name[0])) {
                // Method call: expr.method(args)
                auto args = ParseArgList();
                // Desugar to CallExpr with FieldExpr callee
                auto field = std::make_unique<FieldExpr>();
                field->location = loc;
                field->object = std::move(left);
                field->field = name;
                auto call = std::make_unique<CallExpr>();
                call->location = loc;
                call->callee = std::move(field);
                call->args = std::move(args);
                left = std::move(call);
            }
            else {
                auto e = std::make_unique<FieldExpr>();
                e->location = loc;
                e->object = std::move(left);
                e->field = name;
                left = std::move(e);
            }
            continue;
        }
        // Qualified path: expr::member
        if (Match(TokenKind::ColonColon)) {
            std::string const seg = Expect(TokenKind::Ident, "expected identifier").text;
            // Build or extend a PathExpr
            if (auto *path = dynamic_cast<PathExpr *>(left.get())) {
                path->segments.push_back(seg);
            }
            else {
                // Wrap existing expression in a path — treat the left side
                // as a segment This handles IDENT::IDENT::... chains
                auto p = std::make_unique<PathExpr>();
                p->location = loc;
                if (auto *ident = dynamic_cast<IdentExpr *>(left.get())) {
                    p->segments.push_back(ident->name);
                }
                p->segments.push_back(seg);
                left = std::move(p);
            }
            continue;
        }
        // Qualified initializer: Enum::Variant { field: value, ... }
        if (structInitAllowed && Check(TokenKind::LeftBrace)) {
            if (auto const *path = dynamic_cast<PathExpr const *>(left.get())) {
                auto e = std::make_unique<StructInitExpr>();
                e->location = loc;
                for (std::size_t i = 0; i < path->segments.size(); ++i) {
                    if (i) {
                        e->typeName += "::";
                    }
                    e->typeName += path->segments[i];
                }
                Advance(); // consume '{'
                while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
                    StructInitExpr::Field field;
                    field.location = CurrentLocation();
                    field.name = Expect(TokenKind::Ident, "expected field name").text;
                    Expect(TokenKind::Colon, "expected ':'");
                    field.value = ParseExpr();
                    e->fields.push_back(std::move(field));
                    if (!Match(TokenKind::Comma)) {
                        break;
                    }
                }
                Expect(TokenKind::RightBrace, "expected '}'");
                left = std::move(e);
                continue;
            }
        }
        // Function/direct call: expr(args)
        if (Check(TokenKind::LeftParen)) {
            auto args = ParseArgList();
            auto e = std::make_unique<CallExpr>();
            e->location = loc;
            e->callee = std::move(left);
            e->args = std::move(args);
            left = std::move(e);
            continue;
        }
        // Index: expr[idx]
        if (Match(TokenKind::LeftBracket)) {
            auto idx = ParseExpr();
            Expect(TokenKind::RightBracket, "expected ']'");
            auto e = std::make_unique<IndexExpr>();
            e->location = loc;
            e->object = std::move(left);
            e->index = std::move(idx);
            left = std::move(e);
            continue;
        }
        // Post-increment / post-decrement: expr++ or expr--
        if (Check(TokenKind::PlusPlus) || Check(TokenKind::MinusMinus)) {
            TokenKind const op = Advance().kind;
            auto e = std::make_unique<PostfixExpr>();
            e->location = loc;
            e->op = op;
            e->operand = std::move(left);
            left = std::move(e);
            continue;
        }

        break;
    }
    return left;
}

ExprPtr Parser::ParsePrimary() {
    auto const loc = CurrentLocation();
    if (Match(TokenKind::MatchKeyword)) {
        auto e = std::make_unique<MatchExpr>();
        e->location = loc;
        structInitAllowed = false;
        e->subject = ParseExpr();
        structInitAllowed = true;

        Expect(TokenKind::LeftBrace, "expected '{'");
        while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
            MatchExpr::Arm arm;
            arm.location = CurrentLocation();
            arm.pattern = ParsePattern();
            Expect(TokenKind::FatArrow, "expected '=>'");

            if (Check(TokenKind::LeftBrace)) {
                auto bexpr = std::make_unique<BlockExpr>();
                bexpr->location = CurrentLocation();
                bexpr->block = ParseBlock();
                arm.body = std::move(bexpr);
            }
            else {
                arm.body = ParseExpr();
            }

            e->arms.push_back(std::move(arm));
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
        return e;
    }

    // Literals
    if (Check(TokenKind::IntLiteral) || Check(TokenKind::FloatLiteral) ||
        Check(TokenKind::StringLiteral) || Check(TokenKind::CharLiteral) ||
        Check(TokenKind::BoolLiteral)) {
        auto e = std::make_unique<LiteralExpr>();
        e->location = loc;
        e->token = Advance();
        return e;
    }
    // null literal
    if (Match(TokenKind::NullKeyword)) {
        auto e = std::make_unique<LiteralExpr>();
        e->location = loc;
        e->token = Previous();
        return e;
    }
    // self
    if (Match(TokenKind::SelfKeyword)) {
        auto e = std::make_unique<SelfExpr>();
        e->location = loc;
        return e;
    }
    // Compile-time size query: sizeof(T)
    if (Check(TokenKind::Ident) && Peek().text == "sizeof") {
        Advance();
        auto e = std::make_unique<SizeOfExpr>();
        e->location = loc;
        Expect(TokenKind::LeftParen, "expected '(' after 'sizeof'");
        e->type = ParseType();
        Expect(TokenKind::RightParen, "expected ')' after sizeof type");
        return e;
    }
    // Compile-time intrinsics: #line, #column, #file, #function, #date, #ruxVersion
    // #time
    {
        using K = IntrinsicExpr::Kind;
        static constexpr std::pair<TokenKind, K> intrinsics[] = {
            {TokenKind::HashLine, K::Line},     {TokenKind::HashColumn, K::Column},
            {TokenKind::HashFile, K::File},     {TokenKind::HashFunction, K::Function},
            {TokenKind::HashDate, K::Date},     {TokenKind::HashTime, K::Time},
            {TokenKind::HashModule, K::Module}, {TokenKind::hashRuxVersion, K::RuxVersion},
            {TokenKind::hashOs, K::Os},
        };
        for (auto [tok, kind] : intrinsics) {
            if (Match(tok)) {
                auto e = std::make_unique<IntrinsicExpr>();
                e->location = loc;
                e->kind = kind;
                return e;
            }
        }
    }
    // Slice literal: [a, b, c]
    if (Match(TokenKind::LeftBracket)) {
        auto e = std::make_unique<SliceExpr>();
        e->location = loc;
        while (!Check(TokenKind::RightBracket) && !IsAtEnd()) {
            e->elements.push_back(ParseExpr());
            if (!Match(TokenKind::Comma)) {
                break;
            }
        }
        Expect(TokenKind::RightBracket, "expected ']'");
        return e;
    }
    // Grouped expression or tuple: (expr)  or  (expr, expr, ...)
    if (Match(TokenKind::LeftParen)) {
        auto first = ParseExpr();
        if (Match(TokenKind::Comma)) {
            auto t = std::make_unique<TupleExpr>();
            t->location = loc;
            t->elements.push_back(std::move(first));
            while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
                t->elements.push_back(ParseExpr());
                if (!Match(TokenKind::Comma)) {
                    break;
                }
            }
            Expect(TokenKind::RightParen, "expected ')'");
            return t;
        }
        Expect(TokenKind::RightParen, "expected ')'");
        return first;
    }
    // Identifier, possible struct init, or path expression
    if (Check(TokenKind::Ident)) {
        std::string const name = Advance().text;
        std::vector<TypeExprPtr> typeArgs;
        if (IsGenericStructInitAhead()) {
            typeArgs = ParseTypeArgs();
        }
        // Struct initialization: Name { field: value, ... }
        // Disabled in control-flow condition contexts to avoid ambiguity.
        if (structInitAllowed && Check(TokenKind::LeftBrace)) {
            auto e = std::make_unique<StructInitExpr>();
            e->location = loc;
            e->typeName = name;
            e->typeArgs = std::move(typeArgs);
            Advance(); // consume '{'
            while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
                StructInitExpr::Field field;
                field.location = CurrentLocation();
                field.name = Expect(TokenKind::Ident, "expected field name").text;
                Expect(TokenKind::Colon, "expected ':'");
                field.value = ParseExpr();
                e->fields.push_back(std::move(field));
                if (!Match(TokenKind::Comma)) {
                    break;
                }
            }
            Expect(TokenKind::RightBrace, "expected '}'");
            return e;
        }
        auto e = std::make_unique<IdentExpr>();
        e->location = loc;
        e->name = name;
        return e;
    }
    EmitError(loc, std::format("unexpected token '{}' in expression", Peek().text));
    return nullptr;
}

std::vector<ExprPtr> Parser::ParseArgList() {
    std::vector<ExprPtr> args;
    Expect(TokenKind::LeftParen, "expected '('");
    while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
        auto e = ParseExpr();
        if (Match(TokenKind::DotDotDot)) {
            auto const loc = e->location;
            auto spread = std::make_unique<SpreadExpr>();
            spread->location = loc;
            spread->operand = std::move(e);
            args.push_back(std::move(spread));
        }
        else {
            args.push_back(std::move(e));
        }
        if (!Match(TokenKind::Comma)) {
            break;
        }
    }
    Expect(TokenKind::RightParen, "expected ')'");
    return args;
}

// Patterns
PatternPtr Parser::ParsePattern() {
    auto inner = ParsePrimaryPattern();
    if (!inner) {
        return nullptr;
    }

    // Guard: pattern if condition
    if (Match(TokenKind::IfKeyword)) {
        auto const loc = Previous().location;
        auto guard = ParseExpr();
        auto p = std::make_unique<GuardedPattern>();
        p->location = loc;
        p->inner = std::move(inner);
        p->guard = std::move(guard);
        return p;
    }

    // Range pattern: lo..hi or lo...hi or lo..=hi
    if (Check(TokenKind::DotDot) || Check(TokenKind::DotDotDot) || Check(TokenKind::DotDotEqual)) {
        bool const incl =
            Peek().kind == TokenKind::DotDotDot || Peek().kind == TokenKind::DotDotEqual;
        auto const loc = CurrentLocation();
        Advance();
        auto hi = ParsePrimaryPattern();
        auto p = std::make_unique<RangePattern>();
        p->location = loc;
        p->inclusive = incl;
        p->lo = std::move(inner);
        p->hi = std::move(hi);
        return p;
    }

    return inner;
}

PatternPtr Parser::ParsePrimaryPattern() {
    auto const loc = CurrentLocation();

    // Wildcard: _
    if (Check(TokenKind::Ident) && Peek().text == "_") {
        Advance();
        auto p = std::make_unique<WildcardPattern>();
        p->location = loc;
        return p;
    }

    // Literals
    if (Check(TokenKind::IntLiteral) || Check(TokenKind::FloatLiteral) ||
        Check(TokenKind::StringLiteral) || Check(TokenKind::CharLiteral) ||
        Check(TokenKind::BoolLiteral) || Check(TokenKind::NullKeyword)) {
        auto p = std::make_unique<LiteralPattern>();
        p->location = loc;
        p->value = Advance();
        return p;
    }

    // Negative literal: -42
    if (Check(TokenKind::Minus) && Peek(1).Is(TokenKind::IntLiteral)) {
        Advance(); // consume '-'
        auto p = std::make_unique<LiteralPattern>();
        p->location = loc;
        p->value = Advance();
        // The negative sign is implicit; store it in text for diagnostics.
        p->value.text = "-" + p->value.text;
        return p;
    }

    // Tuple pattern: (a, b, ...)
    if (Match(TokenKind::LeftParen)) {
        auto p = std::make_unique<TuplePattern>();
        p->location = loc;
        while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
            p->elements.push_back(ParsePattern());
            if (!Match(TokenKind::Comma)) {
                break;
            }
        }
        Expect(TokenKind::RightParen, "expected ')'");
        return p;
    }

    // Identifier-started patterns: ident, EnumName::Variant(args), TypeName
    // { fields }
    if (Check(TokenKind::Ident)) {
        std::string const name = Advance().text;

        // Enum pattern: Event::Click(x, y)
        if (Check(TokenKind::ColonColon) && Peek(1).Is(TokenKind::Ident)) {
            std::vector<std::string> path = {name};
            while (Match(TokenKind::ColonColon)) {
                path.push_back(Expect(TokenKind::Ident, "expected variant name").text);
            }
            auto p = std::make_unique<EnumPattern>();
            p->location = loc;
            p->path = std::move(path);
            if (Match(TokenKind::LeftParen)) {
                while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
                    p->args.push_back(ParsePattern());
                    if (!Match(TokenKind::Comma)) {
                        break;
                    }
                }
                Expect(TokenKind::RightParen, "expected ')'");
            }
            else if (Match(TokenKind::LeftBrace)) {
                while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
                    EnumPattern::NamedArg arg;
                    arg.location = CurrentLocation();
                    arg.name = Expect(TokenKind::Ident, "expected variant field name").text;
                    if (Match(TokenKind::Colon)) {
                        arg.pattern = ParsePattern();
                    }
                    else {
                        auto binding = std::make_unique<IdentPattern>();
                        binding->location = arg.location;
                        binding->name = arg.name;
                        arg.pattern = std::move(binding);
                    }
                    p->namedArgs.push_back(std::move(arg));
                    if (!Match(TokenKind::Comma)) {
                        break;
                    }
                }
                Expect(TokenKind::RightBrace, "expected '}'");
            }
            return p;
        }

        // Struct pattern: TypeName { field: pat, ... }
        if (Check(TokenKind::LeftBrace)) {
            Advance(); // consume '{'
            auto p = std::make_unique<StructPattern>();
            p->location = loc;
            p->typeName = name;
            while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
                StructPattern::Field f;
                f.location = CurrentLocation();
                f.name = Expect(TokenKind::Ident, "expected field name").text;
                Expect(TokenKind::Colon, "expected ':'");
                f.pattern = ParsePattern();
                p->fields.push_back(std::move(f));
                if (!Match(TokenKind::Comma)) {
                    break;
                }
            }
            Expect(TokenKind::RightBrace, "expected '}'");
            return p;
        }

        // Simple identifier binding
        auto p = std::make_unique<IdentPattern>();
        p->location = loc;
        p->name = name;
        return p;
    }

    EmitError(loc, std::format("expected a pattern, got '{}'", Peek().text));
    return nullptr;
}

// AstPrinter  –  human-readable tree dump
namespace {
class AstPrinter {
public:
    explicit AstPrinter(std::ostream &out)
        : out(out) {
    }

    void Print(Module const &mod) {
        out << "Module \"" << mod.name << "\"\n";
        ++indent;
        for (auto const &item : mod.items) {
            if (item) {
                PrintDecl(*item);
            }
        }
        --indent;
    }

private:
    std::ostream &out;
    int indent = 0;

    // Helpers
    void Pad() const {
        for (int i = 0; i < indent; ++i) {
            out << "  ";
        }
    }

    static std::string TypeStr(TypeExpr const *t) {
        if (!t) {
            return "<null>";
        }
        if (auto const *n = dynamic_cast<NamedTypeExpr const *>(t)) {
            std::string s = n->name;
            if (!n->typeArgs.empty()) {
                s += "<";
                for (std::size_t i = 0; i < n->typeArgs.size(); ++i) {
                    if (i) {
                        s += ", ";
                    }
                    s += TypeStr(n->typeArgs[i].get());
                }
                s += ">";
            }
            return s;
        }
        if (auto const *p = dynamic_cast<PathTypeExpr const *>(t)) {
            std::string s;
            for (std::size_t i = 0; i < p->segments.size(); ++i) {
                if (i) {
                    s += "::";
                }
                s += p->segments[i];
            }
            return s;
        }
        if (auto const *a = dynamic_cast<SliceTypeExpr const *>(t)) {
            std::string s = TypeStr(a->element.get()) + "[";
            if (a->size) {
                s += "N"; // size is an Expr, not easily stringified
            }
            return s + "]";
        }
        if (auto const *ptr = dynamic_cast<PointerTypeExpr const *>(t)) {
            return "*" + TypeStr(ptr->pointee.get());
        }
        if (auto const *tup = dynamic_cast<TupleTypeExpr const *>(t)) {
            std::string s = "(";
            for (std::size_t i = 0; i < tup->elements.size(); ++i) {
                if (i) {
                    s += ", ";
                }
                s += TypeStr(tup->elements[i].get());
            }
            return s + ")";
        }
        if (dynamic_cast<SelfTypeExpr const *>(t)) {
            return "self";
        }
        return "<type>";
    }

    static std::string_view OpStr(TokenKind const op) noexcept {
        switch (op) {
        case TokenKind::Plus:
            return "+";
        case TokenKind::Minus:
            return "-";
        case TokenKind::Star:
            return "*";
        case TokenKind::Slash:
            return "/";
        case TokenKind::Percent:
            return "%";
        case TokenKind::StarStar:
            return "**";
        case TokenKind::Amp:
            return "&";
        case TokenKind::Pipe:
            return "|";
        case TokenKind::Caret:
            return "^";
        case TokenKind::Tilde:
            return "~";
        case TokenKind::LessLess:
            return "<<";
        case TokenKind::GreaterGreater:
            return ">>";
        case TokenKind::AmpAmp:
            return "&&";
        case TokenKind::PipePipe:
            return "||";
        case TokenKind::Bang:
            return "!";
        case TokenKind::Equal:
            return "==";
        case TokenKind::BangEqual:
            return "!=";
        case TokenKind::Less:
            return "<";
        case TokenKind::LessEqual:
            return "<=";
        case TokenKind::Greater:
            return ">";
        case TokenKind::GreaterEqual:
            return ">=";
        case TokenKind::Assign:
            return "=";
        case TokenKind::PlusAssign:
            return "+=";
        case TokenKind::MinusAssign:
            return "-=";
        case TokenKind::StarAssign:
            return "*=";
        case TokenKind::SlashAssign:
            return "/=";
        case TokenKind::PercentAssign:
            return "%=";
        case TokenKind::AmpAssign:
            return "&=";
        case TokenKind::PipeAssign:
            return "|=";
        case TokenKind::CaretAssign:
            return "^=";
        case TokenKind::LessLessAssign:
            return "<<=";
        case TokenKind::GreaterGreaterAssign:
            return ">>=";
        default:
            return "?";
        }
    }

    // Declarations

    void PrintDecl(Decl const &decl) {
        if (auto const *fn = dynamic_cast<FuncDecl const *>(&decl)) {
            PrintFuncDecl(*fn);
        }
        else if (auto const *st = dynamic_cast<StructDecl const *>(&decl)) {
            PrintStructDecl(*st);
        }
        else if (auto const *en = dynamic_cast<EnumDecl const *>(&decl)) {
            PrintEnumDecl(*en);
        }
        else if (auto const *un = dynamic_cast<UnionDecl const *>(&decl)) {
            PrintUnionDecl(*un);
        }
        else if (auto const *iface = dynamic_cast<InterfaceDecl const *>(&decl)) {
            PrintInterfaceDecl(*iface);
        }
        else if (auto const *impl = dynamic_cast<ImplDecl const *>(&decl)) {
            PrintImplDecl(*impl);
        }
        else if (auto const *mod = dynamic_cast<ModuleDecl const *>(&decl)) {
            PrintModuleDecl(*mod);
        }
        else if (auto const *use = dynamic_cast<UseDecl const *>(&decl)) {
            PrintUseDecl(*use);
        }
        else if (auto const *cnst = dynamic_cast<ConstDecl const *>(&decl)) {
            PrintConstDecl(*cnst);
        }
        else if (auto const *alias = dynamic_cast<TypeAliasDecl const *>(&decl)) {
            PrintTypeAliasDecl(*alias);
        }
        else if (auto const *extFn = dynamic_cast<ExternFuncDecl const *>(&decl)) {
            PrintExternFuncDecl(*extFn);
        }
        else if (auto const *extVar = dynamic_cast<ExternVarDecl const *>(&decl)) {
            PrintExternVarDecl(*extVar);
        }
    }

    void PrintFuncDecl(FuncDecl const &f) {
        Pad();
        if (f.isPublic) {
            out << "pub ";
        }
        if (f.isAsm) {
            out << "asm ";
        }
        out << "FuncDecl '" << f.name << "'";
        // Generic params
        if (!f.typeParams.empty()) {
            out << '<';
            for (std::size_t i = 0; i < f.typeParams.size(); ++i) {
                if (i) {
                    out << ", ";
                }
                out << f.typeParams[i];
            }
            out << '>';
        }
        // Params
        out << " (";
        for (std::size_t i = 0; i < f.params.size(); ++i) {
            if (i) {
                out << ", ";
            }
            auto const &p = f.params[i];
            if (p.isVariadic) {
                out << "...";
                continue;
            }
            out << p.name << ": " << TypeStr(p.type.get());
        }
        out << ')';
        // Return type
        if (f.returnType) {
            out << " -> " << TypeStr(f.returnType->get());
        }
        out << (f.body ? "" : " [signature]") << '\n';
        if (f.body) {
            ++indent;
            PrintBlock(*f.body);
            --indent;
        }
    }

    void PrintStructDecl(StructDecl const &s) {
        Pad();
        if (s.isPublic) {
            out << "pub ";
        }
        out << "StructDecl '" << s.name << "'";
        if (!s.typeParams.empty()) {
            out << '<';
            for (std::size_t i = 0; i < s.typeParams.size(); ++i) {
                if (i) {
                    out << ", ";
                }
                out << s.typeParams[i];
            }
            out << '>';
        }
        out << '\n';
        ++indent;
        for (auto const &f : s.fields) {
            Pad();
            if (f.isPublic) {
                out << "pub ";
            }
            out << "Field '" << f.name << "' : " << TypeStr(f.type.get()) << '\n';
        }
        --indent;
    }

    void PrintEnumDecl(EnumDecl const &e) {
        Pad();
        if (e.isPublic) {
            out << "pub ";
        }
        out << "EnumDecl '" << e.name << "'";
        if (e.baseType) {
            out << " : " << TypeStr(e.baseType.get());
        }
        out << '\n';
        ++indent;
        for (auto const &v : e.variants) {
            Pad();
            out << "Variant '" << v.name << "'";
            if (!v.fields.empty()) {
                out << " (";
                for (std::size_t i = 0; i < v.fields.size(); ++i) {
                    if (i) {
                        out << ", ";
                    }
                    out << TypeStr(v.fields[i].get());
                }
                out << ')';
            }
            if (!v.namedFields.empty()) {
                out << " { ";
                for (std::size_t i = 0; i < v.namedFields.size(); ++i) {
                    if (i) {
                        out << " ";
                    }
                    out << v.namedFields[i].name << ": " << TypeStr(v.namedFields[i].type.get())
                        << ";";
                }
                out << " }";
            }
            if (v.discriminant) {
                out << " = " << *v.discriminant;
            }
            out << '\n';
        }
        --indent;
    }

    void PrintUnionDecl(UnionDecl const &u) {
        Pad();
        if (u.isPublic) {
            out << "pub ";
        }
        out << "UnionDecl '" << u.name << "'\n";
        ++indent;
        for (auto const &f : u.fields) {
            Pad();
            out << "Field '" << f.name << "' : " << TypeStr(f.type.get()) << '\n';
        }
        --indent;
    }

    void PrintInterfaceDecl(InterfaceDecl const &iface) {
        Pad();
        if (iface.isPublic) {
            out << "pub ";
        }
        out << "InterfaceDecl '" << iface.name << "'\n";
        ++indent;
        for (auto const &m : iface.methods) {
            if (m) {
                PrintFuncDecl(*m);
            }
        }
        --indent;
    }

    void PrintImplDecl(ImplDecl const &impl) {
        Pad();
        out << "ImplDecl ";
        if (impl.interfaceName) {
            out << *impl.interfaceName << " for ";
        }
        out << impl.typeName << '\n';
        ++indent;
        for (auto const &m : impl.methods) {
            if (m) {
                PrintFuncDecl(*m);
            }
        }
        --indent;
    }

    void PrintModuleDecl(ModuleDecl const &mod) {
        Pad();
        if (mod.isPublic) {
            out << "pub ";
        }
        out << "ModuleDecl '" << mod.name << "'\n";
        ++indent;
        for (auto const &item : mod.items) {
            if (item) {
                PrintDecl(*item);
            }
        }
        --indent;
    }

    void PrintUseDecl(UseDecl const &u) const {
        Pad();
        if (!u.targetOs.empty()) {
            out << "@[Target(\"" << u.targetOs << "\")]\n";
        }
        Pad();
        out << "ImportDecl '";
        for (std::size_t i = 0; i < u.path.size(); ++i) {
            if (i) {
                out << '.';
            }
            out << u.path[i];
        }
        switch (u.kind) {
        case UseDecl::Kind::Glob:
            out << ".*";
            break;
        case UseDecl::Kind::Multi: {
            out << "::{";
            for (std::size_t i = 0; i < u.names.size(); ++i) {
                if (i) {
                    out << ", ";
                }
                out << u.names[i];
            }
            out << '}';
            break;
        }
        default:
            break;
        }
        out << "'\n";
    }

    void PrintConstDecl(ConstDecl const &c) {
        Pad();
        if (c.isPublic) {
            out << "pub ";
        }
        out << "ConstDecl '" << c.name << "'";
        if (c.type) {
            out << " : " << TypeStr(c.type->get());
        }
        out << '\n';
        ++indent;
        if (c.value) {
            PrintExpr(*c.value);
        }
        --indent;
    }

    void PrintTypeAliasDecl(TypeAliasDecl const &t) const {
        Pad();
        if (t.isPublic) {
            out << "pub ";
        }
        out << "TypeAliasDecl '" << t.name << "' = " << TypeStr(t.type.get()) << '\n';
    }

    void PrintExternFuncDecl(ExternFuncDecl const &f) const {
        if (!f.dll.empty()) {
            Pad();
            out << "@[Import(lib: \"" << f.dll << "\")]\n";
        }
        if (f.callConv == CallingConvention::Win64) {
            Pad();
            out << "@[Call(.Win64)]\n";
        }
        Pad();
        if (f.isPublic) {
            out << "pub ";
        }
        out << "ExternFuncDecl '" << f.name << "' (";
        for (std::size_t i = 0; i < f.params.size(); ++i) {
            if (i) {
                out << ", ";
            }
            out << f.params[i].name << ": " << TypeStr(f.params[i].type.get());
        }
        if (f.isVariadic) {
            out << (f.params.empty() ? "..." : ", ...");
        }
        out << ')';
        if (f.returnType) {
            out << " -> " << TypeStr(f.returnType->get());
        }
        out << '\n';
    }

    void PrintExternVarDecl(ExternVarDecl const &v) const {
        Pad();
        if (v.isPublic) {
            out << "pub ";
        }
        out << "ExternVarDecl '" << v.name << "' : " << TypeStr(v.type.get()) << '\n';
    }

    // Block
    void PrintBlock(Block const &block) {
        Pad();
        out << "Block [" << block.stmts.size() << " stmt" << (block.stmts.size() == 1 ? "" : "s")
            << "]\n";
        ++indent;
        for (auto const &stmt : block.stmts) {
            if (stmt) {
                PrintStmt(*stmt);
            }
        }
        --indent;
    }

    // Statements
    void PrintStmt(Stmt const &stmt) {
        if (auto const *let = dynamic_cast<LetStmt const *>(&stmt)) {
            PrintLetStmt(*let);
        }
        else if (auto const *ifStmt = dynamic_cast<IfStmt const *>(&stmt)) {
            PrintIfStmt(*ifStmt);
        }
        else if (auto const *whileStmt = dynamic_cast<WhileStmt const *>(&stmt)) {
            PrintWhileStmt(*whileStmt);
        }
        else if (auto const *forStmt = dynamic_cast<ForStmt const *>(&stmt)) {
            PrintForStmt(*forStmt);
        }
        else if (auto const *matchStmt = dynamic_cast<MatchStmt const *>(&stmt)) {
            PrintMatchStmt(*matchStmt);
        }
        else if (auto const *ret = dynamic_cast<ReturnStmt const *>(&stmt)) {
            PrintReturnStmt(*ret);
        }
        else if (dynamic_cast<BreakStmt const *>(&stmt)) {
            Pad();
            out << "BreakStmt\n";
        }
        else if (dynamic_cast<ContinueStmt const *>(&stmt)) {
            Pad();
            out << "ContinueStmt\n";
        }
        else if (auto const *exprStmt = dynamic_cast<ExprStmt const *>(&stmt)) {
            Pad();
            out << "ExprStmt\n";
            ++indent;
            if (exprStmt->expr) {
                PrintExpr(*exprStmt->expr);
            }
            --indent;
        }
        else if (auto const *declStmt = dynamic_cast<DeclStmt const *>(&stmt)) {
            if (declStmt->decl) {
                PrintDecl(*declStmt->decl);
            }
        }
    }

    void PrintLetStmt(LetStmt const &s) {
        Pad();
        out << "LetStmt '";
        if (s.pattern) {
            out << "<pattern>";
        }
        else {
            out << s.name;
        }
        out << "' (" << (s.isMut ? "var" : "let") << ")";
        if (s.type) {
            out << " : " << TypeStr(s.type->get());
        }
        out << '\n';
        ++indent;
        if (s.pattern) {
            PrintPattern(*s.pattern);
        }
        if (s.init) {
            PrintExpr(*s.init);
        }
        --indent;
    }

    void PrintIfStmt(IfStmt const &s) {
        Pad();
        out << "IfStmt\n";
        ++indent;

        Pad();
        out << "Condition\n";
        ++indent;
        if (s.condition) {
            PrintExpr(*s.condition);
        }
        --indent;

        Pad();
        out << "Then\n";
        ++indent;
        if (s.thenBlock) {
            PrintBlock(*s.thenBlock);
        }
        --indent;

        for (auto const &elif : s.elseIfs) {
            Pad();
            out << "ElseIf\n";
            ++indent;
            Pad();
            out << "Condition\n";
            ++indent;
            if (elif.condition) {
                PrintExpr(*elif.condition);
            }
            --indent;
            if (elif.block) {
                PrintBlock(*elif.block);
            }
            --indent;
        }

        if (s.elseBlock) {
            Pad();
            out << "Else\n";
            ++indent;
            PrintBlock(*s.elseBlock);
            --indent;
        }
        --indent;
    }

    void PrintWhileStmt(WhileStmt const &s) {
        Pad();
        out << "WhileStmt\n";
        ++indent;
        Pad();
        out << "Condition\n";
        ++indent;
        if (s.condition) {
            PrintExpr(*s.condition);
        }
        --indent;
        if (s.body) {
            PrintBlock(*s.body);
        }
        --indent;
    }

    void PrintForStmt(ForStmt const &s) {
        Pad();
        out << "ForStmt '" << s.variable << "' in\n";
        ++indent;
        if (s.iterable) {
            PrintExpr(*s.iterable);
        }
        if (s.body) {
            PrintBlock(*s.body);
        }
        --indent;
    }

    void PrintMatchStmt(MatchStmt const &s) {
        Pad();
        out << "MatchStmt\n";
        ++indent;
        Pad();
        out << "Subject\n";
        ++indent;
        if (s.subject) {
            PrintExpr(*s.subject);
        }
        --indent;
        for (auto const &arm : s.arms) {
            Pad();
            out << "Arm\n";
            ++indent;
            if (arm.pattern) {
                PrintPattern(*arm.pattern);
            }
            if (arm.body) {
                PrintExpr(*arm.body);
            }
            --indent;
        }
        --indent;
    }

    void PrintReturnStmt(ReturnStmt const &s) {
        Pad();
        out << "ReturnStmt\n";
        if (s.value) {
            ++indent;
            PrintExpr(**s.value);
            --indent;
        }
    }

    // Expressions
    void PrintExpr(Expr const &expr) {
        if (auto const *litExpr = dynamic_cast<LiteralExpr const *>(&expr)) {
            PrintLiteralExpr(*litExpr);
        }
        else if (auto const *identExpr = dynamic_cast<IdentExpr const *>(&expr)) {
            Pad();
            out << "IdentExpr '" << identExpr->name << "'\n";
        }
        else if (auto const *selExpr = dynamic_cast<SelfExpr const *>(&expr)) {
            (void)selExpr;
            Pad();
            out << "SelfExpr\n";
        }
        else if (auto const *pathExpr = dynamic_cast<PathExpr const *>(&expr)) {
            Pad();
            out << "PathExpr '";
            for (std::size_t i = 0; i < pathExpr->segments.size(); ++i) {
                if (i) {
                    out << "::";
                }
                out << pathExpr->segments[i];
            }
            out << "'\n";
        }
        else if (auto const *sizeOfExpr = dynamic_cast<SizeOfExpr const *>(&expr)) {
            Pad();
            out << "SizeOfExpr " << TypeStr(sizeOfExpr->type.get()) << '\n';
        }
        else if (auto const *intr = dynamic_cast<IntrinsicExpr const *>(&expr)) {
            static constexpr char const *names[] = {"#line",     "#column",     "#file",
                                                    "#function", "#date",       "#time",
                                                    "#module",   "#ruxVersion", "#os"};
            Pad();
            out << "IntrinsicExpr " << names[static_cast<int>(intr->kind)] << '\n';
        }
        else if (auto const *unaryExpr = dynamic_cast<UnaryExpr const *>(&expr)) {
            Pad();
            out << "UnaryExpr " << OpStr(unaryExpr->op) << '\n';
            ++indent;
            if (unaryExpr->operand) {
                PrintExpr(*unaryExpr->operand);
            }
            --indent;
        }
        else if (auto const *binaryExpr = dynamic_cast<BinaryExpr const *>(&expr)) {
            Pad();
            out << "BinaryExpr " << OpStr(binaryExpr->op) << '\n';
            ++indent;
            if (binaryExpr->left) {
                PrintExpr(*binaryExpr->left);
            }
            if (binaryExpr->right) {
                PrintExpr(*binaryExpr->right);
            }
            --indent;
        }
        else if (auto const *assignExpr = dynamic_cast<AssignExpr const *>(&expr)) {
            Pad();
            out << "AssignExpr " << OpStr(assignExpr->op) << '\n';
            ++indent;
            if (assignExpr->target) {
                PrintExpr(*assignExpr->target);
            }
            if (assignExpr->value) {
                PrintExpr(*assignExpr->value);
            }
            --indent;
        }
        else if (auto const *tern = dynamic_cast<TernaryExpr const *>(&expr)) {
            Pad();
            out << "TernaryExpr\n";
            ++indent;
            Pad();
            out << "Condition\n";
            ++indent;
            if (tern->condition) {
                PrintExpr(*tern->condition);
            }
            --indent;
            Pad();
            out << "Then\n";
            ++indent;
            if (tern->thenExpr) {
                PrintExpr(*tern->thenExpr);
            }
            --indent;
            Pad();
            out << "Else\n";
            ++indent;
            if (tern->elseExpr) {
                PrintExpr(*tern->elseExpr);
            }
            --indent;
            --indent;
        }
        else if (auto const *rng = dynamic_cast<RangeExpr const *>(&expr)) {
            Pad();
            out << "RangeExpr " << (rng->inclusive ? "..." : "..") << '\n';
            ++indent;
            if (rng->lo) {
                PrintExpr(*rng->lo);
            }
            if (rng->hi) {
                PrintExpr(*rng->hi);
            }
            --indent;
        }
        else if (auto const *call = dynamic_cast<CallExpr const *>(&expr)) {
            Pad();
            out << "CallExpr\n";
            ++indent;
            Pad();
            out << "Callee\n";
            ++indent;
            if (call->callee) {
                PrintExpr(*call->callee);
            }
            --indent;
            if (!call->args.empty()) {
                Pad();
                out << "Args [" << call->args.size() << "]\n";
                ++indent;
                for (auto const &a : call->args) {
                    if (a) {
                        PrintExpr(*a);
                    }
                }
                --indent;
            }
            --indent;
        }
        else if (auto const *index = dynamic_cast<IndexExpr const *>(&expr)) {
            Pad();
            out << "IndexExpr\n";
            ++indent;
            if (index->object) {
                PrintExpr(*index->object);
            }
            if (index->index) {
                PrintExpr(*index->index);
            }
            --indent;
        }
        else if (auto const *fieldExpr = dynamic_cast<FieldExpr const *>(&expr)) {
            Pad();
            out << "FieldExpr '." << fieldExpr->field << "'\n";
            ++indent;
            if (fieldExpr->object) {
                PrintExpr(*fieldExpr->object);
            }
            --indent;
        }
        else if (auto const *structInitExpr = dynamic_cast<StructInitExpr const *>(&expr)) {
            Pad();
            out << "StructInitExpr '" << structInitExpr->typeName;
            if (!structInitExpr->typeArgs.empty()) {
                out << "<";
                for (std::size_t i = 0; i < structInitExpr->typeArgs.size(); ++i) {
                    if (i) {
                        out << ", ";
                    }
                    out << TypeStr(structInitExpr->typeArgs[i].get());
                }
                out << ">";
            }
            out << "'\n";
            ++indent;
            for (auto const &f : structInitExpr->fields) {
                Pad();
                out << "." << f.name << " =\n";
                ++indent;
                if (f.value) {
                    PrintExpr(*f.value);
                }
                --indent;
            }
            --indent;
        }
        else if (auto const *sliceExpr = dynamic_cast<SliceExpr const *>(&expr)) {
            Pad();
            out << "SliceExpr [" << sliceExpr->elements.size() << "]\n";
            ++indent;
            for (auto const &e : sliceExpr->elements) {
                if (e) {
                    PrintExpr(*e);
                }
            }
            --indent;
        }
        else if (auto const *castExpr = dynamic_cast<CastExpr const *>(&expr)) {
            Pad();
            out << "CastExpr as " << TypeStr(castExpr->type.get()) << '\n';
            ++indent;
            if (castExpr->operand) {
                PrintExpr(*castExpr->operand);
            }
            --indent;
        }
        else if (auto const *isExpr = dynamic_cast<IsExpr const *>(&expr)) {
            Pad();
            out << "IsExpr is " << TypeStr(isExpr->type.get()) << '\n';
            ++indent;
            if (isExpr->operand) {
                PrintExpr(*isExpr->operand);
            }
            --indent;
        }
        else if (auto const *blockExpr = dynamic_cast<BlockExpr const *>(&expr)) {
            if (blockExpr->block) {
                PrintBlock(*blockExpr->block);
            }
        }
        else if (auto const *matchExpr = dynamic_cast<MatchExpr const *>(&expr)) {
            Pad();
            out << "MatchExpr\n";
            ++indent;
            if (matchExpr->subject) {
                PrintExpr(*matchExpr->subject);
            }
            for (auto const &arm : matchExpr->arms) {
                Pad();
                out << "Arm\n";
                ++indent;
                PrintPattern(*arm.pattern);
                if (arm.body) {
                    PrintExpr(*arm.body);
                }
                --indent;
            }
            --indent;
        }
    }

    void PrintLiteralExpr(LiteralExpr const &e) const {
        Pad();
        out << "LiteralExpr (";
        switch (e.token.kind) {
        case TokenKind::IntLiteral:
            out << "int";
            break;
        case TokenKind::FloatLiteral:
            out << "float";
            break;
        case TokenKind::StringLiteral:
            out << "string";
            break;
        case TokenKind::CharLiteral:
            out << "char32";
            break;
        case TokenKind::BoolLiteral:
            out << "bool8";
            break;
        case TokenKind::NullKeyword:
            out << "null";
            break;
        default:
            out << "?";
            break;
        }
        out << ") '" << e.token.text << "'\n";
    }

    // Patterns
    void PrintPattern(Pattern const &pat) {
        if (dynamic_cast<WildcardPattern const *>(&pat)) {
            Pad();
            out << "WildcardPattern\n";
        }
        else if (auto const *litPat = dynamic_cast<LiteralPattern const *>(&pat)) {
            Pad();
            out << "LiteralPattern '" << litPat->value.text << "'\n";
        }
        else if (auto const *idPat = dynamic_cast<IdentPattern const *>(&pat)) {
            Pad();
            out << "IdentPattern '" << idPat->name << "'\n";
        }
        else if (auto const *rngPat = dynamic_cast<RangePattern const *>(&pat)) {
            Pad();
            out << "RangePattern " << (rngPat->inclusive ? "..." : "..") << '\n';
            ++indent;
            if (rngPat->lo) {
                PrintPattern(*rngPat->lo);
            }
            if (rngPat->hi) {
                PrintPattern(*rngPat->hi);
            }
            --indent;
        }
        else if (auto const *enumPat = dynamic_cast<EnumPattern const *>(&pat)) {
            Pad();
            out << "EnumPattern '";
            for (std::size_t i = 0; i < enumPat->path.size(); ++i) {
                if (i) {
                    out << '.';
                }
                out << enumPat->path[i];
            }
            out << "'";
            if (!enumPat->args.empty()) {
                out << " [" << enumPat->args.size() << " bindings]";
            }
            if (!enumPat->namedArgs.empty()) {
                out << " [" << enumPat->namedArgs.size() << " fields]";
            }
            out << '\n';
            if (!enumPat->args.empty() || !enumPat->namedArgs.empty()) {
                ++indent;
                for (auto const &a : enumPat->args) {
                    if (a) {
                        PrintPattern(*a);
                    }
                }
                for (auto const &a : enumPat->namedArgs) {
                    Pad();
                    out << "." << a.name << ":\n";
                    ++indent;
                    if (a.pattern) {
                        PrintPattern(*a.pattern);
                    }
                    --indent;
                }
                --indent;
            }
        }
        else if (auto const *structPat = dynamic_cast<StructPattern const *>(&pat)) {
            Pad();
            out << "StructPattern '" << structPat->typeName << "'\n";
            ++indent;
            for (auto const &f : structPat->fields) {
                Pad();
                out << "." << f.name << ":\n";
                ++indent;
                if (f.pattern) {
                    PrintPattern(*f.pattern);
                }
                --indent;
            }
            --indent;
        }
        else if (auto const *tuplePat = dynamic_cast<TuplePattern const *>(&pat)) {
            Pad();
            out << "TuplePattern [" << tuplePat->elements.size() << "]\n";
            ++indent;
            for (auto const &e : tuplePat->elements) {
                if (e) {
                    PrintPattern(*e);
                }
            }
            --indent;
        }
        else if (auto const *guardedPat = dynamic_cast<GuardedPattern const *>(&pat)) {
            Pad();
            out << "GuardedPattern\n";
            ++indent;
            if (guardedPat->inner) {
                PrintPattern(*guardedPat->inner);
            }
            Pad();
            out << "Guard\n";
            ++indent;
            if (guardedPat->guard) {
                PrintExpr(*guardedPat->guard);
            }
            --indent;
            --indent;
        }
    }
};
} // namespace

bool Parser::DumpAst(ParseResult const &result, std::filesystem::path const &path) {
    std::ofstream f(path);
    if (!f) {
        return false;
    }
    AstPrinter printer(f);
    printer.Print(result.module);
    if (!result.diagnostics.empty()) {
        f << "\n--- diagnostics ---\n";
        for (auto const &d : result.diagnostics) {
            f << std::format(
                "{:>4}:{:<4}  {}  {}\n", d.location.line, d.location.column,
                d.severity == ParserDiagnostic::Severity::Error ? "error  " : "warning", d.message);
        }
    }
    return f.good();
}
} // namespace Rux
