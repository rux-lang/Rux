// Declaration parsing: attributes, functions, types, modules, imports.

#include "Syntax/Parser/Parser.h"

#include <algorithm>
#include <array>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Rux {
/// Canonical string key owned by the type parser and used for intrinsic and extension binding names.
std::string ImplTypeName(const TypeExpr &type);

/// Every rule `#Allow` accepts. Naming one the linter does not have is a typo that would otherwise silence nothing, so
/// the list lives beside the parse rather than in the linter: a source is rejected before it is ever linted.
constexpr std::array<std::string_view, 3> kLintRules{"naming.type", "naming.const", "docs.missing"};

[[nodiscard]] static std::string LintRuleList() {
    std::string list;
    for (const std::string_view rule : kLintRules) {
        if (!list.empty()) {
            list += ", ";
        }
        list += rule;
    }
    return list;
}

// Attribute parsing
std::string Parser::DecodeStringLiteralText(const std::string &text) {
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

/// Parses one `#Name(...)` attribute call, with the '#' already consumed. `#Error` and `#Warn` act at each use of the
/// declaration, `#Allow` suppresses a named lint rule for one declaration, `#Link` describes how an extern declaration
/// is imported (`#Library` and `#Symbol` are retained as compatibility spellings), and `#When` conditionally includes
/// the declaration at compile time, `#Abi(...)` selects a calling convention, and `#NoReturn()` marks a function that
/// never returns to its caller.
void Parser::ParseAttributeCall(ParsedAttrs &attrs) {
    const SourceLocation attributeLoc = Previous().location;
    const SourceLocation nameLoc = CurrentLocation();
    const std::string name = Advance().text;

    if (name != "Error" && name != "Warn" && name != "Allow" && name != "Link" && name != "Library" &&
        name != "Symbol" && name != "NoReturn" && name != "Abi") {
        // `#Intrinsic("Name")` became the `intrinsic` keyword, which takes its
        // name from the declaration instead of repeating it in a string.
        EmitError(nameLoc, name == "Intrinsic"
                               ? "the '#Intrinsic' attribute has been removed; write 'intrinsic #name: Type;' "
                                 "or 'intrinsic func Name(...);'"
                               : std::format("unknown attribute call '#{}'", name));
        // Skip a parenthesized argument list, if any, so the declaration that
        // follows still parses.
        if (Match(TokenKind::LeftParen)) {
            while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
                Advance();
            }
            ExpectBefore(TokenKind::RightParen, "')' to close the attribute call");
        }
        return;
    }

    ExpectBefore(TokenKind::LeftParen, std::format("'(' after '#{}'", name));
    if (name == "NoReturn") {
        if (attrs.usedNoReturn) {
            EmitError(nameLoc, "duplicate '#NoReturn' attribute");
        }
        attrs.usedNoReturn = true;
        attrs.noReturnLocation = attributeLoc;
        if (!Check(TokenKind::RightParen)) {
            EmitError(CurrentLocation(), "'#NoReturn' does not accept arguments");
            while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
                Advance();
            }
        }
        ExpectBefore(TokenKind::RightParen, "')' to close the attribute call");
        return;
    }

    if (name == "Abi") {
        if (attrs.usedAbi) {
            EmitError(nameLoc, "duplicate '#Abi' attribute");
        }
        attrs.usedAbi = true;
        attrs.abiLocation = attributeLoc;

        ExpectBefore(TokenKind::Dot, "'.' before the ABI name", "write the ABI as '.C', '.SysV', or '.Win64'");
        const SourceLocation variantLoc = CurrentLocation();
        std::string variant;
        if (Check(TokenKind::Ident)) {
            variant = Advance().text;
        }
        else {
            EmitExpected(variantLoc, "an ABI name after '.'", "use '.C', '.SysV', or '.Win64'");
        }

        if (variant == "C") {
            attrs.callConv = CallingConvention::C;
        }
        else if (variant == "Win64") {
            attrs.callConv = CallingConvention::Win64;
        }
        else if (variant == "SysV") {
            attrs.callConv = CallingConvention::SysV;
        }
        else if (!variant.empty()) {
            EmitError(variantLoc, std::format("unknown ABI '.{}'; valid ABIs are: .C, .SysV, .Win64", variant));
        }

        if (!Check(TokenKind::RightParen)) {
            EmitError(CurrentLocation(), "'#Abi' accepts exactly one argument");
            while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
                Advance();
            }
        }
        ExpectBefore(TokenKind::RightParen, "')' to close the attribute call");
        return;
    }

    if (name == "Allow") {
        attrs.allowLocation = attributeLoc;
        if (!Check(TokenKind::StringLiteral)) {
            EmitExpected(CurrentLocation(), "a lint rule string in '#Allow'",
                         "use '#Allow(\"naming.type\")' or '#Allow(\"naming.const\")'");
        }
        else {
            std::string rule = DecodeStringLiteralText(Advance().text);
            if (std::ranges::find(kLintRules, rule) == kLintRules.end()) {
                EmitError(nameLoc, std::format("unknown lint rule '{}'; valid rules are: {}", rule, LintRuleList()));
            }
            else if (std::find(attrs.allowedLints.begin(), attrs.allowedLints.end(), rule) !=
                     attrs.allowedLints.end()) {
                EmitError(nameLoc, std::format("duplicate '#Allow(\"{}\")' attribute", rule));
            }
            else {
                attrs.allowedLints.push_back(std::move(rule));
            }
        }
        if (!Check(TokenKind::RightParen)) {
            EmitError(CurrentLocation(), "'#Allow' accepts exactly one argument");
            while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
                Advance();
            }
        }
        ExpectBefore(TokenKind::RightParen, "')' to close the attribute call");
        return;
    }

    if (name == "Link") {
        const bool duplicate = attrs.usedLink;
        const bool mixed = attrs.usedLibrary || attrs.usedSymbol;
        if (duplicate) {
            EmitError(nameLoc, "duplicate '#Link' attribute");
        }
        if (mixed) {
            EmitError(nameLoc, "'#Link' cannot be combined with '#Library' or '#Symbol'");
        }
        attrs.usedLink = true;
        attrs.linkLocation = attributeLoc;

        std::string library;
        std::string libraryConst;
        if (Check(TokenKind::StringLiteral)) {
            library = DecodeStringLiteralText(Advance().text);
        }
        else if (Check(TokenKind::Ident)) {
            libraryConst = Advance().text;
        }
        else {
            EmitExpected(CurrentLocation(), "a library string or compile-time string constant in '#Link'",
                         "use '#Link(\"library\")' or '#Link(LibraryName)'");
            while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
                Advance();
            }
            ExpectBefore(TokenKind::RightParen, "')' to close the attribute call");
            return;
        }

        std::string symbol;
        std::string symbolConst;
        if (Match(TokenKind::Comma)) {
            if (Check(TokenKind::StringLiteral)) {
                symbol = DecodeStringLiteralText(Advance().text);
            }
            else if (Check(TokenKind::Ident)) {
                symbolConst = Advance().text;
            }
            else {
                EmitExpected(CurrentLocation(), "a symbol string or compile-time string constant after ',' in '#Link'");
            }
            if (Match(TokenKind::Comma)) {
                EmitError(Previous().location, "'#Link' accepts at most two arguments");
                while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
                    Advance();
                }
            }
        }
        ExpectBefore(TokenKind::RightParen, "')' to close the attribute call");

        if (!duplicate && !mixed) {
            attrs.importLib = std::move(library);
            attrs.importLibConst = std::move(libraryConst);
            attrs.importSymbol = std::move(symbol);
            attrs.importSymbolConst = std::move(symbolConst);
        }
        return;
    }

    if (Check(TokenKind::StringLiteral)) {
        std::string value = DecodeStringLiteralText(Advance().text);
        if (name == "Error") {
            attrs.errorMessage = std::move(value);
        }
        else if (name == "Warn") {
            attrs.warnMessage = std::move(value);
        }
        else if (name == "Library") {
            attrs.usedLibrary = true;
            if (attrs.usedLink) {
                EmitError(nameLoc, "'#Library' cannot be combined with '#Link'");
            }
            else {
                attrs.importLib = std::move(value);
            }
        }
        else {
            attrs.usedSymbol = true;
            if (attrs.usedLink) {
                EmitError(nameLoc, "'#Symbol' cannot be combined with '#Link'");
            }
            else {
                attrs.importSymbol = std::move(value);
            }
        }
    }
    else {
        std::string argument = "message";
        if (name == "Library") {
            argument = "library name";
        }
        else if (name == "Symbol") {
            argument = "imported symbol name";
        }
        EmitExpected(CurrentLocation(), std::format("a {} string in '#{}'", argument, name));
    }
    ExpectBefore(TokenKind::RightParen, "')' to close the attribute call");
}

/// Parses the attributes that precede a declaration. A declaration may carry any number of `#Name(...)` calls. The
/// removed `#{...}` metadata form is consumed only for recovery and always produces an error.
Parser::ParsedAttrs Parser::ParseAttrs() {
    ParsedAttrs attrs;
    while (Check(TokenKind::Hash)) {
        Advance(); // consume '#'

        // #Name("...") — attribute call
        if (Check(TokenKind::Ident)) {
            ParseAttributeCall(attrs);
            continue;
        }

        const SourceLocation hashLoc = Previous().location;
        if (!Match(TokenKind::LeftBrace)) {
            EmitExpected(CurrentLocation(), "an attribute name after '#'",
                         "write attributes as '#Name(...)', for example '#Abi(.Win64)'");
            continue;
        }
        EmitError(hashLoc, "metadata blocks '#{...}' are unsupported; use attribute calls such as '#Abi(.Win64)'");
        while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
            Advance();
        }
        ExpectBefore(TokenKind::RightBrace, "'}' to close the removed metadata block");
    }
    return attrs;
}

DeclPtr Parser::ApplyAttrs(DeclPtr decl, ParsedAttrs &attrs) {
    if (!decl) {
        return nullptr;
    }

    if (decl->warnMessage.empty()) {
        decl->warnMessage = attrs.warnMessage;
    }
    if (decl->errorMessage.empty()) {
        decl->errorMessage = attrs.errorMessage;
    }
    decl->allowedLints.insert(decl->allowedLints.end(), attrs.allowedLints.begin(), attrs.allowedLints.end());

    const bool namesATypeRule = std::ranges::find(attrs.allowedLints, "naming.type") != attrs.allowedLints.end();
    if (namesATypeRule && !dynamic_cast<TypeAliasDecl *>(decl.get()) && !dynamic_cast<StructDecl *>(decl.get()) &&
        !dynamic_cast<EnumDecl *>(decl.get()) && !dynamic_cast<UnionDecl *>(decl.get())) {
        EmitError(attrs.allowLocation, "'#Allow(\"naming.type\")' can only be applied to a type declaration");
    }
    // A raw binding keeps the spelling the platform published, and for a constant that is a C macro's own name --
    // `SEEK_SET`, `O_RDONLY`, `FILE_ATTRIBUTE_NORMAL` -- which no reader porting code would recognize renamed.
    const bool namesAConstRule = std::ranges::find(attrs.allowedLints, "naming.const") != attrs.allowedLints.end();
    if (namesAConstRule && !dynamic_cast<ConstDecl *>(decl.get())) {
        EmitError(attrs.allowLocation, "'#Allow(\"naming.const\")' can only be applied to a constant declaration");
    }
    if (attrs.usedLink && !dynamic_cast<ExternFuncDecl *>(decl.get()) && !dynamic_cast<ExternBlockDecl *>(decl.get())) {
        EmitError(attrs.linkLocation, "'#Link' can only be applied to an extern function or extern block");
    }

    if (attrs.usedNoReturn) {
        if (auto *function = dynamic_cast<FuncDecl *>(decl.get())) {
            function->isNoReturn = true;
            if (function->returnType) {
                EmitError(attrs.noReturnLocation, "'#NoReturn' function cannot declare a return type");
            }
        }
        else if (auto *externFunction = dynamic_cast<ExternFuncDecl *>(decl.get())) {
            externFunction->isNoReturn = true;
            if (externFunction->returnType) {
                EmitError(attrs.noReturnLocation, "'#NoReturn' function cannot declare a return type");
            }
        }
        else {
            EmitError(attrs.noReturnLocation, "'#NoReturn' can only be applied to a function");
        }
    }

    if (attrs.usedAbi && !dynamic_cast<FuncDecl *>(decl.get()) && !dynamic_cast<ExternFuncDecl *>(decl.get()) &&
        !dynamic_cast<ExternBlockDecl *>(decl.get())) {
        EmitError(attrs.abiLocation, "'#Abi' can only be applied to a function or extern block");
    }

    return decl;
}

/// The constant or function after an `intrinsic`. Its name is the intrinsic's: a constant takes its type (`Target`), a
/// free function its own name (`Assert`). A method is namespaced by the type it extends, and is keyed in ParseImplDecl.
DeclPtr Parser::ParseIntrinsicDecl(const bool isPublic, ParsedAttrs &attrs, const SourceLocation intrinsicLoc) {
    if (Match(TokenKind::TypeKeyword)) {
        auto decl = std::make_unique<TypeAliasDecl>();
        decl->location = intrinsicLoc;
        decl->isPublic = isPublic;
        decl->name = ExpectBefore(TokenKind::Ident, "a type name after 'intrinsic type'").text;
        decl->intrinsicName = decl->name;
        auto type = std::make_unique<NamedTypeExpr>();
        type->location = intrinsicLoc;
        type->name = decl->name;
        decl->type = std::move(type);
        ExpectBefore(TokenKind::Semicolon, "';' after the intrinsic type declaration");
        return ApplyAttrs(std::move(decl), attrs);
    }
    if (Check(TokenKind::StructKeyword)) {
        auto decl = ParseStructDecl(isPublic);
        decl->intrinsicName = decl->name;
        return ApplyAttrs(std::move(decl), attrs);
    }
    if (Match(TokenKind::ConstKeyword)) {
        auto decl = std::make_unique<ConstDecl>();
        decl->location = intrinsicLoc;
        decl->isPublic = isPublic;
        decl->name = ExpectBefore(TokenKind::Ident, "a constant name after 'intrinsic const'").text;
        decl->intrinsicName = decl->name;
        ExpectBefore(TokenKind::Colon, "':' after the intrinsic constant name");
        decl->type = ParseType("add the intrinsic constant type after ':'");
        ExpectBefore(TokenKind::Semicolon, "';' after the intrinsic constant declaration");
        return ApplyAttrs(std::move(decl), attrs);
    }
    // A compiler-injected value: `intrinsic #target: Target;`. The '#' name is
    // how the value is referred to; the type it is declared with names the
    // intrinsic the compiler binds.
    if (Check(TokenKind::Hash) && Peek(1).Is(TokenKind::Ident)) {
        const auto declLoc = CurrentLocation();
        Advance(); // consume '#'
        auto decl = std::make_unique<ConstDecl>();
        decl->location = declLoc;
        decl->isPublic = isPublic;
        decl->name = "#" + Advance().text;
        if (Match(TokenKind::Colon)) {
            decl->type = ParseType("add the intrinsic value type after ':'");
        }
        else {
            EmitExpected(CurrentLocation(), "':' after the intrinsic value name",
                         "write intrinsic values as 'intrinsic #name: Type;'");
        }
        if (decl->type) {
            decl->intrinsicName = ImplTypeName(**decl->type);
        }
        ExpectBefore(TokenKind::Semicolon, "';' after the intrinsic value declaration");
        return ApplyAttrs(std::move(decl), attrs);
    }
    if (Check(TokenKind::FuncKeyword)) {
        auto func = ParseFuncDecl(isPublic, false, attrs.callConv);
        if (func) {
            func->intrinsicName = func->name;
            if (func->body) {
                EmitError(intrinsicLoc, "'intrinsic' function cannot have a body");
            }
        }
        return ApplyAttrs(std::move(func), attrs);
    }
    EmitExpected(CurrentLocation(), "a '#'-prefixed value or 'func' after 'intrinsic'",
                 "write 'intrinsic #name: Type;' or 'intrinsic func Name(...);'");
    Recover();
    return nullptr;
}

// struct

} // namespace Rux
