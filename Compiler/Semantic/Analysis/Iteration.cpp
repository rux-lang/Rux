// The iterator convention: what makes a value iterable, what a `for` loop reads out of it, and the signatures a type
// has to declare for the compiler to drive it.
//
// Nothing here is a built-in type. An iterator is any type with a `Next` that advances it and reports either the next
// item or the end, and an iterable is any type that hands out one. `for` over an array, a slice or a range keeps its
// own direct form; the convention is what lets a user-written container join them.

#include "Semantic/Analysis/AnalysisContext.h"

#include <format>

namespace Rux::SemanticDetail {
namespace {
constexpr std::string_view kNextMethod = "Next";
constexpr std::string_view kIterateMethod = "Iterate";
/// The cases a `Next` reports with. They are `Option`'s, named here because the loop matches them by name.
constexpr std::string_view kOptionSomeVariant = "Some";
constexpr std::string_view kOptionNoneVariant = "None";

/// The parameters a method declares besides its receiver.
[[nodiscard]] std::size_t WrittenParameterCount(const FuncDecl &declaration) {
    std::size_t count = 0;
    for (const Param &parameter : declaration.params) {
        if (!parameter.IsReceiver()) {
            ++count;
        }
    }
    return count;
}

[[nodiscard]] bool ReceiverIsMutableBorrow(const FuncDecl &declaration) {
    const Param *receiver = declaration.Receiver();
    if (!receiver) {
        return false;
    }
    if (const auto *reference = dynamic_cast<const ReferenceTypeExpr *>(receiver->type.get())) {
        return reference->pointeeMut;
    }
    const auto *pointer = dynamic_cast<const PointerTypeExpr *>(receiver->type.get());
    return pointer != nullptr && pointer->pointeeMut;
}
} // namespace

const FuncDecl *AnalysisContext::LookupIteratorAdvance(const TypeRef &type) const {
    const std::string typeName = NamedBaseTypeName(type);
    if (typeName.empty()) {
        return nullptr;
    }
    const auto methods = methodsByType.find(typeName);
    if (methods == methodsByType.end()) {
        return nullptr;
    }
    const auto named = methods->second.find(std::string(kNextMethod));
    if (named == methods->second.end()) {
        return nullptr;
    }
    for (const FuncDecl *candidate : named->second) {
        if (WrittenParameterCount(*candidate) != 0 || !ReceiverIsMutableBorrow(*candidate)) {
            continue;
        }
        return candidate;
    }
    return nullptr;
}

std::optional<AnalysisContext::ReportedItem> AnalysisContext::ReportedItemOf(const TypeRef &iteratorType,
                                                                             const FuncDecl &advance) {
    if (!advance.returnType) {
        return std::nullopt;
    }
    const TypeRef returned = ResolveMethodReturnType(iteratorType, advance);
    const auto reported = PropagationShapeOf(returned);
    if (!reported || reported->kind != PropagationShape::Kind::Option) {
        return std::nullopt;
    }
    return ReportedItem{reported->payload, returned, reported->declaration};
}

const FuncDecl *AnalysisContext::LookupIterableEntry(const TypeRef &type) const {
    const std::string typeName = NamedBaseTypeName(type);
    if (typeName.empty()) {
        return nullptr;
    }
    const auto methods = methodsByType.find(typeName);
    if (methods == methodsByType.end()) {
        return nullptr;
    }
    const auto named = methods->second.find(std::string(kIterateMethod));
    if (named == methods->second.end()) {
        return nullptr;
    }
    for (const FuncDecl *candidate : named->second) {
        if (WrittenParameterCount(*candidate) == 0) {
            return candidate;
        }
    }
    return nullptr;
}

std::optional<AnalysisContext::IterationShape> AnalysisContext::IterationShapeOf(const TypeRef &subject) {
    IterationShape shape;
    if (subject.IsIterableRange() && !subject.inner.empty()) {
        shape.kind = IterationShape::Kind::Range;
        shape.itemType = subject.inner[0];
        return shape;
    }
    if (auto element = IndexElementType(subject)) {
        shape.kind = IterationShape::Kind::Indexed;
        shape.itemType = *element;
        return shape;
    }

    if (const FuncDecl *advance = LookupIteratorAdvance(subject)) {
        if (auto reported = ReportedItemOf(subject, *advance)) {
            shape.kind = IterationShape::Kind::Iterator;
            shape.itemType = std::move(reported->itemType);
            shape.iteratorType = subject;
            shape.advance = advance;
            shape.reportedType = std::move(reported->reportedType);
            shape.reportedDeclaration = reported->declaration;
            return shape;
        }
        return std::nullopt;
    }

    // A container hands out an iterator rather than being one, which is what keeps two loops over the same container
    // independent of each other.
    if (const FuncDecl *entry = LookupIterableEntry(subject)) {
        const TypeRef iteratorType = ResolveMethodReturnType(subject, *entry);
        if (const FuncDecl *advance = LookupIteratorAdvance(iteratorType)) {
            if (auto reported = ReportedItemOf(iteratorType, *advance)) {
                shape.kind = IterationShape::Kind::Iterable;
                shape.itemType = std::move(reported->itemType);
                shape.iteratorType = iteratorType;
                shape.advance = advance;
                shape.entry = entry;
                shape.reportedType = std::move(reported->reportedType);
                shape.reportedDeclaration = reported->declaration;
                return shape;
            }
        }
    }
    return std::nullopt;
}

void AnalysisContext::RecordIteration(const ForStmt &statement, const IterationShape &shape) {
    ResolvedIteration iteration;
    switch (shape.kind) {
    case IterationShape::Kind::Range:
        iteration.kind = ResolvedIteration::Kind::Range;
        break;
    case IterationShape::Kind::Indexed:
        iteration.kind = ResolvedIteration::Kind::Indexed;
        break;
    case IterationShape::Kind::Iterator:
        iteration.kind = ResolvedIteration::Kind::Iterator;
        break;
    case IterationShape::Kind::Iterable:
        iteration.kind = ResolvedIteration::Kind::Iterable;
        break;
    }
    iteration.itemType = shape.itemType;
    iteration.iteratorType = shape.iteratorType;
    iteration.advance = shape.advance;
    iteration.entry = shape.entry;
    iteration.reportedType = shape.reportedType;
    if (shape.reportedDeclaration) {
        iteration.optionVariantName = shape.reportedDeclaration->name;
        iteration.someVariant = std::string(kOptionSomeVariant);
        iteration.noneVariant = std::string(kOptionNoneVariant);
    }
    iterations.insert_or_assign(&statement, std::move(iteration));
}

void AnalysisContext::EmitNotIterable(const SourceLocation location, const TypeRef &subject) const {
    std::vector<std::string> notes;
    // A type that came close says so: the mistake is almost always the receiver or the return type of its `Next`, not
    // the absence of one.
    const std::string typeName = NamedBaseTypeName(subject);
    if (const auto methods = methodsByType.find(typeName); methods != methodsByType.end()) {
        if (methods->second.contains(std::string(kNextMethod))) {
            notes.push_back(
                std::format("type '{}' declares 'Next', but not as 'func Next(self: &var {}) -> Option<T>' returning "
                            "an Option-shaped variant",
                            subject.ToString(), typeName));
        }
        else if (methods->second.contains(std::string(kIterateMethod))) {
            notes.push_back(
                std::format("type '{}' declares 'Iterate', but it does not return an iterator", subject.ToString()));
        }
    }
    EmitError(location, std::format("cannot iterate over '{}'", subject.ToString()), std::move(notes),
              "iterate an array, a slice, a range, or a type declaring 'Next' or 'Iterate'");
}

void AnalysisContext::ValidateIteratorConvention(const FuncDecl &declaration, const bool isMethod) {
    if (!isMethod || !currentImpl || currentExtendedType.IsUnknown()) {
        return;
    }

    if (declaration.name == kNextMethod) {
        // Only a `Next` that reports an end is the convention's; one returning anything else is an ordinary method that
        // happens to share the name.
        const TypeRef returned =
            declaration.returnType ? ResolveType(*declaration.returnType->get()) : TypeRef::MakeOpaque();
        const auto reported = PropagationShapeOf(returned);
        if (!reported || reported->kind != PropagationShape::Kind::Option) {
            if (auto issue = PropagationShapeIssue(returned, PropagationShape::Kind::Option)) {
                EmitError(declaration.location,
                          std::format("iterator method 'Next' on '{}' must return an Option-shaped variant",
                                      currentExtendedType.ToString()),
                          {std::move(*issue)}, "return a variant with exactly 'Some(T)' and payload-less 'None' cases");
            }
            return;
        }
        if (WrittenParameterCount(declaration) != 0) {
            EmitError(declaration.location,
                      std::format("iterator method 'Next' on '{}' takes no parameters besides its receiver",
                                  currentExtendedType.ToString()),
                      {"'for' calls 'Next' with no arguments"});
        }
        if (!ReceiverIsMutableBorrow(declaration)) {
            EmitError(declaration.location,
                      std::format("iterator method 'Next' on '{}' must take a mutable receiver",
                                  currentExtendedType.ToString()),
                      {"advancing an iterator writes it, so 'Next' cannot borrow its receiver read-only"},
                      std::format("write the receiver as 'self: &var {}'", NamedBaseTypeName(currentExtendedType)));
        }
        return;
    }

    if (declaration.name == kIterateMethod && WrittenParameterCount(declaration) == 0) {
        // The name and the arity are the convention's, so what it returns has to be drivable: reporting it here names
        // the declaration at fault instead of leaving every `for` over the container to report the same thing.
        const TypeRef iteratorType =
            declaration.returnType ? ResolveType(*declaration.returnType->get()) : TypeRef::MakeOpaque();
        if (iteratorType.IsUnknown()) {
            return;
        }
        const FuncDecl *advance = LookupIteratorAdvance(iteratorType);
        if (advance && ReportedItemOf(iteratorType, *advance)) {
            return;
        }
        EmitError(
            declaration.location,
            std::format("iterator method 'Iterate' on '{}' must return an iterator", currentExtendedType.ToString()),
            {std::format("type '{}' has no 'Next' returning an 'Option'", iteratorType.ToString())},
            "give the returned type 'func Next(self: &var T) -> Option<Item>'");
    }
}
} // namespace Rux::SemanticDetail
