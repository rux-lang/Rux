/*
    Rux Compiler
    Copyright © 2026 Ivan Muzyka
    Licensed under the MIT License
*/

#include "Rux/Parser.h"

#include <cassert>
#include <format>
#include <fstream>
#include <print>

namespace Rux {
    // =========================================================================
    // ParseResult
    // =========================================================================

    bool ParseResult::HasErrors() const noexcept {
        for (const auto &d: diagnostics)
            if (d.severity == ParserDiagnostic::Severity::Error)
                return true;
        return false;
    }

    // =========================================================================
    // Constructor / FromLexResult
    // =========================================================================

    Parser::Parser(std::vector<Token> tokens, std::string sourceName)
        : tokens(std::move(tokens))
          , sourceName(std::move(sourceName)) {
    }

    std::optional<ParseResult> Parser::FromLexResult(const LexerResult &lex,
                                                     const std::string &sourceName) {
        if (lex.HasErrors()) return std::nullopt;
        Parser p(lex.tokens, sourceName);
        return p.Parse();
    }

    // =========================================================================
    // Parse  –  top-level entry point
    // =========================================================================

    ParseResult Parser::Parse() {
        Module mod;
        mod.name = sourceName;

        while (!IsAtEnd()) {
            auto decl = ParseDecl();
            if (decl)
                mod.items.push_back(std::move(decl));
            else
                Synchronize();
        }

        return ParseResult{std::move(mod), std::move(diagnostics)};
    }

    // =========================================================================
    // Token helpers
    // =========================================================================

    const Token &Parser::Peek(std::size_t ahead) const noexcept {
        const std::size_t idx = pos + ahead;
        if (idx < tokens.size()) return tokens[idx];
        return tokens.back(); // EOF sentinel
    }

    const Token &Parser::Advance() noexcept {
        if (!IsAtEnd()) ++pos;
        return tokens[pos - 1];
    }

    bool Parser::Check(const TokenKind kind) const noexcept {
        return Peek().kind == kind;
    }

    bool Parser::CheckAny(std::initializer_list<TokenKind> kinds) const noexcept {
        for (auto k: kinds)
            if (Check(k)) return true;
        return false;
    }

    bool Parser::Match(const TokenKind kind) noexcept {
        if (!Check(kind)) return false;
        Advance();
        return true;
    }

    const Token &Parser::Expect(const TokenKind kind, const std::string_view message) {
        if (Check(kind)) return Advance();
        EmitError(CurrentLocation(), std::string(message));
        return Peek(); // return without advancing
    }

    bool Parser::IsAtEnd() const noexcept {
        return Peek().kind == TokenKind::EndOfFile;
    }

    const Token &Parser::Previous() const noexcept {
        assert(pos > 0);
        return tokens[pos - 1];
    }

    SourceLocation Parser::CurrentLocation() const noexcept {
        return Peek().location;
    }

    // =========================================================================
    // Diagnostics
    // =========================================================================

    void Parser::EmitError(const SourceLocation loc, std::string message) {
        diagnostics.push_back(ParserDiagnostic{
            ParserDiagnostic::Severity::Error,
            loc,
            std::move(message)
        });
    }

    void Parser::EmitWarning(const SourceLocation loc, std::string message) {
        diagnostics.push_back(ParserDiagnostic{
            ParserDiagnostic::Severity::Warning,
            loc,
            std::move(message)
        });
    }

    void Parser::Synchronize() {
        // Skip until we reach a token that likely starts a new declaration or statement.
        while (!IsAtEnd()) {
            const TokenKind k = Peek().kind;

            // These tokens can safely begin a new item.
            if (k == TokenKind::Semicolon) {
                Advance();
                return;
            }
            if (k == TokenKind::RightBrace)
                return;

            if (k == TokenKind::FuncKeyword ||
                k == TokenKind::StructKeyword ||
                k == TokenKind::EnumKeyword ||
                k == TokenKind::UnionKeyword ||
                k == TokenKind::InterfaceKeyword ||
                k == TokenKind::ImplKeyword ||
                k == TokenKind::ModKeyword ||
                k == TokenKind::UseKeyword ||
                k == TokenKind::ConstKeyword ||
                k == TokenKind::TypeKeyword ||
                k == TokenKind::ExternKeyword ||
                k == TokenKind::PubKeyword ||
                k == TokenKind::LetKeyword ||
                k == TokenKind::VarKeyword ||
                k == TokenKind::IfKeyword ||
                k == TokenKind::WhileKeyword ||
                k == TokenKind::ForKeyword ||
                k == TokenKind::ReturnKeyword ||
                k == TokenKind::MatchKeyword)
                return;

            Advance();
        }
    }

    // =========================================================================
    // Attribute parsing
    // =========================================================================

    static std::string DecodeStringLiteralText(const std::string &text) {
        std::string out;
        const std::size_t quote = text.find('"');
        if (quote == std::string::npos) return out;

        for (std::size_t i = quote + 1; i + 1 < text.size(); ++i) {
            if (text[i] != '\\') {
                out += text[i];
                continue;
            }

            if (++i + 1 > text.size()) break;
            switch (text[i]) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case '0': out += '\0'; break;
                case '\\': out += '\\'; break;
                case '\'': out += '\''; break;
                case '"': out += '"'; break;
                default: out += text[i]; break;
            }
        }

        return out;
    }

    // Parses zero or more @[AttrName(...)] attributes that precede a declaration.
    // Returns the lib value from the first @[Import(lib: "...")] found.
    // Unknown attributes are parsed and silently ignored for forward compatibility.
    Parser::ParsedAttrs Parser::ParseAttrs() {
        ParsedAttrs attrs;
        while (Check(TokenKind::At)) {
            Advance(); // consume '@'
            Expect(TokenKind::LeftBracket, "expected '[' after '@'");

            std::string attrName;
            if (Check(TokenKind::Ident))
                attrName = Advance().text;

            if (Check(TokenKind::LeftParen)) {
                Advance(); // consume '('

                if (attrName == "Call" && Check(TokenKind::Dot)) {
                    // @[Call(.Win64)] — positional enum variant
                    Advance(); // consume '.'
                    std::string variant;
                    if (Check(TokenKind::Ident))
                        variant = Advance().text;
                    if (variant == "Win64")
                        attrs.callConv = CallingConvention::Win64;
                    else
                        EmitWarning(CurrentLocation(),
                            std::format("unknown calling convention '.{}'", variant));
                } else {
                    // Parse key: value pairs until ')'
                    while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
                        if (!Check(TokenKind::Ident)) { Advance(); continue; }
                        std::string key = Advance().text;
                        if (!Match(TokenKind::Colon)) continue;
                        if (attrName == "Import" && key == "lib" &&
                            Check(TokenKind::StringLiteral))
                            attrs.importLib = DecodeStringLiteralText(Advance().text);
                        else
                            Advance(); // skip unknown value
                        if (!Match(TokenKind::Comma)) break;
                    }
                }

                Expect(TokenKind::RightParen, "expected ')' to close attribute");
            }

            Expect(TokenKind::RightBracket, "expected ']' to close attribute");
        }
        return attrs;
    }

    // =========================================================================
    // Top-level declarations
    // =========================================================================

    DeclPtr Parser::ParseDecl() {
        const auto loc = CurrentLocation();

        ParsedAttrs attrs = ParseAttrs();

        bool isPublic = false;
        if (Match(TokenKind::PubKeyword))
            isPublic = true;

        // asm func
        if (Check(TokenKind::Ident) && Peek().text == "asm" &&
            Peek(1).Is(TokenKind::FuncKeyword)) {
            Advance(); // consume 'asm'
            return ParseFuncDecl(isPublic, true, attrs.callConv);
        }

        if (Check(TokenKind::FuncKeyword))
            return ParseFuncDecl(isPublic, false, attrs.callConv);
        if (Check(TokenKind::StructKeyword))
            return ParseStructDecl(isPublic);
        if (Check(TokenKind::EnumKeyword))
            return ParseEnumDecl(isPublic);
        if (Check(TokenKind::UnionKeyword))
            return ParseUnionDecl(isPublic);
        if (Check(TokenKind::InterfaceKeyword))
            return ParseInterfaceDecl(isPublic);
        if (Check(TokenKind::ImplKeyword))
            return ParseImplDecl();
        if (Check(TokenKind::ModKeyword))
            return ParseModuleDecl(isPublic);
        if (Check(TokenKind::UseKeyword))
            return ParseUseDecl();
        if (Check(TokenKind::ConstKeyword))
            return ParseConstDecl(isPublic);
        if (Check(TokenKind::TypeKeyword))
            return ParseTypeAliasDecl(isPublic);
        if (Check(TokenKind::ExternKeyword))
            return ParseExternDecl(isPublic, std::move(attrs));

        EmitError(loc,
                  std::format("unexpected token '{}', expected a declaration", Peek().text));
        return nullptr;
    }

    // =========================================================================
    // Shared declaration helpers
    // =========================================================================

    std::vector<std::string> Parser::ParseTypeParams() {
        // <T, U, ...>
        std::vector<std::string> params;
        Expect(TokenKind::Less, "expected '<'");
        while (!Check(TokenKind::Greater) && !IsAtEnd()) {
            auto &t = Expect(TokenKind::Ident, "expected type parameter name");
            params.push_back(t.text);
            if (!Match(TokenKind::Comma)) break;
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
            if (!Match(TokenKind::Comma)) break;
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
            static_cast<NamedTypeExpr *>(p.type.get())->name = "...";
            return p;
        }

        p.name = Expect(TokenKind::Ident, "expected parameter name").text;
        Expect(TokenKind::Colon, "expected ':'");
        p.type = ParseType();
        return p;
    }

    std::vector<Param> Parser::ParseParamList(bool allowVariadic) {
        std::vector<Param> params;
        while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
            params.push_back(ParseParam(allowVariadic));
            if (!Match(TokenKind::Comma)) break;
        }
        return params;
    }

    // =========================================================================
    // func
    // =========================================================================

    std::unique_ptr<FuncDecl> Parser::ParseFuncDecl(bool isPublic, bool isAsm,
                                                     CallingConvention callConv) {
        const auto loc = CurrentLocation();
        Expect(TokenKind::FuncKeyword, "expected 'func'");

        auto decl = std::make_unique<FuncDecl>();
        decl->location = loc;
        decl->isPublic = isPublic;
        decl->isAsm = isAsm;
        decl->callConv = callConv;
        decl->name = Expect(TokenKind::Ident, "expected function name").text;

        if (Check(TokenKind::Less))
            decl->typeParams = ParseTypeParams();

        Expect(TokenKind::LeftParen, "expected '('");
        decl->params = ParseParamList(false);
        Expect(TokenKind::RightParen, "expected ')'");

        if (Match(TokenKind::Arrow))
            decl->returnType = ParseType();

        if (Check(TokenKind::LeftBrace))
            decl->body = ParseBlock();
        else
            Expect(TokenKind::Semicolon, "expected '{' or ';'");

        return decl;
    }

    // =========================================================================
    // struct
    // =========================================================================

    std::unique_ptr<StructDecl> Parser::ParseStructDecl(bool isPublic) {
        const auto loc = CurrentLocation();
        Expect(TokenKind::StructKeyword, "expected 'struct'");

        auto decl = std::make_unique<StructDecl>();
        decl->location = loc;
        decl->isPublic = isPublic;
        decl->name = Expect(TokenKind::Ident, "expected struct name").text;

        if (Check(TokenKind::Less))
            decl->typeParams = ParseTypeParams();

        Expect(TokenKind::LeftBrace, "expected '{'");
        while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
            StructDecl::Field field;
            field.location = CurrentLocation();

            if (Match(TokenKind::PubKeyword))
                field.isPublic = true;

            field.name = Expect(TokenKind::Ident, "expected field name").text;
            Expect(TokenKind::Colon, "expected ':'");
            field.type = ParseType();
            Expect(TokenKind::Semicolon, "expected ';' after field");
            decl->fields.push_back(std::move(field));
        }
        Expect(TokenKind::RightBrace, "expected '}'");
        return decl;
    }

    // =========================================================================
    // enum
    // =========================================================================

    std::unique_ptr<EnumDecl> Parser::ParseEnumDecl(bool isPublic) {
        const auto loc = CurrentLocation();
        Expect(TokenKind::EnumKeyword, "expected 'enum'");

        auto decl = std::make_unique<EnumDecl>();
        decl->location = loc;
        decl->isPublic = isPublic;
        decl->name = Expect(TokenKind::Ident, "expected enum name").text;

        Expect(TokenKind::LeftBrace, "expected '{'");
        while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
            EnumDecl::Variant variant;
            variant.location = CurrentLocation();
            variant.name = Expect(TokenKind::Ident, "expected variant name").text;

            if (Match(TokenKind::LeftParen)) {
                while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
                    variant.fields.push_back(ParseType());
                    if (!Match(TokenKind::Comma)) break;
                }
                Expect(TokenKind::RightParen, "expected ')'");
            }

            decl->variants.push_back(std::move(variant));
            if (!Match(TokenKind::Comma)) break;
        }
        // Allow trailing comma
        Expect(TokenKind::RightBrace, "expected '}'");
        return decl;
    }

    // =========================================================================
    // union
    // =========================================================================

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
            if (!Match(TokenKind::Comma)) break;
        }
        Expect(TokenKind::RightBrace, "expected '}'");
        return decl;
    }

    // =========================================================================
    // interface
    // =========================================================================

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
                Synchronize();
                continue;
            }
            auto method = ParseFuncDecl(false, false);
            if (method) decl->methods.push_back(std::move(method));
        }
        Expect(TokenKind::RightBrace, "expected '}'");
        return decl;
    }

    // =========================================================================
    // impl
    // =========================================================================

    std::unique_ptr<ImplDecl> Parser::ParseImplDecl() {
        const auto loc = CurrentLocation();
        Expect(TokenKind::ImplKeyword, "expected 'impl'");

        auto decl = std::make_unique<ImplDecl>();
        decl->location = loc;

        // impl TypeName  or  impl InterfaceName for TypeName
        const std::string firstName = Expect(TokenKind::Ident, "expected type name").text;
        if (Match(TokenKind::ForKeyword)) {
            decl->interfaceName = firstName;
            decl->typeName = Expect(TokenKind::Ident, "expected type name after 'for'").text;
        } else {
            decl->typeName = firstName;
        }

        Expect(TokenKind::LeftBrace, "expected '{'");
        while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
            bool pub = Match(TokenKind::PubKeyword);
            if (!Check(TokenKind::FuncKeyword)) {
                EmitError(CurrentLocation(), "expected 'func' in impl body");
                Synchronize();
                continue;
            }
            auto method = ParseFuncDecl(pub, false);
            if (method) decl->methods.push_back(std::move(method));
        }
        Expect(TokenKind::RightBrace, "expected '}'");
        return decl;
    }

    // =========================================================================
    // mod
    // =========================================================================

    std::unique_ptr<ModuleDecl> Parser::ParseModuleDecl(bool isPublic) {
        const auto loc = CurrentLocation();
        Expect(TokenKind::ModKeyword, "expected 'mod'");

        auto decl = std::make_unique<ModuleDecl>();
        decl->location = loc;
        decl->isPublic = isPublic;
        decl->name = Expect(TokenKind::Ident, "expected module name").text;

        Expect(TokenKind::LeftBrace, "expected '{'");
        while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
            auto item = ParseDecl();
            if (item)
                decl->items.push_back(std::move(item));
            else
                Synchronize();
        }
        Expect(TokenKind::RightBrace, "expected '}'");
        return decl;
    }

    // =========================================================================
    // use
    // =========================================================================

    std::unique_ptr<UseDecl> Parser::ParseUseDecl() {
        const auto loc = CurrentLocation();
        Expect(TokenKind::UseKeyword, "expected 'use'");

        auto decl = std::make_unique<UseDecl>();
        decl->location = loc;

        // Parse path segments separated by '.' or '::'
        decl->path.push_back(Expect(TokenKind::Ident, "expected module path").text);

        while (!IsAtEnd()) {
            if (Match(TokenKind::Dot)) {
                if (Match(TokenKind::Star)) {
                    // use Std.Io.*;
                    decl->kind = UseDecl::Kind::Glob;
                    break;
                }
                decl->path.push_back(Expect(TokenKind::Ident, "expected identifier").text);
            } else if (Match(TokenKind::ColonColon)) {
                if (Check(TokenKind::LeftBrace)) {
                    // use Http::{ Request, Response };
                    Advance(); // consume '{'
                    decl->kind = UseDecl::Kind::Multi;
                    while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
                        decl->names.push_back(
                            Expect(TokenKind::Ident, "expected name").text);
                        if (!Match(TokenKind::Comma)) break;
                    }
                    Expect(TokenKind::RightBrace, "expected '}'");
                    break;
                }
                if (Match(TokenKind::Star)) {
                    // use Std::Io::*
                    decl->kind = UseDecl::Kind::Glob;
                    break;
                }
                decl->path.push_back(Expect(TokenKind::Ident, "expected identifier").text);
            } else {
                break;
            }
        }

        Expect(TokenKind::Semicolon, "expected ';'");
        return decl;
    }

    // =========================================================================
    // const
    // =========================================================================

    std::unique_ptr<ConstDecl> Parser::ParseConstDecl(bool isPublic) {
        const auto loc = CurrentLocation();
        Expect(TokenKind::ConstKeyword, "expected 'const'");

        auto decl = std::make_unique<ConstDecl>();
        decl->location = loc;
        decl->isPublic = isPublic;
        decl->name = Expect(TokenKind::Ident, "expected constant name").text;

        Expect(TokenKind::Colon, "expected ':'");
        decl->type = ParseType();

        Expect(TokenKind::Assign, "expected '='");
        decl->value = ParseExpr();

        Expect(TokenKind::Semicolon, "expected ';'");
        return decl;
    }

    // =========================================================================
    // type alias
    // =========================================================================

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

    // =========================================================================
    // extern
    // =========================================================================

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
                    while (!IsAtEnd() && !Check(TokenKind::Semicolon) && !Check(TokenKind::RightBrace))
                        Advance();
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
                        if (!Match(TokenKind::Comma)) break;
                    }
                    Expect(TokenKind::RightParen, "expected ')'");
                    if (Match(TokenKind::Arrow))
                        fd->returnType = ParseType();
                    Expect(TokenKind::Semicolon, "expected ';'");
                    block->items.push_back(std::move(fd));
                } else if (Check(TokenKind::Ident)) {
                    auto vd = std::make_unique<ExternVarDecl>();
                    vd->location = CurrentLocation();
                    vd->isPublic = isPublic;
                    vd->name = Advance().text;
                    Expect(TokenKind::Colon, "expected ':'");
                    vd->type = ParseType();
                    Expect(TokenKind::Semicolon, "expected ';'");
                    block->items.push_back(std::move(vd));
                } else {
                    EmitError(CurrentLocation(), "expected 'func' or variable declaration in extern block");
                    Synchronize();
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
                if (!Match(TokenKind::Comma)) break;
            }
            Expect(TokenKind::RightParen, "expected ')'");

            if (Match(TokenKind::Arrow))
                decl->returnType = ParseType();

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

    // =========================================================================
    // Type expressions
    // =========================================================================

    TypeExprPtr Parser::ParseType() {
        const auto loc = CurrentLocation();

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
                if (!Match(TokenKind::Comma)) break;
            }
            Expect(TokenKind::RightParen, "expected ')'");
            return t;
        }

        TypeExprPtr base = ParseBaseType();
        if (!base) return nullptr;

        // Postfix slice suffix: T[]  or  T[N]
        while (Check(TokenKind::LeftBracket)) {
            Advance();
            auto a = std::make_unique<SliceTypeExpr>();
            a->location = loc;
            a->element = std::move(base);
            if (!Check(TokenKind::RightBracket))
                a->size = ParseExpr();
            Expect(TokenKind::RightBracket, "expected ']'");
            base = std::move(a);
        }

        return base;
    }

    TypeExprPtr Parser::ParseBaseType() {
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
                        Expect(TokenKind::Ident, "expected type name").text);
                }
                return p;
            }

            auto n = std::make_unique<NamedTypeExpr>();
            n->location = loc;
            n->name = first;
            if (Check(TokenKind::Less))
                n->typeArgs = ParseTypeArgs();
            return n;
        }

        EmitError(loc, std::format("expected a type, got '{}'", Peek().text));
        return nullptr;
    }

    // =========================================================================
    // Block and statements
    // =========================================================================

    std::unique_ptr<Block> Parser::ParseBlock() {
        const auto loc = CurrentLocation();
        Expect(TokenKind::LeftBrace, "expected '{'");

        auto block = std::make_unique<Block>();
        block->location = loc;

        while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
            auto stmt = ParseStmt();
            if (stmt)
                block->stmts.push_back(std::move(stmt));
            else
                Synchronize();
        }

        Expect(TokenKind::RightBrace, "expected '}'");
        return block;
    }

    StmtPtr Parser::ParseStmt() {
        const auto loc = CurrentLocation();

        if (Check(TokenKind::LetKeyword) || Check(TokenKind::VarKeyword))
            return ParseLetStmt();
        if (Check(TokenKind::IfKeyword))
            return ParseIfStmt();
        if (Check(TokenKind::WhileKeyword))
            return ParseWhileStmt();
        if (Check(TokenKind::ForKeyword))
            return ParseForStmt();
        if (Check(TokenKind::MatchKeyword))
            return ParseMatchStmt();
        if (Check(TokenKind::ReturnKeyword))
            return ParseReturnStmt();

        if (Check(TokenKind::BreakKeyword)) {
            Advance();
            Expect(TokenKind::Semicolon, "expected ';'");
            auto s = std::make_unique<BreakStmt>();
            s->location = loc;
            return s;
        }

        if (Check(TokenKind::ContinueKeyword)) {
            Advance();
            Expect(TokenKind::Semicolon, "expected ';'");
            auto s = std::make_unique<ContinueStmt>();
            s->location = loc;
            return s;
        }

        // Allow nested declarations inside blocks
        if (CheckAny({
            TokenKind::PubKeyword,
            TokenKind::FuncKeyword,
            TokenKind::StructKeyword,
            TokenKind::EnumKeyword,
            TokenKind::UnionKeyword,
            TokenKind::InterfaceKeyword,
            TokenKind::ImplKeyword,
            TokenKind::ModKeyword,
            TokenKind::UseKeyword,
            TokenKind::ConstKeyword,
            TokenKind::TypeKeyword,
            TokenKind::ExternKeyword
        })) {
            auto ds = std::make_unique<DeclStmt>();
            ds->location = loc;
            ds->decl = ParseDecl();
            return ds;
        }

        // Expression statement
        auto expr = ParseExpr();
        if (!expr) return nullptr;
        Expect(TokenKind::Semicolon, "expected ';'");
        auto s = std::make_unique<ExprStmt>();
        s->location = loc;
        s->expr = std::move(expr);
        return s;
    }

    std::unique_ptr<LetStmt> Parser::ParseLetStmt() {
        const auto loc = CurrentLocation();

        auto s = std::make_unique<LetStmt>();
        s->location = loc;

        if (Match(TokenKind::VarKeyword)) {
            s->isMut = true; // var is always mutable
        } else {
            Expect(TokenKind::LetKeyword, "expected 'let' or 'var'");
            s->isMut = Match(TokenKind::MutKeyword);
        }
        s->name = Expect(TokenKind::Ident, "expected variable name").text;

        if (Match(TokenKind::Colon))
            s->type = ParseType();

        Expect(TokenKind::Assign, "expected '='");
        s->init = ParseExpr();
        Expect(TokenKind::Semicolon, "expected ';'");
        return s;
    }

    std::unique_ptr<IfStmt> Parser::ParseIfStmt() {
        const auto loc = CurrentLocation();
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

        if (Match(TokenKind::ElseKeyword))
            s->elseBlock = ParseBlock();

        return s;
    }

    std::unique_ptr<WhileStmt> Parser::ParseWhileStmt() {
        const auto loc = CurrentLocation();
        Expect(TokenKind::WhileKeyword, "expected 'while'");

        auto s = std::make_unique<WhileStmt>();
        s->location = loc;
        structInitAllowed = false;
        s->condition = ParseExpr();
        structInitAllowed = true;
        s->body = ParseBlock();
        return s;
    }

    std::unique_ptr<ForStmt> Parser::ParseForStmt() {
        const auto loc = CurrentLocation();
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
        const auto loc = CurrentLocation();
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
            } else {
                arm.body = ParseExpr();
            }

            s->arms.push_back(std::move(arm));
            if (!Match(TokenKind::Comma)) break;
        }
        Expect(TokenKind::RightBrace, "expected '}'");
        return s;
    }

    std::unique_ptr<ReturnStmt> Parser::ParseReturnStmt() {
        const auto loc = CurrentLocation();
        Expect(TokenKind::ReturnKeyword, "expected 'return'");

        auto s = std::make_unique<ReturnStmt>();
        s->location = loc;

        if (!Check(TokenKind::Semicolon))
            s->value = ParseExpr();

        Expect(TokenKind::Semicolon, "expected ';'");
        return s;
    }

    // =========================================================================
    // Expressions
    // =========================================================================

    ExprPtr Parser::ParseExpr() {
        return ParseAssign();
    }

    // right-associative: a = b = c  =>  a = (b = c)
    ExprPtr Parser::ParseAssign() {
        auto left = ParseRange();
        if (!left) return nullptr;

        static constexpr TokenKind kAssignOps[] = {
            TokenKind::Assign,
            TokenKind::PlusAssign, TokenKind::MinusAssign,
            TokenKind::StarAssign, TokenKind::SlashAssign,
            TokenKind::PercentAssign,
            TokenKind::AmpAssign, TokenKind::PipeAssign,
            TokenKind::CaretAssign, TokenKind::LessLessAssign,
            TokenKind::GreaterGreaterAssign,
        };

        for (auto op: kAssignOps) {
            if (Check(op)) {
                const auto loc = CurrentLocation();
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
        if (!left) return nullptr;

        if (Check(TokenKind::DotDot) || Check(TokenKind::DotDotDot)) {
            const bool incl = Peek().kind == TokenKind::DotDotDot;
            const auto loc = CurrentLocation();
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
        if (!cond) return nullptr;

        if (Match(TokenKind::Question)) {
            const auto loc = Previous().location;
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
            const auto loc = CurrentLocation();
            const auto op = Advance().kind;
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
            const auto loc = CurrentLocation();
            const auto op = Advance().kind;
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
            const auto loc = CurrentLocation();
            const auto op = Advance().kind;
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
            const auto loc = CurrentLocation();
            const auto op = Advance().kind;
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
            const auto loc = CurrentLocation();
            const auto op = Advance().kind;
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
            const auto loc = CurrentLocation();
            const auto op = Advance().kind;
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
        while (CheckAny({
            TokenKind::Less, TokenKind::LessEqual,
            TokenKind::Greater, TokenKind::GreaterEqual
        })) {
            const auto loc = CurrentLocation();
            const auto op = Advance().kind;
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

    ExprPtr Parser::ParseShift() {
        auto left = ParseAdd();
        while (CheckAny({TokenKind::LessLess, TokenKind::GreaterGreater})) {
            const auto loc = CurrentLocation();
            const auto op = Advance().kind;
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
            const auto loc = CurrentLocation();
            const auto op = Advance().kind;
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
            const auto loc = CurrentLocation();
            const auto op = Advance().kind;
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
        if (Check(TokenKind::StarStar)) {
            const auto loc = CurrentLocation();
            Advance();
            auto right = ParseExp(); // right-associative
            auto e = std::make_unique<BinaryExpr>();
            e->location = loc;
            e->op = TokenKind::StarStar;
            e->left = std::move(left);
            e->right = std::move(right);
            return e;
        }
        return left;
    }

    ExprPtr Parser::ParseUnary() {
        if (CheckAny({
            TokenKind::Bang, TokenKind::Minus,
            TokenKind::Tilde, TokenKind::Star, TokenKind::Amp
        })) {
            const auto loc = CurrentLocation();
            const auto op = Advance().kind;
            auto operand = ParseUnary();
            auto e = std::make_unique<UnaryExpr>();
            e->location = loc;
            e->op = op;
            e->operand = std::move(operand);
            return e;
        }
        return ParsePostfix();
    }

    ExprPtr Parser::ParsePostfix() {
        auto left = ParsePrimary();
        if (!left) return nullptr;

        while (true) {
            const auto loc = CurrentLocation();

            // Method/field call: expr.field or expr.method(args)
            if (Match(TokenKind::Dot)) {
                const std::string name = Expect(TokenKind::Ident, "expected field name").text;
                if (Check(TokenKind::LeftParen)) {
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
                } else {
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
                const std::string seg = Expect(TokenKind::Ident, "expected identifier").text;
                // Build or extend a PathExpr
                if (auto *path = dynamic_cast<PathExpr *>(left.get())) {
                    path->segments.push_back(seg);
                } else {
                    // Wrap existing expression in a path — treat the left side as a segment
                    // This handles IDENT::IDENT::... chains
                    auto p = std::make_unique<PathExpr>();
                    p->location = loc;
                    if (auto *ident = dynamic_cast<IdentExpr *>(left.get()))
                        p->segments.push_back(ident->name);
                    p->segments.push_back(seg);
                    left = std::move(p);
                }
                continue;
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

            // as cast
            if (Match(TokenKind::AsKeyword)) {
                auto type = ParseType();
                auto e = std::make_unique<CastExpr>();
                e->location = loc;
                e->operand = std::move(left);
                e->type = std::move(type);
                left = std::move(e);
                continue;
            }

            // is type check
            if (Match(TokenKind::IsKeyword)) {
                auto type = ParseType();
                auto e = std::make_unique<IsExpr>();
                e->location = loc;
                e->operand = std::move(left);
                e->type = std::move(type);
                left = std::move(e);
                continue;
            }

            break;
        }
        return left;
    }

    ExprPtr Parser::ParsePrimary() {
        const auto loc = CurrentLocation();

        // Literals
        if (Check(TokenKind::IntLiteral) ||
            Check(TokenKind::FloatLiteral) ||
            Check(TokenKind::StringLiteral) ||
            Check(TokenKind::CharLiteral) ||
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

        // Slice literal: [a, b, c]
        if (Match(TokenKind::LeftBracket)) {
            auto e = std::make_unique<SliceExpr>();
            e->location = loc;
            while (!Check(TokenKind::RightBracket) && !IsAtEnd()) {
                e->elements.push_back(ParseExpr());
                if (!Match(TokenKind::Comma)) break;
            }
            Expect(TokenKind::RightBracket, "expected ']'");
            return e;
        }

        // Grouped expression: (expr)
        if (Match(TokenKind::LeftParen)) {
            auto inner = ParseExpr();
            Expect(TokenKind::RightParen, "expected ')'");
            return inner;
        }

        // Identifier, possible struct init, or path expression
        if (Check(TokenKind::Ident)) {
            const std::string name = Advance().text;
            std::vector<TypeExprPtr> typeArgs;

            if (Check(TokenKind::Less)) {
                const std::size_t savedPos = pos;
                const std::size_t savedDiagCount = diagnostics.size();
                typeArgs = ParseTypeArgs();
                if (!Check(TokenKind::LeftBrace)) {
                    pos = savedPos;
                    diagnostics.resize(savedDiagCount);
                    typeArgs.clear();
                }
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
                    if (!Match(TokenKind::Comma)) break;
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
            args.push_back(ParseExpr());
            if (!Match(TokenKind::Comma)) break;
        }
        Expect(TokenKind::RightParen, "expected ')'");
        return args;
    }

    // =========================================================================
    // Patterns
    // =========================================================================

    PatternPtr Parser::ParsePattern() {
        auto inner = ParsePrimaryPattern();
        if (!inner) return nullptr;

        // Guard: pattern if condition
        if (Match(TokenKind::IfKeyword)) {
            const auto loc = Previous().location;
            auto guard = ParseExpr();
            auto p = std::make_unique<GuardedPattern>();
            p->location = loc;
            p->inner = std::move(inner);
            p->guard = std::move(guard);
            return p;
        }

        // Range pattern: lo..hi or lo...hi
        if (Check(TokenKind::DotDot) || Check(TokenKind::DotDotDot)) {
            const bool incl = Peek().kind == TokenKind::DotDotDot;
            const auto loc = CurrentLocation();
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
        const auto loc = CurrentLocation();

        // Wildcard: _
        if (Check(TokenKind::Ident) && Peek().text == "_") {
            Advance();
            auto p = std::make_unique<WildcardPattern>();
            p->location = loc;
            return p;
        }

        // Literals
        if (Check(TokenKind::IntLiteral) ||
            Check(TokenKind::FloatLiteral) ||
            Check(TokenKind::StringLiteral) ||
            Check(TokenKind::CharLiteral) ||
            Check(TokenKind::BoolLiteral) ||
            Check(TokenKind::NullKeyword)) {
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
                if (!Match(TokenKind::Comma)) break;
            }
            Expect(TokenKind::RightParen, "expected ')'");
            return p;
        }

        // Identifier-started patterns: ident, EnumName.Variant(args), TypeName { fields }
        if (Check(TokenKind::Ident)) {
            const std::string name = Advance().text;

            // Enum pattern: Event.Click(x, y)
            if (Check(TokenKind::Dot) && Peek(1).Is(TokenKind::Ident)) {
                std::vector<std::string> path = {name};
                while (Match(TokenKind::Dot)) {
                    path.push_back(Expect(TokenKind::Ident, "expected variant name").text);
                }
                auto p = std::make_unique<EnumPattern>();
                p->location = loc;
                p->path = std::move(path);
                if (Match(TokenKind::LeftParen)) {
                    while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
                        p->args.push_back(ParsePattern());
                        if (!Match(TokenKind::Comma)) break;
                    }
                    Expect(TokenKind::RightParen, "expected ')'");
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
                    if (!Match(TokenKind::Comma)) break;
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

    // =========================================================================
    // AstPrinter  –  human-readable tree dump
    // =========================================================================

    namespace {
        class AstPrinter {
        public:
            explicit AstPrinter(std::ostream &out) : out(out) {
            }

            void Print(const Module &mod) {
                out << "Module \"" << mod.name << "\"\n";
                ++indent;
                for (const auto &item: mod.items)
                    if (item) PrintDecl(*item);
                --indent;
            }

        private:
            std::ostream &out;
            int indent = 0;

            // ── Helpers ───────────────────────────────────────────────────────

            void Pad() const {
                for (int i = 0; i < indent; ++i) out << "  ";
            }

            std::string TypeStr(const TypeExpr *t) const {
                if (!t) return "<null>";
                if (const auto *n = dynamic_cast<const NamedTypeExpr *>(t)) {
                    std::string s = n->name;
                    if (!n->typeArgs.empty()) {
                        s += "<";
                        for (std::size_t i = 0; i < n->typeArgs.size(); ++i) {
                            if (i) s += ", ";
                            s += TypeStr(n->typeArgs[i].get());
                        }
                        s += ">";
                    }
                    return s;
                }
                if (const auto *p = dynamic_cast<const PathTypeExpr *>(t)) {
                    std::string s;
                    for (std::size_t i = 0; i < p->segments.size(); ++i) {
                        if (i) s += "::";
                        s += p->segments[i];
                    }
                    return s;
                }
                if (const auto *a = dynamic_cast<const SliceTypeExpr *>(t)) {
                    std::string s = TypeStr(a->element.get()) + "[";
                    if (a->size) s += "N"; // size is an Expr, not easily stringified
                    return s + "]";
                }
                if (const auto *ptr = dynamic_cast<const PointerTypeExpr *>(t))
                    return "*" + TypeStr(ptr->pointee.get());
                if (const auto *tup = dynamic_cast<const TupleTypeExpr *>(t)) {
                    std::string s = "(";
                    for (std::size_t i = 0; i < tup->elements.size(); ++i) {
                        if (i) s += ", ";
                        s += TypeStr(tup->elements[i].get());
                    }
                    return s + ")";
                }
                if (dynamic_cast<const SelfTypeExpr *>(t)) return "self";
                return "<type>";
            }

            static std::string_view OpStr(const TokenKind op) noexcept {
                switch (op) {
                    case TokenKind::Plus: return "+";
                    case TokenKind::Minus: return "-";
                    case TokenKind::Star: return "*";
                    case TokenKind::Slash: return "/";
                    case TokenKind::Percent: return "%";
                    case TokenKind::StarStar: return "**";
                    case TokenKind::Amp: return "&";
                    case TokenKind::Pipe: return "|";
                    case TokenKind::Caret: return "^";
                    case TokenKind::Tilde: return "~";
                    case TokenKind::LessLess: return "<<";
                    case TokenKind::GreaterGreater: return ">>";
                    case TokenKind::AmpAmp: return "&&";
                    case TokenKind::PipePipe: return "||";
                    case TokenKind::Bang: return "!";
                    case TokenKind::Equal: return "==";
                    case TokenKind::BangEqual: return "!=";
                    case TokenKind::Less: return "<";
                    case TokenKind::LessEqual: return "<=";
                    case TokenKind::Greater: return ">";
                    case TokenKind::GreaterEqual: return ">=";
                    case TokenKind::Assign: return "=";
                    case TokenKind::PlusAssign: return "+=";
                    case TokenKind::MinusAssign: return "-=";
                    case TokenKind::StarAssign: return "*=";
                    case TokenKind::SlashAssign: return "/=";
                    case TokenKind::PercentAssign: return "%=";
                    case TokenKind::AmpAssign: return "&=";
                    case TokenKind::PipeAssign: return "|=";
                    case TokenKind::CaretAssign: return "^=";
                    case TokenKind::LessLessAssign: return "<<=";
                    case TokenKind::GreaterGreaterAssign: return ">>=";
                    default: return "?";
                }
            }

            // ── Declarations ──────────────────────────────────────────────────

            void PrintDecl(const Decl &decl) {
                if (const auto *p = dynamic_cast<const FuncDecl *>(&decl))
                    PrintFuncDecl(*p);
                else if (const auto *p = dynamic_cast<const StructDecl *>(&decl))
                    PrintStructDecl(*p);
                else if (const auto *p = dynamic_cast<const EnumDecl *>(&decl))
                    PrintEnumDecl(*p);
                else if (const auto *p = dynamic_cast<const UnionDecl *>(&decl))
                    PrintUnionDecl(*p);
                else if (const auto *p = dynamic_cast<const InterfaceDecl *>(&decl))
                    PrintInterfaceDecl(*p);
                else if (const auto *p = dynamic_cast<const ImplDecl *>(&decl))
                    PrintImplDecl(*p);
                else if (const auto *p = dynamic_cast<const ModuleDecl *>(&decl))
                    PrintModuleDecl(*p);
                else if (const auto *p = dynamic_cast<const UseDecl *>(&decl))
                    PrintUseDecl(*p);
                else if (const auto *p = dynamic_cast<const ConstDecl *>(&decl))
                    PrintConstDecl(*p);
                else if (const auto *p = dynamic_cast<const TypeAliasDecl *>(&decl))
                    PrintTypeAliasDecl(*p);
                else if (const auto *p = dynamic_cast<const ExternFuncDecl *>(&decl))
                    PrintExternFuncDecl(*p);
                else if (const auto *p = dynamic_cast<const ExternVarDecl *>(&decl))
                    PrintExternVarDecl(*p);
            }

            void PrintFuncDecl(const FuncDecl &f) {
                Pad();
                if (f.isPublic) out << "pub ";
                if (f.isAsm) out << "asm ";
                out << "FuncDecl '" << f.name << "'";

                // Generic params
                if (!f.typeParams.empty()) {
                    out << '<';
                    for (std::size_t i = 0; i < f.typeParams.size(); ++i) {
                        if (i) out << ", ";
                        out << f.typeParams[i];
                    }
                    out << '>';
                }

                // Params
                out << " (";
                for (std::size_t i = 0; i < f.params.size(); ++i) {
                    if (i) out << ", ";
                    const auto &p = f.params[i];
                    if (p.isVariadic) {
                        out << "...";
                        continue;
                    }
                    out << p.name << ": " << TypeStr(p.type.get());
                }
                out << ')';

                // Return type
                if (f.returnType)
                    out << " -> " << TypeStr(f.returnType->get());

                out << (f.body ? "" : " [signature]") << '\n';

                if (f.body) {
                    ++indent;
                    PrintBlock(*f.body);
                    --indent;
                }
            }

            void PrintStructDecl(const StructDecl &s) {
                Pad();
                if (s.isPublic) out << "pub ";
                out << "StructDecl '" << s.name << "'";
                if (!s.typeParams.empty()) {
                    out << '<';
                    for (std::size_t i = 0; i < s.typeParams.size(); ++i) {
                        if (i) out << ", ";
                        out << s.typeParams[i];
                    }
                    out << '>';
                }
                out << '\n';
                ++indent;
                for (const auto &f: s.fields) {
                    Pad();
                    if (f.isPublic) out << "pub ";
                    out << "Field '" << f.name << "' : " << TypeStr(f.type.get()) << '\n';
                }
                --indent;
            }

            void PrintEnumDecl(const EnumDecl &e) {
                Pad();
                if (e.isPublic) out << "pub ";
                out << "EnumDecl '" << e.name << "'\n";
                ++indent;
                for (const auto &v: e.variants) {
                    Pad();
                    out << "Variant '" << v.name << "'";
                    if (!v.fields.empty()) {
                        out << " (";
                        for (std::size_t i = 0; i < v.fields.size(); ++i) {
                            if (i) out << ", ";
                            out << TypeStr(v.fields[i].get());
                        }
                        out << ')';
                    }
                    out << '\n';
                }
                --indent;
            }

            void PrintUnionDecl(const UnionDecl &u) {
                Pad();
                if (u.isPublic) out << "pub ";
                out << "UnionDecl '" << u.name << "'\n";
                ++indent;
                for (const auto &f: u.fields) {
                    Pad();
                    out << "Field '" << f.name << "' : " << TypeStr(f.type.get()) << '\n';
                }
                --indent;
            }

            void PrintInterfaceDecl(const InterfaceDecl &iface) {
                Pad();
                if (iface.isPublic) out << "pub ";
                out << "InterfaceDecl '" << iface.name << "'\n";
                ++indent;
                for (const auto &m: iface.methods)
                    if (m) PrintFuncDecl(*m);
                --indent;
            }

            void PrintImplDecl(const ImplDecl &impl) {
                Pad();
                out << "ImplDecl ";
                if (impl.interfaceName)
                    out << *impl.interfaceName << " for ";
                out << impl.typeName << '\n';
                ++indent;
                for (const auto &m: impl.methods)
                    if (m) PrintFuncDecl(*m);
                --indent;
            }

            void PrintModuleDecl(const ModuleDecl &mod) {
                Pad();
                if (mod.isPublic) out << "pub ";
                out << "ModuleDecl '" << mod.name << "'\n";
                ++indent;
                for (const auto &item: mod.items)
                    if (item) PrintDecl(*item);
                --indent;
            }

            void PrintUseDecl(const UseDecl &u) {
                Pad();
                out << "UseDecl '";
                for (std::size_t i = 0; i < u.path.size(); ++i) {
                    if (i) out << '.';
                    out << u.path[i];
                }
                switch (u.kind) {
                    case UseDecl::Kind::Glob:
                        out << ".*";
                        break;
                    case UseDecl::Kind::Multi: {
                        out << "::{";
                        for (std::size_t i = 0; i < u.names.size(); ++i) {
                            if (i) out << ", ";
                            out << u.names[i];
                        }
                        out << '}';
                        break;
                    }
                    default: break;
                }
                out << "'\n";
            }

            void PrintConstDecl(const ConstDecl &c) {
                Pad();
                if (c.isPublic) out << "pub ";
                out << "ConstDecl '" << c.name << "' : " << TypeStr(c.type.get()) << '\n';
                ++indent;
                if (c.value) PrintExpr(*c.value);
                --indent;
            }

            void PrintTypeAliasDecl(const TypeAliasDecl &t) {
                Pad();
                if (t.isPublic) out << "pub ";
                out << "TypeAliasDecl '" << t.name << "' = "
                        << TypeStr(t.type.get()) << '\n';
            }

            void PrintExternFuncDecl(const ExternFuncDecl &f) {
                if (!f.dll.empty()) { Pad(); out << "@[Import(lib: \"" << f.dll << "\")]\n"; }
                if (f.callConv == CallingConvention::Win64) { Pad(); out << "@[Call(.Win64)]\n"; }
                Pad();
                if (f.isPublic) out << "pub ";
                out << "ExternFuncDecl '" << f.name << "' (";
                for (std::size_t i = 0; i < f.params.size(); ++i) {
                    if (i) out << ", ";
                    out << f.params[i].name << ": " << TypeStr(f.params[i].type.get());
                }
                if (f.isVariadic) out << (f.params.empty() ? "..." : ", ...");
                out << ')';
                if (f.returnType) out << " -> " << TypeStr(f.returnType->get());
                out << '\n';
            }

            void PrintExternVarDecl(const ExternVarDecl &v) {
                Pad();
                if (v.isPublic) out << "pub ";
                out << "ExternVarDecl '" << v.name << "' : "
                        << TypeStr(v.type.get()) << '\n';
            }

            // ── Block ─────────────────────────────────────────────────────────

            void PrintBlock(const Block &block) {
                Pad();
                out << "Block [" << block.stmts.size() << " stmt"
                        << (block.stmts.size() == 1 ? "" : "s") << "]\n";
                ++indent;
                for (const auto &stmt: block.stmts)
                    if (stmt) PrintStmt(*stmt);
                --indent;
            }

            // ── Statements ────────────────────────────────────────────────────

            void PrintStmt(const Stmt &stmt) {
                if (const auto *p = dynamic_cast<const LetStmt *>(&stmt))
                    PrintLetStmt(*p);
                else if (const auto *p = dynamic_cast<const IfStmt *>(&stmt))
                    PrintIfStmt(*p);
                else if (const auto *p = dynamic_cast<const WhileStmt *>(&stmt))
                    PrintWhileStmt(*p);
                else if (const auto *p = dynamic_cast<const ForStmt *>(&stmt))
                    PrintForStmt(*p);
                else if (const auto *p = dynamic_cast<const MatchStmt *>(&stmt))
                    PrintMatchStmt(*p);
                else if (const auto *p = dynamic_cast<const ReturnStmt *>(&stmt))
                    PrintReturnStmt(*p);
                else if (const auto *p = dynamic_cast<const BreakStmt *>(&stmt)) {
                    (void) p;
                    Pad();
                    out << "BreakStmt\n";
                } else if (const auto *p = dynamic_cast<const ContinueStmt *>(&stmt)) {
                    (void) p;
                    Pad();
                    out << "ContinueStmt\n";
                } else if (const auto *p = dynamic_cast<const ExprStmt *>(&stmt)) {
                    Pad();
                    out << "ExprStmt\n";
                    ++indent;
                    if (p->expr) PrintExpr(*p->expr);
                    --indent;
                } else if (const auto *p = dynamic_cast<const DeclStmt *>(&stmt)) {
                    if (p->decl) PrintDecl(*p->decl);
                }
            }

            void PrintLetStmt(const LetStmt &s) {
                Pad();
                out << "LetStmt '" << s.name << "' ("
                        << (s.isMut ? "mut" : "immut") << ")";
                if (s.type) out << " : " << TypeStr(s.type->get());
                out << '\n';
                ++indent;
                if (s.init) PrintExpr(*s.init);
                --indent;
            }

            void PrintIfStmt(const IfStmt &s) {
                Pad();
                out << "IfStmt\n";
                ++indent;

                Pad();
                out << "Condition\n";
                ++indent;
                if (s.condition) PrintExpr(*s.condition);
                --indent;

                Pad();
                out << "Then\n";
                ++indent;
                if (s.thenBlock) PrintBlock(*s.thenBlock);
                --indent;

                for (const auto &elif: s.elseIfs) {
                    Pad();
                    out << "ElseIf\n";
                    ++indent;
                    Pad();
                    out << "Condition\n";
                    ++indent;
                    if (elif.condition) PrintExpr(*elif.condition);
                    --indent;
                    if (elif.block) PrintBlock(*elif.block);
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

            void PrintWhileStmt(const WhileStmt &s) {
                Pad();
                out << "WhileStmt\n";
                ++indent;
                Pad();
                out << "Condition\n";
                ++indent;
                if (s.condition) PrintExpr(*s.condition);
                --indent;
                if (s.body) PrintBlock(*s.body);
                --indent;
            }

            void PrintForStmt(const ForStmt &s) {
                Pad();
                out << "ForStmt '" << s.variable << "' in\n";
                ++indent;
                if (s.iterable) PrintExpr(*s.iterable);
                if (s.body) PrintBlock(*s.body);
                --indent;
            }

            void PrintMatchStmt(const MatchStmt &s) {
                Pad();
                out << "MatchStmt\n";
                ++indent;
                Pad();
                out << "Subject\n";
                ++indent;
                if (s.subject) PrintExpr(*s.subject);
                --indent;
                for (const auto &arm: s.arms) {
                    Pad();
                    out << "Arm\n";
                    ++indent;
                    if (arm.pattern) PrintPattern(*arm.pattern);
                    if (arm.body) PrintExpr(*arm.body);
                    --indent;
                }
                --indent;
            }

            void PrintReturnStmt(const ReturnStmt &s) {
                Pad();
                out << "ReturnStmt\n";
                if (s.value) {
                    ++indent;
                    PrintExpr(**s.value);
                    --indent;
                }
            }

            // ── Expressions ───────────────────────────────────────────────────

            void PrintExpr(const Expr &expr) {
                if (const auto *p = dynamic_cast<const LiteralExpr *>(&expr))
                    PrintLiteralExpr(*p);
                else if (const auto *p = dynamic_cast<const IdentExpr *>(&expr)) {
                    Pad();
                    out << "IdentExpr '" << p->name << "'\n";
                } else if (const auto *p = dynamic_cast<const SelfExpr *>(&expr)) {
                    (void) p;
                    Pad();
                    out << "SelfExpr\n";
                } else if (const auto *p = dynamic_cast<const PathExpr *>(&expr)) {
                    Pad();
                    out << "PathExpr '";
                    for (std::size_t i = 0; i < p->segments.size(); ++i) {
                        if (i) out << "::";
                        out << p->segments[i];
                    }
                    out << "'\n";
                } else if (const auto *p = dynamic_cast<const UnaryExpr *>(&expr)) {
                    Pad();
                    out << "UnaryExpr " << OpStr(p->op) << '\n';
                    ++indent;
                    if (p->operand) PrintExpr(*p->operand);
                    --indent;
                } else if (const auto *p = dynamic_cast<const BinaryExpr *>(&expr)) {
                    Pad();
                    out << "BinaryExpr " << OpStr(p->op) << '\n';
                    ++indent;
                    if (p->left) PrintExpr(*p->left);
                    if (p->right) PrintExpr(*p->right);
                    --indent;
                } else if (const auto *p = dynamic_cast<const AssignExpr *>(&expr)) {
                    Pad();
                    out << "AssignExpr " << OpStr(p->op) << '\n';
                    ++indent;
                    if (p->target) PrintExpr(*p->target);
                    if (p->value) PrintExpr(*p->value);
                    --indent;
                } else if (const auto *p = dynamic_cast<const TernaryExpr *>(&expr)) {
                    Pad();
                    out << "TernaryExpr\n";
                    ++indent;
                    Pad();
                    out << "Condition\n";
                    ++indent;
                    if (p->condition) PrintExpr(*p->condition);
                    --indent;
                    Pad();
                    out << "Then\n";
                    ++indent;
                    if (p->thenExpr) PrintExpr(*p->thenExpr);
                    --indent;
                    Pad();
                    out << "Else\n";
                    ++indent;
                    if (p->elseExpr) PrintExpr(*p->elseExpr);
                    --indent;
                    --indent;
                } else if (const auto *p = dynamic_cast<const RangeExpr *>(&expr)) {
                    Pad();
                    out << "RangeExpr " << (p->inclusive ? "..." : "..") << '\n';
                    ++indent;
                    if (p->lo) PrintExpr(*p->lo);
                    if (p->hi) PrintExpr(*p->hi);
                    --indent;
                } else if (const auto *p = dynamic_cast<const CallExpr *>(&expr)) {
                    Pad();
                    out << "CallExpr\n";
                    ++indent;
                    Pad();
                    out << "Callee\n";
                    ++indent;
                    if (p->callee) PrintExpr(*p->callee);
                    --indent;
                    if (!p->args.empty()) {
                        Pad();
                        out << "Args [" << p->args.size() << "]\n";
                        ++indent;
                        for (const auto &a: p->args)
                            if (a) PrintExpr(*a);
                        --indent;
                    }
                    --indent;
                } else if (const auto *p = dynamic_cast<const IndexExpr *>(&expr)) {
                    Pad();
                    out << "IndexExpr\n";
                    ++indent;
                    if (p->object) PrintExpr(*p->object);
                    if (p->index) PrintExpr(*p->index);
                    --indent;
                } else if (const auto *p = dynamic_cast<const FieldExpr *>(&expr)) {
                    Pad();
                    out << "FieldExpr '." << p->field << "'\n";
                    ++indent;
                    if (p->object) PrintExpr(*p->object);
                    --indent;
                } else if (const auto *p = dynamic_cast<const StructInitExpr *>(&expr)) {
                    Pad();
                    out << "StructInitExpr '" << p->typeName;
                    if (!p->typeArgs.empty()) {
                        out << "<";
                        for (std::size_t i = 0; i < p->typeArgs.size(); ++i) {
                            if (i) out << ", ";
                            out << TypeStr(p->typeArgs[i].get());
                        }
                        out << ">";
                    }
                    out << "'\n";
                    ++indent;
                    for (const auto &f: p->fields) {
                        Pad();
                        out << "." << f.name << " =\n";
                        ++indent;
                        if (f.value) PrintExpr(*f.value);
                        --indent;
                    }
                    --indent;
                } else if (const auto *p = dynamic_cast<const SliceExpr *>(&expr)) {
                    Pad();
                    out << "SliceExpr [" << p->elements.size() << "]\n";
                    ++indent;
                    for (const auto &e: p->elements)
                        if (e) PrintExpr(*e);
                    --indent;
                } else if (const auto *p = dynamic_cast<const CastExpr *>(&expr)) {
                    Pad();
                    out << "CastExpr as " << TypeStr(p->type.get()) << '\n';
                    ++indent;
                    if (p->operand) PrintExpr(*p->operand);
                    --indent;
                } else if (const auto *p = dynamic_cast<const IsExpr *>(&expr)) {
                    Pad();
                    out << "IsExpr is " << TypeStr(p->type.get()) << '\n';
                    ++indent;
                    if (p->operand) PrintExpr(*p->operand);
                    --indent;
                } else if (const auto *p = dynamic_cast<const BlockExpr *>(&expr)) {
                    if (p->block) PrintBlock(*p->block);
                }
            }

            void PrintLiteralExpr(const LiteralExpr &e) {
                Pad();
                out << "LiteralExpr (";
                switch (e.token.kind) {
                    case TokenKind::IntLiteral: out << "int";
                        break;
                    case TokenKind::FloatLiteral: out << "float";
                        break;
                    case TokenKind::StringLiteral: out << "string";
                        break;
                    case TokenKind::CharLiteral: out << "char32";
                        break;
                    case TokenKind::BoolLiteral: out << "bool8";
                        break;
                    case TokenKind::NullKeyword: out << "null";
                        break;
                    default: out << "?";
                        break;
                }
                out << ") '" << e.token.text << "'\n";
            }

            // ── Patterns ──────────────────────────────────────────────────────

            void PrintPattern(const Pattern &pat) {
                if (dynamic_cast<const WildcardPattern *>(&pat)) {
                    Pad();
                    out << "WildcardPattern\n";
                } else if (const auto *p = dynamic_cast<const LiteralPattern *>(&pat)) {
                    Pad();
                    out << "LiteralPattern '" << p->value.text << "'\n";
                } else if (const auto *p = dynamic_cast<const IdentPattern *>(&pat)) {
                    Pad();
                    out << "IdentPattern '" << p->name << "'\n";
                } else if (const auto *p = dynamic_cast<const RangePattern *>(&pat)) {
                    Pad();
                    out << "RangePattern " << (p->inclusive ? "..." : "..") << '\n';
                    ++indent;
                    if (p->lo) PrintPattern(*p->lo);
                    if (p->hi) PrintPattern(*p->hi);
                    --indent;
                } else if (const auto *p = dynamic_cast<const EnumPattern *>(&pat)) {
                    Pad();
                    out << "EnumPattern '";
                    for (std::size_t i = 0; i < p->path.size(); ++i) {
                        if (i) out << '.';
                        out << p->path[i];
                    }
                    out << "'";
                    if (!p->args.empty()) out << " [" << p->args.size() << " bindings]";
                    out << '\n';
                    if (!p->args.empty()) {
                        ++indent;
                        for (const auto &a: p->args)
                            if (a) PrintPattern(*a);
                        --indent;
                    }
                } else if (const auto *p = dynamic_cast<const StructPattern *>(&pat)) {
                    Pad();
                    out << "StructPattern '" << p->typeName << "'\n";
                    ++indent;
                    for (const auto &f: p->fields) {
                        Pad();
                        out << "." << f.name << ":\n";
                        ++indent;
                        if (f.pattern) PrintPattern(*f.pattern);
                        --indent;
                    }
                    --indent;
                } else if (const auto *p = dynamic_cast<const TuplePattern *>(&pat)) {
                    Pad();
                    out << "TuplePattern [" << p->elements.size() << "]\n";
                    ++indent;
                    for (const auto &e: p->elements)
                        if (e) PrintPattern(*e);
                    --indent;
                } else if (const auto *p = dynamic_cast<const GuardedPattern *>(&pat)) {
                    Pad();
                    out << "GuardedPattern\n";
                    ++indent;
                    if (p->inner) PrintPattern(*p->inner);
                    Pad();
                    out << "Guard\n";
                    ++indent;
                    if (p->guard) PrintExpr(*p->guard);
                    --indent;
                    --indent;
                }
            }
        };
    } // anonymous namespace

    // =========================================================================
    // DumpAst
    // =========================================================================

    bool Parser::DumpAst(const ParseResult &result, const std::filesystem::path &path) {
        std::ofstream f(path);
        if (!f) return false;

        AstPrinter printer(f);
        printer.Print(result.module);

        if (!result.diagnostics.empty()) {
            f << "\n--- diagnostics ---\n";
            for (const auto &d: result.diagnostics) {
                f << std::format("{:>4}:{:<4}  {}  {}\n",
                                 d.location.line,
                                 d.location.column,
                                 d.severity == ParserDiagnostic::Severity::Error
                                     ? "error  "
                                     : "warning",
                                 d.message);
            }
        }

        return f.good();
    }
} // namespace Rux
