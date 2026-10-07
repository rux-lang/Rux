#include "Documentation/Signatures.h"

#include <algorithm>

namespace Rux::Documentation {
std::string TypeText(const TypeExpr *type) {
    if (type && type->parenthesized) {
        return "(" + UngroupedTypeText(type) + ")";
    }
    return UngroupedTypeText(type);
}

std::string UngroupedTypeText(const TypeExpr *type) {
    if (!type)
        return "?";
    if (const auto *named = dynamic_cast<const NamedTypeExpr *>(type)) {
        std::string text = named->name;
        if (!named->typeArgs.empty()) {
            text += '<';
            for (std::size_t i = 0; i < named->typeArgs.size(); ++i) {
                if (i != 0)
                    text += ", ";
                text += TypeText(named->typeArgs[i].get());
            }
            text += '>';
        }
        return text;
    }
    if (const auto *path = dynamic_cast<const PathTypeExpr *>(type)) {
        std::string text;
        for (std::size_t i = 0; i < path->segments.size(); ++i) {
            if (i != 0)
                text += "::";
            text += path->segments[i];
        }
        return text;
    }
    if (const auto *array = dynamic_cast<const ArrayTypeExpr *>(type)) {
        return TypeText(array->element.get()) + (array->size ? "[N]" : "[]");
    }
    if (const auto *slice = dynamic_cast<const SliceTypeExpr *>(type)) {
        return std::string(slice->elementMut ? "var " : "") + TypeText(slice->element.get()) + "[..]";
    }
    if (const auto *range = dynamic_cast<const RangeTypeExpr *>(type)) {
        return (range->start ? TypeText(range->start.get()) : std::string()) + (range->inclusive ? "..=" : "..") +
               (range->end ? TypeText(range->end.get()) : std::string());
    }
    if (const auto *pointer = dynamic_cast<const PointerTypeExpr *>(type)) {
        return std::string("*") + (pointer->pointeeMut ? "var " : "") + TypeText(pointer->pointee.get());
    }
    if (const auto *reference = dynamic_cast<const ReferenceTypeExpr *>(type)) {
        return std::string("&") + (reference->pointeeMut ? "var " : "") + TypeText(reference->pointee.get());
    }
    if (const auto *tuple = dynamic_cast<const TupleTypeExpr *>(type)) {
        std::string text = "(";
        for (std::size_t i = 0; i < tuple->elements.size(); ++i) {
            if (i != 0)
                text += ", ";
            text += TypeText(tuple->elements[i].get());
        }
        return text + ")";
    }
    if (dynamic_cast<const SelfTypeExpr *>(type))
        return "self";
    if (const auto *function = dynamic_cast<const FunctionTypeExpr *>(type)) {
        std::string text = "func(";
        for (std::size_t i = 0; i < function->params.size(); ++i) {
            if (i != 0)
                text += ", ";
            text += TypeText(function->params[i].get());
        }
        text += ')';
        if (function->returnType)
            text += " -> " + TypeText(function->returnType->get());
        return text;
    }
    if (const auto *sum = dynamic_cast<const SumTypeExpr *>(type)) {
        std::string text;
        for (std::size_t index = 0; index < sum->members.size(); ++index) {
            if (index != 0) {
                text += " | ";
            }
            text += TypeText(sum->members[index].get());
        }
        return text;
    }
    if (const auto *optional = dynamic_cast<const OptionalTypeExpr *>(type)) {
        return TypeText(optional->payload.get()) + "?";
    }
    if (const auto *fallible = dynamic_cast<const FallibleTypeExpr *>(type)) {
        return (fallible->success ? TypeText(fallible->success.get()) + " ! " : std::string("! ")) +
               TypeText(fallible->error.get());
    }
    return "?";
}

std::string TypeParams(const std::vector<TypeParameter> &parameters) {
    if (parameters.empty())
        return {};
    std::string text = "<";
    for (std::size_t i = 0; i < parameters.size(); ++i) {
        if (i != 0)
            text += ", ";
        text += parameters[i].name;
        if (!parameters[i].bounds.empty()) {
            text += ": ";
            for (std::size_t bound = 0; bound < parameters[i].bounds.size(); ++bound) {
                if (bound != 0)
                    text += " + ";
                text += TypeText(parameters[i].bounds[bound].get());
            }
        }
    }
    return text + ">";
}

std::string FunctionSignature(const FuncDecl &function) {
    std::string text = function.isPublic ? "pub " : "";
    text += "func " + function.name + TypeParams(function.typeParams) + "(";
    for (std::size_t i = 0; i < function.params.size(); ++i) {
        if (i != 0)
            text += ", ";
        const auto &parameter = function.params[i];
        if (parameter.isVariadic)
            text += "...";
        else
            text += (parameter.isFormat ? "#Format() " : "") + parameter.name + ": " + TypeText(parameter.type.get());
    }
    text += ')';
    if (function.returnType)
        text += " -> " + TypeText(function.returnType->get());
    return text;
}

std::string ExternFunctionSignature(const ExternFuncDecl &function) {
    std::string text = function.isPublic ? "pub extern func " : "extern func ";
    text += function.name + "(";
    for (std::size_t i = 0; i < function.params.size(); ++i) {
        if (i != 0)
            text += ", ";
        const auto &parameter = function.params[i];
        text += parameter.name + ": " + TypeText(parameter.type.get());
    }
    if (function.isVariadic) {
        if (!function.params.empty())
            text += ", ";
        text += "...";
    }
    text += ')';
    if (function.returnType)
        text += " -> " + TypeText(function.returnType->get());
    return text;
}

std::string DeclName(const Decl &decl) {
    if (const auto *value = dynamic_cast<const FuncDecl *>(&decl))
        return value->name;
    if (const auto *value = dynamic_cast<const StructDecl *>(&decl))
        return value->name;
    if (const auto *value = dynamic_cast<const EnumDecl *>(&decl))
        return value->name;
    if (const auto *value = dynamic_cast<const UnionDecl *>(&decl))
        return value->name;
    if (const auto *value = dynamic_cast<const InterfaceDecl *>(&decl))
        return value->name;
    if (const auto *value = dynamic_cast<const ModuleDecl *>(&decl))
        return value->name;
    if (const auto *value = dynamic_cast<const ConstDecl *>(&decl))
        return value->name;
    if (const auto *value = dynamic_cast<const TypeAliasDecl *>(&decl))
        return value->name;
    if (const auto *value = dynamic_cast<const ExternFuncDecl *>(&decl))
        return value->name;
    if (const auto *value = dynamic_cast<const ExternVarDecl *>(&decl))
        return value->name;
    if (const auto *value = dynamic_cast<const ImplDecl *>(&decl))
        return "extend-" + value->typeName;
    return {};
}

std::string DeclKind(const Decl &decl) {
    if (dynamic_cast<const FuncDecl *>(&decl))
        return "function";
    if (dynamic_cast<const StructDecl *>(&decl))
        return "struct";
    if (const auto *value = dynamic_cast<const EnumDecl *>(&decl))
        return value->IsVariant() ? "variant" : "enum";
    if (dynamic_cast<const UnionDecl *>(&decl))
        return "union";
    if (dynamic_cast<const InterfaceDecl *>(&decl))
        return "interface";
    if (dynamic_cast<const ModuleDecl *>(&decl))
        return "module";
    if (dynamic_cast<const ConstDecl *>(&decl))
        return "constant";
    if (dynamic_cast<const TypeAliasDecl *>(&decl))
        return "type alias";
    if (dynamic_cast<const ExternFuncDecl *>(&decl) || dynamic_cast<const ExternVarDecl *>(&decl))
        return "extern";
    if (dynamic_cast<const ImplDecl *>(&decl))
        return "extension";
    return "declaration";
}

std::string DeclSignature(const Decl &decl) {
    if (const auto *function = dynamic_cast<const FuncDecl *>(&decl))
        return FunctionSignature(*function);
    if (const auto *value = dynamic_cast<const StructDecl *>(&decl))
        return std::string(value->isPublic ? "pub " : "") + (value->intrinsicName.empty() ? "" : "intrinsic ") +
               "struct " + value->name + TypeParams(value->typeParams);
    if (const auto *value = dynamic_cast<const EnumDecl *>(&decl))
        return std::string(value->isPublic ? "pub " : "") + (value->IsVariant() ? "variant " : "enum ") + value->name +
               TypeParams(value->typeParams);
    if (const auto *value = dynamic_cast<const UnionDecl *>(&decl))
        return std::string(value->isPublic ? "pub " : "") + "union " + value->name;
    if (const auto *value = dynamic_cast<const InterfaceDecl *>(&decl))
        return std::string(value->isPublic ? "pub " : "") + "interface " + value->name;
    if (const auto *value = dynamic_cast<const ModuleDecl *>(&decl))
        return std::string(value->isPublic ? "pub " : "") + "module " + value->name;
    if (const auto *value = dynamic_cast<const ConstDecl *>(&decl))
        return std::string(value->isPublic ? "pub " : "") + "const " + value->name +
               (value->type ? ": " + TypeText(value->type->get()) : "");
    if (const auto *value = dynamic_cast<const TypeAliasDecl *>(&decl))
        return std::string(value->isPublic ? "pub " : "") + (value->intrinsicName.empty() ? "" : "intrinsic ") +
               "type " + value->name + (value->intrinsicName.empty() ? " = " + TypeText(value->type.get()) : ";");
    if (const auto *value = dynamic_cast<const ImplDecl *>(&decl))
        return "extend " + value->typeName;
    if (const auto *value = dynamic_cast<const ExternVarDecl *>(&decl))
        return std::string(value->isPublic ? "pub " : "") + "extern " + value->name + ": " +
               TypeText(value->type.get());
    if (const auto *value = dynamic_cast<const ExternFuncDecl *>(&decl))
        return ExternFunctionSignature(*value);
    return DeclName(decl);
}

std::string CaseSignature(const EnumDecl &declaration, const EnumDecl::Variant &variant) {
    std::string text = variant.name;
    if (!declaration.IsVariant()) {
        if (variant.discriminant) {
            text += " = " + *variant.discriminant;
        }
        return text;
    }
    if (!variant.fields.empty()) {
        text += '(';
        for (std::size_t index = 0; index < variant.fields.size(); ++index) {
            if (index != 0) {
                text += ", ";
            }
            text += TypeText(variant.fields[index].get());
        }
        text += ')';
    }
    else if (!variant.namedFields.empty()) {
        text += " { ";
        for (const EnumDecl::Variant::NamedField &field : variant.namedFields) {
            text += field.name + ": " + TypeText(field.type.get()) + "; ";
        }
        text += '}';
    }
    return text;
}

bool Visible(const Decl &decl, const bool includePrivate, const bool containingModulesPublic,
             const std::unordered_set<std::string> &publicTypes) {
    if (includePrivate)
        return true;
    if (const auto *extension = dynamic_cast<const ImplDecl *>(&decl)) {
        const std::string typeName = extension->typeName.substr(0, extension->typeName.find('<'));
        return containingModulesPublic && publicTypes.contains(typeName) &&
               (std::ranges::any_of(extension->methods, [](const auto &method) { return method->isPublic; }) ||
                std::ranges::any_of(extension->constants, [](const auto &constant) { return constant->isPublic; }));
    }
    if (const auto *block = dynamic_cast<const ExternBlockDecl *>(&decl)) {
        return containingModulesPublic &&
               std::ranges::any_of(block->items, [](const auto &item) { return item->isPublic; });
    }
    return containingModulesPublic && decl.isPublic;
}

void CollectPublicTypes(const Decl &decl, const bool containingModulesPublic,
                        std::unordered_set<std::string> &publicTypes, const std::string &prefix) {
    if (const auto *module = dynamic_cast<const ModuleDecl *>(&decl)) {
        const bool modulePublic = containingModulesPublic && module->isPublic;
        const std::string qualified = prefix.empty() ? module->name : prefix + "::" + module->name;
        for (const auto &item : module->items) {
            CollectPublicTypes(*item, modulePublic, publicTypes, qualified);
        }
        return;
    }
    if (!containingModulesPublic || !decl.isPublic ||
        !(dynamic_cast<const StructDecl *>(&decl) || dynamic_cast<const EnumDecl *>(&decl) ||
          dynamic_cast<const UnionDecl *>(&decl) || dynamic_cast<const InterfaceDecl *>(&decl))) {
        return;
    }
    const std::string name = DeclName(decl);
    publicTypes.insert(name);
    if (!prefix.empty()) {
        publicTypes.insert(prefix + "::" + name);
    }
}
} // namespace Rux::Documentation
