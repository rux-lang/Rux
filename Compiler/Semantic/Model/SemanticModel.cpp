// The analyzed model's accessors, and the identity substitution that turns a
// generic declaration's recorded linker name into one instantiation's.

#include "Semantic/Model/SemanticModel.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdlib>
#include <utility>

namespace Rux {
const ConstDecl *SemanticModel::TryGetAssociatedConstant(const Expr &expression) const noexcept {
    const auto found = facts.associatedConstants.find(&expression);
    return found == facts.associatedConstants.end() ? nullptr : found->second;
}

namespace {
bool IdentityCharacter(const char character) {
    return std::isalnum(static_cast<unsigned char>(character)) || character == '_';
}

/// Replace type parameters inside a recorded linker name with the concrete types of one instantiation. This is what
/// turns a generic declaration's symbolic identity into the name a specific emitted instance links under, without
/// re-running overload resolution or mangling.
std::string SubstituteIdentityName(std::string name, const std::unordered_map<std::string, TypeRef> &substitutions) {
    for (const auto &[parameter, type] : substitutions) {
        std::size_t position = 0;
        while ((position = name.find(parameter, position)) != std::string::npos) {
            const bool beginsIdentifier = position != 0 && IdentityCharacter(name[position - 1]);
            const std::size_t end = position + parameter.size();
            const bool endsIdentifier = end < name.size() && IdentityCharacter(name[end]);
            if (beginsIdentifier || endsIdentifier) {
                position = end;
                continue;
            }
            const std::string replacement = type.ToString();
            name.replace(position, parameter.size(), replacement);
            position += replacement.size();
        }
    }
    return name;
}

/// Substitute type parameters throughout a type, including nested ones, so a generic argument is resolved wherever it
/// appears.
TypeRef SubstituteIdentityType(TypeRef type, const std::unordered_map<std::string, TypeRef> &substitutions) {
    if (type.kind == TypeRef::Kind::TypeParam || type.kind == TypeRef::Kind::Named) {
        if (const auto substitution = substitutions.find(type.name); substitution != substitutions.end()) {
            return substitution->second;
        }
        if (type.kind == TypeRef::Kind::Named) {
            type.name = SubstituteIdentityName(std::move(type.name), substitutions);
        }
    }
    for (auto &inner : type.inner) {
        inner = SubstituteIdentityType(std::move(inner), substitutions);
    }
    return type;
}

/// Encode a type into the form a linker name embeds, so two instantiations differing only by argument get distinct
/// symbols.
std::string MangleIdentityType(const TypeRef &type) {
    std::string name;
    for (const char character : type.ToString()) {
        name += IdentityCharacter(character) ? character : '_';
    }
    return name.empty() ? "_" : name;
}

const TypeRef &RequireSubstitution(const std::unordered_map<std::string, TypeRef> &substitutions,
                                   const std::string &parameter) {
    const auto substitution = substitutions.find(parameter);
    assert(substitution != substitutions.end() && "concrete call identity is missing a semantic substitution");
    if (substitution == substitutions.end()) {
        std::abort();
    }
    return substitution->second;
}
} // namespace

std::string ConstraintWitnessKey(const std::string &interfaceName, const TypeRef &type) {
    const TypeRef &owner =
        (type.kind == TypeRef::Kind::Pointer || type.kind == TypeRef::Kind::Reference) && !type.inner.empty()
            ? type.inner.front()
            : type;
    std::string name = owner.kind == TypeRef::Kind::Named ? owner.name : owner.ToString();
    // A slice keeps its element in the name, because `extend int[]` and `extend char8[]` are different method sets. Any
    // other generic instantiation shares one, so it reduces to the declaration that owns it.
    if (const std::size_t arguments = name.find('<'); arguments != std::string::npos && !name.starts_with("Slice<")) {
        name.resize(arguments);
    }
    return interfaceName + "@" + name;
}

std::string
ResolvedCallableBinding::LinkerNameFor(const std::unordered_map<std::string, TypeRef> &concreteSubstitutions) const {
    if (linkerNameBase.empty()) {
        return linkerName;
    }

    std::string name = linkerNameBase;
    if (linkerNameHasOverloadSignature) {
        name += "__";
        for (std::size_t i = 0; i < linkerOverloadTypes.size(); ++i) {
            if (i != 0) {
                name += "_";
            }
            name += MangleIdentityType(SubstituteIdentityType(linkerOverloadTypes[i], concreteSubstitutions));
        }
    }
    for (const auto &parameter : linkerSpecializationParameters) {
        name += "_" + MangleIdentityType(RequireSubstitution(concreteSubstitutions, parameter));
    }
    return name;
}

ResolvedCallableBinding
ResolvedCallableBinding::Instantiate(const std::unordered_map<std::string, TypeRef> &contextSubstitutions) const {
    ResolvedCallableBinding concrete = *this;
    for (auto &[_, type] : concrete.substitutions) {
        type = SubstituteIdentityType(std::move(type), contextSubstitutions);
    }
    if (concrete.receiverType) {
        concrete.receiverType = SubstituteIdentityType(std::move(*concrete.receiverType), contextSubstitutions);
    }
    concrete.linkerName = LinkerNameFor(concrete.substitutions);
    return concrete;
}

SemanticModel::SemanticModel(std::vector<SemanticDiagnostic> inputDiagnostics, std::vector<SemanticSymbol> inputSymbols,
                             std::vector<const Module *> inputModules, CompileTimeContext inputCompileTimeContext,
                             SemanticFacts inputFacts)
    : diagnostics(std::move(inputDiagnostics))
    , symbols(std::move(inputSymbols))
    , modules(std::move(inputModules))
    , compileTimeContext(std::move(inputCompileTimeContext))
    , facts(std::move(inputFacts)) {
}

bool SemanticModel::HasErrors() const noexcept {
    return std::ranges::any_of(
        diagnostics, [](const SemanticDiagnostic &d) { return d.severity == SemanticDiagnostic::Severity::Error; });
}

const TypeRef *SemanticModel::TryGetType(const Expr &expression) const noexcept {
    const auto type = facts.expressionTypes.find(&expression);
    return type == facts.expressionTypes.end() ? nullptr : &type->second;
}

const TypeRef *SemanticModel::TryGetType(const TypeExpr &typeNode) const noexcept {
    const auto type = facts.typeNodeTypes.find(&typeNode);
    return type == facts.typeNodeTypes.end() ? nullptr : &type->second;
}

const TypeRef *SemanticModel::TryGetType(const Pattern &pattern) const noexcept {
    const auto type = facts.patternTypes.find(&pattern);
    return type == facts.patternTypes.end() ? nullptr : &type->second;
}

const ResolvedCasePattern *SemanticModel::TryGetCasePattern(const EnumPattern &pattern) const noexcept {
    const auto fact = facts.casePatterns.find(&pattern);
    return fact == facts.casePatterns.end() ? nullptr : &fact->second;
}

const ResolvedVariantEquality *SemanticModel::TryGetVariantEquality(const BinaryExpr &expression) const noexcept {
    const auto fact = facts.variantEqualities.find(&expression);
    return fact == facts.variantEqualities.end() ? nullptr : &fact->second;
}

const VariantEqualityPlan *SemanticModel::TryGetVariantEqualityPlan(const TypeRef &type) const noexcept {
    const auto plan = facts.variantEqualityPlans.find(type.ToString());
    return plan == facts.variantEqualityPlans.end() ? nullptr : &plan->second;
}

const ValueConsumption *SemanticModel::TryGetConsumption(const Expr &expression) const noexcept {
    const auto consumption = facts.valueConsumptions.find(&expression);
    return consumption == facts.valueConsumptions.end() ? nullptr : &consumption->second;
}

const ValueCopy *SemanticModel::TryGetCopy(const Expr &expression) const noexcept {
    const auto copy = facts.valueCopies.find(&expression);
    return copy == facts.valueCopies.end() ? nullptr : &copy->second;
}

const ResolvedCallableBinding *SemanticModel::TryGetCallableBinding(const CallExpr &call) const noexcept {
    const auto binding = facts.callableBindings.find(&call);
    return binding == facts.callableBindings.end() ? nullptr : &binding->second;
}

const ResolvedDefaultConstructor *SemanticModel::TryGetDefaultConstructor(const LetStmt &statement) const noexcept {
    const auto constructor = facts.defaultConstructors.find(&statement);
    return constructor == facts.defaultConstructors.end() ? nullptr : &constructor->second;
}

bool SemanticModel::IsEffectivelyPublic(const Decl &declaration) const noexcept {
    const auto visibility = facts.effectiveVisibilities.find(&declaration);
    return visibility != facts.effectiveVisibilities.end() && visibility->second;
}

const ResolvedSymbolIdentity *SemanticModel::TryGetSymbolIdentity(const Decl &declaration) const noexcept {
    const auto identity = facts.symbolIdentities.find(&declaration);
    return identity == facts.symbolIdentities.end() ? nullptr : &identity->second;
}

const ResolvedVtableIdentity *SemanticModel::TryGetVtableIdentity(const ImplDecl &declaration) const noexcept {
    const auto identity = facts.vtableIdentities.find(&declaration);
    return identity == facts.vtableIdentities.end() ? nullptr : &identity->second;
}

const ResolvedConstraintWitness *SemanticModel::TryGetConstraintWitness(const std::string &interfaceName,
                                                                        const TypeRef &type) const noexcept {
    const auto witness = facts.constraintWitnesses.find(ConstraintWitnessKey(interfaceName, type));
    return witness == facts.constraintWitnesses.end() ? nullptr : &witness->second;
}

const ResolvedPropagation *SemanticModel::TryGetPropagation(const TryExpr &expression) const noexcept {
    const auto propagation = facts.propagations.find(&expression);
    return propagation == facts.propagations.end() ? nullptr : &propagation->second;
}

const ResolvedCoalescing *SemanticModel::TryGetCoalescing(const BinaryExpr &expression) const noexcept {
    const auto coalescing = facts.coalescings.find(&expression);
    return coalescing == facts.coalescings.end() ? nullptr : &coalescing->second;
}

const ResolvedIndexOperator *SemanticModel::TryGetIndexOperator(const IndexExpr &expression) const noexcept {
    const auto indexOperator = facts.indexOperators.find(&expression);
    return indexOperator == facts.indexOperators.end() ? nullptr : &indexOperator->second;
}

const ResolvedIndexAssignment *SemanticModel::TryGetIndexAssignment(const IndexExpr &expression) const noexcept {
    const auto assignment = facts.indexAssignments.find(&expression);
    return assignment == facts.indexAssignments.end() ? nullptr : &assignment->second;
}

const ResolvedIteration *SemanticModel::TryGetIteration(const ForStmt &statement) const noexcept {
    const auto iteration = facts.iterations.find(&statement);
    return iteration == facts.iterations.end() ? nullptr : &iteration->second;
}

const ResolvedTypeLayout *SemanticModel::TryGetLayout(const TypeRef &type) const noexcept {
    const auto layout = facts.typeLayouts.find(type.ToString());
    return layout == facts.typeLayouts.end() ? nullptr : &layout->second;
}

const ResolvedTypeLayout *SemanticModel::TryGetLayout(const TypeExpr &typeNode) const noexcept {
    const TypeRef *type = TryGetType(typeNode);
    return type ? TryGetLayout(*type) : nullptr;
}

const TypeProperties *SemanticModel::TryGetProperties(const TypeRef &type) const noexcept {
    const auto properties = facts.typeProperties.find(type.ToString());
    return properties == facts.typeProperties.end() ? nullptr : &properties->second;
}

const TypeProperties *SemanticModel::TryGetProperties(const Expr &expression) const noexcept {
    const TypeRef *type = TryGetType(expression);
    return type ? TryGetProperties(*type) : nullptr;
}

const TypeProperties *SemanticModel::TryGetProperties(const TypeExpr &typeNode) const noexcept {
    const TypeRef *type = TryGetType(typeNode);
    return type ? TryGetProperties(*type) : nullptr;
}

const TypeProperties *SemanticModel::TryGetProperties(const Pattern &pattern) const noexcept {
    const TypeRef *type = TryGetType(pattern);
    return type ? TryGetProperties(*type) : nullptr;
}

const DropGluePlan *SemanticModel::TryGetDropGlue(const TypeRef &type) const noexcept {
    const auto glue = facts.dropGluePlans.find(type.ToString());
    return glue == facts.dropGluePlans.end() ? nullptr : &glue->second;
}

const std::unordered_map<std::string, DropGluePlan> &SemanticModel::DropGluePlans() const noexcept {
    return facts.dropGluePlans;
}

const std::uint64_t *SemanticModel::TryGetTypeQueryValue(const TypeQueryExpr &expression) const noexcept {
    const auto value = facts.typeQueryValues.find(&expression);
    return value == facts.typeQueryValues.end() ? nullptr : &value->second;
}
} // namespace Rux
