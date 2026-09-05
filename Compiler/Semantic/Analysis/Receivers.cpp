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
namespace {
/// A slice is a fat pointer whose ABI passes it by address, so it reaches a method the way a pointer receiver does.
bool IsSliceReceiver(const TypeRef &type) {
    return type.isIntrinsicSlice;
}
} // namespace

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

    if (isMethod) {
        Symbol self;
        self.kind = Symbol::Kind::Var;
        self.name = "self";
        self.location = receiver ? receiver->location : declaration.location;
        self.type = currentSelfType.IsUnknown() ? TypeRef::MakeNamed("self") : currentSelfType;
        self.isMut = false;
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
    const bool isReference = isIndirect || IsSliceReceiver(declared);
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
        declared->kind != TypeRef::Kind::Pointer && !IsSliceReceiver(*declared)) {
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
    if (receiverType.kind == TypeRef::Kind::Pointer || receiverType.kind == TypeRef::Kind::Reference) {
        if (!receiverType.inner.empty() && !receiverType.inner.front().isMut) {
            const bool isReferenceType = receiverType.kind == TypeRef::Kind::Reference;
            EmitError(
                call.location,
                isReferenceType ? std::format("cannot call '{}' through immutable reference '{}'", method.name,
                                              receiverType.ToString())
                                : std::format("cannot call '{}' through read-only pointer '{}'", method.name,
                                              receiverType.ToString()),
                {std::format("'{}' declares a writable receiver '{}'", method.name, declared->ToString())},
                std::format("declare the {} as '{}'", isReferenceType ? "reference" : "pointer", declared->ToString()));
            return false;
        }
        return true;
    }
    if (!PlaceIsImmutable(receiver)) {
        return true;
    }
    const auto *identifier = dynamic_cast<const IdentExpr *>(&receiver);
    EmitError(call.location,
              identifier ? std::format("cannot call '{}' on immutable '{}'", method.name, identifier->name)
                         : std::format("cannot call '{}' on an immutable receiver", method.name),
              {std::format("'{}' declares a writable receiver '{}'", method.name, declared->ToString())},
              identifier ? std::format("declare '{}' with 'var' to make it mutable", identifier->name)
                         : std::optional<std::string>{});
    return false;
}
} // namespace Rux::SemanticDetail
