#include "Lexer/Lexer.h"
#include "Numeric/IntegerLiteral.h"
#include "Semantic/Analysis/AnalysisContext.h"
#include "Semantic/Conditional/ConditionalCompilation.h"
#include "Target/Layout.h"
#include "Target/Target.h"
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

std::optional<std::uint64_t> AnalysisContext::CheckedAlignUp(const std::uint64_t value, const std::uint64_t alignment) {
    if (alignment == 0 || value > std::numeric_limits<std::uint64_t>::max() - (alignment - 1)) {
        return std::nullopt;
    }
    return AlignUp(value, alignment);
}

std::optional<ResolvedTypeLayout>
AnalysisContext::LayoutOfTypeRef(const TypeRef &inputType,
                                 const std::unordered_map<std::string, TypeRef> &substitutions) {
    if ((inputType.kind == TypeRef::Kind::Named || inputType.kind == TypeRef::Kind::TypeParam) &&
        substitutions.contains(inputType.name) && substitutions.at(inputType.name).ToString() != inputType.ToString()) {
        return LayoutOfTypeRef(substitutions.at(inputType.name), substitutions);
    }

    const std::string key = inputType.ToString();
    if (const auto known = typeLayouts.find(key); known != typeLayouts.end()) {
        return known->second;
    }
    if (!activeLayoutTypes.insert(key).second) {
        return std::nullopt;
    }

    const auto finish = [&](std::optional<ResolvedTypeLayout> result) {
        activeLayoutTypes.erase(key);
        if (result) {
            typeLayouts.insert_or_assign(key, *result);
        }
        return result;
    };
    const auto checkedAdd = [](const std::uint64_t left, const std::uint64_t right) -> std::optional<std::uint64_t> {
        if (right > std::numeric_limits<std::uint64_t>::max() - left) {
            return std::nullopt;
        }
        return left + right;
    };
    const auto checkedMultiply = [](const std::uint64_t left,
                                    const std::uint64_t right) -> std::optional<std::uint64_t> {
        if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
            return std::nullopt;
        }
        return left * right;
    };

    if (inputType.kind == TypeRef::Kind::Named) {
        if (inputType.isIntrinsicSlice) {
            return finish(ResolvedTypeLayout{16, 8});
        }

        const std::string baseName = BaseTypeName(inputType.name);
        std::unordered_map<std::string, TypeRef> localSubs = substitutions;
        const std::vector<TypeRef> typeArgs = ParseTypeArgsFromTypeName(inputType.name);
        if (const auto structure = structDecls.find(baseName); structure != structDecls.end()) {
            const std::size_t count = std::min(structure->second->typeParams.size(), typeArgs.size());
            for (std::size_t i = 0; i < count; ++i) {
                localSubs[structure->second->typeParams[i].name] = typeArgs[i];
            }
            return finish(LayoutOfStruct(*structure->second, localSubs));
        }
        if (const auto enumeration = enumDecls.find(baseName); enumeration != enumDecls.end()) {
            const std::size_t count = std::min(enumeration->second->typeParams.size(), typeArgs.size());
            for (std::size_t i = 0; i < count; ++i) {
                localSubs[enumeration->second->typeParams[i].name] = typeArgs[i];
            }
            return finish(LayoutOfEnum(*enumeration->second, localSubs));
        }
        if (const auto unionType = unionDecls.find(baseName); unionType != unionDecls.end()) {
            return finish(LayoutOfUnion(*unionType->second, localSubs));
        }

        // Interface values are fat pointers: {data, vtable}.
        if (Symbol *sym = currentScope->Lookup(baseName); sym) {
            if (sym->kind == Symbol::Kind::Interface) {
                return finish(ResolvedTypeLayout{16, 8});
            }
            if (sym->kind == Symbol::Kind::Type && !sym->type.IsUnknown() && !(sym->type == inputType)) {
                return finish(LayoutOfTypeRef(sym->type, localSubs));
            }
        }
        if (!inputType.inner.empty()) {
            return finish(LayoutOfTypeRef(inputType.inner[0], localSubs));
        }
        return finish(std::nullopt);
    }

    if (inputType.kind == TypeRef::Kind::Reference && !inputType.inner.empty()) {
        const TypeRef referent = inputType.inner.front();
        if (referent.kind == TypeRef::Kind::Named) {
            const auto interface = interfaceDecls.find(BaseTypeName(referent.name));
            if (interface != interfaceDecls.end()) {
                return finish(ResolvedTypeLayout{16, 8});
            }
        }
    }

    if (inputType.kind == TypeRef::Kind::Tuple) {
        std::uint64_t offset = 0;
        std::uint64_t alignment = 1;
        for (const TypeRef &element : inputType.inner) {
            const auto elementLayout = LayoutOfTypeRef(element, substitutions);
            if (!elementLayout) {
                return finish(std::nullopt);
            }
            const auto alignedOffset = CheckedAlignUp(offset, elementLayout->alignment);
            if (!alignedOffset) {
                return finish(std::nullopt);
            }
            offset = *alignedOffset;
            const auto end = checkedAdd(offset, elementLayout->size > 0 ? elementLayout->size : 8);
            if (!end) {
                return finish(std::nullopt);
            }
            offset = *end;
            alignment = std::max(alignment, elementLayout->alignment);
        }
        const auto size = CheckedAlignUp(offset, alignment);
        return finish(size ? std::optional(ResolvedTypeLayout{*size, alignment}) : std::nullopt);
    }

    if (inputType.kind == TypeRef::Kind::Array) {
        if (inputType.inner.empty() || !inputType.arrayLength) {
            return finish(std::nullopt);
        }
        const auto elementLayout = LayoutOfTypeRef(inputType.inner[0], substitutions);
        if (!elementLayout) {
            return finish(std::nullopt);
        }
        const auto size = checkedMultiply(elementLayout->size, *inputType.arrayLength);
        return finish(size ? std::optional(ResolvedTypeLayout{*size, elementLayout->alignment}) : std::nullopt);
    }

    if (inputType.IsRange()) {
        if (inputType.kind == TypeRef::Kind::RangeFull) {
            return finish(ResolvedTypeLayout{0, 1});
        }
        if (inputType.inner.empty()) {
            return finish(std::nullopt);
        }
        const auto elementLayout = LayoutOfTypeRef(inputType.inner[0], substitutions);
        if (!elementLayout || elementLayout->size == 0) {
            return finish(std::nullopt);
        }
        const std::uint64_t count =
            inputType.kind == TypeRef::Kind::Range || inputType.kind == TypeRef::Kind::RangeInclusive ? 2 : 1;
        const auto size = checkedMultiply(elementLayout->size, count);
        return finish(size ? std::optional(ResolvedTypeLayout{*size, elementLayout->alignment}) : std::nullopt);
    }

    const auto size = inputType.SizeInBytes();
    return finish(size ? std::optional(ResolvedTypeLayout{*size, Layout::FieldAlign(*size)}) : std::nullopt);
}

std::optional<ResolvedTypeLayout>
AnalysisContext::LayoutOfFields(const std::vector<TypeRef> &fields,
                                const std::unordered_map<std::string, TypeRef> &substitutions) {
    std::uint64_t offset = 0;
    std::uint64_t alignment = 1;
    for (const TypeRef &field : fields) {
        const auto fieldLayout = LayoutOfTypeRef(field, substitutions);
        if (!fieldLayout) {
            return std::nullopt;
        }
        const auto alignedOffset = CheckedAlignUp(offset, fieldLayout->alignment);
        if (!alignedOffset) {
            return std::nullopt;
        }
        offset = *alignedOffset;
        if (fieldLayout->size > std::numeric_limits<std::uint64_t>::max() - offset) {
            return std::nullopt;
        }
        offset += fieldLayout->size > 0 ? fieldLayout->size : 8;
        alignment = std::max(alignment, fieldLayout->alignment);
    }
    const auto size = CheckedAlignUp(offset, alignment);
    return size ? std::optional(ResolvedTypeLayout{*size, alignment}) : std::nullopt;
}

std::optional<ResolvedTypeLayout>
AnalysisContext::LayoutOfEnum(const EnumDecl &decl, const std::unordered_map<std::string, TypeRef> &substitutions) {
    const auto tagLayout = LayoutOfTypeRef(EnumBaseType(decl), substitutions);
    if (!tagLayout) {
        return std::nullopt;
    }
    if (decl.form == EnumDecl::Form::Enumeration) {
        return tagLayout;
    }

    bool hasPayload = false;
    ResolvedTypeLayout maximumPayload;
    for (const auto &variant : decl.variants) {
        std::vector<TypeRef> fields;
        fields.reserve(variant.fields.size() + variant.namedFields.size());
        for (const auto &field : variant.fields) {
            fields.push_back(ResolveTypeWithSubstitution(*field, substitutions));
        }
        for (const auto &field : variant.namedFields) {
            fields.push_back(ResolveTypeWithSubstitution(*field.type, substitutions));
        }
        if (fields.empty()) {
            continue;
        }
        hasPayload = true;
        const auto payload = LayoutOfFields(fields, substitutions);
        if (!payload) {
            return std::nullopt;
        }
        maximumPayload.size = std::max(maximumPayload.size, payload->size);
        maximumPayload.alignment = std::max(maximumPayload.alignment, payload->alignment);
    }

    if (!hasPayload) {
        return tagLayout;
    }
    const std::uint64_t alignment = std::max(tagLayout->alignment, maximumPayload.alignment);
    const auto payloadOffset = CheckedAlignUp(tagLayout->size, maximumPayload.alignment);
    if (!payloadOffset || maximumPayload.size > std::numeric_limits<std::uint64_t>::max() - *payloadOffset) {
        return std::nullopt;
    }
    const auto size = CheckedAlignUp(*payloadOffset + maximumPayload.size, alignment);
    return size ? std::optional(ResolvedTypeLayout{*size, alignment}) : std::nullopt;
}

TypeRef AnalysisContext::EnumBaseType(const EnumDecl &decl) {
    return decl.baseType ? ResolveType(*decl.baseType) : TypeRef::MakeInt();
}

TypeRef AnalysisContext::EnumType(const EnumDecl &decl, const std::vector<TypeRef> &typeArgs) {
    TypeRef type = TypeRef::MakeNamed(TypeRef::InstantiationName(decl.name, typeArgs));
    if (decl.typeParams.empty()) {
        // `inner` carries how large the value is, and nothing reads it as the tag -- the tag's own type is kept
        // beside the declaration. An enum that is only a discriminant is the size of that discriminant, so its
        // base type says it. One carrying a payload is wider than its tag, and only the layout knows by how much;
        // recording the tag there instead sized the whole value as the tag, so a call took the tag alone and left
        // the payload behind.
        // Record the layout under the name whatever the enum's shape: lowering builds this type a second time
        // and reads the size back from here, having no layout machinery of its own, and it needs the size of a
        // plain discriminant enum just as much when substitution has dropped what `inner` said.
        const auto layout = LayoutOfTypeRef(type);
        if (layout) {
            typeLayouts.insert_or_assign(type.name, *layout);
        }
        if (decl.form == EnumDecl::Form::Variant && layout) {
            type.inner.push_back(TypeRef::MakeArray(TypeRef::MakeChar8(), layout->size));
            return type;
        }
        type.inner.push_back(EnumBaseType(decl));
        return type;
    }
    if (typeArgs.size() == decl.typeParams.size()) {
        // An instantiation built out of the enclosing generic's parameters cannot be laid out here and is not
        // spelled anywhere else, so it is noted against the function writing it and composed again at each
        // instantiation of that function -- which is where the arguments are finally types.
        if (currentFunctionDecl && std::ranges::any_of(typeArgs, [this](const TypeRef &argument) {
                return MentionsTypeParameter(argument);
            })) {
            deferredEnumInstantiations[currentFunctionDecl].push_back({&decl, typeArgs});
        }
        std::unordered_map<std::string, TypeRef> substitutions;
        for (std::size_t i = 0; i < typeArgs.size(); ++i) {
            substitutions.emplace(decl.typeParams[i].name, typeArgs[i]);
        }
        if (const auto layout = LayoutOfTypeRef(type, substitutions)) {
            // Under the instantiation's own name, for the reason the branch above records it: lowering builds
            // this type a second time and reads its size back from here, having no layout machinery of its own.
            // An instantiation composed only inside generic bodies -- where nothing concrete ever spells it --
            // otherwise reached lowering with no marker at all.
            typeLayouts.insert_or_assign(type.name, *layout);
            type.inner.push_back(TypeRef::MakeArray(TypeRef::MakeChar8(), layout->size));
        }
    }
    return type;
}

std::optional<ResolvedTypeLayout>
AnalysisContext::LayoutOfStruct(const StructDecl &decl, const std::unordered_map<std::string, TypeRef> &substitutions) {
    std::uint64_t offset = 0;
    std::uint64_t maxAlign = 1;
    for (const auto &field : decl.fields) {
        const TypeRef fieldType = ResolveTypeWithSubstitution(*field.type, substitutions);
        std::optional<ResolvedTypeLayout> fieldLayout;
        if (fieldType.kind == TypeRef::Kind::Array && !fieldType.arrayLength && !fieldType.inner.empty()) {
            const auto elementLayout = LayoutOfTypeRef(fieldType.inner[0], substitutions);
            if (elementLayout) {
                fieldLayout = ResolvedTypeLayout{0, elementLayout->alignment};
            }
        }
        else {
            fieldLayout = LayoutOfTypeRef(fieldType, substitutions);
        }
        if (!fieldLayout) {
            return std::nullopt;
        }
        const auto alignedOffset = CheckedAlignUp(offset, fieldLayout->alignment);
        if (!alignedOffset) {
            return std::nullopt;
        }
        offset = *alignedOffset;
        if (fieldLayout->size > std::numeric_limits<std::uint64_t>::max() - offset) {
            return std::nullopt;
        }
        offset += fieldLayout->size;
        maxAlign = std::max(maxAlign, fieldLayout->alignment);
    }
    const auto size = CheckedAlignUp(offset, maxAlign);
    return size ? std::optional(ResolvedTypeLayout{*size, maxAlign}) : std::nullopt;
}

std::optional<ResolvedTypeLayout>
AnalysisContext::LayoutOfUnion(const UnionDecl &decl, const std::unordered_map<std::string, TypeRef> &substitutions) {
    std::uint64_t size = 0;
    std::uint64_t alignment = 1;
    for (const auto &field : decl.fields) {
        const TypeRef fieldType = ResolveTypeWithSubstitution(*field.type, substitutions);
        const auto fieldLayout = LayoutOfTypeRef(fieldType, substitutions);
        if (!fieldLayout) {
            return std::nullopt;
        }
        size = std::max(size, fieldLayout->size);
        alignment = std::max(alignment, fieldLayout->alignment);
    }
    const auto alignedSize = CheckedAlignUp(size, alignment);
    return alignedSize ? std::optional(ResolvedTypeLayout{*alignedSize, alignment}) : std::nullopt;
}

std::optional<ResolvedTypeLayout>
AnalysisContext::LayoutOfTypeExpr(const TypeExpr &expr, const std::unordered_map<std::string, TypeRef> &substitutions) {
    return LayoutOfTypeRef(ResolveTypeWithSubstitution(expr, substitutions), substitutions);
}

std::optional<ResolvedTypeLayout> AnalysisContext::LayoutOfTypeExpression(const TypeExpr &expr) {
    return LayoutOfTypeExpr(expr);
}

void AnalysisContext::RecordResolvedTypeLayouts() {
    std::vector<TypeRef> resolvedTypes;
    resolvedTypes.reserve(typeNodeTypes.size() + expressionTypes.size() + patternTypes.size());
    for (const auto &[_, type] : typeNodeTypes) {
        resolvedTypes.push_back(type);
    }
    for (const auto &[_, type] : expressionTypes) {
        resolvedTypes.push_back(type);
    }
    for (const auto &[_, type] : patternTypes) {
        resolvedTypes.push_back(type);
    }
    for (const auto &[_, binding] : callableBindings) {
        for (const auto &[__, type] : binding.substitutions) {
            resolvedTypes.push_back(type);
        }
        if (binding.receiverType) {
            resolvedTypes.push_back(*binding.receiverType);
        }
    }
    for (const TypeRef &type : resolvedTypes) {
        LayoutOfTypeRef(type);
    }
}

void AnalysisContext::CheckDecl(const Decl &decl) {
    if (auto *fn = dynamic_cast<const FuncDecl *>(&decl)) {
        if (const Symbol *symbol = currentScope->LookupLocal(fn->name); symbol && symbol->kind == Symbol::Kind::Func) {
            ValidateFunctionSignature(*fn, symbol->funcOverloads);
        }
        CheckFuncDecl(*fn);
    }
    else if (auto *structDecl = dynamic_cast<const StructDecl *>(&decl)) {
        CheckStructDecl(*structDecl);
    }
    else if (auto *enumDecl = dynamic_cast<const EnumDecl *>(&decl)) {
        CheckEnumDecl(*enumDecl);
    }
    else if (auto *unionDecl = dynamic_cast<const UnionDecl *>(&decl)) {
        CheckUnionDecl(*unionDecl);
    }
    else if (auto *ifaceDecl = dynamic_cast<const InterfaceDecl *>(&decl)) {
        CheckInterfaceDecl(*ifaceDecl);
    }
    else if (auto *implDecl = dynamic_cast<const ImplDecl *>(&decl)) {
        CheckImplDecl(*implDecl);
    }
    else if (auto *modDecl = dynamic_cast<const ModuleDecl *>(&decl)) {
        CheckModuleDecl(*modDecl);
    }
    else if (auto *constDecl = dynamic_cast<const ConstDecl *>(&decl)) {
        CheckConstDecl(*constDecl);
    }
    else if (auto *aliasDecl = dynamic_cast<const TypeAliasDecl *>(&decl)) {
        if (!aliasDecl->intrinsicName.empty()) {
            CheckIntrinsicType(*aliasDecl);
        }
        else {
            ValidateArrayType(*aliasDecl->type);
            ResolveType(*aliasDecl->type);
        }
    }
    else if (auto *externFn = dynamic_cast<const ExternFuncDecl *>(&decl)) {
        if (externFn->dll.empty()) {
            EmitError(externFn->location, std::format("extern function '{}' must specify a "
                                                      "source DLL via "
                                                      "#Link(\"dll.dll\")",
                                                      externFn->name));
        }
        if (externFn->returnType) {
            ValidateArrayType(*externFn->returnType->get());
            const TypeRef returnType = ResolveType(*externFn->returnType->get());
            ValidateStoredType(returnType, externFn->returnType->get()->location, "extern return type");
        }
        for (auto &p : externFn->params) {
            if (!p.isVariadic) {
                ValidateArrayType(*p.type);
                const TypeRef parameterType = ResolveType(*p.type);
                if (parameterType.kind != TypeRef::Kind::Reference) {
                    ValidateStoredType(parameterType, p.location, "extern parameter");
                }
            }
        }
    }
    else if (auto *externVar = dynamic_cast<const ExternVarDecl *>(&decl)) {
        ValidateArrayType(*externVar->type);
        ValidateStoredType(ResolveType(*externVar->type), externVar->location, "extern variable");
    }
    else if (auto *externBlock = dynamic_cast<const ExternBlockDecl *>(&decl)) {
        for (auto &item : externBlock->items) {
            CheckDecl(*item);
        }
    }
    else if (auto *useDecl = dynamic_cast<const UseDecl *>(&decl)) {
        CheckUseDecl(*useDecl);
    }
    ValidatePublicDeclaration(decl);
}
} // namespace Rux::SemanticDetail
