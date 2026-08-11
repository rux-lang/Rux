// Declaration parsing: attributes, functions, types, modules, imports.

#include "Syntax/Parser/Parser.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Rux {
// Canonical string key for a type used as an intrinsic's binding name; defined below.
static std::string ImplTypeName(const TypeExpr &type);

// Inside an asm body, any identifier-like token — a plain identifier or a
// language keyword such as `loop` or `for` — may name a mnemonic, register,
// symbol or label. The lexer has already classified keywords, so recover their
// identifier role here.
static bool IsAsmNameToken(const Token &t) {
    return t.Is(TokenKind::Ident) || t.IsKeyword();
}

// x86-64 mnemonics that never take an operand. They have to be listed because
// an identifier after one of them starts the next instruction rather than an
// operand, and x86-64 operand syntax gives no other way to tell. AArch64 needs
// no such list: its operands are registers, `#` immediates and brackets, so
// `CanStartAsmOperand` asks the mnemonic table instead.
static bool IsZeroOperandAsmMnemonic(const std::string_view mnemonic) {
    return mnemonic == "ret" || mnemonic == "leave" || mnemonic == "nop" || mnemonic == "syscall" ||
           mnemonic == "cqo" || mnemonic == "cdq" || mnemonic == "cdqe";
}

static std::string LowerAsmName(std::string name) {
    for (char &c : name) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return name;
}

// AArch64 shift and extend keywords, as written after a register or immediate
// operand and inside a register-offset memory operand.
static AsmShiftKind AsmShiftFromName(const std::string_view name) noexcept {
    if (name == "lsl") {
        return AsmShiftKind::Lsl;
    }
    if (name == "lsr") {
        return AsmShiftKind::Lsr;
    }
    if (name == "asr") {
        return AsmShiftKind::Asr;
    }
    if (name == "ror") {
        return AsmShiftKind::Ror;
    }
    return AsmShiftKind::None;
}

static AsmExtendKind AsmExtendFromName(const std::string_view name) noexcept {
    static constexpr std::pair<std::string_view, AsmExtendKind> table[8] = {
        {"uxtb", AsmExtendKind::Uxtb}, {"uxth", AsmExtendKind::Uxth}, {"uxtw", AsmExtendKind::Uxtw},
        {"uxtx", AsmExtendKind::Uxtx}, {"sxtb", AsmExtendKind::Sxtb}, {"sxth", AsmExtendKind::Sxth},
        {"sxtw", AsmExtendKind::Sxtw}, {"sxtx", AsmExtendKind::Sxtx},
    };
    for (const auto &[text, kind] : table) {
        if (name == text) {
            return kind;
        }
    }
    return AsmExtendKind::None;
}

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

// Parses one `#Name(...)` attribute call, with the '#' already consumed.
// `#Error` and `#Warn` act at each use of the declaration, `#Allow` suppresses
// a named lint rule for one declaration, `#Link` describes
// how an extern declaration is imported (`#Library` and `#Symbol` are retained
// as compatibility spellings), and `#When` conditionally includes the
// declaration at compile time, `#Abi(...)` selects a calling convention, and
// `#NoReturn()` marks a function that never returns to its caller.
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
            Expect(TokenKind::RightParen, "expected ')' to close the attribute call");
        }
        return;
    }

    Expect(TokenKind::LeftParen, std::format("expected '(' after '#{}'", name));
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
        Expect(TokenKind::RightParen, "expected ')' to close the attribute call");
        return;
    }

    if (name == "Abi") {
        if (attrs.usedAbi) {
            EmitError(nameLoc, "duplicate '#Abi' attribute");
        }
        attrs.usedAbi = true;
        attrs.abiLocation = attributeLoc;

        Expect(TokenKind::Dot, "expected '.' before an ABI");
        const SourceLocation variantLoc = CurrentLocation();
        std::string variant;
        if (Check(TokenKind::Ident)) {
            variant = Advance().text;
        }
        else {
            EmitError(variantLoc, "expected an ABI name");
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
        Expect(TokenKind::RightParen, "expected ')' to close the attribute call");
        return;
    }

    if (name == "Allow") {
        attrs.allowLocation = attributeLoc;
        if (!Check(TokenKind::StringLiteral)) {
            EmitError(CurrentLocation(), "'#Allow' takes a lint rule string");
        }
        else {
            std::string rule = DecodeStringLiteralText(Advance().text);
            if (rule != "naming.type") {
                EmitError(nameLoc, std::format("unknown lint rule '{}'; valid rules are: naming.type", rule));
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
        Expect(TokenKind::RightParen, "expected ')' to close the attribute call");
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
            EmitError(CurrentLocation(), "'#Link' requires a library name string or compile-time string constant");
            while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
                Advance();
            }
            Expect(TokenKind::RightParen, "expected ')' to close the attribute call");
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
                EmitError(CurrentLocation(), "'#Link' symbol name must be a string or compile-time string constant");
            }
            if (Match(TokenKind::Comma)) {
                EmitError(Previous().location, "'#Link' accepts at most two arguments");
                while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
                    Advance();
                }
            }
        }
        Expect(TokenKind::RightParen, "expected ')' to close the attribute call");

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
        EmitError(CurrentLocation(), std::format("'#{}' takes a {} string", name, argument));
    }
    Expect(TokenKind::RightParen, "expected ')' to close the attribute call");
}

// Parses the attributes that precede a declaration. A declaration may carry
// any number of `#Name(...)` calls. The removed `#{...}` metadata form is
// consumed only for recovery and always produces an error.
Parser::ParsedAttrs Parser::ParseAttrs() {
    ParsedAttrs attrs;
    while (Check(TokenKind::Hash)) {
        Advance(); // consume '#'

        // #Name("...") — attribute call
        if (Check(TokenKind::Ident)) {
            ParseAttributeCall(attrs);
            continue;
        }

        const SourceLocation metadataLoc = Previous().location;
        Expect(TokenKind::LeftBrace, "expected an attribute name after '#'");
        EmitError(metadataLoc, "metadata blocks '#{...}' are unsupported; use attribute calls such as '#Abi(.Win64)'");
        while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
            Advance();
        }
        Expect(TokenKind::RightBrace, "expected '}' to close the removed metadata block");
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

    if (!attrs.allowedLints.empty() && !dynamic_cast<TypeAliasDecl *>(decl.get()) &&
        !dynamic_cast<StructDecl *>(decl.get()) && !dynamic_cast<EnumDecl *>(decl.get()) &&
        !dynamic_cast<UnionDecl *>(decl.get())) {
        EmitError(attrs.allowLocation, "'#Allow(\"naming.type\")' can only be applied to a type declaration");
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

// The constant or function after an `intrinsic`. Its name is the intrinsic's:
// a constant takes its type (`Target`), a free function its own name (`Assert`).
// A method is namespaced by the type it extends, and is keyed in ParseImplDecl.
DeclPtr Parser::ParseIntrinsicDecl(const bool isPublic, ParsedAttrs &attrs, const SourceLocation intrinsicLoc) {
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
            decl->type = ParseType();
        }
        if (decl->type) {
            decl->intrinsicName = ImplTypeName(**decl->type);
        }
        else {
            EmitError(decl->location, "'intrinsic' value requires an explicit type, which names the intrinsic");
        }
        Expect(TokenKind::Semicolon, "expected ';'");
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
    EmitError(intrinsicLoc, "'intrinsic' can only be applied to a '#'-prefixed value or a function");
    Recover();
    return nullptr;
}

// Top-level declarations
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

    p.isMut = Match(TokenKind::VarKeyword);
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

// asm body: a sequence of instructions and label definitions between the
// braces of an `asm func`. Newlines are not significant to the lexer, so an
// instruction's operand list simply ends at the first token that is not a
// comma — the next mnemonic, a label, or the closing brace.
std::vector<AsmInstr> Parser::ParseAsmBody() {
    std::vector<AsmInstr> instrs;
    while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
        // A label definition: `name:`.
        if (IsAsmNameToken(Peek()) && Peek(1).Is(TokenKind::Colon)) {
            AsmInstr label;
            label.arch = arch;
            label.location = CurrentLocation();
            label.labelDef = Advance().text; // name
            Advance();                       // ':'
            instrs.push_back(std::move(label));
            continue;
        }

        if (!IsAsmNameToken(Peek())) {
            EmitError(CurrentLocation(), std::format("expected an assembly mnemonic, found '{}'", Peek().text));
            Advance(); // skip the offending token to make progress
            continue;
        }

        AsmInstr instr;
        instr.arch = arch;
        instr.location = CurrentLocation();
        instr.mnemonic = LowerAsmName(Advance().text);

        // AArch64 writes a branch's condition into its name — `B.EQ` — and the
        // lexer hands the three pieces over separately.
        if (arch == Target::Arch::AArch64 && Check(TokenKind::Dot) && IsAsmNameToken(Peek(1))) {
            Advance(); // '.'
            instr.mnemonic += '.';
            instr.mnemonic += LowerAsmName(Advance().text);
        }

        // Operands, comma-separated. Stop when the operand is not followed
        // by a comma (i.e. the next token starts a new instruction).
        const bool zeroOperand = arch != Target::Arch::AArch64 && IsZeroOperandAsmMnemonic(instr.mnemonic);
        if (zeroOperand || (!Check(TokenKind::RightBrace) && !CanStartAsmOperand())) {
            instrs.push_back(std::move(instr));
            continue;
        }
        while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
            instr.operands.push_back(ParseAsmOperand());
            if (!Match(TokenKind::Comma)) {
                break;
            }
        }
        instrs.push_back(std::move(instr));
    }
    return instrs;
}

// True when the current token can begin an operand of the instruction whose
// mnemonic was just consumed. Used to tell a zero-operand instruction (ret,
// syscall) followed by another mnemonic apart from one that takes operands.
bool Parser::CanStartAsmOperand() const noexcept {
    switch (Peek().kind) {
    case TokenKind::IntLiteral:
    case TokenKind::LeftBracket:
    case TokenKind::Minus:
    case TokenKind::Plus:
        return true;
    case TokenKind::Hash:
        // AArch64 marks an immediate with '#'. Nothing in x86-64 syntax starts
        // with one, so it is accepted there too and reported as the foreign
        // instruction it belongs to rather than as a stray token.
        return true;
    default:
        // An identifier-like token begins a new instruction if it is itself a
        // label definition (`name :`); otherwise it is a register / symbol.
        if (!IsAsmNameToken(Peek()) || Peek(1).Is(TokenKind::Colon)) {
            return false;
        }
        if (arch == Target::Arch::AArch64) {
            // `RET` takes an optional register and `B` a label, so whether an
            // identifier continues this instruction or starts the next one is
            // decided by what the identifier is: a register or a symbol
            // continues, an instruction name starts.
            const std::string lowered = LowerAsmName(Peek().text);
            return IsRegisterName(arch, lowered) || !IsAsmMnemonic(arch, lowered);
        }
        return true;
    }
}

// Parse one operand: a register, an immediate, a `[...]` memory reference, a
// size-prefixed memory reference (qword [...]), or a symbol / label name.
AsmOperand Parser::ParseAsmOperand() {
    AsmOperand op;
    op.location = CurrentLocation();

    // Optional size specifier before a memory operand: byte/word/dword/qword.
    int sizeHint = 0;
    if (Check(TokenKind::Ident)) {
        const std::string &t = Peek().text;
        if (t == "byte") {
            sizeHint = 1;
        }
        else if (t == "word") {
            sizeHint = 2;
        }
        else if (t == "dword") {
            sizeHint = 4;
        }
        else if (t == "qword") {
            sizeHint = 8;
        }
        if (sizeHint != 0 && Peek(1).Is(TokenKind::LeftBracket)) {
            Advance();               // size keyword
            Match(TokenKind::Ident); // optional 'ptr'
        }
        else {
            sizeHint = 0;
        }
    }

    if (Check(TokenKind::LeftBracket)) {
        ParseAsmMemory(op);
        op.memSize = sizeHint;
        return op;
    }

    // AArch64 marks an immediate with '#'; both architectures accept a bare
    // integer, which is the only spelling x86-64 has.
    const bool hashed = Match(TokenKind::Hash);
    if (hashed || CheckAny({TokenKind::IntLiteral, TokenKind::Minus, TokenKind::Plus})) {
        op.kind = AsmOperand::Kind::Imm;
        op.imm = ParseAsmInt();
        ParseAsmShift(op); // `#1, LSL #12`
        return op;
    }

    // An identifier-like token: either a register or a symbol / label reference.
    if (!IsAsmNameToken(Peek())) {
        EmitError(CurrentLocation(), std::format("expected an assembly operand, found '{}'", Peek().text));
        return op;
    }
    const Token &tok = Advance();
    std::string name = tok.text;
    std::string lowered = LowerAsmName(name);
    if (IsRegisterName(arch, lowered)) {
        op.kind = AsmOperand::Kind::Reg;
        op.name = std::move(lowered);
        ParseAsmShift(op); // `X1, LSL #3`
    }
    else {
        // A condition (`CSEL X0, X1, X2, EQ`) arrives here too: which of the
        // two an instruction wanted is the assembler's to say.
        op.kind = AsmOperand::Kind::Sym;
        op.name = std::move(name);
    }
    return op;
}

// AArch64: the shift or extend a register or immediate operand may carry, as
// the tail of the operand rather than as an operand of its own. Recognized
// only after a comma, so an instruction whose name is a shift keyword — `LSL`
// is an alias of `UBFM` — still starts a new instruction when one follows.
//
// Read for every architecture rather than only for AArch64: an AArch64 body
// compiled for x86-64 is diagnosed by its mnemonics, and reading it as far as
// the assembler keeps that one diagnostic from arriving behind a pile of
// syntax errors about a syntax the target simply does not have.
void Parser::ParseAsmShift(AsmOperand &op) {
    if (!Check(TokenKind::Comma) || !IsAsmNameToken(Peek(1))) {
        return;
    }
    const std::string keyword = LowerAsmName(Peek(1).text);
    const AsmShiftKind shift = AsmShiftFromName(keyword);
    const AsmExtendKind extend = AsmExtendFromName(keyword);
    if (shift == AsmShiftKind::None && extend == AsmExtendKind::None) {
        return;
    }
    Advance(); // ','
    Advance(); // the keyword
    op.shift = shift;
    op.extend = extend;
    // An extend without an amount shifts by nothing; a shift always says how far.
    if (Match(TokenKind::Hash) || CheckAny({TokenKind::IntLiteral, TokenKind::Minus, TokenKind::Plus})) {
        op.shiftAmount = static_cast<int>(ParseAsmInt());
    }
    else if (shift != AsmShiftKind::None) {
        EmitError(CurrentLocation(), std::format("expected a shift amount after '{}'", keyword));
    }
}

// Parse a memory operand. x86-64 writes `[base + index*scale +/- disp]`, where
// any of the three may be omitted; AArch64 writes `[Xn]`, `[Xn, #off]`,
// `[Xn, Xm]`, `[Xn, Xm, LSL #3]` and `[Xn, Wm, UXTW #2]`, followed by `!` for
// pre-index or, outside the brackets, `, #off` for post-index. One loop reads
// both: the separators an architecture does not use simply never appear.
void Parser::ParseAsmMemory(AsmOperand &op) {
    op.kind = AsmOperand::Kind::Mem;
    Expect(TokenKind::LeftBracket, "expected '['");
    bool negateNext = false;
    while (!Check(TokenKind::RightBracket) && !IsAtEnd()) {
        if (Match(TokenKind::Plus)) {
            negateNext = false;
            continue;
        }
        if (Match(TokenKind::Minus)) {
            negateNext = true;
            continue;
        }
        if (Match(TokenKind::Comma) || Match(TokenKind::Hash)) {
            continue;
        }
        if (Check(TokenKind::IntLiteral)) {
            std::int64_t v = ParseAsmInt();
            op.imm += negateNext ? -v : v;
            negateNext = false;
            continue;
        }
        if (IsAsmNameToken(Peek())) {
            std::string name = Advance().text;
            std::string lowered = LowerAsmName(name);
            // Scaled index: reg * scale.
            if (Match(TokenKind::Star)) {
                op.memIndex = std::move(lowered);
                op.memScale = static_cast<int>(ParseAsmInt());
            }
            else if (lowered == "rip") {
                op.memBase = "rip";
            }
            else if (AsmShiftFromName(lowered) != AsmShiftKind::None) {
                op.shift = AsmShiftFromName(lowered);
                Match(TokenKind::Hash);
                op.shiftAmount = static_cast<int>(ParseAsmInt());
            }
            else if (AsmExtendFromName(lowered) != AsmExtendKind::None) {
                op.extend = AsmExtendFromName(lowered);
                // The amount is optional: `[X0, W1, UXTW]` scales by nothing.
                if (Match(TokenKind::Hash) || Check(TokenKind::IntLiteral)) {
                    op.shiftAmount = static_cast<int>(ParseAsmInt());
                }
            }
            else if (IsRegisterName(arch, lowered)) {
                if (op.memBase.empty()) {
                    op.memBase = std::move(lowered);
                }
                else {
                    op.memIndex = std::move(lowered);
                }
            }
            else {
                op.memSym = std::move(name);
            }
            continue;
        }
        EmitError(CurrentLocation(), std::format("unexpected token '{}' in memory operand", Peek().text));
        Advance();
    }
    Expect(TokenKind::RightBracket, "expected ']'");

    // Writeback: `[X0, #8]!` updates the base before the access, `[X0], #8`
    // after it. The post-index offset sits outside the brackets, so it has to
    // be taken here — the operand list would otherwise read it as an operand
    // of its own. It is recognized by the '#', which is what keeps the x86-64
    // `mov qword [rsp - 8], 5` from being read as one.
    if (Match(TokenKind::Bang)) {
        op.indexMode = AsmIndexMode::PreIndex;
    }
    else if (Check(TokenKind::Comma) && Peek(1).Is(TokenKind::Hash)) {
        Advance(); // ','
        Advance(); // '#'
        op.imm = ParseAsmInt();
        op.indexMode = AsmIndexMode::PostIndex;
    }
}

// Parse an optionally-signed integer literal (decimal, hex, octal, binary).
std::int64_t Parser::ParseAsmInt() {
    bool negative = false;
    if (Match(TokenKind::Minus)) {
        negative = true;
    }
    else {
        Match(TokenKind::Plus);
    }
    const Token &tok = Expect(TokenKind::IntLiteral, "expected an integer");
    std::string text;
    for (const char c : tok.text) {
        if (c != '_') {
            text.push_back(c);
        }
    }
    int base = 10;
    std::string_view digits(text);
    if (digits.size() > 2 && digits[0] == '0') {
        switch (digits[1]) {
        case 'x':
        case 'X':
            base = 16;
            digits.remove_prefix(2);
            break;
        case 'b':
        case 'B':
            base = 2;
            digits.remove_prefix(2);
            break;
        case 'o':
        case 'O':
            base = 8;
            digits.remove_prefix(2);
            break;
        default:
            break;
        }
    }
    std::uint64_t value = 0;
    std::from_chars(digits.data(), digits.data() + digits.size(), value, base);
    auto result = static_cast<std::int64_t>(value);
    return negative ? -result : result;
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
        field.documentation = ParseDocumentation();
        field.location = CurrentLocation();

        if (Match(TokenKind::PubKeyword)) {
            field.isPublic = true;
        }

        // Keywords are contextual after a field declaration starts. This lets
        // ordinary package APIs expose members such as `#source.module`.
        field.name =
            Check(TokenKind::ModuleKeyword) ? Advance().text : Expect(TokenKind::Ident, "expected field name").text;
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
    if (Check(TokenKind::Less)) {
        decl->typeParams = ParseTypeParams();
    }
    if (Match(TokenKind::Colon)) {
        decl->baseType = ParseType();
    }

    Expect(TokenKind::LeftBrace, "expected '{'");
    while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
        EnumDecl::Variant variant;
        variant.documentation = ParseDocumentation();
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
                field.documentation = ParseDocumentation();
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
        field.documentation = ParseDocumentation();
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
        std::string documentation = ParseDocumentation();
        if (!Check(TokenKind::FuncKeyword)) {
            EmitError(CurrentLocation(), "expected 'func' in interface body");
            Recover();
            continue;
        }
        if (auto method = ParseFuncDecl(false, false)) {
            method->documentation = std::move(documentation);
            decl->methods.push_back(std::move(method));
        }
    }
    Expect(TokenKind::RightBrace, "expected '}'");
    return decl;
}

// Language aliases that ResolveType normalizes; mirror them here so the
// canonical key produced from the type expression matches the resolved
// receiver type's spelling (e.g. `bool[]` and a `bool8[]` receiver agree).
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

// Canonical string key for an `extend` target. Named types keep their bare name
// (generic-agnostic, matching struct behaviour); arrays retain their source
// spelling and are validated semantically.
static std::string ImplTypeName(const TypeExpr &type) {
    if (const auto *n = dynamic_cast<const NamedTypeExpr *>(&type)) {
        std::string result = NormalizePrimitiveName(n->name);
        if (!n->typeArgs.empty()) {
            result += "<";
            for (std::size_t i = 0; i < n->typeArgs.size(); ++i) {
                if (i) {
                    result += ", ";
                }
                result += ImplTypeName(*n->typeArgs[i]);
            }
            result += ">";
        }
        return result;
    }
    if (const auto *s = dynamic_cast<const ArrayTypeExpr *>(&type)) {
        return ImplTypeName(*s->element) + (s->size ? "[N]" : "[]");
    }
    if (const auto *p = dynamic_cast<const PointerTypeExpr *>(&type)) {
        return "*" + ImplTypeName(*p->pointee);
    }
    if (const auto *pt = dynamic_cast<const PathTypeExpr *>(&type)) {
        std::string result;
        for (std::size_t i = 0; i < pt->segments.size(); ++i) {
            if (i) {
                result += "::";
            }
            result += pt->segments[i];
        }
        return result;
    }
    return "?";
}

// extend
std::unique_ptr<ImplDecl> Parser::ParseImplDecl() {
    const auto loc = CurrentLocation();
    Expect(TokenKind::ExtendKeyword, "expected 'extend'");

    auto decl = std::make_unique<ImplDecl>();
    decl->location = loc;

    // extend Type  or  extend Type : InterfaceName  or  extend InterfaceName
    // for Type. The leading item is parsed as a full type expression so that
    // compound receivers such as `int[]` are supported.
    TypeExprPtr firstType = ParseType();
    const std::string firstName = firstType ? ImplTypeName(*firstType) : "?";
    if (Match(TokenKind::Colon)) {
        decl->extendedType = std::move(firstType);
        decl->typeName = firstName;
        decl->interfaceName = Expect(TokenKind::Ident, "expected interface name after ':'").text;
    }
    else if (Match(TokenKind::ForKeyword)) {
        decl->interfaceName = firstName;
        decl->extendedType = ParseType();
        decl->typeName = decl->extendedType ? ImplTypeName(*decl->extendedType) : "?";
    }
    else {
        decl->extendedType = std::move(firstType);
        decl->typeName = firstName;
    }

    Expect(TokenKind::LeftBrace, "expected '{'");
    while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
        std::string documentation = ParseDocumentation();
        // Methods can be conditionally compiled like any other declaration.
        // Conditional compilation later moves those of the taken branch into
        // `methods`.
        if (Check(TokenKind::WhenKeyword)) {
            if (auto conditional = ParseWhenDecl()) {
                conditional->documentation = std::move(documentation);
                decl->conditionals.push_back(std::move(conditional));
            }
            continue;
        }
        ParsedAttrs attrs = ParseAttrs();
        bool pub = Match(TokenKind::PubKeyword);
        const bool isIntrinsic = Match(TokenKind::IntrinsicKeyword);
        const auto intrinsicLoc = Previous().location;
        if (!Check(TokenKind::FuncKeyword)) {
            EmitError(CurrentLocation(), isIntrinsic ? "expected 'func' after 'intrinsic' in extend body"
                                                     : "expected 'func' in extend body");
            Recover();
            continue;
        }
        if (auto method = ParseFuncDecl(pub, false, attrs.callConv)) {
            method->documentation = std::move(documentation);
            if (isIntrinsic) {
                // A method's intrinsic is namespaced by the type it extends, so
                // `extend Target { intrinsic func HasFeature }` is
                // `Target.HasFeature`.
                method->intrinsicName = decl->typeName + "." + method->name;
                if (method->body) {
                    EmitError(intrinsicLoc, "'intrinsic' function cannot have a body");
                }
            }
            DeclPtr attributed = ApplyAttrs(std::move(method), attrs);
            auto *methodDecl = static_cast<FuncDecl *>(attributed.release());
            decl->methods.emplace_back(methodDecl);
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

    auto nested = std::make_unique<ModuleDecl>();
    nested->location = loc;
    nested->isPublic = path.size() == 1 ? isPublic : false;
    nested->name = std::move(path.back());
    nested->items = std::move(items);

    for (std::size_t i = path.size() - 1; i-- > 0;) {
        auto decl = std::make_unique<ModuleDecl>();
        decl->location = loc;
        decl->isPublic = (i == 0) ? isPublic : false;
        decl->name = std::move(path[i]);
        decl->items.push_back(std::move(nested));
        nested = std::move(decl);
    }
    return nested;
}

// import
std::unique_ptr<UseDecl> Parser::ParseUseDecl(const bool requireSemicolon) {
    const auto loc = CurrentLocation();
    Expect(TokenKind::ImportKeyword, "expected 'import'");

    auto decl = std::make_unique<UseDecl>();
    decl->location = loc;

    // Parse path segments separated by '.' or '::'
    decl->path.push_back(Expect(TokenKind::Ident, "expected module path").text);

    while (!IsAtEnd()) {
        if (Match(TokenKind::Dot)) {
            if (Match(TokenKind::Star)) {
                // import Rux.Primitives.*;
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
                    // An intrinsic value is imported by its '#'-prefixed name.
                    if (Check(TokenKind::Hash) && Peek(1).Is(TokenKind::Ident)) {
                        Advance(); // consume '#'
                        decl->names.push_back("#" + Advance().text);
                    }
                    else {
                        decl->names.push_back(Expect(TokenKind::Ident, "expected name").text);
                    }
                    if (!Match(TokenKind::Comma)) {
                        break;
                    }
                }
                Expect(TokenKind::RightBrace, "expected '}'");
                break;
            }
            if (Match(TokenKind::Star)) {
                decl->kind = UseDecl::Kind::Glob;
                break;
            }
            decl->path.push_back(Expect(TokenKind::Ident, "expected identifier").text);
        }
        else {
            break;
        }
    }

    // As a compile-time match arm body (`.Linux => import Linux::{...}`) the
    // trailing ';' is optional, since the arm is delimited by the next pattern.
    if (requireSemicolon) {
        Expect(TokenKind::Semicolon, "expected ';'");
    }
    else {
        Match(TokenKind::Semicolon);
    }
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

// when cond { decls } else when cond { decls } else { decls }
std::unique_ptr<WhenDecl> Parser::ParseWhenDecl() {
    const auto loc = CurrentLocation();
    Expect(TokenKind::WhenKeyword, "expected 'when'");
    return ParseWhenBody(loc);
}

// The chain after its opening keyword, so that a rejected `#if` can still be
// parsed as the `when` it should have been and report only its own error.
std::unique_ptr<WhenDecl> Parser::ParseWhenBody(const SourceLocation loc) {
    auto decl = std::make_unique<WhenDecl>();
    decl->location = loc;

    auto parseItems = [&] {
        std::vector<DeclPtr> items;
        Expect(TokenKind::LeftBrace, "expected '{'");
        while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
            if (auto item = ParseDecl()) {
                items.push_back(std::move(item));
            }
            else {
                Recover();
            }
        }
        Expect(TokenKind::RightBrace, "expected '}'");
        return items;
    };

    auto parseCondition = [&] {
        structInitAllowed = false;
        auto condition = ParseExpr();
        structInitAllowed = true;
        return condition;
    };

    WhenDecl::Branch first;
    first.location = loc;
    first.condition = parseCondition();

    // Compile-time match: `when subject { pattern => ..., else => ... }`.
    if (Check(TokenKind::LeftBrace) && NextBraceIsMatchArms()) {
        return ParseWhenMatchBody(loc, std::move(first.condition));
    }

    first.items = parseItems();
    decl->branches.push_back(std::move(first));

    while (Check(TokenKind::ElseKeyword)) {
        WhenDecl::Branch branch;
        branch.location = CurrentLocation();
        Advance(); // consume 'else'
        // A `when` chain is compile-time throughout, so its arms are `else when`;
        // `else if` would read as a run-time test of a branch that was already
        // selected during compilation. It is still parsed as the `else when` it
        // meant, so the rest of the chain reports nothing further.
        const bool isElseIf = Check(TokenKind::IfKeyword);
        if (isElseIf) {
            EmitError(CurrentLocation(), "expected 'when' after 'else' in a compile-time 'when' chain; "
                                         "'if' is the run-time conditional");
            Advance();
        }
        const bool isElseWhen = Match(TokenKind::WhenKeyword) || isElseIf;
        if (isElseWhen) {
            branch.condition = parseCondition();
        }
        branch.items = parseItems();
        decl->branches.push_back(std::move(branch));
        if (!isElseWhen) {
            break; // a bare `else` ends the chain
        }
    }

    return decl;
}

// `when subject { pattern => body, ... else => body }` at declaration level. An
// arm body is a `#Error`/`#Warn` directive, a `{ decls }` block, or a single
// declaration. Arms may be comma-separated, but declarations self-terminate so
// the comma is optional.
std::unique_ptr<WhenDecl> Parser::ParseWhenMatchBody(const SourceLocation loc, ExprPtr subject) {
    auto decl = std::make_unique<WhenDecl>();
    decl->location = loc;
    decl->matchSubject = std::move(subject);

    Expect(TokenKind::LeftBrace, "expected '{'");
    bool sawElse = false;
    while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
        WhenDecl::Branch branch;
        branch.location = CurrentLocation();
        if (Match(TokenKind::ElseKeyword)) {
            sawElse = true;
        }
        else {
            structInitAllowed = false;
            branch.patterns.push_back(ParseExpr());
            // Commas before `=>` separate patterns that share this arm's body.
            while (Match(TokenKind::Comma) && !Check(TokenKind::FatArrow)) {
                branch.patterns.push_back(ParseExpr());
            }
            structInitAllowed = true;
        }
        Expect(TokenKind::FatArrow, "expected '=>' after a 'when' arm pattern");

        if (Check(TokenKind::Hash) && Peek(1).Is(TokenKind::Ident) &&
            (Peek(1).text == "Error" || Peek(1).text == "Warn") && Peek(2).Is(TokenKind::LeftParen)) {
            branch.directiveLocation = CurrentLocation();
            Advance(); // '#'
            const std::string name = Advance().text;
            branch.directive = name == "Error" ? WhenDecl::Directive::Error : WhenDecl::Directive::Warn;
            Expect(TokenKind::LeftParen, std::format("expected '(' after '#{}'", name));
            if (Check(TokenKind::StringLiteral)) {
                branch.directiveMessage = DecodeStringLiteralText(Advance().text);
            }
            else {
                EmitError(CurrentLocation(), std::format("'#{}' message must be a string literal", name));
            }
            Expect(TokenKind::RightParen, "expected ')'");
        }
        else if (Check(TokenKind::LeftBrace)) {
            Advance(); // '{'
            while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
                if (auto item = ParseDecl()) {
                    branch.items.push_back(std::move(item));
                }
                else {
                    Recover();
                }
            }
            Expect(TokenKind::RightBrace, "expected '}'");
        }
        else if (Check(TokenKind::ImportKeyword)) {
            // A bare `import` arm body needs no trailing ';'.
            branch.items.push_back(ParseUseDecl(false));
        }
        else if (auto item = ParseDecl()) {
            branch.items.push_back(std::move(item));
        }

        decl->branches.push_back(std::move(branch));
        Match(TokenKind::Comma); // optional separator
        if (sawElse && !Check(TokenKind::RightBrace)) {
            EmitError(CurrentLocation(), "the 'else' arm must be last in a 'when' match");
        }
    }
    Expect(TokenKind::RightBrace, "expected '}' to close the 'when' match");
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
DeclPtr Parser::ParseExternDecl(bool isPublic, ParsedAttrs &attrs) {
    const auto loc = CurrentLocation();
    Expect(TokenKind::ExternKeyword, "expected 'extern'");

    if (Check(TokenKind::LeftBrace)) {
        // #Link(...) [#Abi(...)] extern { func ...; ... }
        Advance(); // consume '{'
        // One symbol name cannot stand for every function in the block; it has
        // to sit on the individual declaration.
        if (!attrs.importSymbol.empty() || !attrs.importSymbolConst.empty()) {
            EmitError(loc, "an imported symbol name cannot be applied to an extern block; "
                           "use the one-argument '#Link(\"library\")' form");
        }
        auto block = std::make_unique<ExternBlockDecl>();
        block->location = loc;
        block->dll = attrs.importLib;
        block->dllConst = attrs.importLibConst;
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
                fd->dllConst = attrs.importLibConst;
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
        // #Link("..."[, "symbol"]) extern func Name(params) -> Type;
        Advance(); // consume 'func'

        auto decl = std::make_unique<ExternFuncDecl>();
        decl->location = loc;
        decl->isPublic = isPublic;
        decl->dll = std::move(attrs.importLib);
        decl->dllConst = std::move(attrs.importLibConst);
        decl->symbolName = std::move(attrs.importSymbol);
        decl->symbolNameConst = std::move(attrs.importSymbolConst);
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
