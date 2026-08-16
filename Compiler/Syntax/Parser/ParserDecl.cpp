// Declaration parsing: attributes, functions, types, modules, imports.

#include "Syntax/Parser/Parser.h"

#include <algorithm>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Rux {
// Canonical string key owned by the type parser and used for intrinsic and
// extension binding names.
std::string ImplTypeName(const TypeExpr &type);

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
            EmitExpected(CurrentLocation(), "a lint rule string in '#Allow'", "use '#Allow(\"naming.type\")'");
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
std::unique_ptr<StructDecl> Parser::ParseStructDecl(bool isPublic) {
    const auto loc = CurrentLocation();
    ExpectBefore(TokenKind::StructKeyword, "'struct' to start the structure declaration");

    auto decl = std::make_unique<StructDecl>();
    decl->location = loc;
    decl->isPublic = isPublic;
    decl->name = ExpectBefore(TokenKind::Ident, "a structure name after 'struct'").text;

    if (Check(TokenKind::Less)) {
        decl->typeParams = ParseTypeParams();
    }

    if (!ConsumeBodyStart("the structure body")) {
        return decl;
    }
    while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
        StructDecl::Field field;
        field.documentation = ParseDocumentation();
        field.location = CurrentLocation();

        if (Match(TokenKind::PubKeyword)) {
            field.isPublic = true;
        }

        if (!Check(TokenKind::Ident) && !Check(TokenKind::ModuleKeyword)) {
            EmitExpected(CurrentLocation(), "a structure field name");
            while (!CheckAny({TokenKind::Semicolon, TokenKind::RightBrace}) && !IsAtEnd()) {
                Advance();
            }
            Match(TokenKind::Semicolon);
            continue;
        }

        // Keywords are contextual after a field declaration starts. This lets
        // ordinary package APIs expose members such as `#source.module`.
        field.name = Check(TokenKind::ModuleKeyword) ? Advance().text
                                                     : ExpectBefore(TokenKind::Ident, "a structure field name").text;
        ExpectBefore(TokenKind::Colon, "':' after the field name", "write fields as 'name: Type;'");
        field.type = ParseType("add the field type after ':'");
        if (field.type) {
            ExpectBefore(TokenKind::Semicolon, "';' after the structure field");
        }
        decl->fields.push_back(std::move(field));
    }
    ExpectBefore(TokenKind::RightBrace, "'}' to close the structure body");
    return decl;
}

// enum
std::unique_ptr<EnumDecl> Parser::ParseEnumDecl(const bool isPublic) {
    const auto loc = CurrentLocation();
    ExpectBefore(TokenKind::EnumKeyword, "'enum' to start the enum declaration");

    auto decl = std::make_unique<EnumDecl>();
    decl->location = loc;
    decl->isPublic = isPublic;
    decl->name = ExpectBefore(TokenKind::Ident, "an enum name after 'enum'").text;
    if (Check(TokenKind::Less)) {
        decl->typeParams = ParseTypeParams();
    }
    if (Match(TokenKind::Colon)) {
        decl->baseType = ParseType("add the enum base type after ':'");
    }

    if (!ConsumeBodyStart("the enum body")) {
        return decl;
    }
    while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
        EnumDecl::Variant variant;
        variant.documentation = ParseDocumentation();
        variant.location = CurrentLocation();
        if (!Check(TokenKind::Ident)) {
            EmitExpected(CurrentLocation(), "an enum variant name");
            while (!CheckAny({TokenKind::Comma, TokenKind::RightBrace}) && !IsAtEnd()) {
                Advance();
            }
            Match(TokenKind::Comma);
            continue;
        }
        variant.name = Advance().text;

        if (Match(TokenKind::LeftParen)) {
            while (!Check(TokenKind::RightParen) && !IsAtEnd()) {
                auto fieldType = ParseType("add the enum variant field type after '(' or ','");
                if (!fieldType) {
                    while (!CheckAny({TokenKind::Comma, TokenKind::RightParen, TokenKind::RightBrace}) && !IsAtEnd()) {
                        Advance();
                    }
                    if (Match(TokenKind::Comma)) {
                        continue;
                    }
                    break;
                }
                variant.fields.push_back(std::move(fieldType));
                if (Match(TokenKind::Comma)) {
                    continue;
                }
                if (Check(TokenKind::RightParen) || IsAtEnd()) {
                    break;
                }
                EmitExpected(CurrentLocation(), "',' between enum variant field types");
            }
            ExpectBefore(TokenKind::RightParen, "')' to close the enum variant fields");
        }
        else if (Match(TokenKind::LeftBrace)) {
            while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
                EnumDecl::Variant::NamedField field;
                field.documentation = ParseDocumentation();
                field.location = CurrentLocation();
                if (!Check(TokenKind::Ident)) {
                    EmitExpected(CurrentLocation(), "an enum variant field name");
                    while (!CheckAny({TokenKind::Semicolon, TokenKind::RightBrace}) && !IsAtEnd()) {
                        Advance();
                    }
                    Match(TokenKind::Semicolon);
                    continue;
                }
                field.name = Advance().text;
                ExpectBefore(TokenKind::Colon, "':' after the variant field name",
                             "write named variant fields as 'name: Type;'");
                field.type = ParseType("add the variant field type after ':'");
                if (field.type) {
                    ExpectBefore(TokenKind::Semicolon, "';' after the enum variant field");
                }
                variant.namedFields.push_back(std::move(field));
            }
            ExpectBefore(TokenKind::RightBrace, "'}' to close the enum variant fields");
        }

        if (Match(TokenKind::Assign)) {
            std::string value;
            if (Match(TokenKind::Minus)) {
                value = "-";
            }
            value += ExpectBefore(TokenKind::IntLiteral, "an integer enum discriminant after '='").text;
            variant.discriminant = std::move(value);
        }

        decl->variants.push_back(std::move(variant));
        if (Match(TokenKind::Comma)) {
            if (Check(TokenKind::RightBrace)) {
                EmitError(Previous().location, "trailing comma is not allowed in enum declarations");
            }
        }
        else if (Check(TokenKind::RightBrace) || IsAtEnd()) {
            break;
        }
        else {
            EmitExpected(CurrentLocation(), "',' between enum variants", "separate adjacent enum variants with ','");
        }
    }
    ExpectBefore(TokenKind::RightBrace, "'}' to close the enum body");
    return decl;
}

// union
std::unique_ptr<UnionDecl> Parser::ParseUnionDecl(bool isPublic) {
    const auto loc = CurrentLocation();
    ExpectBefore(TokenKind::UnionKeyword, "'union' to start the union declaration");

    auto decl = std::make_unique<UnionDecl>();
    decl->location = loc;
    decl->isPublic = isPublic;
    decl->name = ExpectBefore(TokenKind::Ident, "a union name after 'union'").text;

    if (!ConsumeBodyStart("the union body")) {
        return decl;
    }
    while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
        UnionDecl::Field field;
        field.documentation = ParseDocumentation();
        field.location = CurrentLocation();
        if (!Check(TokenKind::Ident)) {
            EmitExpected(CurrentLocation(), "a union field name");
            while (!CheckAny({TokenKind::Comma, TokenKind::RightBrace}) && !IsAtEnd()) {
                Advance();
            }
            Match(TokenKind::Comma);
            continue;
        }
        field.name = Advance().text;
        ExpectBefore(TokenKind::Colon, "':' after the union field name", "write fields as 'name: Type'");
        field.type = ParseType("add the union field type after ':'");
        decl->fields.push_back(std::move(field));
        if (Match(TokenKind::Comma)) {
            continue;
        }
        if (Check(TokenKind::RightBrace) || IsAtEnd()) {
            break;
        }
        EmitExpected(CurrentLocation(), "',' between union fields", "separate adjacent union fields with ','");
    }
    ExpectBefore(TokenKind::RightBrace, "'}' to close the union body");
    return decl;
}

// interface
std::unique_ptr<InterfaceDecl> Parser::ParseInterfaceDecl(bool isPublic) {
    const auto loc = CurrentLocation();
    ExpectBefore(TokenKind::InterfaceKeyword, "'interface' to start the interface declaration");

    auto decl = std::make_unique<InterfaceDecl>();
    decl->location = loc;
    decl->isPublic = isPublic;
    decl->name = ExpectBefore(TokenKind::Ident, "an interface name after 'interface'").text;

    if (!ConsumeBodyStart("the interface body")) {
        return decl;
    }
    while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
        std::string documentation = ParseDocumentation();
        if (!Check(TokenKind::FuncKeyword)) {
            EmitExpected(CurrentLocation(), "'func' to start an interface method");
            Recover();
            continue;
        }
        if (auto method = ParseFuncDecl(false, false)) {
            method->documentation = std::move(documentation);
            decl->methods.push_back(std::move(method));
        }
    }
    ExpectBefore(TokenKind::RightBrace, "'}' to close the interface body");
    return decl;
}

// extend
std::unique_ptr<ImplDecl> Parser::ParseImplDecl() {
    const auto loc = CurrentLocation();
    ExpectBefore(TokenKind::ExtendKeyword, "'extend' to start the extension declaration");

    auto decl = std::make_unique<ImplDecl>();
    decl->location = loc;

    // extend Type  or  extend Type : InterfaceName  or  extend InterfaceName
    // for Type. The leading item is parsed as a full type expression so that
    // compound receivers such as `int[]` are supported.
    TypeExprPtr firstType = ParseType("add the extended type after 'extend'");
    const std::string firstName = firstType ? ImplTypeName(*firstType) : "?";
    if (Match(TokenKind::Colon)) {
        decl->extendedType = std::move(firstType);
        decl->typeName = firstName;
        decl->interfaceName = ExpectBefore(TokenKind::Ident, "an interface name after ':' in the extension").text;
    }
    else if (Match(TokenKind::ForKeyword)) {
        decl->interfaceName = firstName;
        decl->extendedType = ParseType("add the extended type after 'for'");
        decl->typeName = decl->extendedType ? ImplTypeName(*decl->extendedType) : "?";
    }
    else {
        decl->extendedType = std::move(firstType);
        decl->typeName = firstName;
    }

    if (!ConsumeBodyStart("the extension body")) {
        return decl;
    }
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
            EmitExpected(CurrentLocation(), isIntrinsic ? "'func' after 'intrinsic' in the extension body"
                                                        : "'func' to start a method in the extension body");
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
    ExpectBefore(TokenKind::RightBrace, "'}' to close the extension body");
    return decl;
}

// module
std::unique_ptr<ModuleDecl> Parser::ParseModuleDecl(bool isPublic) {
    const auto loc = CurrentLocation();
    ExpectBefore(TokenKind::ModuleKeyword, "'module' to start the module declaration");

    std::vector<std::string> path;
    path.push_back(ExpectBefore(TokenKind::Ident, "a module name after 'module'").text);
    while (Match(TokenKind::ColonColon)) {
        path.push_back(ExpectBefore(TokenKind::Ident, "a module name after '::'").text);
    }

    if (!Match(TokenKind::LeftBrace)) {
        EmitExpected(CurrentLocation(), "'{' to start the module body");
        auto module = std::make_unique<ModuleDecl>();
        module->location = loc;
        module->isPublic = isPublic;
        module->name = std::move(path.back());
        return module;
    }
    std::vector<DeclPtr> items;
    while (!Check(TokenKind::RightBrace) && !IsAtEnd()) {
        if (auto item = ParseDecl()) {
            items.push_back(std::move(item));
        }
        else {
            Recover();
        }
    }
    ExpectBefore(TokenKind::RightBrace, "'}' to close the module body");

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
    ExpectBefore(TokenKind::ImportKeyword, "'import' to start the import declaration");

    auto decl = std::make_unique<UseDecl>();
    decl->location = loc;

    // Parse path segments separated by '.' or '::'
    decl->path.push_back(ExpectBefore(TokenKind::Ident, "a module path after 'import'").text);

    while (!IsAtEnd()) {
        if (Match(TokenKind::Dot)) {
            if (Match(TokenKind::Star)) {
                // import Rux.Primitives.*;
                decl->kind = UseDecl::Kind::Glob;
                break;
            }
            decl->path.push_back(ExpectBefore(TokenKind::Ident, "a module path segment after '.'").text);
        }
        // A lone ':' here is a mistyped '::'. Falling through to the ordinary
        // "expected ';'" would name the wrong token and leave the rest of the
        // path to fail a second time as a stray declaration, so the separator
        // is reported once and then parsed as if it had been written correctly.
        else if (Check(TokenKind::ColonColon) || Check(TokenKind::Colon)) {
            if (Check(TokenKind::Colon)) {
                const auto &next = Peek(1);
                std::string help = "separate import path segments with '::'";
                if (next.Is(TokenKind::Ident)) {
                    help += std::format(", as in '{}::{}'", decl->path.back(), next.text);
                }
                EmitExpected(CurrentLocation(), "'::' in the import path", std::move(help));
            }
            Advance();
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
                        if (!Check(TokenKind::Ident)) {
                            EmitExpected(CurrentLocation(), "an imported name");
                            while (!CheckAny({TokenKind::Comma, TokenKind::RightBrace}) && !IsAtEnd()) {
                                Advance();
                            }
                            if (Match(TokenKind::Comma)) {
                                continue;
                            }
                            break;
                        }
                        decl->names.push_back(Advance().text);
                    }
                    if (Match(TokenKind::Comma)) {
                        continue;
                    }
                    if (Check(TokenKind::RightBrace) || IsAtEnd()) {
                        break;
                    }
                    EmitExpected(CurrentLocation(), "',' between imported names",
                                 "separate adjacent imported names with ','");
                }
                ExpectBefore(TokenKind::RightBrace, "'}' to close the imported name list");
                break;
            }
            if (Match(TokenKind::Star)) {
                decl->kind = UseDecl::Kind::Glob;
                break;
            }
            decl->path.push_back(ExpectBefore(TokenKind::Ident, "a module path segment after '::'").text);
        }
        else {
            break;
        }
    }

    // As a compile-time match arm body (`.Linux => import Linux::{...}`) the
    // trailing ';' is optional, since the arm is delimited by the next pattern.
    if (requireSemicolon) {
        ExpectBefore(TokenKind::Semicolon, "';' after the import declaration");
    }
    else {
        Match(TokenKind::Semicolon);
    }
    return decl;
}

// const
std::unique_ptr<ConstDecl> Parser::ParseConstDecl(bool isPublic) {
    const auto loc = CurrentLocation();
    ExpectBefore(TokenKind::ConstKeyword, "'const' to start the constant declaration");

    auto decl = std::make_unique<ConstDecl>();
    decl->location = loc;
    decl->isPublic = isPublic;
    decl->name = ExpectBefore(TokenKind::Ident, "a constant name after 'const'").text;

    if (Match(TokenKind::Colon)) {
        decl->type = ParseType("add the constant type after ':'");
    }

    ExpectBefore(TokenKind::Assign, "'=' before the constant value");
    decl->value = ParseRequiredExpr("after '=' in the constant declaration");

    ExpectBefore(TokenKind::Semicolon, "';' after the constant declaration");
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

    auto parseCondition = [&](const std::string_view context) {
        structInitAllowed = false;
        auto condition = ParseRequiredExpr(context);
        structInitAllowed = true;
        return condition;
    };

    WhenDecl::Branch first;
    first.location = loc;
    first.condition = parseCondition("after 'when'");

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
            branch.condition = parseCondition("after 'else when'");
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
            branch.patterns.push_back(ParseRequiredExpr("at the start of the 'when' arm"));
            // Commas before `=>` separate patterns that share this arm's body.
            while (Match(TokenKind::Comma) && !Check(TokenKind::FatArrow)) {
                branch.patterns.push_back(ParseRequiredExpr("after ',' in the 'when' arm pattern list"));
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
    ExpectBefore(TokenKind::TypeKeyword, "'type' to start the type alias declaration");

    auto decl = std::make_unique<TypeAliasDecl>();
    decl->location = loc;
    decl->isPublic = isPublic;
    decl->name = ExpectBefore(TokenKind::Ident, "a type alias name after 'type'").text;

    ExpectBefore(TokenKind::Assign, "'=' after the type alias name", "write type aliases as 'type Name = Type;'");
    decl->type = ParseType("add the aliased type after '='");

    ExpectBefore(TokenKind::Semicolon, "';' after the type alias declaration");
    return decl;
}

// extern
DeclPtr Parser::ParseExternDecl(bool isPublic, ParsedAttrs &attrs) {
    const auto loc = CurrentLocation();
    ExpectBefore(TokenKind::ExternKeyword, "'extern' to start the external declaration");

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
                fd->name = ExpectBefore(TokenKind::Ident, "a function name after 'func'").text;
                if (Match(TokenKind::LeftParen)) {
                    fd->params = ParseParamList(true);
                    if (!fd->params.empty() && fd->params.back().name == "...") {
                        fd->isVariadic = true;
                        fd->params.pop_back();
                    }
                    ExpectBefore(TokenKind::RightParen, "')' to close the external function parameter list");
                }
                else {
                    EmitExpected(CurrentLocation(), "'(' after the external function name");
                }
                if (Match(TokenKind::Arrow)) {
                    fd->returnType = ParseType("add the external function return type after '->'");
                }
                ExpectBefore(TokenKind::Semicolon, "';' after the external function declaration");
                block->items.push_back(std::move(fd));
            }
            else if (Check(TokenKind::Ident)) {
                auto vd = std::make_unique<ExternVarDecl>();
                vd->location = CurrentLocation();
                vd->isPublic = isPublic;
                vd->name = Advance().text;
                ExpectBefore(TokenKind::Colon, "':' after the external variable name",
                             "write external variables as 'Name: Type;'");
                vd->type = ParseType("add the external variable type after ':'");
                ExpectBefore(TokenKind::Semicolon, "';' after the external variable declaration");
                block->items.push_back(std::move(vd));
            }
            else {
                EmitExpected(CurrentLocation(), "'func' or a variable declaration in the external block");
                Recover();
            }
        }
        ExpectBefore(TokenKind::RightBrace, "'}' to close the external block");
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
        decl->name = ExpectBefore(TokenKind::Ident, "a function name after 'extern func'").text;

        if (Match(TokenKind::LeftParen)) {
            decl->params = ParseParamList(true);
            if (!decl->params.empty() && decl->params.back().name == "...") {
                decl->isVariadic = true;
                decl->params.pop_back();
            }
            ExpectBefore(TokenKind::RightParen, "')' to close the external function parameter list");
        }
        else {
            EmitExpected(CurrentLocation(), "'(' after the external function name");
        }

        if (Match(TokenKind::Arrow)) {
            decl->returnType = ParseType("add the external function return type after '->'");
        }

        ExpectBefore(TokenKind::Semicolon, "';' after the external function declaration");
        return decl;
    }

    if (!Check(TokenKind::Ident)) {
        EmitExpected(CurrentLocation(), "a function, variable name, or '{' after 'extern'",
                     "write 'extern func Name(...);', 'extern Name: Type;', or 'extern { ... }'");
        Recover();
        return nullptr;
    }

    // extern Name: Type;
    auto decl = std::make_unique<ExternVarDecl>();
    decl->location = loc;
    decl->isPublic = isPublic;
    decl->name = Advance().text;

    ExpectBefore(TokenKind::Colon, "':' after the external variable name",
                 "write external variables as 'extern Name: Type;'");
    decl->type = ParseType("add the external variable type after ':'");

    ExpectBefore(TokenKind::Semicolon, "';' after the external variable declaration");
    return decl;
}
} // namespace Rux
