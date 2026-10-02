// Postfix recovery: `outcome catch { arms }` removes exactly one outer fallible level. The success passes through
// unchanged, and the arms, which see only the error payload, either produce a value of the success type or leave.

#include "Semantic/Analysis/AnalysisContext.h"

#include <format>

namespace Rux::SemanticDetail {
namespace {
bool IsBlockArm(const Expr &body) {
    return dynamic_cast<const BlockExpr *>(&body) != nullptr;
}
} // namespace

void AnalysisContext::CheckRecoveryArmsWithoutSubject(const std::vector<MatchExpr::Arm> &arms) {
    for (const auto &arm : arms) {
        PushScope();
        DefinePatternBindings(*arm.pattern);
        static_cast<void>(CheckExpr(*arm.body));
        PopScope();
    }
}

TypeRef AnalysisContext::CheckCatchExpression(const CatchExpr &expression) {
    const TypeRef subjectType = CheckExpr(*expression.subject);
    if (subjectType.IsUnknown()) {
        CheckRecoveryArmsWithoutSubject(expression.arms);
        return TypeRef::MakeUnknown();
    }
    if (subjectType.kind == TypeRef::Kind::Reference) {
        EmitError(expression.location,
                  std::format("'catch' cannot consume the borrowed value '{}'", subjectType.ToString()),
                  {"'catch' moves the success onward or the error into its arms, and a reference owns neither"},
                  "match the borrowed value to inspect it, or recover an owned value");
        CheckRecoveryArmsWithoutSubject(expression.arms);
        return TypeRef::MakeUnknown();
    }
    if (!subjectType.IsFallible()) {
        const bool legacy = PropagationShapeOf(subjectType).has_value();
        EmitError(
            expression.location,
            std::format("'catch' recovers a native fallible, but the subject has type '{}'", subjectType.ToString()),
            {},
            legacy ? "a legacy outcome variant is handled with 'match'" : "'catch' applies to a value of type 'T ! E'");
        CheckRecoveryArmsWithoutSubject(expression.arms);
        return TypeRef::MakeUnknown();
    }

    const TypeRef success = subjectType.FallibleSuccess();
    const TypeRef error = subjectType.FallibleError();
    // The subject is evaluated and handed over once; the success passes through it and every arm takes the error.
    ConsumeValue(*expression.subject, subjectType, ValueConsumptionKind::CatchSubject, expression.subject->location);

    const TrackedFlow entry = SaveTrackedFlow();
    std::vector<TrackedFlow> exits = {entry};
    std::vector<const Pattern *> patterns;
    const TypeProperties errorProperties = ClassifyTypeProperties(error);
    for (const auto &arm : expression.arms) {
        RestoreTrackedFlow(entry);
        PushScope();
        CheckPattern(*arm.pattern, error);
        if (errorProperties.IsResolved() && !errorProperties.IsMovable() && PatternBindsValue(*arm.pattern)) {
            EmitError(arm.pattern->location,
                      std::format("'catch' cannot bind error payload '{}' by value because moving it is prohibited",
                                  error.ToString()),
                      {"a catch arm owns the error it binds, which moves it out of the subject"},
                      "match the subject with a non-binding pattern, or permit the canonical move operation");
        }
        const TypeRef bodyType = CheckExpr(*arm.body);
        const bool blockArm = IsBlockArm(*arm.body);
        const bool diverging = IsDivergingExpression(*arm.body) ||
                               (blockArm && BlockDefinitelyReturns(*static_cast<const BlockExpr &>(*arm.body).block));
        if (!diverging) {
            if (blockArm) {
                // A normally completing block has type `()`, which only a unit success can take; a non-unit success
                // never acquires an invented value.
                if (!success.IsUnit()) {
                    EmitError(arm.location,
                              std::format("a block arm completes with '()', but 'catch' must recover a value of type "
                                          "'{}'",
                                          success.ToString()),
                              {}, "give the arm a value, or leave it with 'fail', 'return', or a call to 'Panic'");
                }
            }
            else if (!bodyType.IsUnknown() && !success.IsUnknown()) {
                if (CanAssignExprTo(*arm.body, bodyType, success)) {
                    ConsumeValue(*arm.body, bodyType, ValueConsumptionKind::ConditionalArm, arm.location);
                }
                else {
                    EmitError(arm.location,
                              AssignmentErrorMessage(
                                  *arm.body, success,
                                  std::format("'catch' arm produces '{}', but the recovered value has type '{}'",
                                              bodyType.ToString(), success.ToString())));
                }
            }
        }
        PopScope();
        exits.push_back(SaveTrackedFlow());
        patterns.push_back(arm.pattern.get());
    }

    // Recovery handles every error: coverage is checked over the error payload alone.
    ValidateMatchPatterns(patterns, error);
    if (!MatchPatternsAreExhaustive(patterns, error) && !IsNativeMatchSubject(error) &&
        !(error.kind == TypeRef::Kind::Named && EnumNamed(BaseTypeName(error.name)))) {
        EmitError(expression.location,
                  std::format("'catch' arms do not cover every error of type '{}'", error.ToString()), {},
                  "add an 'else' arm, or bind the whole error, as in 'e => ...'");
    }
    MergeTrackedFlows(exits);
    return success;
}
} // namespace Rux::SemanticDetail
