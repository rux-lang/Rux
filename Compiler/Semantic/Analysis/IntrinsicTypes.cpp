#include "Semantic/Analysis/AnalysisContext.h"
#include "Types/PrimitiveCatalog.h"

#include <format>

namespace Rux::SemanticDetail {
bool AnalysisContext::IsVisibleTypeSymbol(const Symbol &symbol) const {
    if (!symbol.declaration) {
        return false;
    }
    if (symbol.ownerPackage == currentPackage) {
        return true;
    }
    const auto imported = explicitTypeImports.find(currentFile);
    return imported != explicitTypeImports.end() && imported->second.contains(symbol.declaration);
}

const Decl *AnalysisContext::VisibleIntrinsicType(const TypeRef &type, const SourceLocation location) const {
    const Symbol *selected = nullptr;
    for (const Scope *scope = currentScope; scope; scope = scope->Parent()) {
        for (const auto &[name, symbol] : scope->Table()) {
            (void)name;
            if (symbol.kind != Symbol::Kind::Type || symbol.type != type || !IsVisibleTypeSymbol(symbol)) {
                continue;
            }
            if (selected && selected->ownerPackage != symbol.ownerPackage) {
                EmitError(location,
                          std::format("intrinsic type '{}' has ambiguous visible providers", type.ToString()));
                return nullptr;
            }
            selected = &symbol;
        }
    }
    if (!selected) {
        EmitError(location, std::format("type '{}' requires a visible intrinsic declaration", type.ToString()), {},
                  "import the type from a package that declares it, or provide an intrinsic declaration");
    }
    return selected ? selected->declaration : nullptr;
}

const ConstDecl *AnalysisContext::LookupAssociatedConstant(const Symbol &type, const std::string &name) const {
    // A built-in scalar name provides representation, but no package API.
    if (!IsVisibleTypeSymbol(type)) {
        return nullptr;
    }
    for (const ImplDecl *implementation : implDecls) {
        const auto owner = declarationInfos.find(implementation);
        if (owner == declarationInfos.end() || owner->second.ownerPackage != type.ownerPackage) {
            continue;
        }
        const auto primitive = PrimitiveTypeFromName(implementation->typeName);
        if (implementation->typeName != type.name && (!primitive || *primitive != type.type)) {
            continue;
        }
        for (const auto &constant : implementation->constants) {
            if (constant->name == name) {
                return constant.get();
            }
        }
    }
    return nullptr;
}

TypeRef AnalysisContext::CheckAssociatedConstant(const ConstDecl &declaration) {
    if (!checkingAssociatedConstants.insert(&declaration).second) {
        EmitError(declaration.location,
                  std::format("associated constant '{}' has a cyclic initializer", declaration.name));
        return TypeRef::MakeUnknown();
    }
    Scope *savedScope = currentScope;
    const std::string savedFile = currentFile;
    const std::string savedPackage = currentPackage;
    if (const auto owner = declarationInfos.find(&declaration); owner != declarationInfos.end()) {
        currentFile = owner->second.sourceName;
        currentPackage = owner->second.ownerPackage;
        const auto package = packageModuleScopes.find(currentPackage);
        if (package != packageModuleScopes.end()) {
            const auto scope = package->second.find(owner->second.modulePath);
            if (scope != package->second.end()) {
                currentScope = scope->second;
            }
        }
    }
    PushScope();
    Symbol symbol;
    symbol.kind = Symbol::Kind::Const;
    symbol.name = declaration.name;
    Define(symbol);
    CheckConstDecl(declaration);
    const TypeRef type = currentScope->Lookup(declaration.name)->type;
    if (!declaration.intrinsicName.empty() &&
        (!(type.kind == TypeRef::Kind::Float32 || type.kind == TypeRef::Kind::Float64) ||
         (declaration.name != "Infinity" && declaration.name != "NaN"))) {
        EmitError(declaration.location, "intrinsic associated constants support only floating-point Infinity and NaN");
    }
    PopScope();
    currentScope = savedScope;
    currentFile = savedFile;
    currentPackage = savedPackage;
    checkingAssociatedConstants.erase(&declaration);
    return type;
}

void AnalysisContext::CheckIntrinsicType(const Decl &declaration) {
    const std::string &name = declaration.intrinsicName;
    const auto primitive = PrimitiveTypeFromName(name);
    if (const auto *alias = dynamic_cast<const TypeAliasDecl *>(&declaration)) {
        if (!primitive || primitive->IsString() || IsUnimplementedPrimitiveType(name)) {
            EmitError(declaration.location, std::format("'{}' is not a supported intrinsic scalar type", name));
        }
        else {
            typeNodeTypes[alias->type.get()] = *primitive;
        }
        return;
    }
    const auto *structure = dynamic_cast<const StructDecl *>(&declaration);
    if (!structure) {
        return;
    }
    const bool string = primitive && primitive->IsString();
    const bool slice = name == "Slice" || name == "MutableSlice";
    const bool start = name == "Range" || name == "RangeInclusive" || name == "RangeFrom";
    const bool end = name == "Range" || name == "RangeInclusive" || name == "RangeTo" || name == "RangeToInclusive";
    if (!string && !slice && !start && !end && name != "RangeFull") {
        EmitError(declaration.location, std::format("'{}' is not a supported intrinsic struct", name));
        return;
    }
    const std::size_t arity = string || name == "RangeFull" ? 0 : 1;
    if (structure->typeParams.size() != arity) {
        EmitError(declaration.location, std::format("intrinsic struct '{}' requires {} type parameters", name, arity));
        return;
    }
    const std::string element = string     ? (name == "string16"   ? "char16"
                                              : name == "string32" ? "char32"
                                                                   : "char8")
                              : arity == 1 ? structure->typeParams[0].name
                                           : "";
    std::vector<std::pair<std::string, std::string>> fields;
    if (string || slice) {
        fields = {{"data", (name == "MutableSlice" ? "*var " : "*") + element}, {"length", "uint"}};
    }
    else {
        if (start) {
            fields.emplace_back("start", element);
        }
        if (end) {
            fields.emplace_back("end", element);
        }
    }
    if (structure->fields.size() != fields.size()) {
        EmitError(declaration.location, std::format("intrinsic struct '{}' requires {} fields", name, fields.size()));
        return;
    }
    const auto typeName = [](const TypeExpr &type) -> std::string {
        if (const auto *named = dynamic_cast<const NamedTypeExpr *>(&type); named && named->typeArgs.empty()) {
            return named->name;
        }
        if (const auto *pointer = dynamic_cast<const PointerTypeExpr *>(&type)) {
            if (const auto *named = dynamic_cast<const NamedTypeExpr *>(pointer->pointee.get());
                named && named->typeArgs.empty()) {
                return (pointer->pointeeMut ? "*var " : "*") + named->name;
            }
        }
        return {};
    };
    for (std::size_t index = 0; index < fields.size(); ++index) {
        const auto &field = structure->fields[index];
        if (!field.isPublic || field.name != fields[index].first || typeName(*field.type) != fields[index].second) {
            EmitError(field.location, std::format("intrinsic struct '{}' requires field {} to be 'pub {}: {};'", name,
                                                  index + 1, fields[index].first, fields[index].second));
        }
    }
}
} // namespace Rux::SemanticDetail
