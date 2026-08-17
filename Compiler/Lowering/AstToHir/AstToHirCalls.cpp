// Call lowering: the callable analysis selected, default arguments, and the
// compile-time values a call site can be handed.

#include "Lowering/AstToHir/Detail/AstToHirContext.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <utility>

namespace Rux::AstToHirDetail {
namespace {
template <typename Fact>
/// Unwrap a semantic fact that analysis is required to have recorded.
///
/// A missing fact means analysis accepted a node without resolving it, which is a compiler bug that would otherwise
/// surface as a null dereference somewhere further down. Failing here names the invariant instead.
const Fact &RequireSemanticFact(const Fact *fact) {
    assert(fact != nullptr && "accepted AST node is missing a required semantic fact");
    if (!fact) {
        std::abort();
    }
    return *fact;
}

/// The function overload resolution chose for this call. Lowering never re-resolves: the binding analysis recorded is
/// the answer.
const FuncDecl &SelectedFunction(const ResolvedCallableBinding &binding) {
    const auto *function = dynamic_cast<const FuncDecl *>(binding.selectedDeclaration);
    assert(function != nullptr && "call binding does not select a function declaration");
    if (!function) {
        std::abort();
    }
    return *function;
}

} // namespace

std::optional<std::string> AstToHirContext::CompilerParamRoot(const Expr &expression) const {
    const auto *identifier = dynamic_cast<const IdentExpr *>(&expression);
    if (!identifier) {
        return std::nullopt;
    }
    const HirSymbol *symbol = currentScope->Lookup(identifier->name);
    if (!symbol || symbol->kind != HirSymbol::Kind::Const || symbol->intrinsicName.empty()) {
        return std::nullopt;
    }
    return symbol->intrinsicName;
}

std::string AstToHirContext::LogicalCurrentFilePath() const {
    const std::filesystem::path path(currentFile);
    if (!context.sourceRoot.empty()) {
        const auto relative = path.lexically_relative(context.sourceRoot);
        if (!relative.empty() && relative.begin()->generic_string() != "..") {
            return relative.generic_string();
        }
    }
    return path.generic_string();
}

ResolvedCallableBinding AstToHirContext::ResolvedCall(const CallExpr &call) const {
    return RequireSemanticFact(model.TryGetCallableBinding(call)).Instantiate(currentSubstitutions);
}

HirExprPtr AstToHirContext::LowerDefaultArgument(const Expr &expression, const TypeRef &targetType,
                                                 const SourceLocation &callSiteLocation) {
    if (const auto *intrinsic = dynamic_cast<const IntrinsicExpr *>(&expression);
        intrinsic && intrinsic->args.empty()) {
        IntrinsicExpr callSiteIntrinsic;
        callSiteIntrinsic.location = callSiteLocation;
        callSiteIntrinsic.kind = intrinsic->kind;
        return LowerExprAs(callSiteIntrinsic, targetType);
    }
    return LowerExprAs(expression, targetType);
}

std::vector<HirExprPtr> AstToHirContext::LowerBoundArguments(const CallExpr &call, const FuncDecl &declaration,
                                                             const ResolvedCallableBinding &binding,
                                                             const TypeRef &functionType, const bool hasReceiver) {
    std::vector<const Param *> parameters;
    parameters.reserve(declaration.params.size());
    for (const auto &parameter : declaration.params) {
        if (!hasReceiver || parameter.name != "self") {
            parameters.push_back(&parameter);
        }
    }

    const std::size_t typeOffset = hasReceiver ? 1 : 0;
    const std::size_t fixedCount = binding.variadicBoundary.value_or(parameters.size());
    std::vector<HirExprPtr> arguments;
    arguments.reserve(std::max(call.args.size(), fixedCount) + (binding.variadicBoundary ? 1 : 0));
    for (std::size_t i = 0; i < std::min(call.args.size(), fixedCount); ++i) {
        const std::size_t typeIndex = typeOffset + i;
        arguments.push_back(typeIndex + 1 < functionType.inner.size()
                                ? LowerExprAs(*call.args[i], functionType.inner[typeIndex])
                                : LowerExpr(*call.args[i]));
    }
    for (std::size_t i = arguments.size(); i < fixedCount; ++i) {
        assert(i < parameters.size() && parameters[i]->defaultValue &&
               "accepted call is missing a recorded default argument");
        const std::size_t typeIndex = typeOffset + i;
        const TypeRef parameterType =
            typeIndex + 1 < functionType.inner.size() ? functionType.inner[typeIndex] : TypeRef::MakeUnknown();
        arguments.push_back(LowerDefaultArgument(**parameters[i]->defaultValue, parameterType, call.location));
    }

    if (!binding.variadicBoundary) {
        return arguments;
    }

    assert(fixedCount < parameters.size() && parameters[fixedCount]->isVariadic &&
           "Rux variadic binding has no variadic parameter");
    const TypeRef elementType = ResolveTypeWithSubstitution(*parameters[fixedCount]->type, binding.substitutions);
    const bool singleSpread =
        call.args.size() == fixedCount + 1 && dynamic_cast<const SpreadExpr *>(call.args[fixedCount].get()) != nullptr;
    if (singleSpread) {
        HirExprPtr slice = LowerExpr(*call.args[fixedCount]);
        slice->type = TypeRef::MakeNamed(SliceTypeName(elementType));
        arguments.push_back(std::move(slice));
        return arguments;
    }

    auto slice = std::make_unique<HirArrayExpr>();
    slice->location = call.location;
    slice->elementType = elementType;
    slice->type = TypeRef::MakeNamed(SliceTypeName(elementType));
    for (std::size_t i = fixedCount; i < call.args.size(); ++i) {
        slice->elements.push_back(LowerExprAs(*call.args[i], elementType));
    }
    arguments.push_back(std::move(slice));
    return arguments;
}

void AstToHirContext::EnsureBoundFunctionInstance(const FuncDecl &declaration, const ResolvedCallableBinding &binding) {
    if (declaration.typeParams.empty()) {
        return;
    }
    assert(!binding.linkerName.empty() && "generic call binding is missing its linker name");
    if (generatedMonomorphizedFuncNames.insert(binding.linkerName).second) {
        monomorphizedFuncs.push_back(LowerFunc(declaration, false, binding.substitutions, binding.linkerName));
    }
}

void AstToHirContext::EnsureBoundMethodInstance(const FuncDecl &method, const ResolvedCallableBinding &binding) {
    if (binding.substitutions.empty() || MethodIsFromConcreteImpl(method)) {
        return;
    }
    assert(binding.receiverType && !binding.linkerName.empty() &&
           "generic method binding is missing its receiver or linker name");
    if (generatedMonomorphizedFuncNames.insert(binding.linkerName).second) {
        const TypeRef savedSelfType = currentSelfType;
        currentSelfType = binding.receiverType->kind == TypeRef::Kind::Pointer
                            ? *binding.receiverType
                            : TypeRef::MakePointer(*binding.receiverType);
        monomorphizedFuncs.push_back(LowerFunc(method, true, binding.substitutions, binding.linkerName));
        currentSelfType = savedSelfType;
    }
}

HirExprPtr AstToHirContext::LowerBoundDirectCall(const CallExpr &call, const ResolvedCallableBinding &binding) {
    if (const auto *external = dynamic_cast<const ExternFuncDecl *>(binding.selectedDeclaration)) {
        TypeRef functionType = MakeFuncType(external->params, external->returnType);
        functionType.isVariadic = external->isVariadic;
        auto lowered = std::make_unique<HirCallExpr>();
        lowered->location = call.location;
        lowered->isNoReturn = external->isNoReturn;
        lowered->type = functionType.inner.back();
        auto callee = std::make_unique<HirVarExpr>();
        callee->location = call.callee->location;
        // HIR-to-LIR owns the source-name to imported-symbol mapping.
        callee->name = external->name;
        callee->type = functionType;
        lowered->callee = std::move(callee);
        for (std::size_t i = 0; i < call.args.size(); ++i) {
            lowered->args.push_back(i < external->params.size() ? LowerExprAs(*call.args[i], functionType.inner[i])
                                                                : LowerExpr(*call.args[i]));
        }
        return lowered;
    }

    const FuncDecl &function = SelectedFunction(binding);
    const TypeRef functionType =
        MakeFuncTypeWithSubstitution(function.params, function.returnType, binding.substitutions, function.typeParams);
    auto lowered = std::make_unique<HirCallExpr>();
    lowered->location = call.location;
    lowered->isNoReturn = function.isNoReturn;
    lowered->type = functionType.inner.back();
    auto callee = std::make_unique<HirVarExpr>();
    callee->location = call.callee->location;
    callee->name = binding.linkerName;
    callee->type = functionType;
    lowered->callee = std::move(callee);
    lowered->args = LowerBoundArguments(call, function, binding, functionType, false);
    EnsureBoundFunctionInstance(function, binding);
    return lowered;
}

HirExprPtr AstToHirContext::LowerBoundMethodCall(const CallExpr &call, const ResolvedCallableBinding &binding) {
    const FuncDecl &method = SelectedFunction(binding);
    assert(binding.receiverType && !binding.linkerName.empty() &&
           "method call binding is missing its receiver or linker name");
    EnsureBoundMethodInstance(method, binding);

    auto lowered = std::make_unique<HirCallExpr>();
    lowered->location = call.location;
    lowered->isNoReturn = method.isNoReturn;
    auto callee = std::make_unique<HirVarExpr>();
    callee->location = call.callee->location;
    callee->name = binding.linkerName;

    if (const auto *field = dynamic_cast<const FieldExpr *>(call.callee.get())) {
        HirExprPtr receiver = LowerExpr(*field->object);
        const bool byValue = ReceiverIsByValue(method);
        const bool isPointer = receiver->type.kind == TypeRef::Kind::Pointer;
        HirExprPtr self;
        if (byValue && isPointer) {
            // A by-value receiver reached through a pointer is the value the pointer names, copied like any argument.
            auto load = std::make_unique<HirUnaryExpr>();
            load->location = receiver->location;
            load->op = TokenKind::Star;
            load->type = receiver->type.inner.front();
            load->operand = std::move(receiver);
            self = std::move(load);
        }
        else if (byValue || isPointer) {
            self = std::move(receiver);
        }
        else {
            auto address = std::make_unique<HirUnaryExpr>();
            address->location = receiver->location;
            address->op = TokenKind::At;
            address->type = TypeRef::MakePointer(receiver->type);
            address->operand = std::move(receiver);
            self = std::move(address);
        }
        callee->type = MethodType(self->type, method);
        lowered->args.push_back(std::move(self));
        std::vector<HirExprPtr> arguments = LowerBoundArguments(call, method, binding, callee->type, true);
        for (auto &argument : arguments) {
            lowered->args.push_back(std::move(argument));
        }
    }
    else {
        assert(dynamic_cast<const PathExpr *>(call.callee.get()) != nullptr &&
               "method binding has neither a receiver nor an associated path");
        callee->type = AssociatedFunctionType(*binding.receiverType, method);
        lowered->args = LowerBoundArguments(call, method, binding, callee->type, false);
    }
    lowered->type = callee->type.inner.back();
    lowered->callee = std::move(callee);
    return lowered;
}

HirExprPtr AstToHirContext::LowerBoundInterfaceCall(const CallExpr &call, const ResolvedCallableBinding &binding) {
    const FuncDecl &method = SelectedFunction(binding);
    const auto *field = dynamic_cast<const FieldExpr *>(call.callee.get());
    assert(field && binding.receiverType && "interface binding is missing its receiver");
    const std::string interfaceName = BaseTypeName(binding.receiverType->name);
    const auto interface = interfaceDecls.find(interfaceName);
    assert(interface != interfaceDecls.end() && "interface binding names an unknown interface");
    const auto methodIt = std::ranges::find_if(interface->second->methods,
                                               [&](const auto &candidate) { return candidate.get() == &method; });
    assert(methodIt != interface->second->methods.end() && "bound interface method has no vtable slot");

    const TypeRef functionType =
        MakeFuncTypeWithSubstitution(method.params, method.returnType, binding.substitutions, method.typeParams);
    auto lowered = std::make_unique<HirInterfaceCallExpr>();
    lowered->location = call.location;
    lowered->methodIdx = static_cast<int>(std::distance(interface->second->methods.begin(), methodIt));
    lowered->type = functionType.inner.back();
    lowered->fatPtrExpr = LowerExpr(*field->object);
    lowered->args = LowerBoundArguments(call, method, binding, functionType, false);
    return lowered;
}

HirExprPtr AstToHirContext::LowerBoundIndirectCall(const CallExpr &call, const ResolvedCallableBinding &binding) {
    auto lowered = std::make_unique<HirCallExpr>();
    lowered->location = call.location;
    lowered->callee = LowerExpr(*call.callee);
    lowered->type = lowered->callee->type.inner.back();
    const std::size_t fixedCount = binding.variadicBoundary.value_or(call.args.size());
    for (std::size_t i = 0; i < call.args.size(); ++i) {
        lowered->args.push_back(i < fixedCount && i + 1 < lowered->callee->type.inner.size()
                                    ? LowerExprAs(*call.args[i], lowered->callee->type.inner[i])
                                    : LowerExpr(*call.args[i]));
    }
    return lowered;
}

HirExprPtr AstToHirContext::LowerBoundEnumCall(const CallExpr &call, const ResolvedCallableBinding &binding) {
    const auto *declaration = dynamic_cast<const EnumDecl *>(binding.selectedDeclaration);
    assert(declaration && binding.selectedVariant && "enum constructor binding is incomplete");
    std::vector<TypeRef> typeArguments;
    typeArguments.reserve(declaration->typeParams.size());
    for (const auto &parameter : declaration->typeParams) {
        typeArguments.push_back(binding.substitutions.at(parameter));
    }
    const TypeRef constructor = EnumVariantConstructorType(*declaration, *binding.selectedVariant, typeArguments);
    auto lowered = std::make_unique<HirEnumConstructExpr>();
    lowered->location = call.location;
    lowered->type = constructor.inner.back();
    for (std::size_t i = 0; i < call.args.size(); ++i) {
        lowered->payloads.push_back(LowerExprAs(*call.args[i], constructor.inner[i]));
    }
    lowered->discriminant = LookupEnumVariantDiscriminant(declaration->name, binding.selectedVariant->name).value();
    return lowered;
}

HirExprPtr AstToHirContext::LowerCallExpr(const CallExpr &call) {
    if (const auto *field = dynamic_cast<const FieldExpr *>(call.callee.get())) {
        if (const auto root = CompilerParamRoot(*field->object)) {
            if (HirExprPtr value = LowerCompilerParamCall(*root, field->field, call)) {
                return value;
            }
        }
    }

    const auto *builtinIdentifier = dynamic_cast<const IdentExpr *>(call.callee.get());
    HirSymbol *calleeSymbol = builtinIdentifier ? currentScope->Lookup(builtinIdentifier->name) : nullptr;
    const bool isAssertion =
        calleeSymbol && (calleeSymbol->intrinsicName == "Assert" || calleeSymbol->intrinsicName == "DebugAssert");
    const bool isPanic = calleeSymbol && calleeSymbol->intrinsicName == "Panic";
    if (isAssertion || isPanic) {
        auto lowered = std::make_unique<HirCallExpr>();
        lowered->location = call.location;
        lowered->type = TypeRef::MakeOpaque();
        lowered->sourceFile = LogicalCurrentFilePath();
        lowered->sourceFunction = currentFunctionName;
        lowered->sourceLine = call.location.line;
        lowered->sourceColumn = call.location.column;

        const TypeRef stringType = TypeRef::MakeNamed(SliceTypeName(TypeRef::MakeChar8()));
        auto callee = std::make_unique<HirVarExpr>();
        callee->location = builtinIdentifier->location;
        callee->type = isPanic ? TypeRef::MakeFunc({stringType}, TypeRef::MakeOpaque())
                               : TypeRef::MakeFunc({TypeRef::MakeBool(), stringType}, TypeRef::MakeOpaque());
        const bool disabled = calleeSymbol->intrinsicName == "DebugAssert" && !context.DebugAssertions();
        callee->name = isPanic ? "__builtin_panic" : disabled ? "__builtin_debug_assert_disabled" : "__builtin_assert";
        lowered->callee = std::move(callee);
        lowered->isNoReturn = isPanic;

        // Disabled debug assertions are still checked by semantic analysis,
        // but their arguments are not evaluated at runtime.
        if (isPanic && call.args.size() == 1) {
            lowered->args.push_back(LowerExprAs(*call.args[0], stringType));
        }
        else if (!disabled && call.args.size() == 2) {
            lowered->args.push_back(LowerExprAs(*call.args[0], TypeRef::MakeBool()));
            lowered->args.push_back(LowerExprAs(*call.args[1], stringType));
        }
        return lowered;
    }

    const ResolvedCallableBinding binding = ResolvedCall(call);
    using Dispatch = ResolvedCallableBinding::DispatchKind;
    switch (binding.dispatch) {
    case Dispatch::Direct:
        return LowerBoundDirectCall(call, binding);
    case Dispatch::Method:
        return LowerBoundMethodCall(call, binding);
    case Dispatch::Interface:
        return LowerBoundInterfaceCall(call, binding);
    case Dispatch::Indirect:
        return LowerBoundIndirectCall(call, binding);
    case Dispatch::EnumVariant:
        return LowerBoundEnumCall(call, binding);
    }
    std::abort();
}
} // namespace Rux::AstToHirDetail
