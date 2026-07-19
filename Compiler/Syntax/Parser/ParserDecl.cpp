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
// Inside an asm body, any identifier-like token — a plain identifier or a
// language keyword such as `loop` or `for` — may name a mnemonic, register,
// symbol or label. The lexer has already classified keywords, so recover their
// identifier role here.
static bool IsAsmNameToken(const Token &t) {
    return t.Is(TokenKind::Ident) || t.IsKeyword();
}

static bool IsZeroOperandAsmMnemonic(const std::string_view mnemonic) {
    return mnemonic == "ret" || mnemonic == "leave" || mnemonic == "nop" || mnemonic == "syscall" ||
           mnemonic == "cqo" || mnemonic == "cdq" || mnemonic == "cdqe";
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
                               ? "the '#Intrinsic' attribute has been removed; write 'intrinsic const name: Type;' "
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
    if (Check(TokenKind::ConstKeyword)) {
        return ApplyAttrs(ParseConstDecl(isPublic, true), attrs);
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
    EmitError(intrinsicLoc, "'intrinsic' can only be applied to a constant or function");
    Recover();
    return nullptr;
}

// Top-level declarations
DeclPtr Parser::ParseDecl() {
    const auto loc = CurrentLocation();

    // Conditional compilation.
    if (Check(TokenKind::WhenKeyword)) {
        return ParseWhenDecl();
    }
    // The forms `when` replaced. Both are diagnosed here rather than left to the
    // attribute parser, which would only report that '#' wants a name.
    if (Check(TokenKind::Hash) && Peek(1).Is(TokenKind::IfKeyword)) {
        EmitError(loc, "'#if' is no longer conditional compilation; write 'when <condition> { ... }'");
        Advance(); // '#'
        Advance(); // 'if'
        // Parse it as the `when` it meant, so the chain reports only its spelling.
        return ParseWhenBody(loc);
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
        return ParseDecl();
    }

    // The form `intrinsic` replaced. Caught before ParseAttrs, which would
    // otherwise consume it as an attribute and leave the constant that follows
    // looking like an ordinary one that forgot its value. Recovered as the
    // keyword it meant, so it reports only its spelling.
    if (Check(TokenKind::Hash) && Peek(1).Is(TokenKind::Ident) && Peek(1).text == "Intrinsic") {
        EmitError(loc, "the '#Intrinsic' attribute has been removed; write 'intrinsic const name: Type;' "
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
        return ParseIntrinsicDecl(pub, rest, loc);
    }

    ParsedAttrs attrs = ParseAttrs();

    bool isPublic = false;
    if (Match(TokenKind::PubKeyword)) {
        isPublic = true;
    }

    // intrinsic const/func: the compiler supplies the value or the body. The
    // declaration itself names the intrinsic, so there is nothing to write twice.
    if (Match(TokenKind::IntrinsicKeyword)) {
        return ParseIntrinsicDecl(isPublic, attrs, Previous().location);
    }

    // asm func
    if (Check(TokenKind::Ident) && Peek().text == "asm" && Peek(1).Is(TokenKind::FuncKeyword)) {
        Advance(); // consume 'asm'
        return ApplyAttrs(ParseFuncDecl(isPublic, true, attrs.callConv), attrs);
    }

    if (Check(TokenKind::FuncKeyword)) {
        return ApplyAttrs(ParseFuncDecl(isPublic, false, attrs.callConv), attrs);
    }
    if (Check(TokenKind::StructKeyword)) {
        return ApplyAttrs(ParseStructDecl(isPublic), attrs);
    }
    if (Check(TokenKind::EnumKeyword)) {
        return ApplyAttrs(ParseEnumDecl(isPublic), attrs);
    }
    if (Check(TokenKind::UnionKeyword)) {
        return ApplyAttrs(ParseUnionDecl(isPublic), attrs);
    }
    if (Check(TokenKind::InterfaceKeyword)) {
        return ApplyAttrs(ParseInterfaceDecl(isPublic), attrs);
    }
    if (Check(TokenKind::ExtendKeyword)) {
        return ApplyAttrs(ParseImplDecl(), attrs);
    }
    if (Check(TokenKind::ModuleKeyword)) {
        return ApplyAttrs(ParseModuleDecl(isPublic), attrs);
    }
    if (Check(TokenKind::ImportKeyword)) {
        return ApplyAttrs(ParseUseDecl(), attrs);
    }
    if (Check(TokenKind::ConstKeyword)) {
        return ApplyAttrs(ParseConstDecl(isPublic), attrs);
    }
    if (Check(TokenKind::TypeKeyword)) {
        return ApplyAttrs(ParseTypeAliasDecl(isPublic), attrs);
    }
    if (Check(TokenKind::ExternKeyword)) {
        return ApplyAttrs(ParseExternDecl(isPublic, attrs), attrs);
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
        instr.location = CurrentLocation();
        instr.mnemonic = Advance().text;
        for (char &c : instr.mnemonic) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        // Operands, comma-separated. Stop when the operand is not followed
        // by a comma (i.e. the next token starts a new instruction).
        if (IsZeroOperandAsmMnemonic(instr.mnemonic) || (!Check(TokenKind::RightBrace) && !CanStartAsmOperand())) {
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
    default:
        // An identifier-like token begins a new instruction if it is itself a
        // label definition (`name :`); otherwise it is a register / symbol.
        return IsAsmNameToken(Peek()) && !Peek(1).Is(TokenKind::Colon);
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

    if (CheckAny({TokenKind::IntLiteral, TokenKind::Minus, TokenKind::Plus})) {
        op.kind = AsmOperand::Kind::Imm;
        op.imm = ParseAsmInt();
        return op;
    }

    // An identifier-like token: either a register or a symbol / label reference.
    if (!IsAsmNameToken(Peek())) {
        EmitError(CurrentLocation(), std::format("expected an assembly operand, found '{}'", Peek().text));
        return op;
    }
    const Token &tok = Advance();
    std::string name = tok.text;
    std::string lowered = name;
    for (char &c : lowered) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (IsX64RegisterName(lowered)) {
        op.kind = AsmOperand::Kind::Reg;
        op.name = std::move(lowered);
    }
    else {
        op.kind = AsmOperand::Kind::Sym;
        op.name = std::move(name);
    }
    return op;
}

// Parse a memory operand `[base + index*scale +/- disp]`. Any of base, index
// and displacement may be omitted.
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
        if (Check(TokenKind::IntLiteral)) {
            std::int64_t v = ParseAsmInt();
            op.imm += negateNext ? -v : v;
            negateNext = false;
            continue;
        }
        if (IsAsmNameToken(Peek())) {
            std::string name = Advance().text;
            std::string lowered = name;
            for (char &c : lowered) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            // Scaled index: reg * scale.
            if (Match(TokenKind::Star)) {
                op.memIndex = std::move(lowered);
                op.memScale = static_cast<int>(ParseAsmInt());
            }
            else if (lowered == "rip") {
                op.memBase = "rip";
            }
            else if (IsX64RegisterName(lowered)) {
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
        field.location = CurrentLocation();

        if (Match(TokenKind::PubKeyword)) {
            field.isPublic = true;
        }

        // Keywords are contextual after a field declaration starts. This lets
        // ordinary package APIs expose members such as `CurrentSource.module`.
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

// Language aliases that ResolveType normalizes; mirror them here so the
// canonical key produced from the type expression matches the resolved
// receiver type's spelling (e.g. `bool[]` and a `bool8[]` receiver agree).
static std::string NormalizePrimitiveName(const std::string &name) {
    if (name == "bool") {
        return "bool8";
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
// (generic-agnostic, matching struct behaviour); slices become `Slice<Elem>` to
// match the internal slice type spelling used for method lookup.
static std::string ImplTypeName(const TypeExpr &type) {
    if (const auto *n = dynamic_cast<const NamedTypeExpr *>(&type)) {
        return NormalizePrimitiveName(n->name);
    }
    if (const auto *s = dynamic_cast<const SliceTypeExpr *>(&type)) {
        return "Slice<" + ImplTypeName(*s->element) + ">";
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
        // Methods can be conditionally compiled like any other declaration.
        // Conditional compilation later moves those of the taken branch into
        // `methods`.
        if (Check(TokenKind::WhenKeyword)) {
            if (auto conditional = ParseWhenDecl()) {
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
std::unique_ptr<UseDecl> Parser::ParseUseDecl() {
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
                    decl->names.push_back(Expect(TokenKind::Ident, "expected name").text);
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

    Expect(TokenKind::Semicolon, "expected ';'");
    return decl;
}

// const
std::unique_ptr<ConstDecl> Parser::ParseConstDecl(bool isPublic, bool isIntrinsic) {
    const auto loc = CurrentLocation();
    Expect(TokenKind::ConstKeyword, "expected 'const'");

    auto decl = std::make_unique<ConstDecl>();
    decl->location = loc;
    decl->isPublic = isPublic;
    decl->name = Expect(TokenKind::Ident, "expected constant name").text;

    if (Match(TokenKind::Colon)) {
        decl->type = ParseType();
    }

    if (isIntrinsic) {
        // The type names the intrinsic: `intrinsic const CurrentTarget: Target;` binds
        // the compiler's `Target`. The constant is only a name for it, so it can
        // be renamed without rebinding.
        if (decl->type) {
            decl->intrinsicName = ImplTypeName(**decl->type);
        }
        else {
            EmitError(decl->location, "'intrinsic' constant requires an explicit type, which names the intrinsic");
        }
        if (Match(TokenKind::Assign)) {
            EmitError(decl->location, "'intrinsic' constant cannot have a source initializer");
            decl->value = ParseExpr();
        }
    }
    else {
        Expect(TokenKind::Assign, "expected '='");
        decl->value = ParseExpr();
    }

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
