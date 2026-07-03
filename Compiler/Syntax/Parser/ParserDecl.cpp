// Declaration parsing: attributes, functions, types, modules, imports.

#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Syntax/Parser/Parser.h"

namespace Rux {
// Attribute parsing
static std::string DecodeStringLiteralText(const std::string &text) {
    std::string out;
    const std::size_t quote = text.find('"');
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
        case 'a':
            out += '\a';
            break;
        case 'b':
            out += '\b';
            break;
        case 'f':
            out += '\f';
            break;
        case 'v':
            out += '\v';
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

        const SourceLocation attrLoc = CurrentLocation();
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
                    EmitWarning(CurrentLocation(), std::format("unknown calling convention '.{}'", variant));
                }
            }
            else if (attrName == "Target" && Check(TokenKind::StringLiteral)) {
                // @[Target("Windows")] — positional OS string
                const Token tok = Advance();
                std::string os = DecodeStringLiteralText(tok.text);
                if (os == "MacOS" || os == "Macos" || os == "macos") {
                    os = "macOS";
                }
                if (os != "BSD" && os != "Illumos" && os != "Linux" && os != "macOS" && os != "Windows") {
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
    const auto loc = CurrentLocation();

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
std::unique_ptr<FuncDecl> Parser::ParseFuncDecl(bool isPublic, bool isAsm, CallingConvention callConv) {
    const auto loc = CurrentLocation();
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
    const auto loc = CurrentLocation();
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
std::unique_ptr<EnumDecl> Parser::ParseEnumDecl(const bool isPublic) {
    const auto loc = CurrentLocation();
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
                EmitError(Previous().location, "trailing comma is not allowed in enum declarations");
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
    const auto loc = CurrentLocation();
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
    const auto loc = CurrentLocation();
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
    const auto loc = CurrentLocation();
    Expect(TokenKind::ExtendKeyword, "expected 'extend'");

    auto decl = std::make_unique<ImplDecl>();
    decl->location = loc;

    // extend TypeName  or  extend TypeName : InterfaceName  or  extend
    // InterfaceName for TypeName
    const std::string firstName = Expect(TokenKind::Ident, "expected type name").text;
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
    const auto loc = CurrentLocation();
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
    const auto loc = CurrentLocation();
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
    const auto loc = CurrentLocation();
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
    const auto loc = CurrentLocation();
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
    const auto loc = CurrentLocation();
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
                while (!IsAtEnd() && !Check(TokenKind::Semicolon) && !Check(TokenKind::RightBrace)) {
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
} // namespace Rux
