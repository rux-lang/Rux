#include "Semantic/Conditional/ConditionalEvaluatorInternal.h"
#include "Types/PrimitiveCatalog.h"

#include <algorithm>
#include <format>
#include <limits>
#include <unordered_set>

namespace Rux {
void ConditionalEvaluator::Impl::BindImportedDeclaration(const Decl &declaration, const std::vector<Module *> &modules,
                                                         const Module &source, const bool external) {
    if (const auto *constant = dynamic_cast<const ConstDecl *>(&declaration)) {
        if (!constant->intrinsicName.empty()) {
            intrinsicBindings[constant->name] = constant->intrinsicName;
            ruxImports.insert(constant->name);
            externalIntrinsicTypes.erase(constant->name);
            if (external) {
                const StructDecl *type = nullptr;
                const auto find = [&](this auto &&self, const std::vector<DeclPtr> &items) -> void {
                    for (const auto &item : items) {
                        if (const auto *structure = dynamic_cast<const StructDecl *>(item.get());
                            structure && structure->name == constant->intrinsicName) {
                            type = structure;
                        }
                        else if (const auto *nested = dynamic_cast<const ModuleDecl *>(item.get())) {
                            self(nested->items);
                        }
                    }
                };
                for (const Module *module : modules) {
                    find(module->items);
                }
                if (type) {
                    externalIntrinsicTypes.emplace(constant->name, type);
                }
            }
        }
        else if (external && constant->value) {
            importedConstants.insert_or_assign(constant->name, ConstantBinding{constant, modules, &source});
        }
        return;
    }
    if (const auto *enumeration = dynamic_cast<const EnumDecl *>(&declaration)) {
        ruxImports.insert(enumeration->name);
        auto &variants = enumVariants[enumeration->name];
        variants.clear();
        for (const auto &variant : enumeration->variants) {
            variants.push_back(variant.name);
        }
        if (enumeration->IsVariant()) {
            programVariantNames.insert(enumeration->name);
        }
        return;
    }
    if (const auto *extension = dynamic_cast<const ImplDecl *>(&declaration)) {
        // A package sees its own extensions of a primitive without importing them.
        if (const PrimitiveInfo *primitive = FindPrimitive(extension->typeName); primitive && !external) {
            BindPrimitiveExtension(*extension, std::string(primitive->name), modules, source, false);
        }
        return;
    }
    const auto *alias = dynamic_cast<const TypeAliasDecl *>(&declaration);
    if (!alias) {
        return;
    }
    const auto *named = dynamic_cast<const NamedTypeExpr *>(alias->type.get());
    if (!named || !named->typeArgs.empty()) {
        return;
    }
    // A primitive's constants are keyed by its canonical name, so an alias inherits them under that name.
    const std::string target(CanonicalPrimitiveName(named->name));
    if (FindPrimitive(target)) {
        aliasPrimitiveTargets.insert_or_assign(alias->name, target);
    }
    else if (const auto aliased = aliasPrimitiveTargets.find(target); aliased != aliasPrimitiveTargets.end()) {
        aliasPrimitiveTargets.insert_or_assign(alias->name, aliased->second);
    }
    else {
        for (const Module *module : modules) {
            const auto collect = [&](this auto &&self, const Decl *item) -> void {
                if (const auto *extension = dynamic_cast<const ImplDecl *>(item)) {
                    if (extension->typeName == target) {
                        BindPrimitiveExtension(*extension, alias->name, modules, *module, external);
                    }
                }
                else if (const auto *nested = dynamic_cast<const ModuleDecl *>(item);
                         nested && (!external || nested->isPublic)) {
                    for (const auto &child : nested->items)
                        self(child.get());
                }
            };
            for (const auto &item : module->items)
                collect(item.get());
            for (const auto &[selected, owner] : selectedDeclarations) {
                if (owner == module)
                    collect(selected);
            }
        }
    }
    if (!activeTypeAliases.contains(alias)) {
        const auto inherit = [&](const auto &bindings) {
            std::vector<std::pair<std::string, ConstantBinding>> inherited;
            for (const auto &[key, value] : bindings) {
                if (key.starts_with(target + "::"))
                    inherited.emplace_back(alias->name + key.substr(target.size()), value);
            }
            for (const auto &[key, value] : inherited) {
                const auto found = associatedDeclarations.find(key);
                if (found != associatedDeclarations.end() && found->second.declaration != value.declaration) {
                    found->second.declaration = nullptr;
                }
                else
                    associatedDeclarations.insert_or_assign(key, value);
            }
        };
        if (external) {
            Impl owner(context, modules, resolveImports);
            owner.activeTypeAliases = activeTypeAliases;
            owner.activeTypeAliases.insert(alias);
            owner.SetSourceContext(source.name, {}, {});
            owner.SetImports(source);
            inherit(owner.associatedDeclarations);
        }
        else
            inherit(associatedDeclarations);
    }
}

void ConditionalEvaluator::Impl::BindPrimitiveExtension(const ImplDecl &extension, const std::string &typeName,
                                                        const std::vector<Module *> &modules, const Module &source,
                                                        const bool external) {
    for (const auto &member : extension.constants) {
        if (external && !member->isPublic) {
            continue;
        }
        const std::string key = typeName + "::" + member->name;
        const auto previous = associatedDeclarations.find(key);
        if (previous != associatedDeclarations.end() && previous->second.declaration != member.get()) {
            previous->second.declaration = nullptr;
        }
        else {
            associatedDeclarations.insert_or_assign(key, ConstantBinding{member.get(), modules, &source});
        }
    }
}

void ConditionalEvaluator::Impl::BindPrimitiveExtensions(const TypeRef::Kind kind,
                                                         const std::vector<Module *> &modules) {
    for (const Module *module : modules) {
        const auto collect = [&](this auto &&self, const std::vector<DeclPtr> &items) -> void {
            for (const auto &item : items) {
                if (const auto *extension = dynamic_cast<const ImplDecl *>(item.get())) {
                    if (const PrimitiveInfo *primitive = FindPrimitive(extension->typeName);
                        primitive && primitive->kind == kind) {
                        BindPrimitiveExtension(*extension, std::string(primitive->name), modules, *module, true);
                    }
                }
                else if (const auto *nested = dynamic_cast<const ModuleDecl *>(item.get());
                         nested && nested->isPublic) {
                    self(nested->items);
                }
            }
        };
        collect(module->items);
    }
}

void ConditionalEvaluator::Impl::ImportDeclarations(const UseDecl &use) {
    if (!resolveImports || use.path.empty()) {
        return;
    }
    const std::vector<Module *> imported = resolveImports(use.path.front(), currentFile);
    std::vector<std::string> modulePath(use.path.begin() + 1, use.path.end());
    std::vector<std::string> names = use.names;
    if (use.kind == UseDecl::Kind::Single && !modulePath.empty()) {
        names.push_back(modulePath.back());
        modulePath.pop_back();
    }
    // A primitive is built in, so importing one by name, or a glob reaching a module that extends it, imports the
    // package's extensions of it.
    std::unordered_set<TypeRef::Kind> extended;
    for (const Module *module : imported) {
        const auto visit = [&](this auto &&self, const std::vector<DeclPtr> &items, const std::size_t depth) -> void {
            for (const auto &item : items) {
                const auto *extension = dynamic_cast<const ImplDecl *>(item.get());
                if (extension && depth == modulePath.size()) {
                    const PrimitiveInfo *primitive = FindPrimitive(extension->typeName);
                    const auto imports = [&](const std::string &name) {
                        const PrimitiveInfo *named = FindPrimitive(name);
                        return named && primitive && named->kind == primitive->kind;
                    };
                    if (primitive && (use.kind == UseDecl::Kind::Glob || std::ranges::any_of(names, imports))) {
                        extended.insert(primitive->kind);
                    }
                    continue;
                }
                if (!item || !item->isPublic) {
                    continue;
                }
                if (depth < modulePath.size()) {
                    if (const auto *nested = dynamic_cast<const ModuleDecl *>(item.get());
                        nested && nested->name == modulePath[depth]) {
                        self(nested->items, depth + 1);
                    }
                    continue;
                }
                std::string name;
                if (const auto *constant = dynamic_cast<const ConstDecl *>(item.get())) {
                    name = constant->name;
                }
                else if (const auto *alias = dynamic_cast<const TypeAliasDecl *>(item.get())) {
                    name = alias->name;
                }
                else if (const auto *enumeration = dynamic_cast<const EnumDecl *>(item.get())) {
                    name = enumeration->name;
                }
                if (use.kind == UseDecl::Kind::Glob || std::ranges::contains(names, name)) {
                    BindImportedDeclaration(*item, imported, *module);
                }
            }
        };
        visit(module->items, 0);
    }
    for (const TypeRef::Kind kind : extended) {
        BindPrimitiveExtensions(kind, imported);
    }
}

std::optional<CompileTimeValue> ConditionalEvaluator::Impl::EvalDeclaredConstant(const ConstantBinding &binding,
                                                                                 const SourceLocation location) {
    if (!binding.declaration) {
        EmitError(location, "associated constant has ambiguous imported declarations");
        reportedError = true;
        return std::nullopt;
    }
    const ConstDecl &constant = *binding.declaration;
    if (activeAssociatedConstants.contains(&constant)) {
        EmitError(location, std::format("compile-time constant '{}' depends on itself", constant.name));
        reportedError = true;
        return std::nullopt;
    }
    if (!constant.intrinsicName.empty()) {
        const auto *named = constant.type ? dynamic_cast<const NamedTypeExpr *>(constant.type->get()) : nullptr;
        const auto type = named ? PrimitiveTypeFromName(named->name) : std::nullopt;
        if (type && (type->kind == TypeRef::Kind::Float32 || type->kind == TypeRef::Kind::Float64)) {
            if (constant.name == "Infinity") {
                return Value{std::numeric_limits<double>::infinity()};
            }
            if (constant.name == "NaN") {
                return Value{std::numeric_limits<double>::quiet_NaN()};
            }
        }
        return std::nullopt;
    }
    Impl owner(context, binding.modules, resolveImports);
    owner.activeAssociatedConstants = activeAssociatedConstants;
    owner.activeAssociatedConstants.insert(&constant);
    owner.SetSourceContext(binding.source->name, {}, {});
    owner.selectedDeclarations = selectedDeclarations;
    owner.SetImports(*binding.source);
    if (binding.source->name == currentFile) {
        // Selected declarations may already have moved out of a conditional branch.
        // Their indexed bindings remain valid until the enclosing fold completes.
        owner.associatedDeclarations = associatedDeclarations;
        owner.aliasPrimitiveTargets = aliasPrimitiveTargets;
        owner.importedConstants = importedConstants;
        owner.intrinsicBindings = intrinsicBindings;
        owner.externalIntrinsicTypes = externalIntrinsicTypes;
        owner.constExprs = constExprs;
        owner.constSignedIntegerWidths = constSignedIntegerWidths;
        owner.constUnsignedIntegerWidths = constUnsignedIntegerWidths;
    }
    owner.RegisterConstantImpl(constant);
    IdentExpr reference;
    reference.name = constant.name;
    reference.location = constant.location;
    auto value = owner.EvalConstantReference(reference);
    auto errors = owner.TakeDiagnostics();
    diags.insert(diags.end(), std::make_move_iterator(errors.begin()), std::make_move_iterator(errors.end()));
    reportedError = reportedError || owner.reportedError;
    return value;
}
} // namespace Rux
