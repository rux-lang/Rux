#include "Semantic/Conditional/ConditionalEvaluatorInternal.h"
#include "Types/PrimitiveCatalog.h"

#include <algorithm>
#include <format>
#include <limits>

namespace Rux {
void ConditionalEvaluator::Impl::BindImportedDeclaration(const Decl &declaration, const std::vector<Module *> &modules,
                                                         const Module &source, const bool external) {
    if (const auto *constant = dynamic_cast<const ConstDecl *>(&declaration)) {
        if (!constant->intrinsicName.empty()) {
            intrinsicBindings[constant->name] = constant->intrinsicName;
            ruxImports.insert(constant->name);
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
    const auto *alias = dynamic_cast<const TypeAliasDecl *>(&declaration);
    if (!alias) {
        return;
    }
    std::string target = alias->intrinsicName;
    if (target.empty()) {
        const auto *named = dynamic_cast<const NamedTypeExpr *>(alias->type.get());
        if (!named || !named->typeArgs.empty()) {
            return;
        }
        target = named->name;
    }
    const auto primitive = PrimitiveTypeFromName(target);
    for (const Module *module : modules) {
        const auto collect = [&](this auto &&self, const Decl *item) -> void {
            if (const auto *extension = dynamic_cast<const ImplDecl *>(item)) {
                const auto extended = PrimitiveTypeFromName(extension->typeName);
                if (extension->typeName != target && (!primitive || !extended || *primitive != *extended)) {
                    return;
                }
                for (const auto &member : extension->constants) {
                    if (external && !member->isPublic) {
                        continue;
                    }
                    const std::string key = alias->name + "::" + member->name;
                    const auto previous = associatedDeclarations.find(key);
                    if (previous != associatedDeclarations.end() && previous->second.declaration != member.get()) {
                        previous->second.declaration = nullptr;
                    }
                    else {
                        associatedDeclarations.insert_or_assign(key, ConstantBinding{member.get(), modules, module});
                    }
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
    if (alias->intrinsicName.empty() && (!primitive || !primitive->IsString()) && !activeTypeAliases.contains(alias)) {
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

void ConditionalEvaluator::Impl::ImportDeclarations(const UseDecl &use) {
    if (!resolveImports || use.path.empty()) {
        return;
    }
    const std::vector<Module *> imported = resolveImports(use.path.front());
    std::vector<std::string> modulePath(use.path.begin() + 1, use.path.end());
    std::vector<std::string> names = use.names;
    if (use.kind == UseDecl::Kind::Single && !modulePath.empty()) {
        names.push_back(modulePath.back());
        modulePath.pop_back();
    }
    for (const Module *module : imported) {
        const auto visit = [&](this auto &&self, const std::vector<DeclPtr> &items, const std::size_t depth) -> void {
            for (const auto &item : items) {
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
        owner.importedConstants = importedConstants;
        owner.intrinsicBindings = intrinsicBindings;
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
