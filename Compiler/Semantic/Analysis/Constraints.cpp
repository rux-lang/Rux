// Generic constraint resolution: what an interface bound written on a type parameter means, and whether the type
// argument a call or a type reference supplies actually provides the operations that bound promises.

#include "Semantic/Analysis/AnalysisContext.h"

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Rux::SemanticDetail {
namespace {
/// The bound as the reader wrote it. A bound is a plain interface name, so anything with a type argument list or a
/// module path is reported at its own spelling rather than silently reduced to the last segment.
[[nodiscard]] const NamedTypeExpr *BoundName(const TypeExpr &bound) {
    return AstCast<const NamedTypeExpr>(&bound);
}
} // namespace

std::vector<AnalysisContext::ResolvedTypeBound>
AnalysisContext::ResolveTypeParameterBounds(const TypeParameter &parameter, const bool report,
                                            const std::vector<TypeParameter> *declaringParameters) {
    std::vector<ResolvedTypeBound> resolved;
    std::unordered_set<std::string> seen;
    for (const TypeExprPtr &bound : parameter.bounds) {
        const ScopedTypeOwner owner(*this, *bound);
        const NamedTypeExpr *named = BoundName(*bound);
        if (!named) {
            if (report) {
                EmitError(bound->location,
                          std::format("bound on type parameter '{}' must name an interface", parameter.name));
            }
            continue;
        }

        if (!seen.insert(named->name).second) {
            if (report) {
                EmitError(named->location,
                          std::format("type parameter '{}' repeats interface bound '{}'", parameter.name, named->name));
            }
            continue;
        }

        // A bound belongs to the declaration that wrote it, not to whoever calls it. Resolving a use site against the
        // caller's scope would demand that every caller import an interface it never names, and a caller that had not
        // would resolve the bound to nothing at all -- silently, since a use site does not report -- leaving lowering
        // with a conformance it was promised and cannot find. So a use site reads the program's interfaces directly,
        // and only the declaration itself is held to what is in scope where it was written.
        const InterfaceDecl *interface = nullptr;
        if (!report) {
            const auto known = interfaceDecls.find(NominalTypeName(named->name));
            interface = known == interfaceDecls.end() ? nullptr : known->second;
        }
        else {
            const Symbol *symbol = currentScope->Lookup(named->name);
            if (!symbol) {
                std::optional<std::string> help;
                if (const Symbol *suggestion = currentScope->Suggest(named->name)) {
                    help = std::format("did you mean '{}'?", suggestion->name);
                }
                EmitError(named->location, std::format("interface '{}' is not defined", named->name), {},
                          std::move(help));
                resolved.push_back({named->name, nullptr, named->location, {}});
                continue;
            }
            if (symbol->kind != Symbol::Kind::Interface) {
                EmitError(named->location,
                          std::format("name '{}' is a {}, not an interface, and cannot bound type parameter '{}'",
                                      named->name, SymbolKindName(symbol->kind), parameter.name),
                          {DeclarationNote(*symbol)});
                resolved.push_back({named->name, nullptr, named->location, {}});
                continue;
            }
            const auto declaration = interfaceDecls.find(NominalTypeName(named->name));
            interface = declaration == interfaceDecls.end() ? nullptr : declaration->second;
        }

        // A generic interface takes exactly as many type arguments as it declares parameters, and any other interface
        // takes none. The arguments are read in the declaring generic's terms: a use site names that declaration's
        // parameters while resolving them, so `I: Cursor<T>` refers to its own `T` whoever is calling.
        const std::size_t expected = interface ? interface->typeParams.size() : 0;
        if (named->typeArgs.size() != expected && (interface || !named->typeArgs.empty())) {
            if (report) {
                if (expected == 0) {
                    EmitError(named->location,
                              std::format("interface bound '{}' cannot take type arguments", named->name));
                }
                else {
                    EmitError(named->location,
                              std::format("interface bound '{}' takes {} type argument{}, but {} {} given", named->name,
                                          expected, expected == 1 ? "" : "s", named->typeArgs.size(),
                                          named->typeArgs.size() == 1 ? "was" : "were"));
                }
            }
            resolved.push_back({NominalTypeName(named->name), nullptr, named->location, {}});
            continue;
        }
        std::vector<TypeRef> arguments;
        if (!named->typeArgs.empty()) {
            const auto savedTypeParams = currentTypeParams;
            if (declaringParameters) {
                currentTypeParams.clear();
                AppendTypeParameterNames(currentTypeParams, *declaringParameters);
            }
            for (const TypeExprPtr &argument : named->typeArgs) {
                arguments.push_back(ResolveType(*argument));
            }
            currentTypeParams = savedTypeParams;
        }
        resolved.push_back({NominalTypeName(named->name), interface, named->location, std::move(arguments)});
    }
    return resolved;
}

void AnalysisContext::DeduceTypeArgumentsFromBounds(const std::vector<TypeParameter> &parameters,
                                                    std::unordered_map<std::string, TypeRef> &substitutions) {
    if (substitutions.size() == parameters.size()) {
        return;
    }
    std::unordered_set<std::string> names;
    for (const TypeParameter &parameter : parameters) {
        names.insert(parameter.name);
    }
    // Each pass can settle an argument that a later bound's arguments mention, so passes repeat while they learn.
    for (std::size_t pass = 0; pass < parameters.size() && substitutions.size() < parameters.size(); ++pass) {
        const std::size_t before = substitutions.size();
        for (const TypeParameter &parameter : parameters) {
            const auto argument = substitutions.find(parameter.name);
            if (argument == substitutions.end() || argument->second.IsUnknown() ||
                argument->second->kind == TypeRef::Kind::TypeParam) {
                continue;
            }
            const TypeRef receiver = argument->second;
            const std::string typeName = NamedBaseTypeName(receiver);
            if (typeName.empty()) {
                continue;
            }
            for (const ResolvedTypeBound &bound : ResolveTypeParameterBounds(parameter, false, &parameters)) {
                if (!bound.interface || bound.typeArguments.empty()) {
                    continue;
                }
                for (const auto &required : bound.interface->methods) {
                    const FuncDecl *operation = SelectBoundOperation(typeName, *required);
                    if (!operation) {
                        continue;
                    }
                    const TypeRef pattern =
                        ResolveInterfaceMethodReturnType(*required, receiver, bound.interface, bound.typeArguments);
                    DeduceTypeArgument(pattern, ResolveMethodReturnType(receiver, *operation), names, substitutions);
                    const std::vector<TypeRef> patterns =
                        ResolveInterfaceMethodParamTypes(*required, receiver, bound.interface, bound.typeArguments);
                    const std::vector<TypeRef> actuals = ResolveMethodParamTypes(receiver, *operation);
                    for (std::size_t index = 0; index < patterns.size() && index < actuals.size(); ++index) {
                        DeduceTypeArgument(patterns[index], actuals[index], names, substitutions);
                    }
                }
            }
        }
        if (substitutions.size() == before) {
            break;
        }
    }
}

void AnalysisContext::DeclareTypeParameterBounds(const std::vector<TypeParameter> &parameters) {
    for (const TypeParameter &parameter : parameters) {
        std::vector<ResolvedTypeBound> bounds = ResolveTypeParameterBounds(parameter, /*report=*/true);
        currentTypeParamBounds[parameter.name] = std::move(bounds);
    }
}

AnalysisContext::ScopedTypeParameterBounds::ScopedTypeParameterBounds(AnalysisContext &owner,
                                                                      const std::vector<TypeParameter> *parameters,
                                                                      const bool replaceEnclosing)
    : context(owner)
    , saved(owner.currentTypeParamBounds) {
    if (replaceEnclosing) {
        context.currentTypeParamBounds.clear();
    }
    if (parameters) {
        context.DeclareTypeParameterBounds(*parameters);
    }
}

AnalysisContext::ScopedTypeParameterBounds::~ScopedTypeParameterBounds() {
    context.currentTypeParamBounds = std::move(saved);
}

std::optional<std::string> AnalysisContext::UnsendablePart(const TypeRef &type) {
    if (type->kind == TypeRef::Kind::Reference) {
        return type.DisplayString();
    }
    if (type->kind == TypeRef::Kind::Array || type->kind == TypeRef::Kind::Tuple || type.IsSum() || type.IsOptional() ||
        type.IsFallible()) {
        for (const TypeRef &element : type->inner) {
            if (auto part = UnsendablePart(element)) {
                return part;
            }
        }
        return std::nullopt;
    }
    if (type->kind != TypeRef::Kind::Named || !sendableChecks.insert(type->name).second) {
        return std::nullopt;
    }
    const std::string base = BaseTypeName(type->name);
    const std::vector<TypeRef> arguments = ParseTypeArgsFromTypeName(type->name);
    const auto substitutionsFor = [&](const std::vector<TypeParameter> &parameters) {
        std::unordered_map<std::string, TypeRef> substitutions;
        for (std::size_t index = 0; index < std::min(arguments.size(), parameters.size()); ++index) {
            substitutions.emplace(parameters[index].name, arguments[index]);
        }
        return substitutions;
    };
    std::optional<std::string> part;
    if (const auto structure = structDecls.find(base); structure != structDecls.end()) {
        if (structure->second->isUnsendable) {
            part = base;
        }
        const auto substitutions = substitutionsFor(structure->second->typeParams);
        for (const StructDecl::Field &field : structure->second->fields) {
            if (part) {
                break;
            }
            part = UnsendablePart(ResolveTypeWithSubstitution(*field.type, substitutions));
        }
    }
    else if (const auto enumeration = enumDecls.find(base); enumeration != enumDecls.end()) {
        if (enumeration->second->isUnsendable) {
            part = base;
        }
        const auto substitutions = substitutionsFor(enumeration->second->typeParams);
        for (const EnumDecl::Variant &variant : enumeration->second->variants) {
            for (const auto &field : variant.fields) {
                if (!part) {
                    part = UnsendablePart(ResolveTypeWithSubstitution(*field, substitutions));
                }
            }
            for (const auto &field : variant.namedFields) {
                if (!part) {
                    part = UnsendablePart(ResolveTypeWithSubstitution(*field.type, substitutions));
                }
            }
        }
    }
    else if (const auto unionType = unionDecls.find(base); unionType != unionDecls.end()) {
        if (unionType->second->isUnsendable) {
            part = base;
        }
        for (const UnionDecl::Field &field : unionType->second->fields) {
            if (!part) {
                part = UnsendablePart(ResolveType(*field.type));
            }
        }
    }
    sendableChecks.erase(type->name);
    return part;
}

namespace {
/// The compiler-known marker a thread's context and result are bound by: an interface named `Sendable` that requires
/// nothing. A type satisfies it unless it is or holds a type declared `#Unsendable()`, or a reference.
bool IsSendableMarker(const InterfaceDecl &interface) {
    return interface.name == "Sendable" && interface.methods.empty() && interface.typeParams.empty();
}
} // namespace

bool AnalysisContext::TypeSatisfiesBound(const TypeRef &argument, const InterfaceDecl &interface, std::string &reason,
                                         const std::vector<TypeRef> &interfaceArguments) {
    if (IsSendableMarker(interface) && argument->kind != TypeRef::Kind::TypeParam) {
        if (const auto part = UnsendablePart(argument)) {
            const bool itself = *part == argument.DisplayString() ||
                                (argument->kind == TypeRef::Kind::Named && *part == BaseTypeName(argument->name));
            if (argument->kind == TypeRef::Kind::Reference) {
                reason = std::format("'{}' is a reference, which cannot leave the call that borrowed it", *part);
            }
            else if (itself) {
                reason = std::format("'{}' is declared '#Unsendable()', so it stays on the thread that made it",
                                     argument.DisplayString());
            }
            else {
                reason =
                    std::format("'{}' holds '{}', which is declared '#Unsendable()'", argument.DisplayString(), *part);
            }
            return false;
        }
        return true;
    }
    // An interface that requires nothing is satisfied by everything, so a marker bound costs no conformance work. A
    // type parameter given for `Sendable` has to carry the bound itself, which the check below asks of it.
    if (interface.methods.empty() && !(IsSendableMarker(interface) && argument->kind == TypeRef::Kind::TypeParam)) {
        return true;
    }

    // A type parameter passed on as a type argument carries only what its own declaration promised. Checking that
    // promise here rather than at each instantiation is what keeps a generic body checkable on its own: the caller
    // already had to satisfy the outer bound, so the inner one follows.
    if (argument->kind == TypeRef::Kind::TypeParam) {
        const auto bounds = currentTypeParamBounds.find(argument->name);
        // A parameter with no recorded bounds belongs to a declaration that is not the one being checked -- a signature
        // is resolved before its body, and a type is read back in contexts that carry names but no constraints. What
        // that parameter promised is unknown here, so the pass over its own declaration decides instead of this one.
        if (bounds == currentTypeParamBounds.end()) {
            return true;
        }
        for (const ResolvedTypeBound &bound : bounds->second) {
            if (bound.interface == &interface && bound.typeArguments == interfaceArguments) {
                return true;
            }
        }
        reason = std::format("type parameter '{}' is not constrained by '{}'", argument->name, interface.name);
        return false;
    }

    const std::string typeName = NamedBaseTypeName(argument);
    if (typeName.empty()) {
        reason = std::format("type '{}' cannot carry the methods interface '{}' requires", argument.ToString(),
                             interface.name);
        return false;
    }

    // The operations themselves decide. `extend T: I` states the conformance outright, but its methods are indexed the
    // same way a plain extend block's are, so one rule covers both: a type satisfies the bound when every operation the
    // interface names has exactly one method to call. Naming that method here is also what makes the bound usable --
    // lowering calls it directly per instantiation, so a conformance with no callable method would satisfy nothing.
    ResolvedConstraintWitness witness;
    witness.interfaceName = programIndex.NominalName(interface);
    witness.typeName = typeName;
    for (const auto &required : interface.methods) {
        const FuncDecl *operation = SelectBoundOperation(typeName, *required);
        if (!operation) {
            reason = std::format("interface '{}' requires method '{}', which type '{}' does not implement",
                                 interface.name, required->name, argument.ToString());
            return false;
        }
        // The method found by name has to take and return what the requirement says, since every call through the
        // bound is typed by the requirement and lowered to that method.
        if (std::ranges::none_of(interfaceArguments, [](const TypeRef &type) {
                return type.IsUnknown() || type->kind == TypeRef::Kind::TypeParam;
            })) {
            const TypeRef requiredReturn =
                ResolveInterfaceMethodReturnType(*required, argument, &interface, interfaceArguments);
            const TypeRef actualReturn = ResolveMethodReturnType(argument, *operation);
            if (!requiredReturn.IsUnknown() && !actualReturn.IsUnknown() && !(requiredReturn == actualReturn)) {
                reason =
                    std::format("method '{}' of type '{}' returns '{}', but the bound requires '{}'", required->name,
                                argument.ToString(), actualReturn.ToString(), requiredReturn.ToString());
                return false;
            }
            if (const auto mismatch =
                    RequirementSignatureMismatch(argument, *operation, *required, interface, interfaceArguments)) {
                reason = std::format("method '{}' of type '{}' takes '{}', but the bound requires '{}'", required->name,
                                     argument.ToString(), mismatch->declared, mismatch->required);
                return false;
            }
        }
        witness.operations.push_back(operation);
    }
    constraintWitnesses.insert_or_assign(ConstraintWitnessKey(programIndex.NominalName(interface), argument),
                                         std::move(witness));
    return true;
}

const FuncDecl *AnalysisContext::SelectBoundOperation(const std::string &typeName, const FuncDecl &required) const {
    const auto methods = methodsByType.find(typeName);
    if (methods == methodsByType.end()) {
        return nullptr;
    }
    const auto named = methods->second.find(required.name);
    if (named == methods->second.end() || named->second.empty()) {
        return nullptr;
    }
    if (named->second.size() == 1) {
        return named->second.front();
    }

    // An overloaded name needs the overload the interface asked for. Written arity is what the interface states, since
    // its own declaration carries no receiver, and an ambiguous name provides no single operation to call.
    const auto writtenCount = [](const FuncDecl &function) {
        std::size_t count = 0;
        for (const Param &parameter : function.params) {
            if (!parameter.IsReceiver()) {
                ++count;
            }
        }
        return count;
    };
    const FuncDecl *selected = nullptr;
    for (const FuncDecl *candidate : named->second) {
        if (writtenCount(*candidate) != writtenCount(required)) {
            continue;
        }
        if (selected) {
            return nullptr;
        }
        selected = candidate;
    }
    return selected;
}

std::optional<AnalysisContext::ConstrainedOperation>
AnalysisContext::LookupConstrainedOperation(const TypeRef &receiverType, const std::string &methodName) const {
    const TypeRef &receiver =
        (receiverType->kind == TypeRef::Kind::Pointer || receiverType->kind == TypeRef::Kind::Reference) &&
                !receiverType->inner.empty()
            ? receiverType->inner.front()
            : receiverType;
    if (receiver->kind != TypeRef::Kind::TypeParam) {
        return std::nullopt;
    }
    const auto bounds = currentTypeParamBounds.find(receiver->name);
    if (bounds == currentTypeParamBounds.end()) {
        return std::nullopt;
    }
    for (const ResolvedTypeBound &bound : bounds->second) {
        if (!bound.interface) {
            continue;
        }
        for (std::size_t index = 0; index < bound.interface->methods.size(); ++index) {
            if (bound.interface->methods[index]->name != methodName) {
                continue;
            }
            return ConstrainedOperation{receiver->name, bound.name,      bound.interface->methods[index].get(),
                                        index,          bound.interface, bound.typeArguments};
        }
    }
    return std::nullopt;
}

void AnalysisContext::RecordConstrainedBinding(const CallExpr &call, const ConstrainedOperation &operation,
                                               const TypeRef &receiverType) {
    RecordFunctionBinding(call, *operation.operation, ResolvedCallableBinding::DispatchKind::Constrained, {},
                          receiverType);
    ResolvedCallableBinding &binding = callableBindings.at(&call);
    binding.constraintInterface = operation.interfaceName;
    binding.constraintOperationIndex = operation.operationIndex;
}

void AnalysisContext::EmitMissingConstrainedOperation(const SourceLocation location, const TypeRef &receiverType,
                                                      const std::string &methodName) const {
    const TypeRef &receiver =
        (receiverType->kind == TypeRef::Kind::Pointer || receiverType->kind == TypeRef::Kind::Reference) &&
                !receiverType->inner.empty()
            ? receiverType->inner.front()
            : receiverType;
    std::vector<std::string> notes;
    std::string names;
    if (const auto bounds = currentTypeParamBounds.find(receiver->name); bounds != currentTypeParamBounds.end()) {
        for (const ResolvedTypeBound &bound : bounds->second) {
            names += names.empty() ? "" : ", ";
            names += std::format("'{}'", bound.name);
        }
    }
    notes.push_back(names.empty() ? std::format("type parameter '{}' has no interface bounds", receiver->name)
                                  : std::format("type parameter '{}' is bound by {}", receiver->name, names));
    EmitError(location,
              std::format("no interface bound on type parameter '{}' provides method '{}'", receiver->name, methodName),
              std::move(notes),
              std::format("add a bound whose interface declares '{}', as in '{}: SomeInterface'", methodName,
                          receiver->name));
}

void AnalysisContext::CheckTypeArgumentConstraints(const std::vector<TypeParameter> &parameters,
                                                   const std::unordered_map<std::string, TypeRef> &substitutions,
                                                   const SourceLocation location, const std::string &subject) {
    for (const TypeParameter &parameter : parameters) {
        const auto argument = substitutions.find(parameter.name);
        if (argument == substitutions.end() || argument->second.IsUnknown()) {
            continue;
        }
        // A generic body is checked once, for every type its parameter may stand for, so nothing there proves a value
        // of that type is not stored: in a field, a thread's context, a collection. A reference may not be stored
        // beyond the call that borrowed it, so it is no type argument.
        if (TypeStoresReference(argument->second)) {
            EmitError(location,
                      std::format("type argument '{}' of {} cannot be or hold a reference",
                                  argument->second.DisplayString(), subject),
                      {std::format("type parameter '{}' may be stored by the generic code, and a reference cannot "
                                   "outlive the call that borrowed it",
                                   parameter.name)},
                      "pass an owned value, or a raw pointer when the address must be shared");
            continue;
        }
        if (parameter.bounds.empty()) {
            continue;
        }

        for (const ResolvedTypeBound &bound : ResolveTypeParameterBounds(parameter, /*report=*/false, &parameters)) {
            // A bound that named nothing usable was reported where it was written; repeating it at each use would bury
            // the one diagnostic that can be acted on.
            if (!bound.interface) {
                continue;
            }
            std::vector<TypeRef> interfaceArguments;
            for (const TypeRef &type : bound.typeArguments) {
                interfaceArguments.push_back(SubstituteTypeParameters(type, substitutions));
            }
            std::string reason;
            if (TypeSatisfiesBound(argument->second, *bound.interface, reason, interfaceArguments)) {
                continue;
            }
            const std::string argumentName = argument->second.ToString();
            std::vector<std::string> notes{std::move(reason)};
            notes.push_back(
                std::format("type parameter '{}' of {} is bound by '{}'", parameter.name, subject, bound.name));
            std::optional<std::string> help;
            if (argument->second->kind == TypeRef::Kind::TypeParam) {
                help =
                    std::format("add the bound to the enclosing declaration, as in '{}: {}'", argumentName, bound.name);
            }
            else if (IsSendableMarker(*bound.interface)) {
                // No extension makes a type sendable: what it holds decides, so the help points at what to hold.
                help = "keep the value on the thread that made it, or share it through a type made for threads, such "
                       "as 'Arc' in place of 'Rc'";
            }
            else {
                help =
                    std::format("implement the interface, as in 'extend {}: {} {{ ... }}'", argumentName, bound.name);
            }
            EmitError(location,
                      std::format("type argument '{}' does not satisfy interface bound '{}' on type parameter '{}'",
                                  argumentName, bound.name, parameter.name),
                      std::move(notes), std::move(help));
        }
    }
}

void AnalysisContext::CheckTypeReferenceConstraints(const TypeExpr &expression,
                                                    const std::vector<TypeParameter> &parameters,
                                                    const std::vector<TypeRef> &typeArguments,
                                                    const std::string &subject) {
    if (parameters.size() != typeArguments.size() || reportedTypeArgumentConstraints.contains(&expression)) {
        return;
    }
    std::unordered_map<std::string, TypeRef> substitutions;
    for (std::size_t index = 0; index < parameters.size(); ++index) {
        substitutions.emplace(parameters[index].name, typeArguments[index]);
    }
    // Only a report consumes the spelling. A pass that reached this type before its enclosing declaration's bounds were
    // recorded decided nothing, and suppressing the later pass on the strength of it would lose the diagnostic.
    const std::size_t before = diags.size();
    CheckTypeArgumentConstraints(parameters, substitutions, expression.location, subject);
    if (diags.size() != before) {
        reportedTypeArgumentConstraints.insert(&expression);
    }
}

void AnalysisContext::CheckWrittenTypeArgumentConstraints(const std::vector<TypeParameter> &parameters,
                                                          const std::vector<TypeExprPtr> &typeArguments,
                                                          const SourceLocation location, const std::string &subject) {
    if (parameters.size() != typeArguments.size()) {
        return;
    }
    std::unordered_map<std::string, TypeRef> substitutions;
    for (std::size_t index = 0; index < parameters.size(); ++index) {
        substitutions.emplace(parameters[index].name, ResolveType(*typeArguments[index]));
    }
    CheckTypeArgumentConstraints(parameters, substitutions, location, subject);
}

bool AnalysisContext::TypeArgumentsSatisfyBounds(const std::vector<TypeParameter> &parameters,
                                                 const std::unordered_map<std::string, TypeRef> &substitutions) {
    for (const TypeParameter &parameter : parameters) {
        if (parameter.bounds.empty()) {
            continue;
        }
        const auto argument = substitutions.find(parameter.name);
        if (argument == substitutions.end() || argument->second.IsUnknown()) {
            continue;
        }
        for (const ResolvedTypeBound &bound : ResolveTypeParameterBounds(parameter, /*report=*/false, &parameters)) {
            if (!bound.interface) {
                continue;
            }
            std::vector<TypeRef> interfaceArguments;
            for (const TypeRef &type : bound.typeArguments) {
                interfaceArguments.push_back(SubstituteTypeParameters(type, substitutions));
            }
            std::string reason;
            if (!TypeSatisfiesBound(argument->second, *bound.interface, reason, interfaceArguments)) {
                return false;
            }
        }
    }
    return true;
}
} // namespace Rux::SemanticDetail
