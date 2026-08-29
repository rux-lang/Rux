// Failure propagation lowering: `expr?` becomes a match over the operand whose failure arm returns from the enclosing
// function, so the early return is an ordinary return and carries the same destruction of every live local.

#include "Lowering/AstToHir/Detail/AstToHirContext.h"

#include <cassert>
#include <cstdlib>
#include <format>
#include <utility>

namespace Rux::AstToHirDetail {
namespace {
/// Names the two bindings a propagation introduces. They stand for values the source never named, so the spelling is
/// one no identifier can collide with, and the counter keeps two propagations in one expression apart.
[[nodiscard]] std::string PropagationBindingName(const std::string_view role, const std::size_t ordinal) {
    return std::format("$try.{}.{}", role, ordinal);
}
} // namespace

HirExprPtr AstToHirContext::LowerTryExpr(const TryExpr &expression) {
    const ResolvedPropagation *fact = model.TryGetPropagation(expression);
    assert(fact != nullptr && "accepted propagation is missing its semantic fact");
    if (!fact) {
        std::abort();
    }

    // Every type is read back already substituted: the operand's and the expression's own from the recorded facts, and
    // the failure's from the type the enclosing function returns, which names the same error type by construction.
    const TypeRef operandType = ResolvedExpressionType(*expression.operand);
    const TypeRef payloadType = ResolvedExpressionType(expression);
    const TypeRef returnType = currentReturnType;
    const std::size_t ordinal = propagationOrdinal++;
    const std::string payloadName = PropagationBindingName("value", ordinal);
    const std::string failureName = PropagationBindingName("failure", ordinal);

    // Every unit variant of the operand's enum, which is how pattern lowering tells a payload-carrying value in the
    // compact representation from one that has no payload to read.
    std::vector<std::string> unitDiscriminants;
    if (const auto declaration = enumDecls.find(fact->enumName); declaration != enumDecls.end()) {
        for (const auto &variant : declaration->second->variants) {
            if (variant.fields.empty() && variant.namedFields.empty()) {
                if (auto discriminant = LookupEnumVariantDiscriminant(fact->enumName, variant.name)) {
                    unitDiscriminants.push_back(*discriminant);
                }
            }
        }
    }

    const auto variantPattern = [&](const std::string &variant, const std::string &bindingName,
                                    const TypeRef &bindingType, const bool hasPayload) {
        auto pattern = std::make_unique<HirEnumPattern>();
        pattern->location = expression.location;
        pattern->path = {fact->enumName, variant};
        pattern->resolvedType = operandType;
        pattern->discriminant = LookupEnumVariantDiscriminant(fact->enumName, variant);
        pattern->hasPayload = hasPayload;
        pattern->unitDiscriminants = unitDiscriminants;
        if (hasPayload) {
            auto binding = std::make_unique<HirBindingPattern>();
            binding->location = expression.location;
            binding->name = bindingName;
            binding->type = bindingType;
            pattern->argIndices.push_back(0);
            pattern->args.push_back(std::move(binding));
        }
        return pattern;
    };

    const auto namedValue = [&](const std::string &name, const TypeRef &type) {
        auto value = std::make_unique<HirVarExpr>();
        value->location = expression.location;
        value->name = name;
        value->type = type;
        return value;
    };

    HirMatchArm success;
    success.location = expression.location;
    success.pattern = variantPattern(fact->successVariant, payloadName, payloadType, true);
    success.body = namedValue(payloadName, payloadType);

    // The failure travels out unchanged: the same payload, re-wrapped in the failure variant of what this function
    // returns. Building it as an ordinary return is what gives it the destruction of every live local for free.
    TypeRef failureType = TypeRef::MakeUnknown();
    if (fact->failureType) {
        const std::vector<TypeRef> returnArguments = ParseTypeArgsFromTypeName(returnType.name);
        constexpr std::size_t kFailureArgument = 1;
        if (returnArguments.size() > kFailureArgument) {
            failureType = returnArguments[kFailureArgument];
        }
    }
    auto failureValue = std::make_unique<HirEnumConstructExpr>();
    failureValue->location = expression.location;
    failureValue->form = CaseTypeForm::Variant;
    failureValue->type = returnType;
    failureValue->discriminant =
        LookupEnumVariantDiscriminant(fact->returnEnumName, fact->failureVariant).value_or("0");
    if (fact->failureType) {
        failureValue->payloads.push_back(namedValue(failureName, failureType));
    }

    auto earlyReturn = std::make_unique<HirReturnStmt>();
    earlyReturn->location = expression.location;
    earlyReturn->value = std::move(failureValue);
    earlyReturn->cleanups = FunctionCleanups();

    auto failureBody = std::make_unique<HirBlockExpr>();
    failureBody->location = expression.location;
    failureBody->type = payloadType;
    failureBody->block.location = expression.location;
    failureBody->block.stmts.push_back(std::move(earlyReturn));

    HirMatchArm failure;
    failure.location = expression.location;
    failure.pattern = variantPattern(fact->failureVariant, failureName, failureType, fact->failureType.has_value());
    failure.body = std::move(failureBody);

    auto lowered = std::make_unique<HirMatchExpr>();
    lowered->location = expression.location;
    lowered->type = payloadType;
    lowered->subject = LowerExpr(*expression.operand);
    lowered->arms.push_back(std::move(success));
    lowered->arms.push_back(std::move(failure));
    return lowered;
}
} // namespace Rux::AstToHirDetail
