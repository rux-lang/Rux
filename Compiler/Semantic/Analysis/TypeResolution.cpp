#include "Lexer/Lexer.h"
#include "Numeric/IntegerLiteral.h"
#include "Semantic/Analysis/AnalysisContext.h"
#include "Semantic/Conditional/ConditionalCompilation.h"
#include "Target/Layout.h"
#include "Target/Target.h"
#include "Types/PrimitiveCatalog.h"
#include "Types/Type.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <charconv>
#include <format>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace Rux::SemanticDetail {
using Layout::AlignUp;

// Type resolution
std::string AnalysisContext::GenericTypeName(const NamedTypeExpr &type) {
    std::vector<TypeRef> typeArgs;
    typeArgs.reserve(type.typeArgs.size());
    for (const auto &typeArg : type.typeArgs) {
        typeArgs.push_back(ResolveType(*typeArg));
    }
    return TypeRef::InstantiationName(type.name, typeArgs);
}

std::string AnalysisContext::BaseTypeName(const std::string &name) const {
    const std::size_t pos = name.find('<');
    return pos == std::string::npos ? name : name.substr(0, pos);
}

/// The type parameters `name` declares, as seen from the file being checked.
///
/// The file comes first because the struct AnalysisContext::and enum indexes are keyed by bare name across every
/// package: a program declaring its own `Option` displaces `Core`'s, AnalysisContext::and `Core`'s own `extend
/// Option<T>` would then read the wrong arity AnalysisContext::and reject its own parameter. An `extend` block is
/// written beside the type it extends, so the declaration in the same file is the one it means.
const std::vector<TypeParameter> *AnalysisContext::AggregateTypeParams(const std::string &name) const {
    if (const std::vector<TypeParameter> *local = programIndex.TypeParamsIn(currentFile, name)) {
        return local;
    }
    if (const auto structure = structDecls.find(name); structure != structDecls.end()) {
        return &structure->second->typeParams;
    }
    if (const auto enumeration = enumDecls.find(name); enumeration != enumDecls.end()) {
        return &enumeration->second->typeParams;
    }
    return nullptr;
}

std::vector<std::string> AnalysisContext::ImplTypeParams(const ImplDecl &decl) const {
    std::vector<std::string> params;
    const auto *target = dynamic_cast<const NamedTypeExpr *>(decl.extendedType.get());
    if (!target) {
        return params;
    }
    const std::vector<TypeParameter> *typeParams = AggregateTypeParams(target->name);
    if (!typeParams) {
        return params;
    }

    const std::size_t count = std::min(typeParams->size(), target->typeArgs.size());
    for (std::size_t i = 0; i < count; ++i) {
        const auto *arg = dynamic_cast<const NamedTypeExpr *>(target->typeArgs[i].get());
        if (arg && arg->typeArgs.empty() && arg->name == (*typeParams)[i].name) {
            params.push_back(arg->name);
        }
    }
    return params;
}

TypeRef AnalysisContext::ParseTypeRefFromString(std::string str) const {
    auto trim = [](std::string &s) {
        s.erase(0, s.find_first_not_of(" \t\r\n"));
        s.erase(s.find_last_not_of(" \t\r\n") + 1);
    };
    trim(str);
    if (str.empty()) {
        return TypeRef::MakeUnknown();
    }

    if (str == "?") {
        return TypeRef::MakeUnknown();
    }
    if (str == "opaque") {
        return TypeRef::MakeOpaque();
    }
    if (const auto primitive = PrimitiveTypeFromName(str)) {
        return *primitive;
    }

    if (str[0] == '*' || str[0] == '&') {
        // Pointer and reference names render writability as `var` in front of the inner type, so reading a name
        // back has to take the qualifier off and restore it on that inner type.
        const bool isReference = str[0] == '&';
        std::string pointee = str.substr(1);
        trim(pointee);
        const bool writable = pointee.starts_with("var ");
        if (writable) {
            pointee = pointee.substr(4);
            trim(pointee);
        }
        TypeRef inner = ParseTypeRefFromString(std::move(pointee));
        inner.isMut = writable;
        return isReference ? TypeRef::MakeReference(std::move(inner)) : TypeRef::MakePointer(std::move(inner));
    }

    if (str.size() >= 2 && str.compare(str.size() - 2, 2, "[]") == 0) {
        return TypeRef::MakeArray(ParseTypeRefFromString(str.substr(0, str.size() - 2)));
    }

    if (str[0] == '(' && str.back() == ')') {
        std::vector<TypeRef> elems;
        std::string content = str.substr(1, str.size() - 2);
        std::size_t start = 0;
        int depth = 0;
        for (std::size_t i = 0; i < content.size(); ++i) {
            if (content[i] == '<' || content[i] == '(') {
                depth++;
            }
            else if (content[i] == '>' || content[i] == ')') {
                depth--;
            }
            else if (content[i] == ',' && depth == 0) {
                elems.push_back(ParseTypeRefFromString(content.substr(start, i - start)));
                start = i + 1;
            }
        }
        if (start < content.size()) {
            elems.push_back(ParseTypeRefFromString(content.substr(start)));
        }
        return TypeRef::MakeTuple(elems);
    }

    const std::string aggregateName = str.substr(0, str.find('<'));
    if (const auto declaration = structDecls.find(aggregateName);
        declaration != structDecls.end() && !declaration->second->intrinsicName.empty()) {
        std::vector<TypeRef> arguments;
        const auto begin = str.find('<');
        if (begin != std::string::npos && str.back() == '>') {
            arguments.push_back(ParseTypeRefFromString(str.substr(begin + 1, str.size() - begin - 2)));
        }
        if (const auto aggregate = IntrinsicAggregateType(declaration->second->intrinsicName, arguments)) {
            return *aggregate;
        }
    }

    // A type parameter in scope is that parameter, not a type that happens to share its name. This path reads a
    // type back out of a printed name, where `T` and a struct called `T` look alike, so the parameter list is the
    // only thing that tells them apart -- and ResolveType answers the same way for the same spelling written out.
    for (const auto &parameter : currentTypeParams) {
        if (parameter == str) {
            return TypeRef::MakeTypeParam(str);
        }
    }

    return TypeRef::MakeNamed(str);
}

std::vector<TypeRef> AnalysisContext::ParseTypeArgsFromTypeName(const std::string &typeName) const {
    std::vector<TypeRef> args;
    const std::size_t pos = typeName.find('<');
    if (pos == std::string::npos || typeName.back() != '>') {
        return args;
    }
    std::string content = typeName.substr(pos + 1, typeName.size() - pos - 2);
    std::size_t start = 0;
    int depth = 0;
    for (std::size_t i = 0; i < content.size(); ++i) {
        if (content[i] == '<' || content[i] == '(') {
            depth++;
        }
        else if (content[i] == '>' || content[i] == ')') {
            depth--;
        }
        else if (content[i] == ',' && depth == 0) {
            args.push_back(ParseTypeRefFromString(content.substr(start, i - start)));
            start = i + 1;
        }
    }
    if (start < content.size()) {
        args.push_back(ParseTypeRefFromString(content.substr(start)));
    }
    return args;
}

TypeRef AnalysisContext::ResolveTypeImpl(const TypeExpr &expr) {
    if (const auto *t = dynamic_cast<const NamedTypeExpr *>(&expr)) {
        if (const auto primitive = PrimitiveTypeFromName(t->name); primitive && primitive->IsString()) {
            const Symbol *symbol = currentScope ? currentScope->Lookup(t->name) : nullptr;
            if (!symbol || !IsVisibleTypeSymbol(*symbol) || symbol->type != *primitive) {
                EmitError(expr.location,
                          std::format("type '{}' requires an imported or local intrinsic declaration", t->name));
                return TypeRef::MakeUnknown();
            }
            if (!t->typeArgs.empty()) {
                EmitGenericArityError(expr, std::format("string type '{}'", t->name), 0, t->typeArgs.size());
                return TypeRef::MakeUnknown();
            }
            return *primitive;
        }
        if (IsUnimplementedPrimitiveType(t->name)) {
            EmitError(expr.location, std::format("primitive type '{}' is reserved but is not implemented in this "
                                                 "compiler version",
                                                 t->name));
            return TypeRef::MakeUnknown();
        }

        for (const auto &tp : currentTypeParams) {
            if (tp == t->name) {
                if (!t->typeArgs.empty()) {
                    EmitError(expr.location, std::format("Type parameter '{}' cannot "
                                                         "take type arguments",
                                                         t->name));
                    return TypeRef::MakeUnknown();
                }
                return TypeRef::MakeTypeParam(t->name);
            }
        }

        std::vector<TypeRef> resolvedArgs;
        bool hasUnknownArgs = false;
        for (const auto &argExpr : t->typeArgs) {
            TypeRef argType = ResolveType(*argExpr);
            if (argType.IsUnknown()) {
                hasUnknownArgs = true;
            }
            resolvedArgs.push_back(argType);
        }

        if (hasUnknownArgs) {
            return TypeRef::MakeUnknown();
        }

        if (const EnumDecl *enumeration = EnumNamed(t->name)) {
            const auto &decl = *enumeration;
            if (resolvedArgs.size() != decl.typeParams.size()) {
                EmitGenericArityError(expr, std::format("variant type '{}'", t->name), decl.typeParams.size(),
                                      resolvedArgs.size());
                return TypeRef::MakeUnknown();
            }
            CheckTypeReferenceConstraints(expr, decl.typeParams, resolvedArgs, std::format("enum '{}'", t->name));
            return EnumType(decl, resolvedArgs);
        }

        if (auto structType = ResolveStructTypeReference(expr, t->name, resolvedArgs)) {
            return *structType;
        }

        Symbol *sym = currentScope ? currentScope->Lookup(t->name) : nullptr;
        if (sym && (sym->kind == Symbol::Kind::Type || sym->kind == Symbol::Kind::Interface)) {
            // Return base type if no generic arguments are provided
            if (t->typeArgs.empty() && !sym->type.IsUnknown()) {
                return sym->type;
            }

            return TypeRef::MakeNamed(GenericTypeName(*t));
        }
        if (!sym) {
            sym = FindUniquePackageType(t->name);
            if (sym && (sym->kind == Symbol::Kind::Type || sym->kind == Symbol::Kind::Interface)) {
                if (t->typeArgs.empty() && !sym->type.IsUnknown()) {
                    return sym->type;
                }
                return TypeRef::MakeNamed(GenericTypeName(*t));
            }
        }

        if (sym) {
            EmitError(expr.location, std::format("name '{}' is a {}, not a type", t->name, SymbolKindName(sym->kind)),
                      {DeclarationNote(*sym)});
        }
        else {
            std::optional<std::string> help;
            if (currentScope) {
                if (const Symbol *suggestion = currentScope->Suggest(t->name)) {
                    help = std::format("did you mean '{}'?", suggestion->name);
                }
            }
            EmitError(expr.location, std::format("type '{}' is not defined in this scope", t->name), {},
                      std::move(help));
        }
        return TypeRef::MakeUnknown();
    }

    if (const auto *t = dynamic_cast<const PathTypeExpr *>(&expr)) {
        if (t->segments.empty()) {
            EmitError(expr.location, "empty type path");
            return TypeRef::MakeUnknown();
        }

        std::string fullPath = t->segments.front();
        for (size_t i = 1; i < t->segments.size(); ++i) {
            fullPath += "::" + t->segments[i];
        }
        return TypeRef::MakeNamed(fullPath);
    }

    if (const auto *t = dynamic_cast<const PointerTypeExpr *>(&expr)) {
        TypeRef pointeeType = ResolveType(*t->pointee);
        if (pointeeType.IsUnknown()) {
            return TypeRef::MakeUnknown();
        }
        pointeeType.isMut = pointeeType.isMut || t->pointeeMut;
        return TypeRef::MakePointer(std::move(pointeeType));
    }

    if (const auto *t = dynamic_cast<const ReferenceTypeExpr *>(&expr)) {
        TypeRef pointeeType = ResolveType(*t->pointee);
        if (pointeeType.IsUnknown()) {
            return TypeRef::MakeUnknown();
        }
        pointeeType.isMut = pointeeType.isMut || t->pointeeMut;
        return TypeRef::MakeReference(std::move(pointeeType));
    }

    if (const auto *t = dynamic_cast<const ArrayTypeExpr *>(&expr)) {
        TypeRef elemType = ResolveType(*t->element);
        if (elemType.IsUnknown()) {
            return TypeRef::MakeUnknown();
        }
        return TypeRef::MakeArray(std::move(elemType), t->size ? EvalArrayLength(*t->size) : std::nullopt);
    }

    if (const auto *t = dynamic_cast<const TupleTypeExpr *>(&expr)) {
        std::vector<TypeRef> elems;
        elems.reserve(t->elements.size());

        for (const auto &e : t->elements) {
            TypeRef elem = ResolveType(*e);
            if (elem.IsUnknown()) {
                return TypeRef::MakeUnknown();
            }
            elems.push_back(elem);
        }

        return TypeRef::MakeTuple(std::move(elems));
    }

    if (dynamic_cast<const SelfTypeExpr *>(&expr)) {
        return currentSelfType.IsUnknown() ? TypeRef::MakeNamed("self") : currentSelfType;
    }

    if (const auto *t = dynamic_cast<const FunctionTypeExpr *>(&expr)) {
        std::vector<TypeRef> paramTypes;
        paramTypes.reserve(t->params.size());
        for (const auto &p : t->params) {
            TypeRef pt = ResolveType(*p);
            if (pt.IsUnknown()) {
                return TypeRef::MakeUnknown();
            }
            paramTypes.push_back(std::move(pt));
        }
        TypeRef ret = t->returnType ? ResolveType(*t->returnType->get()) : TypeRef::MakeOpaque();
        if (ret.IsUnknown()) {
            return TypeRef::MakeUnknown();
        }
        TypeRef funcType = TypeRef::MakeFunc(std::move(paramTypes), std::move(ret));
        funcType.isVariadic = t->isVariadic;
        return funcType;
    }

    return TypeRef::MakeUnknown();
}

TypeRef AnalysisContext::ResolveTypeWithSubstitution(const TypeExpr &expr,
                                                     const std::unordered_map<std::string, TypeRef> &substitutions) {
    if (const auto accepted = typeNodeTypes.find(&expr);
        accepted != typeNodeTypes.end() &&
        (accepted->second.IsString() || accepted->second.isIntrinsicSlice || accepted->second.IsRange())) {
        return SubstituteTypeParameters(accepted->second, substitutions);
    }
    if (auto *t = dynamic_cast<const NamedTypeExpr *>(&expr)) {
        if (t->typeArgs.empty()) {
            if (auto it = substitutions.find(t->name); it != substitutions.end()) {
                return it->second;
            }
            return ResolveType(expr);
        }

        std::vector<TypeRef> resolvedArgs;
        resolvedArgs.reserve(t->typeArgs.size());
        for (const auto &typeArg : t->typeArgs) {
            resolvedArgs.push_back(ResolveTypeWithSubstitution(*typeArg, substitutions));
        }
        if (const Symbol *symbol = currentScope->Lookup(t->name);
            symbol && symbol->declaration && !symbol->intrinsicName.empty() && IsVisibleTypeSymbol(*symbol)) {
            if (const auto aggregate = IntrinsicAggregateType(symbol->intrinsicName, resolvedArgs)) {
                return *aggregate;
            }
        }
        // An enum instantiation is composed in one place, so that a type reached through a substitution -- a
        // return type resolved while a signature is built -- carries the layout marker one resolved directly has.
        const EnumDecl *enumeration = EnumNamed(t->name);
        return enumeration ? EnumType(*enumeration, resolvedArgs)
                           : TypeRef::MakeNamed(TypeRef::InstantiationName(t->name, resolvedArgs));
    }
    if (auto *t = dynamic_cast<const PointerTypeExpr *>(&expr)) {
        TypeRef pointeeType = ResolveTypeWithSubstitution(*t->pointee, substitutions);
        pointeeType.isMut = pointeeType.isMut || t->pointeeMut;
        return TypeRef::MakePointer(std::move(pointeeType));
    }
    if (auto *t = dynamic_cast<const ReferenceTypeExpr *>(&expr)) {
        TypeRef pointeeType = ResolveTypeWithSubstitution(*t->pointee, substitutions);
        pointeeType.isMut = pointeeType.isMut || t->pointeeMut;
        return TypeRef::MakeReference(std::move(pointeeType));
    }
    if (auto *t = dynamic_cast<const ArrayTypeExpr *>(&expr)) {
        return TypeRef::MakeArray(ResolveTypeWithSubstitution(*t->element, substitutions),
                                  t->size ? EvalArrayLength(*t->size) : std::nullopt);
    }
    if (auto *t = dynamic_cast<const TupleTypeExpr *>(&expr)) {
        std::vector<TypeRef> elems;
        for (auto &elem : t->elements) {
            elems.push_back(ResolveTypeWithSubstitution(*elem, substitutions));
        }
        return TypeRef::MakeTuple(std::move(elems));
    }
    // A callback parameter names its type parameters like any other type does, so `func(T, T) -> bool` has to be
    // substituted through as well. Without this the parameter kept an unsubstituted `T` and no argument could ever
    // match it. Lowering has resolved function types this way all along; only the analyzer was missing the case.
    if (auto *t = dynamic_cast<const FunctionTypeExpr *>(&expr)) {
        std::vector<TypeRef> paramTypes;
        paramTypes.reserve(t->params.size());
        for (const auto &param : t->params) {
            paramTypes.push_back(ResolveTypeWithSubstitution(*param, substitutions));
        }
        TypeRef returnType =
            t->returnType ? ResolveTypeWithSubstitution(*t->returnType->get(), substitutions) : TypeRef::MakeOpaque();
        TypeRef functionType = TypeRef::MakeFunc(std::move(paramTypes), std::move(returnType));
        functionType.isVariadic = t->isVariadic;
        return functionType;
    }
    return ResolveType(expr);
}

[[nodiscard]] const FuncDecl *AnalysisContext::LookupMethod(const TypeRef &receiverType, const std::string &methodName,
                                                            const std::vector<TypeRef> &argTypes,
                                                            const bool requireAccessible) {
    const std::string typeName = NamedBaseTypeName(receiverType);
    if (typeName.empty()) {
        return nullptr;
    }
    const auto typeIt = methodsByType.find(typeName);
    if (typeIt == methodsByType.end()) {
        return nullptr;
    }
    const auto methodIt = typeIt->second.find(methodName);
    if (methodIt == typeIt->second.end()) {
        return nullptr;
    }
    const std::vector<const FuncDecl *> accessible =
        requireAccessible ? AccessibleMethodCandidates(receiverType, methodName) : methodIt->second;
    const auto &overloads = accessible;
    if (overloads.empty()) {
        return nullptr;
    }
    // Best-effort scrape for property access (missing args).
    if (argTypes.empty()) {
        return overloads[0];
    }
    if (overloads.size() == 1) {
        // Single overload: validate arity and assignability before
        // returning.
        const auto *decl = overloads[0];
        std::vector<TypeRef> paramTypes = ResolveMethodParamTypes(receiverType, *decl);
        if (paramTypes.size() != argTypes.size()) {
            return nullptr;
        }
        for (std::size_t i = 0; i < argTypes.size(); ++i) {
            if (argTypes[i].IsUnknown() || paramTypes[i].IsUnknown()) {
                continue;
            }
            if (!argTypes[i].IsAssignableTo(paramTypes[i]) && !argTypes[i].CanImplicitlyBorrowTo(paramTypes[i]) &&
                !(argTypes[i].IsInteger() && paramTypes[i].IsInteger())) {
                return nullptr;
            }
        }
        return decl;
    }
    for (const auto *decl : overloads) {
        std::vector<TypeRef> paramTypes = ResolveMethodParamTypes(receiverType, *decl);
        if (paramTypes.size() != argTypes.size()) {
            continue;
        }
        bool match = true;
        for (std::size_t i = 0; i < argTypes.size(); ++i) {
            if (!argTypes[i].IsUnknown() && !paramTypes[i].IsUnknown() && !argTypes[i].IsAssignableTo(paramTypes[i]) &&
                !argTypes[i].CanImplicitlyBorrowTo(paramTypes[i]) &&
                !(argTypes[i].IsInteger() && paramTypes[i].IsInteger())) {
                match = false;
                break;
            }
        }
        if (match) {
            return decl;
        }
    }
    return nullptr;
}
} // namespace Rux::SemanticDetail
