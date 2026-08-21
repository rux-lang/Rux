#include "Semantic/Detail/SemanticAnalyzerContext.h"

#include <algorithm>
#include <cctype>
#include <ranges>
#include <utility>

namespace Rux::SemanticDetail {
namespace {
using Substitutions = std::unordered_map<std::string, TypeRef>;

Substitutions BindTypeArguments(const std::vector<TypeParameter> &parameters, const std::vector<TypeRef> &arguments) {
    Substitutions substitutions;
    const std::size_t count = std::min(parameters.size(), arguments.size());
    for (std::size_t index = 0; index < count; ++index) {
        substitutions.emplace(parameters[index].name, arguments[index]);
    }
    return substitutions;
}

DropGlueStep AggregateStep(const DropGlueStep::Kind kind, TypeRef type, std::string name, const std::uint64_t ordinal,
                           std::vector<DropGlueStep> children) {
    DropGlueStep step;
    step.kind = kind;
    step.type = std::move(type);
    step.name = std::move(name);
    step.ordinal = ordinal;
    step.children = std::move(children);
    return step;
}
} // namespace

bool SemanticAnalyzerContext::TypeImplementsDrop(const std::string &baseName) const {
    const auto implementations = typeImplementsInterfaces.find(baseName);
    return implementations != typeImplementsInterfaces.end() &&
           std::ranges::any_of(implementations->second,
                               [](const std::string &name) { return name == "Drop" || name.ends_with("::Drop"); });
}

std::string SemanticAnalyzerContext::DropGlueSymbol(const TypeRef &type) {
    std::string symbol = "__rux_drop__";
    constexpr char HexDigits[] = "0123456789abcdef";
    for (const char character : type.ToString()) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isalnum(byte)) {
            symbol += character;
        }
        else {
            symbol += '_';
            symbol += HexDigits[byte >> 4U];
            symbol += HexDigits[byte & 0x0fU];
            symbol += '_';
        }
    }
    return symbol;
}

std::vector<DropGlueStep> SemanticAnalyzerContext::BuildDropGlueSteps(const TypeRef &type,
                                                                      std::unordered_set<std::string> &activeTypes) {
    if (!ClassifyTypeProperties(type).IsDroppable()) {
        return {};
    }

    const std::string key = type.ToString();
    if (!activeTypes.insert(key).second) {
        return {};
    }

    std::vector<DropGlueStep> steps;
    if (type.kind == TypeRef::Kind::Array && !type.inner.empty()) {
        std::vector<DropGlueStep> elementSteps = BuildDropGlueSteps(type.inner.front(), activeTypes);
        if (!elementSteps.empty()) {
            DropGlueStep elements;
            elements.kind = DropGlueStep::Kind::ArrayElements;
            elements.type = type.inner.front();
            elements.count = type.arrayLength.value_or(0);
            elements.reverse = true;
            elements.children = std::move(elementSteps);
            steps.push_back(std::move(elements));
        }
    }
    else if (type.kind == TypeRef::Kind::Tuple) {
        for (std::size_t offset = 0; offset < type.inner.size(); ++offset) {
            const std::size_t index = type.inner.size() - offset - 1;
            std::vector<DropGlueStep> children = BuildDropGlueSteps(type.inner[index], activeTypes);
            if (!children.empty()) {
                steps.push_back(
                    AggregateStep(DropGlueStep::Kind::TupleElement, type.inner[index], {}, index, std::move(children)));
            }
        }
    }
    else if (type.kind == TypeRef::Kind::Named) {
        const std::string baseName = NamedBaseTypeName(type);
        if (TypeImplementsDrop(baseName)) {
            DropGlueStep invoke;
            invoke.kind = DropGlueStep::Kind::InvokeDrop;
            invoke.type = type;
            steps.push_back(std::move(invoke));
        }

        const std::vector<TypeRef> arguments = ParseTypeArgsFromTypeName(type.name);
        if (const auto structure = structDecls.find(baseName);
            structure != structDecls.end() && arguments.size() == structure->second->typeParams.size()) {
            const StructDecl &declaration = *structure->second;
            const Substitutions substitutions = BindTypeArguments(declaration.typeParams, arguments);
            for (std::size_t offset = 0; offset < declaration.fields.size(); ++offset) {
                const std::size_t index = declaration.fields.size() - offset - 1;
                const StructDecl::Field &field = declaration.fields[index];
                TypeRef fieldType = ResolveTypeWithSubstitution(*field.type, substitutions);
                std::vector<DropGlueStep> children = BuildDropGlueSteps(fieldType, activeTypes);
                if (!children.empty()) {
                    steps.push_back(AggregateStep(DropGlueStep::Kind::Field, std::move(fieldType), field.name, index,
                                                  std::move(children)));
                }
            }
        }
        else if (const auto enumeration = enumDecls.find(baseName);
                 enumeration != enumDecls.end() && arguments.size() == enumeration->second->typeParams.size()) {
            const EnumDecl &declaration = *enumeration->second;
            const Substitutions substitutions = BindTypeArguments(declaration.typeParams, arguments);
            for (std::size_t variantIndex = 0; variantIndex < declaration.variants.size(); ++variantIndex) {
                const EnumDecl::Variant &variant = declaration.variants[variantIndex];
                // One list for both spellings of a payload, in the order a construction of the variant writes them,
                // so a child step's ordinal indexes the same sequence its offset is measured along.
                std::vector<TypeRef> payloadTypes;
                for (const EnumDecl::Variant::NamedField &field : variant.namedFields) {
                    payloadTypes.push_back(ResolveTypeWithSubstitution(*field.type, substitutions));
                }
                for (const auto &field : variant.fields) {
                    payloadTypes.push_back(ResolveTypeWithSubstitution(*field, substitutions));
                }

                std::vector<DropGlueStep> payload;
                for (std::size_t offset = 0; offset < payloadTypes.size(); ++offset) {
                    const std::size_t index = payloadTypes.size() - offset - 1;
                    const bool named = index < variant.namedFields.size();
                    std::vector<DropGlueStep> children = BuildDropGlueSteps(payloadTypes[index], activeTypes);
                    if (!children.empty()) {
                        payload.push_back(AggregateStep(
                            named ? DropGlueStep::Kind::Field : DropGlueStep::Kind::TupleElement, payloadTypes[index],
                            named ? variant.namedFields[index].name : std::string{}, index, std::move(children)));
                    }
                }
                if (!payload.empty()) {
                    DropGlueStep step = AggregateStep(DropGlueStep::Kind::EnumVariant, type, variant.name, variantIndex,
                                                      std::move(payload));
                    step.payloadTypes = std::move(payloadTypes);
                    steps.push_back(std::move(step));
                }
            }
        }
    }

    activeTypes.erase(key);
    return steps;
}

void SemanticAnalyzerContext::SynthesizeResolvedDropGlue() {
    dropGluePlans.clear();
    const auto synthesize = [&](const TypeRef &type) {
        const std::string key = type.ToString();
        if (dropGluePlans.contains(key) || !ClassifyTypeProperties(type).IsDroppable()) {
            return;
        }
        std::unordered_set<std::string> activeTypes;
        std::vector<DropGlueStep> steps = BuildDropGlueSteps(type, activeTypes);
        if (!steps.empty()) {
            dropGluePlans.emplace(key, DropGluePlan{type, DropGlueSymbol(type), std::move(steps)});
        }
    };
    for (const auto &[_, type] : expressionTypes) {
        synthesize(type);
    }
    for (const auto &[_, type] : typeNodeTypes) {
        synthesize(type);
    }
    for (const auto &[_, type] : patternTypes) {
        synthesize(type);
    }
    for (const TypeRef &type : instantiatedTypes) {
        synthesize(type);
    }
    for (const auto &[name, declaration] : structDecls) {
        if (declaration->typeParams.empty()) {
            synthesize(TypeRef::MakeNamed(name));
        }
    }
    for (const auto &[name, declaration] : enumDecls) {
        if (declaration->typeParams.empty()) {
            synthesize(TypeRef::MakeNamed(name));
        }
    }
}
} // namespace Rux::SemanticDetail
