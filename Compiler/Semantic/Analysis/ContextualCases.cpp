// Cases written without their type: `.Case` and `.Case(...)` take their enum or variant from the type their context
// expects -- an annotated binding, a parameter, a field, a return value, a `fail` operand, an assignment target, or the
// type of the match whose arm they are. A `try` passes the type it is expected to have on to its operand and its
// `catch` arms. Nothing searches for a case name anywhere else. A user variant that is itself the expected type also
// takes over `.Some(v)`, `.Success(v)` and `.Failure(v)` when it declares that case.

#include "Semantic/Analysis/AnalysisContext.h"

#include <algorithm>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace Rux::SemanticDetail {
bool AnalysisContext::IsContextualCaseExpression(const Expr &expression) noexcept {
    if (AstCast<const EnumShorthandExpr>(&expression) || AstCast<const NativeConstructExpr>(&expression)) {
        return true;
    }
    const auto *call = AstCast<const CallExpr>(&expression);
    return call && AstCast<const EnumShorthandExpr>(call->callee.get());
}

void AnalysisContext::ExpectContextualCaseArguments(const CallExpr &call, const std::vector<TypeRef> &parameterTypes) {
    for (std::size_t index = 0; index < call.args.size() && index < parameterTypes.size(); ++index) {
        const Expr *argument = call.args[index].get();
        if (argument && IsContextualCaseExpression(*argument) && !parameterTypes[index].IsUnknown() &&
            !expectedExpressionTypes.contains(argument)) {
            expectedExpressionTypes.insert_or_assign(argument, parameterTypes[index]);
        }
    }
}

void AnalysisContext::ExpectContextualCaseArgument(const Expr *expression, const TypeRef &type) {
    // A `try` passes the type on to its operand, which may be such a case.
    if (expression && (IsContextualCaseExpression(*expression) || AstCast<const TryMarkExpr>(expression)) &&
        !type.IsUnknown()) {
        expectedExpressionTypes.insert_or_assign(expression, type);
    }
}

std::optional<TypeRef> AnalysisContext::ExpectedCaseType(const Expr &expression, const bool throughOptional) const {
    const auto expected = expectedExpressionTypes.find(&expression);
    if (expected == expectedExpressionTypes.end()) {
        return std::nullopt;
    }
    TypeRef type = expected->second;
    if (type->kind == TypeRef::Kind::Reference && !type->inner.empty()) {
        type = type->inner.front();
    }
    // A case of `T` converts to `T?` the way any value of `T` does, so an optional destination names its payload.
    while (throughOptional && type.IsOptional() && !type->inner.empty()) {
        type = type->inner.front();
    }
    if (type->kind != TypeRef::Kind::Named || !EnumNamed(BaseTypeName(type->name))) {
        return std::nullopt;
    }
    return type;
}

std::vector<TypeRef> AnalysisContext::ExpectedCaseTypeArgumentsOf(const TypeRef &expected,
                                                                  const EnumDecl &declaration) const {
    if (declaration.typeParams.empty()) {
        return {};
    }
    std::vector<TypeRef> arguments = ParseTypeArgsFromTypeName(expected->name);
    if (arguments.size() != declaration.typeParams.size()) {
        return {};
    }
    return arguments;
}

void AnalysisContext::ReportContextualCase(const Expr &expression, const SourceLocation location,
                                           const std::string &caseName, const std::optional<TypeRef> &expected) {
    // The context named an enum or variant, but not one with this case.
    if (expected) {
        const std::string typeName = BaseTypeName((*expected)->name);
        const EnumDecl *declaration = EnumNamed(typeName);
        const bool variant = declaration && declaration->IsVariant();
        EmitError(location, variant ? std::format("variant '{}' has no case '{}'", typeName, caseName)
                                    : std::format("enum '{}' has no enumerator '{}'", typeName, caseName));
        return;
    }
    // The context named a type that has no cases.
    if (const auto other = expectedExpressionTypes.find(&expression);
        other != expectedExpressionTypes.end() && !other->second.IsUnknown()) {
        EmitError(location, std::format("case '.{}' needs an enum or variant, but the expected type is '{}'", caseName,
                                        other->second.ToString()));
        return;
    }
    // Without a destination type, nothing says which enum or variant the case belongs to. The help names a type that
    // declares it only to suggest the spelling; the case is never resolved from it.
    std::vector<std::string> owners;
    for (const auto &[key, declaration] : enumDecls) {
        if (!declaration || std::ranges::none_of(declaration->variants, [&](const EnumDecl::Variant &variant) {
                return variant.name == caseName;
            })) {
            continue;
        }
        const Symbol *symbol = currentScope->Lookup(declaration->name);
        if (symbol && symbol->kind == Symbol::Kind::Type) {
            owners.push_back(declaration->name);
        }
    }
    std::ranges::sort(owners);
    owners.erase(std::ranges::unique(owners).begin(), owners.end());
    std::vector<std::string> notes;
    if (owners.size() > 1) {
        for (const std::string &owner : owners) {
            notes.push_back(std::format("'{}::{}' is in scope", owner, caseName));
        }
    }
    EmitError(location, std::format("case '.{}' needs an expected type to name its enum or variant", caseName),
              std::move(notes),
              owners.size() == 1 ? std::format("write '{}::{}'", owners.front(), caseName)
                                 : std::format("write 'Type::{}' with the name of its enum or variant", caseName));
}

TypeRef AnalysisContext::CheckContextualCase(const EnumShorthandExpr &expression) {
    const std::optional<TypeRef> expected = ExpectedCaseType(expression, true);
    const std::optional<ResolvedCase> resolved =
        expected ? LookupCase(BaseTypeName((*expected)->name), expression.variant) : std::nullopt;
    if (!resolved) {
        ReportContextualCase(expression, expression.location, expression.variant, expected);
        return TypeRef::MakeUnknown();
    }
    const EnumDecl &declaration = *resolved->declaration;
    const EnumDecl::Variant &selected = *resolved->selectedCase;
    if (!selected.fields.empty() || !selected.namedFields.empty()) {
        EmitError(expression.location,
                  std::format("case '{}::{}' carries a payload, so '.{}' needs its values", declaration.name,
                              selected.name, selected.name),
                  {}, std::format("write '.{}(...)' with a value for each field", selected.name));
        return TypeRef::MakeUnknown();
    }
    const std::vector<TypeRef> typeArguments = ExpectedCaseTypeArgumentsOf(*expected, declaration);
    if (typeArguments.size() != declaration.typeParams.size()) {
        return TypeRef::MakeUnknown();
    }
    ResolvedContextualCase fact;
    fact.declaration = &declaration;
    fact.selectedCase = &selected;
    for (std::size_t index = 0; index < typeArguments.size(); ++index) {
        fact.substitutions.emplace(declaration.typeParams[index].name, typeArguments[index]);
    }
    contextualCases.insert_or_assign(&expression, std::move(fact));
    return EnumType(declaration, typeArguments);
}

std::optional<TypeRef> AnalysisContext::CheckContextualNativeCase(const NativeConstructExpr &construct) {
    // Only a variant that is the expected type itself takes the native spelling over; through an optional, `.Some(v)`
    // is the native presence it always was.
    const std::optional<TypeRef> expected = ExpectedCaseType(construct, false);
    if (!expected) {
        return std::nullopt;
    }
    const std::string caseName = construct.kind == NativeConstructExpr::Kind::Some    ? "Some"
                               : construct.kind == NativeConstructExpr::Kind::Success ? "Success"
                                                                                      : "Failure";
    const std::optional<ResolvedCase> resolved = LookupCase(BaseTypeName((*expected)->name), caseName);
    if (!resolved || resolved->form != EnumDecl::Form::Variant) {
        return std::nullopt;
    }
    const EnumDecl &declaration = *resolved->declaration;
    const EnumDecl::Variant &selected = *resolved->selectedCase;
    const std::vector<TypeRef> typeArguments = ExpectedCaseTypeArgumentsOf(*expected, declaration);
    if (typeArguments.size() != declaration.typeParams.size()) {
        return std::nullopt;
    }
    if (selected.fields.size() != 1 || !selected.namedFields.empty()) {
        EmitError(construct.location,
                  std::format("case '{}::{}' does not take exactly one value", declaration.name, selected.name), {},
                  std::format("write '{}::{}(...)' with a value for each field", declaration.name, selected.name));
        return TypeRef::MakeUnknown();
    }
    ResolvedContextualCase fact;
    fact.declaration = &declaration;
    fact.selectedCase = &selected;
    for (std::size_t index = 0; index < typeArguments.size(); ++index) {
        fact.substitutions.emplace(declaration.typeParams[index].name, typeArguments[index]);
    }
    const TypeRef fieldType = ResolveTypeWithSubstitution(*selected.fields.front(), fact.substitutions);
    contextualCases.insert_or_assign(&construct, std::move(fact));
    if (construct.operand) {
        ExpectContextualCaseArgument(construct.operand.get(), fieldType);
        const TypeRef operand = CheckExpr(*construct.operand);
        if (!operand.IsUnknown() && !fieldType.IsUnknown() &&
            !CanAssignExprTo(*construct.operand, operand, fieldType)) {
            EmitError(construct.operand->location,
                      std::format("argument 1 to variant case '{}::{}' has type '{}', but field 1 requires '{}'",
                                  declaration.name, selected.name, operand.DisplayString(), fieldType.ToString()));
        }
        // The constructed value takes its operand by value, exactly once however often the constructor is checked.
        else if (!operand.IsUnknown() && consumedConstructorOperands.insert(&construct).second) {
            ConsumeValue(*construct.operand, operand, ValueConsumptionKind::ConstructorOperand,
                         construct.operand->location);
        }
    }
    return EnumType(declaration, typeArguments);
}
} // namespace Rux::SemanticDetail
