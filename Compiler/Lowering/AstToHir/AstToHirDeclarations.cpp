// Declaration lowering, and the literal decoding it needs: character and
// string escapes are resolved here so HIR carries values rather than syntax.

#include "Ir/Hir/HirInternal.h"
#include "Lowering/AstToHir/Detail/AstToHirContext.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Rux::AstToHirDetail {
std::uint32_t AstToHirContext::DecodeUtf8CodePoint(const std::string &text, std::size_t i) {
    const auto byte = [&](std::size_t offset) {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(text[i + offset]));
    };

    const std::uint32_t b0 = byte(0);
    if ((b0 & 0x80u) == 0) {
        return b0;
    }
    if ((b0 & 0xE0u) == 0xC0u && i + 1 < text.size()) {
        return ((b0 & 0x1Fu) << 6) | (byte(1) & 0x3Fu);
    }
    if ((b0 & 0xF0u) == 0xE0u && i + 2 < text.size()) {
        return ((b0 & 0x0Fu) << 12) | ((byte(1) & 0x3Fu) << 6) | (byte(2) & 0x3Fu);
    }
    if ((b0 & 0xF8u) == 0xF0u && i + 3 < text.size()) {
        return ((b0 & 0x07u) << 18) | ((byte(1) & 0x3Fu) << 12) | ((byte(2) & 0x3Fu) << 6) | (byte(3) & 0x3Fu);
    }
    return b0;
}

void AstToHirContext::AppendUtf8(std::string &out, std::uint32_t cp) {
    if (cp <= 0x7F) {
        out += static_cast<char>(cp);
    }
    else if (cp <= 0x7FF) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else if (cp <= 0xFFFF) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::size_t AstToHirContext::ParseUnicodeEscape(const std::string &text, std::size_t uPos, std::uint32_t &cp) {
    std::size_t j = uPos + 1;
    if (j >= text.size() || text[j] != '{') {
        return uPos;
    }
    ++j;
    std::uint32_t value = 0;
    std::size_t digits = 0;
    for (; j < text.size() && text[j] != '}'; ++j, ++digits) {
        const char h = text[j];
        if (h >= '0' && h <= '9') {
            value = (value << 4) | static_cast<std::uint32_t>(h - '0');
        }
        else if (h >= 'a' && h <= 'f') {
            value = (value << 4) | static_cast<std::uint32_t>(h - 'a' + 10);
        }
        else if (h >= 'A' && h <= 'F') {
            value = (value << 4) | static_cast<std::uint32_t>(h - 'A' + 10);
        }
        else {
            return uPos;
        }
    }
    if (digits == 0 || j >= text.size() || text[j] != '}') {
        return uPos;
    }
    cp = value;
    return j;
}

std::string AstToHirContext::DecodeCharLiteral(const std::string &text) {
    // text is raw source like 'A' or '\n'; strip quotes and decode.
    std::uint32_t cp = 0;
    const std::size_t quote = text.find('\'');
    if (quote != std::string::npos && quote + 1 < text.size()) {
        std::size_t i = quote + 1; // skip opening '
        if (text[i] == '\\' && i + 1 < text.size()) {
            switch (text[i + 1]) {
            case 'n':
                cp = '\n';
                break;
            case 't':
                cp = '\t';
                break;
            case 'r':
                cp = '\r';
                break;
            case 'a':
                cp = '\a';
                break;
            case 'b':
                cp = '\b';
                break;
            case 'f':
                cp = '\f';
                break;
            case 'v':
                cp = '\v';
                break;
            case '0':
                cp = 0;
                break;
            case '\\':
                cp = '\\';
                break;
            case '\'':
                cp = '\'';
                break;
            case '"':
                cp = '"';
                break;
            case 'u': {
                // \u{XXXX} — Unicode escape ('u' sits at i + 1)
                std::uint32_t u = 0;
                if (ParseUnicodeEscape(text, i + 1, u) != i + 1) {
                    cp = u;
                }
                break;
            }
            default:
                cp = static_cast<unsigned char>(text[i + 1]);
                break;
            }
        }
        else if (text[i] != '\'') {
            cp = DecodeUtf8CodePoint(text, i);
        }
    }
    return std::to_string(cp);
}

std::string AstToHirContext::DecodeStringLiteral(const std::string &text) {
    // text is raw source like "hello\n" — strip quotes and decode
    // escapes
    std::string out;
    if (text.size() < 2) {
        return out;
    }
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
        case 'u': {
            // \u{XXXX} — Unicode escape, encoded as UTF-8 ('u' sits at
            // i)
            std::uint32_t u = 0;
            if (const std::size_t end = ParseUnicodeEscape(text, i, u); end != i) {
                AppendUtf8(out, u);
                i = end; // the loop's ++i then steps past the closing
                // '}'
            }
            break;
        }
        default:
            break;
        }
    }
    return out;
}

TypeRef AstToHirContext::LiteralType(const Token &tok) const {
    switch (tok.kind) {
    case TokenKind::IntLiteral:
    case TokenKind::FloatLiteral:
        return SuffixedLiteralType(tok);
    case TokenKind::StringLiteral:
        return StringLiteralType(tok);
    case TokenKind::CharLiteral:
        return CharLiteralType(tok);
    case TokenKind::BoolLiteral:
        return TypeRef::MakeBool();
    default:
        return TypeRef::MakeUnknown();
    }
}

std::vector<HirParam> AstToHirContext::LowerParams(const std::vector<Param> &params, const bool skipReceiver) {
    std::vector<HirParam> out;
    out.reserve(params.size());
    for (const auto &p : params) {
        if (skipReceiver && p.IsReceiver()) {
            continue;
        }
        HirParam hp;
        hp.name = p.name;
        hp.isVariadic = p.isVariadic;
        hp.type = p.isVariadic ? TypeRef::MakeNamed(SliceTypeName(ResolveType(*p.type))) : ResolveType(*p.type);
        if (const HirSymbol *symbol = currentScope->Lookup(hp.name)) {
            hp.bindingId = symbol->bindingId;
        }
        out.push_back(std::move(hp));
    }
    return out;
}

HirFunc AstToHirContext::LowerFunc(const FuncDecl &d, bool isMethod,
                                   const std::unordered_map<std::string, TypeRef> &substitutions,
                                   const std::string &overrideName) {
    auto savedTypeParams = currentTypeParams;
    currentTypeParams = substitutions.empty() ? TypeParameterNames(d.typeParams) : std::vector<std::string>{};
    auto savedSubstitutions = currentSubstitutions;
    currentSubstitutions = substitutions;
    // The receiver's declared type is what `self` is throughout the body, so it has to be in place before the name, the
    // return type or any parameter is resolved. A bare `self` carries no type and keeps the extend block's.
    const TypeRef savedSelfType = currentSelfType;
    if (const Param *receiver = d.Receiver(); receiver && !dynamic_cast<const SelfTypeExpr *>(receiver->type.get())) {
        currentSelfType = ResolveType(*receiver->type);
    }
    TypeRef retType = d.returnType ? ResolveType(**d.returnType) : TypeRef::MakeOpaque();
    auto savedRet = currentReturnType;
    currentReturnType = retType;
    auto savedFuncName = currentFunctionName;
    if (isMethod) {
        currentFunctionName = NamedBaseTypeName(currentSelfType) + "::" + d.name;
    }
    else {
        currentFunctionName = declModulePath.empty() ? d.name : declModulePath + "::" + d.name;
    }
    PushScope();
    const CleanupPlanner::FunctionToken cleanupFunction = cleanupPlanner.BeginFunction();
    if (substitutions.empty()) {
        for (const auto &tp : d.typeParams) {
            HirSymbol sym;
            sym.kind = HirSymbol::Kind::Type;
            sym.name = tp.name;
            sym.type = TypeRef::MakeTypeParam(tp.name);
            Define(sym);
        }
    }
    if (isMethod) {
        HirSymbol self;
        self.kind = HirSymbol::Kind::Var;
        self.name = "self";
        self.type = currentSelfType.IsUnknown() ? TypeRef::MakeNamed("self") : currentSelfType;
        self.isMut = true;
        self.bindingId = RegisterCleanupBinding(self.name, self.type, d.location);
        Define(self);
    }
    for (const auto &param : d.params) {
        if (param.IsReceiver()) {
            continue;
        }
        HirSymbol sym;
        sym.kind = HirSymbol::Kind::Var;
        sym.name = param.name;
        sym.type =
            param.isVariadic ? TypeRef::MakeNamed(SliceTypeName(ResolveType(*param.type))) : ResolveType(*param.type);
        sym.isMut = param.isMut;
        sym.bindingId = RegisterCleanupBinding(sym.name, sym.type, param.location);
        Define(sym);
    }
    std::optional<HirBlock> body;
    if (d.body) {
        body = LowerBlock(*d.body);
        AppendCurrentScopeCleanups(*body);
    }
    HirFunc hf;
    hf.name = overrideName.empty() ? d.name : overrideName;
    hf.isPublic = d.isPublic;
    hf.isAsm = d.isAsm;
    hf.isNoReturn = d.isNoReturn;
    hf.asmBody = d.asmBody;
    hf.callConv = d.callConv;
    hf.typeParams = substitutions.empty() ? TypeParameterNames(d.typeParams) : std::vector<std::string>{};
    hf.params = LowerParams(d.params);
    hf.returnType = retType;
    hf.body = std::move(body);
    hf.location = d.location;

    cleanupPlanner.EndFunction(cleanupFunction);
    PopScope();
    currentSelfType = savedSelfType;
    currentReturnType = savedRet;
    currentTypeParams = savedTypeParams;
    currentSubstitutions = savedSubstitutions;
    currentFunctionName = savedFuncName;
    return hf;
}

HirStruct AstToHirContext::LowerStruct(const StructDecl &d) {
    auto savedTypeParams = currentTypeParams;
    currentTypeParams = TypeParameterNames(d.typeParams);
    PushScope();
    for (const auto &tp : d.typeParams) {
        HirSymbol sym;
        sym.kind = HirSymbol::Kind::Type;
        sym.name = tp.name;
        sym.type = TypeRef::MakeTypeParam(tp.name);
        Define(sym);
    }
    HirStruct hs;
    hs.name = d.name;
    hs.isPublic = d.isPublic;
    hs.typeParams = TypeParameterNames(d.typeParams);
    hs.location = d.location;
    for (const auto &f : d.fields) {
        HirStructField hf;
        hf.name = f.name;
        hf.isPublic = f.isPublic;
        hf.type = ResolveType(*f.type);
        hs.fields.push_back(std::move(hf));
    }
    PopScope();
    currentTypeParams = savedTypeParams;
    return hs;
}

void AstToHirContext::NoteStructInstantiation(const TypeRef &type) const {
    for (const auto &inner : type.inner) {
        NoteStructInstantiation(inner);
    }
    if (type.kind != TypeRef::Kind::Named || type.name.find('<') == std::string::npos) {
        return;
    }
    const std::string base = BaseTypeNameImpl(type.name);
    // The fat pointers below have a shape the runtime fixes rather than a declaration, so an instantiated declaration
    // would restate what the back ends already know and could only disagree with it.
    if (base == "Slice" || base == "StringArray" || base == "SystemTime") {
        return;
    }
    const auto declaration = structDecls.find(base);
    if (declaration == structDecls.end() || declaration->second->typeParams.empty()) {
        return;
    }
    const std::vector<TypeRef> args = ParseTypeArgsFromTypeName(type.name);
    if (args.size() != declaration->second->typeParams.size()) {
        return;
    }
    // A type argument naming a type parameter still in scope is the generic declaration itself, not an instantiation of
    // it; its layout is only knowable once that parameter has been substituted.
    for (const auto &arg : args) {
        if (arg.kind == TypeRef::Kind::TypeParam) {
            return;
        }
        if (arg.kind == TypeRef::Kind::Named &&
            std::find(currentTypeParams.begin(), currentTypeParams.end(), arg.name) != currentTypeParams.end()) {
            return;
        }
        NoteStructInstantiation(arg);
    }
    if (seenStructInstantiations.insert(type.name).second) {
        pendingStructInstantiations.push_back(type.name);
    }
}

HirStruct AstToHirContext::LowerStructInstantiation(const StructDecl &d, const std::string &name,
                                                    const std::vector<TypeRef> &typeArgs) {
    auto savedSubstitutions = currentSubstitutions;
    currentSubstitutions.clear();
    for (std::size_t i = 0; i < d.typeParams.size() && i < typeArgs.size(); ++i) {
        currentSubstitutions.insert_or_assign(d.typeParams[i].name, typeArgs[i]);
    }
    HirStruct instantiation = LowerStruct(d);
    currentSubstitutions = std::move(savedSubstitutions);

    // The instantiation stands beside the generic declaration under its own name, with nothing left to substitute.
    instantiation.name = name;
    instantiation.typeParams.clear();
    return instantiation;
}

HirEnum AstToHirContext::LowerEnum(const EnumDecl &d) {
    const auto savedTypeParams = currentTypeParams;
    AppendTypeParameterNames(currentTypeParams, d.typeParams);
    HirEnum he;
    he.name = d.name;
    he.isPublic = d.isPublic;
    he.typeParams = TypeParameterNames(d.typeParams);
    he.baseType = EnumBaseType(d);
    he.location = d.location;
    std::int64_t next = 0;
    for (const auto &v : d.variants) {
        HirEnumVariant hv;
        hv.name = v.name;
        std::int64_t value = next;
        if (v.discriminant) {
            if (const auto parsed = ParseEnumDiscriminant(*v.discriminant)) {
                value = *parsed;
            }
        }
        hv.discriminant = std::to_string(value);
        next = value + 1;
        for (const auto &f : v.fields) {
            hv.fields.push_back(ResolveType(*f));
        }
        for (const auto &f : v.namedFields) {
            hv.fields.push_back(ResolveType(*f.type));
        }
        he.variants.push_back(std::move(hv));
    }
    currentTypeParams = savedTypeParams;
    return he;
}

HirUnion AstToHirContext::LowerUnion(const UnionDecl &d) {
    HirUnion hu;
    hu.name = d.name;
    hu.isPublic = d.isPublic;
    hu.location = d.location;
    for (const auto &f : d.fields) {
        HirUnionField hf;
        hf.name = f.name;
        hf.type = ResolveType(*f.type);
        hu.fields.push_back(std::move(hf));
    }
    return hu;
}

HirInterface AstToHirContext::LowerInterface(const InterfaceDecl &d) {
    HirInterface hi;
    hi.name = d.name;
    hi.isPublic = d.isPublic;
    hi.location = d.location;
    for (const auto &m : d.methods) {
        HirInterfaceMethod hm;
        hm.name = m->name;
        hm.location = m->location;
        hm.returnType = m->returnType ? ResolveType(**m->returnType) : TypeRef::MakeOpaque();
        // A vtable slot is reached through the interface value, whose data half is the receiver, so the slot's written
        // parameters are the ones after it.
        hm.params = LowerParams(m->params, /*skipReceiver=*/true);
        hi.methods.push_back(std::move(hm));
    }
    return hi;
}

HirImplBlock AstToHirContext::LowerImpl(const ImplDecl &d) {
    bool savedInImpl = inImpl;
    TypeRef savedSelfType = currentSelfType;
    inImpl = true;
    TypeRef extendedType = d.extendedType ? ResolveType(*d.extendedType) : TypeRef::MakeUnknown();
    const bool isSliceReceiver = extendedType.kind == TypeRef::Kind::Array ||
                                 (extendedType.kind == TypeRef::Kind::Named && extendedType.name.starts_with("Slice<"));
    if (isSliceReceiver) {
        // `self` is the slice value; the slice ABI passes its address, so
        // slice indexing and iteration inside the method work as usual.
        currentSelfType = extendedType;
    }
    else {
        TypeRef selfBase = extendedType.IsUnknown() ? TypeRef::MakeNamed(d.typeName) : extendedType;
        currentSelfType = TypeRef::MakePointer(selfBase);
    }

    HirImplBlock hib;
    hib.typeName = d.typeName.starts_with("Slice<") ? d.typeName : BaseTypeName(d.typeName);
    hib.interfaceName = d.interfaceName;
    hib.location = d.location;
    for (const auto &m : d.methods) {
        if (!m->intrinsicName.empty() && !m->body && !m->isAsm) {
            continue;
        }
        HirFunc hf = LowerFunc(*m, /*isMethod=*/true);
        const auto *identity = model.TryGetSymbolIdentity(*m);
        assert(identity && "method declaration is missing its semantic symbol identity");
        if (!identity) {
            continue;
        }
        const std::string prefix = hib.typeName + "::";
        assert(identity->linkerName.starts_with(prefix));
        hf.name = identity->linkerName.substr(prefix.size());
        hib.methodLinkerNames.push_back(identity->linkerName);
        hib.methods.push_back(std::move(hf));
    }
    if (const auto *identity = model.TryGetVtableIdentity(d)) {
        hib.vtableLabel = identity->linkerName;
        hib.vtableEntries = identity->entries;
    }

    currentSelfType = savedSelfType;
    inImpl = savedInImpl;
    return hib;
}

HirConst AstToHirContext::LowerConst(const ConstDecl &d) {
    HirConst hc;
    hc.name = d.name;
    hc.isPublic = d.isPublic;
    const std::optional<TypeRef> explicitType =
        d.type ? std::optional<TypeRef>(ResolveType(*d.type->get())) : std::nullopt;
    hc.value = explicitType ? LowerExprAs(*d.value, *explicitType) : LowerExpr(*d.value);
    hc.type = explicitType ? *explicitType : hc.value->type;
    if (HirSymbol *sym = currentScope->Lookup(d.name)) {
        sym->type = hc.type;
    }
    hc.location = d.location;
    RegisterConstInteger(hc.name, *hc.value);
    return hc;
}

HirExternFunc AstToHirContext::LowerExternFunc(const ExternFuncDecl &d) {
    HirExternFunc hef;
    hef.name = d.name;
    hef.dll = d.dll;
    if (const auto *identity = model.TryGetSymbolIdentity(d); identity && identity->linkerName != d.name) {
        hef.symbolName = identity->linkerName;
    }
    hef.isPublic = d.isPublic;
    hef.isNoReturn = d.isNoReturn;
    hef.callConv = d.callConv;
    hef.isVariadic = d.isVariadic;
    hef.returnType = d.returnType ? ResolveType(**d.returnType) : TypeRef::MakeOpaque();
    hef.params = LowerParams(d.params);
    hef.location = d.location;
    return hef;
}

HirExternVar AstToHirContext::LowerExternVar(const ExternVarDecl &d) {
    HirExternVar hev;
    hev.name = d.name;
    hev.isPublic = d.isPublic;
    hev.type = ResolveType(*d.type);
    hev.location = d.location;
    return hev;
}

HirTypeAlias AstToHirContext::LowerTypeAlias(const TypeAliasDecl &d) {
    HirTypeAlias hta;
    hta.name = d.name;
    hta.isPublic = d.isPublic;
    hta.type = ResolveType(*d.type);
    hta.location = d.location;
    return hta;
}

std::string AstToHirContext::LowerLiteralValue(const LiteralExpr &expression) const {
    if (expression.token.kind == TokenKind::CharLiteral) {
        return DecodeCharLiteral(expression.token.text);
    }
    if (expression.token.kind == TokenKind::StringLiteral) {
        return DecodeStringLiteral(expression.token.text);
    }
    if (expression.token.kind == TokenKind::IntLiteral || expression.token.kind == TokenKind::FloatLiteral) {
        return StripNumericLiteralSuffix(expression.token.text);
    }
    return expression.token.text;
}

} // namespace Rux::AstToHirDetail
