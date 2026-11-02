#include "Semantic/Analysis/AnalysisContext.h"

#include <cctype>
#include <format>
#include <string>
#include <vector>

// A function returns a reference, `&T`, `&var T` or `&T?`, only when its signature says what the reference borrows
// from: the receiver when it is taken by reference, otherwise the one reference or slice parameter. Everything about
// such a function is decided here once: the signature, what each `return` may yield, and which argument a call's
// result borrows. The borrow and move checks then treat the result as a reference to that argument's storage.
namespace Rux::SemanticDetail {
namespace {
/// The reference an optional reference carries, or null when `type` is not one.
const TypeRef *OptionalReference(const TypeRef &type) noexcept {
    if (!type.IsOptional() || type->inner.size() != 1 || type->inner.front()->kind != TypeRef::Kind::Reference ||
        type->inner.front()->inner.empty()) {
        return nullptr;
    }
    return &type->inner.front();
}

/// Whether a parameter's declared type lets a returned reference borrow from it: a reference, or a slice that views
/// the caller's storage. A variadic parameter is a slice the call builds, so nothing returned from it outlives the
/// call.
bool CanLendTo(const Param &parameter) {
    if (parameter.isVariadic || !parameter.type) {
        return false;
    }
    return AstCast<const ReferenceTypeExpr>(parameter.type.get()) != nullptr ||
           AstCast<const SliceTypeExpr>(parameter.type.get()) != nullptr;
}

/// Where a returned reference was found to come from.
struct BorrowOrigin {
    enum class Kind {
        /// The source parameter, a part of it, or a reference returned from it.
        Source,
        /// Storage reached through a raw pointer, which its user vouches for inside `unsafe`.
        Raw,
        /// `none`: nothing is borrowed.
        Absent,
        /// A local, or a parameter taken by value, that the return destroys.
        Local,
        /// A reference parameter other than the source.
        OtherParameter,
        /// A value the expression computes, which lives no longer than the statement.
        Temporary,
        /// A reference whose storage analysis cannot follow.
        Unknown,
    };

    Kind kind = Kind::Unknown;
    std::string name;
    SourceLocation location;
    /// A local that is a parameter taken by value.
    bool parameter = false;
};
} // namespace

bool AnalysisContext::IsOptionalReference(const TypeRef &type) noexcept {
    return OptionalReference(type) != nullptr;
}

bool AnalysisContext::IsBorrowReturnType(const TypeRef &type) noexcept {
    return (type->kind == TypeRef::Kind::Reference && !type->inner.empty()) || OptionalReference(type) != nullptr;
}

TypeRef AnalysisContext::BorrowedReference(const TypeRef &type) {
    const TypeRef *reference = OptionalReference(type);
    return reference ? *reference : type;
}

std::optional<ResolvedBorrowReturn> AnalysisContext::BorrowReturnOf(const FuncDecl &function) {
    if (!function.returnType || !*function.returnType) {
        return std::nullopt;
    }
    const TypeExpr *returned = function.returnType->get();
    ResolvedBorrowReturn borrow;
    if (const auto *optional = AstCast<const OptionalTypeExpr>(returned)) {
        returned = optional->payload.get();
        borrow.optional = true;
    }
    const auto *reference = AstCast<const ReferenceTypeExpr>(returned);
    if (!reference) {
        return std::nullopt;
    }
    borrow.exclusive = reference->pointeeMut;
    if (const Param *receiver = function.Receiver()) {
        if (AstCast<const ReferenceTypeExpr>(receiver->type.get())) {
            borrow.source = receiver;
            borrow.parameterIndex = 0;
            return borrow;
        }
    }
    std::size_t lenders = 0;
    for (std::size_t index = 0; index < function.params.size(); ++index) {
        const Param &parameter = function.params[index];
        if (!parameter.IsReceiver() && CanLendTo(parameter)) {
            ++lenders;
            borrow.source = &parameter;
            borrow.parameterIndex = index;
        }
    }
    if (lenders != 1) {
        borrow.source = nullptr;
        borrow.parameterIndex = 0;
    }
    return borrow;
}

void AnalysisContext::CheckBorrowReturnSignature(const FuncDecl &function, const SourceLocation location) {
    const std::optional<ResolvedBorrowReturn> borrow = BorrowReturnOf(function);
    if (!borrow) {
        return;
    }
    // An operator is called by its spelling, `v[i]` or `a + b`, which is no call a borrow could be traced through.
    if (!function.name.empty() && !std::isalpha(static_cast<unsigned char>(function.name.front())) &&
        function.name.front() != '_') {
        EmitError(location, std::format("operator '{}' cannot return a reference", function.name),
                  {"what an operator's result borrows is followed only through a method called by name"},
                  "declare a method such as 'At' that returns the reference, or return an owned value");
        return;
    }
    if (borrow->source) {
        borrowReturns.insert_or_assign(&function, *borrow);
        return;
    }
    std::vector<std::string> lenders;
    for (const Param &parameter : function.params) {
        if (!parameter.IsReceiver() && CanLendTo(parameter)) {
            lenders.push_back(std::format("'{}'", parameter.name));
        }
    }
    std::string note;
    if (lenders.empty()) {
        note = std::format("'{}' takes no reference or slice parameter, and no receiver by reference", function.name);
    }
    else {
        std::string names;
        for (std::size_t index = 0; index < lenders.size(); ++index) {
            names += index == 0 ? "" : index + 1 == lenders.size() ? " and " : ", ";
            names += lenders[index];
        }
        note = std::format("{} could each be the source, and nothing in the signature says which", names);
    }
    EmitError(location, "cannot tell what the returned reference borrows from",
              {std::move(note), "a reference result borrows from the receiver taken by reference, or else from the "
                                "only reference or slice parameter"},
              "return an owned value");
}

const Expr *AnalysisContext::BorrowReturnSourceArgument(const CallExpr &call) const {
    const auto binding = callableBindings.find(&call);
    if (binding == callableBindings.end()) {
        return nullptr;
    }
    const auto *function = AstCast<const FuncDecl>(binding->second.selectedDeclaration);
    if (!function) {
        return nullptr;
    }
    const std::optional<ResolvedBorrowReturn> borrow = BorrowReturnOf(*function);
    if (!borrow || !borrow->source) {
        return nullptr;
    }
    // A method call passes its receiver before the argument list; a call through the type passes it among them.
    const auto *member = AstCast<const FieldExpr>(call.callee.get());
    const bool receiverOutside =
        binding->second.dispatch == ResolvedCallableBinding::DispatchKind::Method && member != nullptr;
    if (borrow->source->IsReceiver() && receiverOutside) {
        return member->object.get();
    }
    std::size_t index = borrow->parameterIndex;
    if (receiverOutside && function->Receiver()) {
        --index;
    }
    return index < call.args.size() ? call.args[index].get() : nullptr;
}

const Expr *AnalysisContext::BorrowResultSource(const Expr &expression) const {
    const Expr *current = &expression;
    while (true) {
        if (const auto *field = AstCast<const FieldExpr>(current)) {
            current = field->object.get();
        }
        else if (const auto *index = AstCast<const IndexExpr>(current); index && !IsIndexOperatorCall(*index)) {
            current = index->object.get();
        }
        else {
            break;
        }
    }
    if (const auto *call = AstCast<const CallExpr>(current)) {
        return BorrowReturnSourceArgument(*call);
    }
    if (const auto *binary = AstCast<const BinaryExpr>(current); binary && binary->op == TokenKind::QuestionQuestion) {
        if (const auto type = expressionTypes.find(binary->left.get());
            type != expressionTypes.end() && OptionalReference(type->second)) {
            return binary->left.get();
        }
    }
    return nullptr;
}

void AnalysisContext::CheckBorrowReturnCall(const CallExpr &call, const TypeRef &type) {
    if (!IsBorrowReturnType(type)) {
        return;
    }
    const auto binding = callableBindings.find(&call);
    if (binding == callableBindings.end()) {
        return;
    }
    if (binding->second.dispatch == ResolvedCallableBinding::DispatchKind::Indirect) {
        EmitError(call.location,
                  std::format("cannot call a function value that returns reference '{}'", type.ToString()),
                  {"what a returned reference borrows from is known only for a function called by name"},
                  "call the function by name");
        return;
    }
    const Expr *source = BorrowReturnSourceArgument(call);
    if (!source) {
        return;
    }
    if (const auto sourceType = expressionTypes.find(source);
        sourceType != expressionTypes.end() &&
        (sourceType->second->kind == TypeRef::Kind::Reference || sourceType->second.IsSlice())) {
        return;
    }
    if (ResolveBorrowPlace(*source)) {
        return;
    }
    EmitError(source->location, "the returned reference would borrow from a temporary value",
              {"the temporary is destroyed at the end of the statement, while the reference could still be in use"},
              "store the value in a local and call the function on the local");
}

void AnalysisContext::CheckBorrowReturnValue(const Expr &value, const SourceLocation location) {
    if (!currentBorrowSource) {
        return;
    }
    const auto origin = [&](this const auto &self, const Expr &expression) -> BorrowOrigin {
        const auto typeOf = [&](const Expr &part) -> const TypeRef * {
            const auto found = expressionTypes.find(&part);
            return found == expressionTypes.end() ? nullptr : &found->second;
        };
        const auto symbolOrigin = [&](const std::string &name, const SourceLocation where) -> BorrowOrigin {
            const Symbol *symbol = currentScope->Lookup(name);
            if (!symbol || symbol->kind != Symbol::Kind::Var) {
                return {BorrowOrigin::Kind::Unknown, name, where};
            }
            if (symbol == currentBorrowSource) {
                return {BorrowOrigin::Kind::Source, name, where};
            }
            const TypeRef &type = symbol->type;
            if (type->kind == TypeRef::Kind::Pointer) {
                return {BorrowOrigin::Kind::Raw, name, where};
            }
            const bool lends = type->kind == TypeRef::Kind::Reference || type.IsSlice();
            if (symbol->isParameter) {
                return {lends ? BorrowOrigin::Kind::OtherParameter : BorrowOrigin::Kind::Local, name, where, true};
            }
            if (!lends && !OptionalReference(type)) {
                return {BorrowOrigin::Kind::Local, name, where};
            }
            // A local reference lends what it was given.
            const ActiveBorrow *borrow = nullptr;
            if (const auto active = activeBorrows.find(symbol); active != activeBorrows.end()) {
                borrow = &active->second;
            }
            else if (const auto ended = endedBorrowProvenance.find(symbol); ended != endedBorrowProvenance.end()) {
                borrow = &ended->second;
            }
            if (!borrow || !borrow->place.root) {
                return {BorrowOrigin::Kind::Unknown, name, where};
            }
            const Symbol *root = borrow->place.root;
            if (root == currentBorrowSource) {
                return {BorrowOrigin::Kind::Source, name, where};
            }
            if (root->type->kind == TypeRef::Kind::Pointer) {
                return {BorrowOrigin::Kind::Raw, root->name, where};
            }
            if (root->isParameter && (root->type->kind == TypeRef::Kind::Reference || root->type.IsSlice())) {
                return {BorrowOrigin::Kind::OtherParameter, root->name, where};
            }
            return {BorrowOrigin::Kind::Local, root->name, where, root->isParameter};
        };
        // Every way the value can be produced must lend from the source; the first that does not is reported.
        const auto both = [&](const BorrowOrigin &left, const BorrowOrigin &right) {
            const auto lends = [](const BorrowOrigin &candidate) {
                return candidate.kind == BorrowOrigin::Kind::Source || candidate.kind == BorrowOrigin::Kind::Raw ||
                       candidate.kind == BorrowOrigin::Kind::Absent;
            };
            return lends(left) ? right : left;
        };

        if (const auto *identifier = AstCast<const IdentExpr>(&expression)) {
            return symbolOrigin(identifier->name, expression.location);
        }
        if (AstCast<const SelfExpr>(&expression)) {
            return symbolOrigin("self", expression.location);
        }
        if (AstCast<const NoneExpr>(&expression)) {
            return {BorrowOrigin::Kind::Absent, {}, expression.location};
        }
        if (const auto *field = AstCast<const FieldExpr>(&expression)) {
            if (const TypeRef *object = typeOf(*field->object); object && (*object)->kind == TypeRef::Kind::Pointer) {
                return {BorrowOrigin::Kind::Raw, {}, expression.location};
            }
            return self(*field->object);
        }
        if (const auto *index = AstCast<const IndexExpr>(&expression)) {
            if (IsIndexOperatorCall(*index)) {
                return {BorrowOrigin::Kind::Temporary, {}, expression.location};
            }
            if (const TypeRef *object = typeOf(*index->object); object && (*object)->kind == TypeRef::Kind::Pointer) {
                return {BorrowOrigin::Kind::Raw, {}, expression.location};
            }
            return self(*index->object);
        }
        if (const auto *unary = AstCast<const UnaryExpr>(&expression); unary && unary->op == TokenKind::Star) {
            return {BorrowOrigin::Kind::Raw, {}, expression.location};
        }
        if (const auto *call = AstCast<const CallExpr>(&expression)) {
            if (const Expr *source = BorrowReturnSourceArgument(*call)) {
                return self(*source);
            }
            return {BorrowOrigin::Kind::Temporary, {}, expression.location};
        }
        if (const auto *construct = AstCast<const NativeConstructExpr>(&expression);
            construct && construct->kind == NativeConstructExpr::Kind::Some && construct->operand) {
            return self(*construct->operand);
        }
        if (const auto *binary = AstCast<const BinaryExpr>(&expression);
            binary && binary->op == TokenKind::QuestionQuestion) {
            // A fallback that leaves, such as `?? Panic(...)`, yields nothing to borrow.
            return both(self(*binary->left), IsDivergingExpression(*binary->right)
                                                 ? BorrowOrigin{BorrowOrigin::Kind::Absent, {}, expression.location}
                                                 : self(*binary->right));
        }
        if (const auto *block = AstCast<const BlockExpr>(&expression)) {
            const auto *tail = block->block && !block->block->stmts.empty()
                                 ? AstCast<const ExprStmt>(block->block->stmts.back().get())
                                 : nullptr;
            if (tail && tail->yieldsValue && tail->expr) {
                return self(*tail->expr);
            }
            return {BorrowOrigin::Kind::Unknown, {}, expression.location};
        }
        if (const auto *match = AstCast<const MatchExpr>(&expression)) {
            BorrowOrigin result{BorrowOrigin::Kind::Absent, {}, expression.location};
            for (const MatchExpr::Arm &arm : match->arms) {
                if (arm.body && !IsDivergingExpression(*arm.body)) {
                    result = both(result, self(*arm.body));
                }
            }
            return result;
        }
        return {BorrowOrigin::Kind::Temporary, {}, expression.location};
    };

    const BorrowOrigin found = origin(value);
    const std::string function = currentFunctionDecl ? std::format("'{}'", currentFunctionDecl->name) : "the function";
    const std::string source = currentBorrowSource->name;
    switch (found.kind) {
    case BorrowOrigin::Kind::Source:
    case BorrowOrigin::Kind::Raw:
    case BorrowOrigin::Kind::Absent:
        return;
    case BorrowOrigin::Kind::Local:
        EmitError(
            location,
            std::format("cannot return a reference to {} '{}'", found.parameter ? "parameter" : "local", found.name),
            {std::format("'{}' {}is destroyed when {} returns, while the reference would still be in use", found.name,
                         found.parameter ? "is taken by value, so it " : "", function),
             std::format("the reference {} returns must borrow from '{}'", function, source)},
            "return an owned value");
        return;
    case BorrowOrigin::Kind::OtherParameter:
        EmitError(location, std::format("cannot return a reference borrowed from '{}'", found.name),
                  {std::format("the reference {} returns borrows from '{}', so callers keep only '{}' borrowed",
                               function, source, source)},
                  std::format("return a reference derived from '{}', or an owned value", source));
        return;
    case BorrowOrigin::Kind::Temporary:
        EmitError(location, "cannot return a reference to a temporary value",
                  {"the value is destroyed when the return completes, while the reference would still be in use",
                   std::format("the reference {} returns must borrow from '{}'", function, source)},
                  "return an owned value");
        return;
    case BorrowOrigin::Kind::Unknown:
        EmitError(location, std::format("cannot tell that the returned reference borrows from '{}'", source),
                  {std::format("the reference {} returns must be a field or element of '{}', or a reference "
                               "returned from it",
                               function, source)},
                  "return the place itself rather than a reference that holds it");
        return;
    }
}

bool AnalysisContext::RejectImmutableBorrowReturn(const Expr &value, const TypeRef &valueType,
                                                  const SourceLocation location) {
    const TypeRef reference = BorrowedReference(currentReturnType);
    if (reference->kind != TypeRef::Kind::Reference || reference->inner.empty() || !reference->inner.front().isMut ||
        valueType->kind == TypeRef::Kind::Reference || valueType->kind == TypeRef::Kind::Pointer ||
        valueType.IsOptional() || valueType.IsUnknown()) {
        return false;
    }
    if (!CanAssignExprTo(value, valueType, reference.WithWritableInner(false)) || !PlaceIsImmutable(value)) {
        return false;
    }
    const Expr *root = &value;
    while (true) {
        if (const auto *field = AstCast<const FieldExpr>(root)) {
            root = field->object.get();
        }
        else if (const auto *index = AstCast<const IndexExpr>(root); index && !IsIndexOperatorCall(*index)) {
            root = index->object.get();
        }
        else {
            break;
        }
    }
    std::string name;
    if (const auto *identifier = AstCast<const IdentExpr>(root)) {
        name = identifier->name;
    }
    else if (AstCast<const SelfExpr>(root)) {
        name = "self";
    }
    const Symbol *binding = name.empty() ? nullptr : currentScope->Lookup(name);
    const std::string subject = name.empty()   ? std::string("an immutable value")
                              : root == &value ? std::format("immutable '{}'", name)
                                               : std::format("a part of immutable '{}'", name);
    std::optional<std::string> help;
    if (binding && binding->kind == Symbol::Kind::Var) {
        help =
            binding->type->kind == TypeRef::Kind::Reference
                ? std::format("take '{}' as '&var {}'", name,
                              binding->type->inner.empty() ? std::string("T") : binding->type->inner.front().ToString())
                : ImmutableBindingHelp(*binding);
        if (name == "self" && binding->type->kind == TypeRef::Kind::Reference) {
            help = "take the receiver as '&var self'";
        }
    }
    EmitError(location, std::format("cannot return {} as '{}'", subject, reference.ToString()),
              {"a mutable reference can only borrow storage the function may write"}, std::move(help));
    return true;
}
} // namespace Rux::SemanticDetail
