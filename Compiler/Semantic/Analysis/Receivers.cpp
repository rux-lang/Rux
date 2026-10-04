// The rules a method receiver has to satisfy, at its declaration and at the call sites that reach it.
//
// A receiver is an ordinary parameter named `self`, so most of what governs it is the parameter machinery elsewhere in
// this component. What is left is the part no other parameter has: it names the type being extended, it is the argument
// the call site writes to the left of the dot rather than inside the parentheses, and it reaches the method by a route
// the signature chooses — copied, or addressed.

#include "Semantic/Analysis/AnalysisContext.h"

#include <format>
#include <utility>

namespace Rux::SemanticDetail {

/// Brings `self` into scope for a declaration and checks the receiver it was written with. What `self` means in a body
/// is what the receiver declares: `self: Vector` is a copy the method owns, `self: &Vector` borrows it for reading,
/// and `self: &var Vector` borrows it for writing. Legacy pointer receivers remain accepted during migration. Returns
/// the enclosing `self` type for the caller to put back once the declaration is checked.
TypeRef AnalysisContext::DeclareReceiver(const FuncDecl &declaration, const bool isMethod) {
    const Param *receiver = declaration.Receiver();
    const TypeRef savedSelfType = currentSelfType;
    if (receiver) {
        // Resolving the receiver is what records its type for lowering, so it happens either way; `self` written as a
        // type, which is how an interface names a receiver it has no concrete type for, keeps the enclosing meaning.
        const TypeRef declared = ResolveType(*receiver->type);
        if (!dynamic_cast<const SelfTypeExpr *>(receiver->type.get())) {
            currentSelfType = declared;
            CheckReceiverType(declaration, *receiver, declared);
        }
    }
    CheckReceiverPlacement(declaration, isMethod);
    ValidateIteratorConvention(declaration, isMethod);
    ValidateCheckedArithmeticIntrinsic(declaration);
    ValidateZeroizeIntrinsic(declaration);
    ValidateIntrinsicFunction(declaration);

    if (isMethod) {
        Symbol self;
        self.kind = Symbol::Kind::Var;
        self.name = "self";
        self.location = receiver ? receiver->location : declaration.location;
        self.type = currentSelfType.IsUnknown() ? TypeRef::MakeNamed("self") : currentSelfType;
        self.isMut = false;
        self.isParameter = true;
        DefineTrackedLocal(std::move(self), true);
    }
    return savedSelfType;
}

/// A receiver in `extend T` is written `T`, `&T`, `&var T`, or a legacy pointer form and nothing else: it names the
/// type being extended, so
/// anything else is a typo rather than a conversion to work out. An extend block that names an interface is narrower
/// still — dispatch reaches the method through a vtable slot that is handed an address, so the receiver has to be
/// one.
void AnalysisContext::CheckReceiverType(const FuncDecl &declaration, const Param &receiver, const TypeRef &declared) {
    if (!currentImpl || declared.IsUnknown() || currentExtendedType.IsUnknown()) {
        return;
    }
    const bool isIndirect = (declared.kind == TypeRef::Kind::Pointer || declared.kind == TypeRef::Kind::Reference) &&
                            !declared.inner.empty();
    // A slice is a fat pointer whose ABI passes it by address, so it reaches a method the way a pointer receiver does.
    const bool isReference = isIndirect || declared.IsSlice();
    if (const TypeRef &base = isIndirect ? declared.inner.front() : declared;
        !base.IsUnknown() && base != currentExtendedType) {
        EmitError(receiver.location,
                  std::format("receiver type '{}' does not name the extended type '{}'", declared.ToString(),
                              currentExtendedType.ToString()),
                  {}, std::format("write the receiver as 'self: &var {}'", currentExtendedType.ToString()));
        return;
    }
    if (currentImpl->interfaceName && !isReference) {
        EmitError(receiver.location,
                  std::format("method '{}' cannot take its receiver by value because this block implements "
                              "interface '{}'",
                              declaration.name, *currentImpl->interfaceName),
                  {"a method reached through an interface receives its receiver by reference"},
                  std::format("write the receiver as 'self: &{}'", currentExtendedType.ToString()));
    }
}

/// `self` names the receiver, so it is a parameter of a method and only of a method, and it is the one the call site
/// writes to the left of the dot. Both mistakes are reported against the parameter rather than the declaration, because
/// that is what has to move.
void AnalysisContext::CheckReceiverPlacement(const FuncDecl &declaration, const bool isMethod) {
    for (std::size_t index = 0; index < declaration.params.size(); ++index) {
        const Param &param = declaration.params[index];
        if (!param.IsReceiver()) {
            continue;
        }
        if (!isMethod) {
            EmitError(param.location,
                      std::format("function '{}' cannot take a receiver because it is not a method", declaration.name),
                      {}, "declare it inside an 'extend' block, or rename the parameter");
        }
        else if (index != 0) {
            EmitError(param.location,
                      std::format("receiver 'self' must be the first parameter of method '{}'", declaration.name));
        }
    }
}

/// What the method asked for, in terms of this call's receiver. Resolving it needs `self` to mean the receiver being
/// called on, because a receiver may be written through the `self` type rather than the concrete one.
std::optional<TypeRef> AnalysisContext::ResolveMethodReceiverType(const TypeRef &receiverType, const FuncDecl &method) {
    const Param *receiver = method.Receiver();
    if (!receiver) {
        return std::nullopt;
    }
    const TypeRef savedSelfType = currentSelfType;
    currentSelfType = receiverType.kind == TypeRef::Kind::Pointer || receiverType.kind == TypeRef::Kind::Reference
                        ? receiverType
                        : TypeRef::MakePointer(receiverType);
    TypeRef declared = ResolveTypeWithSubstitution(*receiver->type, MethodTypeSubstitutions(receiverType));
    currentSelfType = savedSelfType;
    return declared;
}

/// A method that declares `self: *var T` writes through its receiver, so the call site has to be able to hand it one.
/// The receiver reaches the method one of two ways and each has its own answer: a receiver that is already a pointer
/// carries the permission in its own type, while a receiver named as a place is addressed at the call, and that address
/// is writable only when the place is.
bool AnalysisContext::CheckReceiverMutability(const CallExpr &call, const Expr &receiver, const TypeRef &receiverType,
                                              const FuncDecl &method) {
    const std::optional<TypeRef> declared = ResolveMethodReceiverType(receiverType, method);
    if (!declared) {
        return true;
    }
    if (declared->kind == TypeRef::Kind::Reference && receiverType.kind == TypeRef::Kind::Pointer) {
        EmitError(call.location,
                  std::format("cannot create safe receiver '{}' from raw pointer '{}'", declared->ToString(),
                              receiverType.ToString()),
                  {"raw pointers do not prove a valid non-null borrow"}, "call the method on an owning value");
        return false;
    }
    if (declared->kind == TypeRef::Kind::Pointer && receiverType.kind == TypeRef::Kind::Reference) {
        EmitError(call.location,
                  std::format("cannot pass reference '{}' to raw-pointer receiver '{}' implicitly",
                              receiverType.ToString(), declared->ToString()),
                  {"crossing into a raw-pointer API must be explicit"});
        return false;
    }
    if (receiverType.kind == TypeRef::Kind::Reference && declared->kind != TypeRef::Kind::Reference &&
        declared->kind != TypeRef::Kind::Pointer && !declared->IsSlice()) {
        EmitError(call.location,
                  std::format("cannot pass value behind reference '{}' to by-value receiver of '{}'",
                              receiverType.ToString(), method.name),
                  {"references do not transfer ownership of the value they borrow"},
                  "declare the method receiver as a reference");
        return false;
    }
    const bool requiresWrite =
        (declared->kind == TypeRef::Kind::Pointer || declared->kind == TypeRef::Kind::Reference) &&
        !declared->inner.empty() && declared->inner.front().isMut;
    if (!requiresWrite) {
        return true;
    }
    return CheckWritableReceiver(call, receiver, receiverType, method.name, declared->ToString());
}

bool AnalysisContext::CheckWritableReceiver(const CallExpr &call, const Expr &receiver, const TypeRef &receiverType,
                                            const std::string &methodName, const std::string &declared) {
    if (receiverType.kind == TypeRef::Kind::Pointer || receiverType.kind == TypeRef::Kind::Reference) {
        if (!receiverType.inner.empty() && !receiverType.inner.front().isMut) {
            const bool isReferenceType = receiverType.kind == TypeRef::Kind::Reference;
            TypeRef writable = receiverType;
            writable.inner.front().isMut = true;
            EmitError(
                call.location,
                isReferenceType ? std::format("cannot call '{}' through immutable reference '{}'", methodName,
                                              receiverType.ToString())
                                : std::format("cannot call '{}' through read-only pointer '{}'", methodName,
                                              receiverType.ToString()),
                {std::format("'{}' declares a writable receiver '{}'", methodName, declared)},
                std::format("declare the {} as '{}'", isReferenceType ? "reference" : "pointer", writable.ToString()));
            return false;
        }
        return true;
    }
    if (!PlaceIsImmutable(receiver)) {
        return true;
    }
    const auto *identifier = dynamic_cast<const IdentExpr *>(&receiver);
    const Symbol *binding = identifier ? currentScope->Lookup(identifier->name) : nullptr;
    EmitError(call.location,
              identifier ? std::format("cannot call '{}' on immutable '{}'", methodName, identifier->name)
                         : std::format("cannot call '{}' on an immutable receiver", methodName),
              {std::format("'{}' declares a writable receiver '{}'", methodName, declared)},
              binding ? ImmutableBindingHelp(*binding) : std::optional<std::string>{});
    return false;
}

/// An interface value is borrowed or held whole, and its requirement decides what the call may do with it: one that
/// declares `self: &var Self` writes the implementing value, so the view or storage it is called through has to be
/// writable, exactly as for a concrete receiver.
bool AnalysisContext::CheckRequirementReceiverMutability(const CallExpr &call, const Expr &receiver,
                                                         const TypeRef &receiverType, const FuncDecl &requirement) {
    if (!RequirementWritesReceiver(requirement)) {
        return true;
    }
    return CheckWritableReceiver(call, receiver, receiverType, requirement.name, "&var Self");
}

/// A requirement is called through an interface view whose data half points at the implementing value, so its
/// receiver can only be a borrow of that value: `&Self` to read it or `&var Self` to write it.
void AnalysisContext::ValidateRequirementReceiver(const FuncDecl &requirement, const std::string &interfaceName) {
    const Param *receiver = requirement.Receiver();
    if (!receiver || !receiver->type || dynamic_cast<const SelfTypeExpr *>(receiver->type.get())) {
        return;
    }
    // A legacy pointer receiver, and `self` written as the type, still name the implementing value during migration.
    const TypeExpr *pointee = nullptr;
    if (const auto *reference = dynamic_cast<const ReferenceTypeExpr *>(receiver->type.get())) {
        pointee = reference->pointee.get();
    }
    else if (const auto *pointer = dynamic_cast<const PointerTypeExpr *>(receiver->type.get())) {
        pointee = pointer->pointee.get();
    }
    const auto *named = dynamic_cast<const NamedTypeExpr *>(pointee);
    if (dynamic_cast<const SelfTypeExpr *>(pointee) ||
        (named && named->name == SelfTypeName && named->typeArgs.empty())) {
        return;
    }
    EmitError(receiver->location,
              std::format("receiver of requirement '{}' in interface '{}' must be '&Self' or '&var Self'",
                          requirement.name, interfaceName),
              {"a requirement reaches the implementing value through the data half of an interface view"},
              std::format("write 'self: &var Self' if '{}' writes through its receiver, or 'self: &Self' if it only "
                          "reads it",
                          requirement.name));
}

/// An implementation may read through a receiver its requirement lets it write, but not the reverse: a requirement
/// without a receiver, or with `self: &Self`, is callable through a read-only view, so a writable implementation would
/// modify storage the caller only lent for reading.
void AnalysisContext::CheckImplementationReceiver(const FuncDecl &implementation, const FuncDecl &requirement,
                                                  const std::string &interfaceName, const std::string &typeName) {
    if (!DeclaresWritableReceiver(implementation) || RequirementWritesReceiver(requirement)) {
        return;
    }
    const Param *receiver = implementation.Receiver();
    EmitError(receiver->location,
              std::format("method '{}' of '{}' writes through its receiver, but requirement '{}' of interface '{}' "
                          "only reads it",
                          implementation.name, typeName, requirement.name, interfaceName),
              {"a requirement without a receiver, or with 'self: &Self', is callable through a read-only view"},
              std::format("declare 'self: &var Self' on requirement '{}' in interface '{}', or take 'self: &{}'",
                          requirement.name, interfaceName, typeName));
}
} // namespace Rux::SemanticDetail
