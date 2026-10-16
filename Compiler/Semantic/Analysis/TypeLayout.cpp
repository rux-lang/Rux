#include "Lexer/Lexer.h"
#include "Numeric/IntegerLiteral.h"
#include "Semantic/Analysis/AnalysisContext.h"
#include "Semantic/Conditional/ConditionalCompilation.h"
#include "Target/Layout.h"
#include "Target/Target.h"
#include "Types/NativeLayout.h"
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
    if ((inputType->kind == TypeRef::Kind::Named || inputType->kind == TypeRef::Kind::TypeParam) &&
        substitutions.contains(inputType->name) &&
        substitutions.at(inputType->name).ToString() != inputType.ToString()) {
        // A substitution is already in the terms of whoever asked, so it is laid out as it is: applying the map to it
        // again would chase `K -> V -> K` forever when a type's own parameters are passed swapped.
        return LayoutOfTypeRef(substitutions.at(inputType->name), {});
    }

    const std::string &key = inputType.ToString();
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

    if (inputType.IsSlice()) {
        return finish(ResolvedTypeLayout{16, 8});
    }

    if (inputType->kind == TypeRef::Kind::Named) {
        const std::string baseName = BaseTypeName(inputType->name);
        std::unordered_map<std::string, TypeRef> localSubs = substitutions;
        const std::vector<TypeRef> typeArgs = ParseTypeArgsFromTypeName(inputType->name);
        // A generic declaration's fields name only its own parameters, so they are mapped to the arguments, read in
        // the caller's terms, and nothing else: the caller's parameters may share their names.
        const auto declarationSubstitutions = [&](const std::vector<TypeParameter> &parameters) {
            std::unordered_map<std::string, TypeRef> own;
            const std::size_t count = std::min(parameters.size(), typeArgs.size());
            for (std::size_t i = 0; i < count; ++i) {
                own.emplace(parameters[i].name, SubstituteTypeParameters(typeArgs[i], substitutions));
            }
            return own;
        };
        if (const auto structure = structDecls.find(baseName); structure != structDecls.end()) {
            return finish(LayoutOfStruct(*structure->second, declarationSubstitutions(structure->second->typeParams)));
        }
        if (const auto enumeration = enumDecls.find(baseName); enumeration != enumDecls.end()) {
            return finish(
                LayoutOfEnum(*enumeration->second, declarationSubstitutions(enumeration->second->typeParams)));
        }
        if (const auto unionType = unionDecls.find(baseName); unionType != unionDecls.end()) {
            return finish(LayoutOfUnion(*unionType->second, localSubs));
        }

        // Interface values are fat pointers: {data, vtable}.
        if (interfaceDecls.contains(baseName))
            return finish(ResolvedTypeLayout{16, 8});
        if (Symbol *sym = currentScope->Lookup(baseName); sym) {
            if (sym->kind == Symbol::Kind::Interface) {
                return finish(ResolvedTypeLayout{16, 8});
            }
            if (sym->kind == Symbol::Kind::Type && !sym->type.IsUnknown() && !(sym->type == inputType)) {
                return finish(LayoutOfTypeRef(sym->type, localSubs));
            }
        }
        if (!inputType->inner.empty()) {
            return finish(LayoutOfTypeRef(inputType->inner[0], localSubs));
        }
        return finish(std::nullopt);
    }

    if (inputType->kind == TypeRef::Kind::Reference && !inputType->inner.empty()) {
        const TypeRef referent = inputType->inner.front();
        if (referent->kind == TypeRef::Kind::Named) {
            const auto interface = interfaceDecls.find(BaseTypeName(referent->name));
            if (interface != interfaceDecls.end()) {
                return finish(ResolvedTypeLayout{16, 8});
            }
        }
    }

    if (inputType->kind == TypeRef::Kind::Tuple) {
        std::uint64_t offset = 0;
        std::uint64_t alignment = 1;
        for (const TypeRef &element : inputType->inner) {
            const auto elementLayout = LayoutOfTypeRef(element, substitutions);
            if (!elementLayout) {
                return finish(std::nullopt);
            }
            const auto alignedOffset = CheckedAlignUp(offset, elementLayout->alignment);
            if (!alignedOffset) {
                return finish(std::nullopt);
            }
            offset = *alignedOffset;
            // A zero-sized element, such as the unit, reserves no bytes, as it reserves none in a struct.
            const auto end = checkedAdd(offset, elementLayout->size);
            if (!end) {
                return finish(std::nullopt);
            }
            offset = *end;
            alignment = std::max(alignment, elementLayout->alignment);
        }
        const auto size = CheckedAlignUp(offset, alignment);
        return finish(size ? std::optional(ResolvedTypeLayout{*size, alignment}) : std::nullopt);
    }

    if (inputType->kind == TypeRef::Kind::Array) {
        if (inputType->inner.empty() || !inputType->arrayLength) {
            return finish(std::nullopt);
        }
        const auto elementLayout = LayoutOfTypeRef(inputType->inner[0], substitutions);
        if (!elementLayout) {
            return finish(std::nullopt);
        }
        const auto size = checkedMultiply(elementLayout->size, *inputType->arrayLength);
        return finish(size ? std::optional(ResolvedTypeLayout{*size, elementLayout->alignment}) : std::nullopt);
    }

    if (inputType.IsRange()) {
        if (inputType->kind == TypeRef::Kind::RangeFull) {
            return finish(ResolvedTypeLayout{0, 1});
        }
        if (inputType->inner.empty()) {
            return finish(std::nullopt);
        }
        const auto elementLayout = LayoutOfTypeRef(inputType->inner[0], substitutions);
        if (!elementLayout || elementLayout->size == 0) {
            return finish(std::nullopt);
        }
        const std::uint64_t count =
            inputType->kind == TypeRef::Kind::Range || inputType->kind == TypeRef::Kind::RangeInclusive ? 2 : 1;
        const auto size = checkedMultiply(elementLayout->size, count);
        return finish(size ? std::optional(ResolvedTypeLayout{*size, elementLayout->alignment}) : std::nullopt);
    }

    // A native sum, optional, or fallible takes the shared native layout, laid out from its members' own layouts.
    if (inputType.IsSum() || inputType.IsOptional() || inputType.IsFallible()) {
        const auto native =
            ComputeNativeLayout(inputType, [&](const TypeRef &member) -> std::optional<SizeAndAlignment> {
                const auto memberLayout = LayoutOfTypeRef(member, substitutions);
                return memberLayout ? std::optional(SizeAndAlignment{memberLayout->size, memberLayout->alignment})
                                    : std::nullopt;
            });
        return finish(native ? std::optional(ResolvedTypeLayout{native->size, native->alignment}) : std::nullopt);
    }

    // A scalar takes the alignment its type states -- a primitive's from the catalog, which is what code generation
    // lays it out by too -- rather than one derived from its size.
    const auto size = inputType.SizeInBytes();
    const auto alignment = inputType.AlignInBytes();
    return finish(size && alignment ? std::optional(ResolvedTypeLayout{*size, *alignment}) : std::nullopt);
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
        offset += fieldLayout->size;
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
        const auto payload = LayoutOfFields(fields, {});
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
    TypeRef type = TypeRef::MakeNamed(TypeRef::InstantiationName(programIndex.NominalName(decl), typeArgs));
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
            typeLayouts.insert_or_assign(type->name, *layout);
        }
        if (decl.form == EnumDecl::Form::Variant && layout) {
            const TypeRef marker = TypeRef::MakeStorageMarker(layout->size, layout->alignment);
            return type.WithInner({&marker, 1});
        }
        const TypeRef base = EnumBaseType(decl);
        return type.WithInner({&base, 1});
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
            typeLayouts.insert_or_assign(type->name, *layout);
            const TypeRef marker = TypeRef::MakeStorageMarker(layout->size, layout->alignment);
            return type.WithInner({&marker, 1});
        }
    }
    return type;
}

std::optional<ResolvedTypeLayout>
AnalysisContext::LayoutOfStruct(const StructDecl &decl, const std::unordered_map<std::string, TypeRef> &substitutions) {
    std::uint64_t offset = 0;
    std::uint64_t maxAlign = 1;
    for (const auto &field : decl.fields) {
        // The field's type is resolved under the substitutions, so it is laid out as it is.
        const TypeRef fieldType = ResolveTypeWithSubstitution(*field.type, substitutions);
        std::optional<ResolvedTypeLayout> fieldLayout;
        if (fieldType->kind == TypeRef::Kind::Array && !fieldType->arrayLength && !fieldType->inner.empty()) {
            const auto elementLayout = LayoutOfTypeRef(fieldType->inner[0], {});
            if (elementLayout) {
                fieldLayout = ResolvedTypeLayout{0, elementLayout->alignment};
            }
        }
        else {
            fieldLayout = LayoutOfTypeRef(fieldType, {});
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
        const auto fieldLayout = LayoutOfTypeRef(fieldType, {});
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

void AnalysisContext::RecordResolvedTypeLayouts(const std::vector<TypeRef> &recordedTypes) {
    // A layout depends only on the spelling, so each spelling is laid out once. Laying out resolves types, which
    // records more, so the types are gathered first.
    std::unordered_set<std::string> spellings;
    std::vector<TypeRef> resolvedTypes;
    const auto record = [&](const TypeRef &type) {
        if (spellings.insert(type.ToString()).second)
            resolvedTypes.push_back(type);
    };
    for (const TypeRef &type : recordedTypes) {
        record(type);
    }
    for (const auto &[_, binding] : callableBindings) {
        for (const auto &[__, type] : binding.substitutions) {
            record(type);
        }
        if (binding.receiverType) {
            record(*binding.receiverType);
        }
    }
    for (const TypeRef &type : resolvedTypes) {
        LayoutOfTypeRef(type);
    }
}

void AnalysisContext::ResolveReusedParameters(const FuncDecl &function) {
    // The body is reused, but the name an overload is called by spells its parameter types, which resolving them is
    // what records, as checking the function would.
    for (const auto &parameter : function.params)
        if (!parameter.IsReceiver() && parameter.type)
            static_cast<void>(ResolveType(*parameter.type));
}

bool AnalysisContext::SkipsReusedFunction(const FuncDecl &function) const {
    return checkingDeclarationsOnly && currentTypeParams.empty() && function.typeParams.empty() && !function.isAsm &&
           function.body != nullptr &&
           std::ranges::none_of(function.params, [](const Param &param) { return param.defaultValue.has_value(); });
}

void AnalysisContext::CheckDecl(const Decl &decl) {
    const CheckingDeclaration checking(*this, decl);
    if (auto *fn = AstCast<const FuncDecl>(&decl)) {
        if (SkipsReusedFunction(*fn)) {
            ResolveReusedParameters(*fn);
            return;
        }
        if (const Symbol *symbol = currentScope->LookupLocal(fn->name); symbol && symbol->kind == Symbol::Kind::Func) {
            ValidateFunctionSignature(*fn, symbol->funcOverloads);
        }
        CheckFuncDecl(*fn);
    }
    else if (precheckedDeclarations.contains(&decl)) {
        // Checked before any body, by PrecheckDecl.
    }
    else if (auto *structDecl = AstCast<const StructDecl>(&decl)) {
        CheckStructDecl(*structDecl);
    }
    else if (auto *enumDecl = AstCast<const EnumDecl>(&decl)) {
        CheckEnumDecl(*enumDecl);
    }
    else if (auto *unionDecl = AstCast<const UnionDecl>(&decl)) {
        CheckUnionDecl(*unionDecl);
    }
    else if (auto *ifaceDecl = AstCast<const InterfaceDecl>(&decl)) {
        CheckInterfaceDecl(*ifaceDecl);
    }
    else if (auto *implDecl = AstCast<const ImplDecl>(&decl)) {
        CheckImplDecl(*implDecl);
    }
    else if (auto *modDecl = AstCast<const ModuleDecl>(&decl)) {
        CheckModuleDecl(*modDecl);
    }
    else if (auto *constDecl = AstCast<const ConstDecl>(&decl)) {
        // A reused module's constant waits for something checked to read it, as in PrecheckModule.
        if (!checkingDeclarationsOnly || currentFunctionDecl != nullptr)
            (void)CheckNamedConstant(*constDecl);
    }
    else if (auto *globalDecl = AstCast<const GlobalDecl>(&decl)) {
        CheckGlobalDecl(*globalDecl);
    }
    else if (auto *aliasDecl = AstCast<const TypeAliasDecl>(&decl)) {
        ValidateArrayType(*aliasDecl->type);
        ResolveType(*aliasDecl->type);
    }
    else if (auto *externFn = AstCast<const ExternFuncDecl>(&decl)) {
        CheckAbiForTarget(externFn->callConv, externFn->abiLocation);
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
            ValidateCAbiType(returnType, externFn->returnType->get()->location,
                             std::format("the return value of extern function '{}'", externFn->name));
        }
        for (auto &p : externFn->params) {
            if (!p.isVariadic) {
                ValidateArrayType(*p.type);
                const TypeRef parameterType = ResolveType(*p.type);
                if (parameterType->kind != TypeRef::Kind::Reference) {
                    ValidateStoredType(parameterType, p.location, "extern parameter");
                }
                ValidateCAbiType(parameterType, p.location, std::format("extern parameter '{}'", p.name));
            }
        }
    }
    else if (auto *externVar = AstCast<const ExternVarDecl>(&decl)) {
        ValidateArrayType(*externVar->type);
        ValidateStoredType(ResolveType(*externVar->type), externVar->location, "extern variable");
    }
    else if (auto *externBlock = AstCast<const ExternBlockDecl>(&decl)) {
        CheckAbiForTarget(externBlock->callConv, externBlock->abiLocation);
        for (auto &item : externBlock->items) {
            CheckDecl(*item);
        }
    }
    else if (auto *useDecl = AstCast<const UseDecl>(&decl)) {
        if (!refreshedImports.contains(useDecl))
            ReapplyUseDecl(*useDecl);
    }
    ValidatePublicDeclaration(decl);
}
} // namespace Rux::SemanticDetail
