// Named arguments: `Open(path, create: true, truncate: false)`. A label names the parameter its argument binds. Labeled
// arguments follow the positional ones, in parameter order, and may skip parameters that have default values; the
// arguments are never reordered, so they are evaluated in source order. Labels never choose an overload: the call is
// resolved by the existing rules and its labels are then checked against the declaration it selected, whose parameter
// names are therefore part of its API. A call whose labels skip a parameter records which parameter each argument
// binds, which the argument checks and lowering read.

#include "Semantic/Analysis/AnalysisContext.h"

#include <algorithm>
#include <format>
#include <string>
#include <vector>

namespace Rux::SemanticDetail {
namespace {
/// The parameters a call's arguments bind: a method called through its receiver does not pass it as an argument.
std::vector<const Param *> ArgumentParameters(const FuncDecl &declaration, const bool isMethod) {
    std::vector<const Param *> parameters;
    for (const Param &parameter : declaration.params) {
        if (!(isMethod && parameter.IsReceiver())) {
            parameters.push_back(&parameter);
        }
    }
    return parameters;
}

/// The parameter names, quoted and joined, as a note lists them.
std::string ParameterNames(const std::vector<const Param *> &parameters) {
    std::string names;
    for (const Param *parameter : parameters) {
        names += std::format("{}'{}'", names.empty() ? "" : ", ", parameter->name);
    }
    return names.empty() ? std::string("none") : names;
}
} // namespace

std::optional<std::vector<std::size_t>>
AnalysisContext::ArgumentSlots(const CallExpr &call, const FuncDecl &declaration, const bool isMethod) {
    if (call.labels.empty() || call.labels.size() != call.args.size()) {
        return std::nullopt;
    }
    const auto parameters = ArgumentParameters(declaration, isMethod);
    std::vector<std::size_t> slots;
    slots.reserve(call.args.size());
    bool skips = false;
    for (std::size_t index = 0; index < call.args.size(); ++index) {
        const std::size_t next = slots.empty() ? 0 : slots.back() + 1;
        if (call.labels[index].name.empty()) {
            slots.push_back(next);
            continue;
        }
        std::size_t slot = next;
        while (slot < parameters.size() && parameters[slot]->name != call.labels[index].name) {
            if (!parameters[slot]->defaultValue || parameters[slot]->isVariadic) {
                return std::nullopt;
            }
            ++slot;
        }
        if (slot == parameters.size() || parameters[slot]->isVariadic) {
            return std::nullopt;
        }
        skips = skips || slot != next;
        slots.push_back(slot);
    }
    if (!skips) {
        return std::nullopt;
    }
    for (std::size_t slot = slots.back() + 1; slot < parameters.size(); ++slot) {
        if (!parameters[slot]->defaultValue && !parameters[slot]->isVariadic) {
            return std::nullopt;
        }
    }
    return slots;
}

bool AnalysisContext::ValidateArgumentLabels(const CallExpr &call, const FuncDecl &declaration, const bool isMethod) {
    const auto parameters = ArgumentParameters(declaration, isMethod);
    const std::string &callee = declaration.name;
    std::size_t next = 0;
    for (std::size_t index = 0; index < call.labels.size() && index < call.args.size(); ++index) {
        const ArgumentLabel &label = call.labels[index];
        if (label.name.empty()) {
            ++next;
            continue;
        }
        std::size_t slot = 0;
        while (slot < parameters.size() && parameters[slot]->name != label.name) {
            ++slot;
        }
        if (slot == parameters.size()) {
            EmitError(label.location, std::format("'{}' has no parameter named '{}'", callee, label.name),
                      {std::format("its parameters are {}", ParameterNames(parameters))});
            return false;
        }
        if (parameters[slot]->isVariadic) {
            EmitError(label.location, std::format("variadic argument '{}' cannot be labeled", label.name), {},
                      "pass the values without a label");
            return false;
        }
        if (slot < next) {
            EmitError(label.location, std::format("argument label '{}' is out of order", label.name),
                      {std::format("labeled arguments follow the order of the parameters of '{}': {}", callee,
                                   ParameterNames(parameters))},
                      "write the labeled arguments in parameter order; arguments are never reordered");
            return false;
        }
        for (std::size_t skipped = next; skipped < slot; ++skipped) {
            if (!parameters[skipped]->defaultValue) {
                EmitError(
                    label.location,
                    std::format("argument for parameter '{}' of '{}' is missing", parameters[skipped]->name, callee),
                    {std::format("label '{}' skips '{}', and a label may skip only parameters with default "
                                 "values",
                                 label.name, parameters[skipped]->name)},
                    std::format("pass a value for '{}' before '{}'", parameters[skipped]->name, label.name));
                return false;
            }
        }
        next = slot + 1;
    }
    return true;
}

bool AnalysisContext::PrecheckArgumentLabels(const CallExpr &call) {
    if (call.labels.empty()) {
        return true;
    }
    // A name with one declaration is the declaration the call selects, so its labels are checked before its
    // arguments are matched: a label that skips a parameter without a default is reported as that, not as the type
    // mismatch the arguments then make.
    const auto *identifier = AstCast<const IdentExpr>(call.callee.get());
    const Symbol *symbol = identifier ? currentScope->Lookup(identifier->name) : nullptr;
    if (!symbol || symbol->kind != Symbol::Kind::Func || symbol->funcOverloads.size() != 1) {
        return true;
    }
    if (ValidateArgumentLabels(call, *symbol->funcOverloads.front(), false)) {
        return true;
    }
    for (const ExprPtr &argument : call.args) {
        if (argument) {
            static_cast<void>(CheckExpr(*argument));
        }
    }
    return false;
}

void AnalysisContext::CheckArgumentLabels(const CallExpr &call) {
    const bool labeled =
        std::ranges::any_of(call.labels, [](const ArgumentLabel &label) { return !label.name.empty(); });
    if (!labeled || labelsReported.contains(&call)) {
        return;
    }
    const auto binding = callableBindings.find(&call);
    const auto *declaration =
        binding == callableBindings.end() ? nullptr : AstCast<const FuncDecl>(binding->second.selectedDeclaration);
    if (!declaration) {
        const auto first =
            std::ranges::find_if(call.labels, [](const ArgumentLabel &label) { return !label.name.empty(); });
        EmitError(first->location, "argument labels need a call to a declared function or method",
                  {"a function value, a case constructor or an extern function has no parameter names to match"},
                  "remove the labels");
        return;
    }
    const bool isMethod = declaration->Receiver() != nullptr && AstCast<const FieldExpr>(call.callee.get()) != nullptr;
    if (!ValidateArgumentLabels(call, *declaration, isMethod)) {
        return;
    }
    if (const auto slots = ArgumentSlots(call, *declaration, isMethod)) {
        argumentSlots.insert_or_assign(&call, *slots);
    }
}
} // namespace Rux::SemanticDetail
