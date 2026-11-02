// The unsafe boundary: which operations need an `unsafe` block or an `unsafe func` body, and which blocks need none.
//
// Every operation is judged once, where `CheckExpr` has just checked it and recorded the types of its operands, so the
// rule lives here rather than in each place an operation is typed. Nothing is recorded for lowering: an `unsafe`
// block is lowered as the block it holds.

#include "Semantic/Analysis/AnalysisContext.h"

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <utility>

namespace Rux::SemanticDetail {
namespace {
/// The raw pointer `type` is, or the one a reference to it reads: indexing looks through one reference, as it does for
/// arrays and slices.
const TypeRef *RawPointerOf(const TypeRef &type) {
    if (type->kind == TypeRef::Kind::Pointer) {
        return &type;
    }
    if (type->kind == TypeRef::Kind::Reference && !type->inner.empty() &&
        type->inner.front()->kind == TypeRef::Kind::Pointer) {
        return &type->inner.front();
    }
    return nullptr;
}

bool AddsPointeeType(const TypeRef &operand, const TypeRef &target) {
    // A cast to `*opaque` forgets the type; one that keeps it, perhaps dropping write access, only weakens.
    const TypeRef &pointee = target->inner.front();
    if (pointee.IsOpaque()) {
        return false;
    }
    if (operand->kind != TypeRef::Kind::Pointer || operand->inner.empty()) {
        return true;
    }
    TypeRef from = operand->inner.front();
    TypeRef to = pointee;
    from.isMut = false;
    to.isMut = false;
    return from != to;
}
} // namespace

bool AnalysisContext::InUnsafeContext() const noexcept {
    return unsafeFunction != nullptr || !unsafeBlockFrames.empty();
}

std::optional<std::string> AnalysisContext::UnsafeCalleeOf(const CallExpr &call) {
    const auto describe = [](const FuncDecl &function) -> std::optional<std::string> {
        if (function.isAsm) {
            return std::format("'asm' function '{}'", function.name);
        }
        if (function.isUnsafe) {
            return std::format("unsafe {} '{}'", function.Receiver() ? "method" : "function", function.name);
        }
        return std::nullopt;
    };
    if (const auto binding = callableBindings.find(&call); binding != callableBindings.end()) {
        if (const auto *external = AstCast<const ExternFuncDecl>(binding->second.selectedDeclaration)) {
            return std::format("extern function '{}'", external->name);
        }
        if (const auto *function = AstCast<const FuncDecl>(binding->second.selectedDeclaration)) {
            return describe(*function);
        }
        return std::nullopt;
    }
    // A call that failed to bind still names what it calls when the name has one meaning.
    if (!call.callee) {
        return std::nullopt;
    }
    const Symbol *symbol = LookupCalleeSymbol(*call.callee);
    if (!symbol) {
        return std::nullopt;
    }
    if (symbol->externDecl) {
        return std::format("extern function '{}'", symbol->externDecl->name);
    }
    if (symbol->funcOverloads.size() == 1) {
        return describe(*symbol->funcOverloads.front());
    }
    return std::nullopt;
}

std::optional<std::string> AnalysisContext::UnsafeOperationOf(const Expr &expression, const TypeRef &type) {
    const auto recorded = [&](const Expr *operand) -> TypeRef {
        if (!operand) {
            return TypeRef::MakeUnknown();
        }
        const auto found = expressionTypes.find(operand);
        return found != expressionTypes.end() ? found->second : TypeRef::MakeUnknown();
    };

    if (const auto *unary = AstCast<const UnaryExpr>(&expression)) {
        if (unary->op == TokenKind::Star && recorded(unary->operand.get())->kind == TypeRef::Kind::Pointer) {
            return "dereferencing a raw pointer";
        }
        return std::nullopt;
    }
    if (const auto *index = AstCast<const IndexExpr>(&expression)) {
        // A declared `[]` or `[]=` is a call of a method, and the pointer indexing is built in only.
        if (indexOperators.contains(index) || indexAssignments.contains(index)) {
            return std::nullopt;
        }
        if (RawPointerOf(recorded(index->object.get()))) {
            return "indexing a raw pointer";
        }
        return std::nullopt;
    }
    if (const auto *field = AstCast<const FieldExpr>(&expression)) {
        if (recorded(field->object.get())->kind == TypeRef::Kind::Pointer) {
            return std::format("accessing field '{}' through a raw pointer", field->field);
        }
        return std::nullopt;
    }
    if (const auto *binary = AstCast<const BinaryExpr>(&expression)) {
        if ((binary->op == TokenKind::Plus || binary->op == TokenKind::Minus) &&
            (RawPointerOf(recorded(binary->left.get())) || RawPointerOf(recorded(binary->right.get())))) {
            return "pointer arithmetic";
        }
        return std::nullopt;
    }
    if (const auto *assignment = AstCast<const AssignExpr>(&expression)) {
        if ((assignment->op == TokenKind::PlusAssign || assignment->op == TokenKind::MinusAssign) &&
            RawPointerOf(recorded(assignment->target.get()))) {
            return "pointer arithmetic";
        }
        return std::nullopt;
    }
    if (const auto *cast = AstCast<const CastExpr>(&expression)) {
        TypeRef operand = recorded(cast->operand.get());
        if (operand->kind == TypeRef::Kind::Reference && !operand->inner.empty()) {
            operand = operand->inner.front();
        }
        if (operand.IsUnknown() || type.IsUnknown()) {
            return std::nullopt;
        }
        // A cast that would give a read-only pointer write access is rejected outright, and once is enough.
        if (operand->kind == TypeRef::Kind::Pointer && type->kind == TypeRef::Kind::Pointer &&
            !operand->inner.empty() && !type->inner.empty() && !operand->inner.front().isMut &&
            type->inner.front().isMut && !operand->inner.front().IsOpaque() && !type->inner.front().IsOpaque()) {
            return std::nullopt;
        }
        // A pointer that gains a type it did not have, from an integer, from untyped memory or from another type, and
        // a function made from an address, are the casts that create what the compiler cannot check.
        const bool toTypedPointer =
            type->kind == TypeRef::Kind::Pointer && !type->inner.empty() && AddsPointeeType(operand, type);
        const bool toFunction = type->kind == TypeRef::Kind::Func && operand->kind != TypeRef::Kind::Func;
        if (toTypedPointer || toFunction) {
            return std::format("casting '{}' to '{}'", operand.DisplayString(), type.DisplayString());
        }
        return std::nullopt;
    }
    if (const auto *call = AstCast<const CallExpr>(&expression)) {
        if (const std::optional<std::string> callee = UnsafeCalleeOf(*call)) {
            return std::format("call to {}", *callee);
        }
        return std::nullopt;
    }
    if (AstCast<const IdentExpr>(&expression) || AstCast<const PathExpr>(&expression)) {
        if (const auto global = globalReferences.find(&expression); global != globalReferences.end()) {
            // `@Name` takes the address and touches nothing; reading or writing through it is what needs `unsafe`.
            const auto *external = AstCast<const ExternVarDecl>(global->second);
            if (external && &expression != addressOperand) {
                return std::format("access to extern variable '{}'", external->name);
            }
            return std::nullopt;
        }
        // A function named as a value can be called later through it, where nothing marks the call.
        if (type->kind != TypeRef::Kind::Func || &expression == calleeBeingChecked) {
            return std::nullopt;
        }
        const Symbol *symbol = LookupCalleeSymbol(expression);
        if (!symbol) {
            return std::nullopt;
        }
        if (symbol->externDecl) {
            return std::format("using extern function '{}' as a value", symbol->externDecl->name);
        }
        if (!symbol->funcOverloads.empty() && std::ranges::all_of(symbol->funcOverloads, [](const FuncDecl *function) {
                return function->isUnsafe || function->isAsm;
            })) {
            const FuncDecl &function = *symbol->funcOverloads.front();
            return std::format("using {} function '{}' as a value", function.isAsm ? "'asm'" : "unsafe", function.name);
        }
    }
    return std::nullopt;
}

void AnalysisContext::CheckUnsafeOperation(const Expr &expression, const TypeRef &type) {
    const std::optional<std::string> operation = UnsafeOperationOf(expression, type);
    if (!operation) {
        return;
    }
    if (!unsafeBlockFrames.empty()) {
        ++unsafeBlockFrames.back().operations;
        return;
    }
    if (unsafeFunction) {
        return;
    }
    std::vector<std::string> notes;
    if (operation->starts_with("call to unsafe")) {
        notes.emplace_back("its documentation states what the caller must uphold");
    }
    else if (operation->starts_with("call to extern")) {
        notes.emplace_back("the compiler cannot check what a foreign function does with its arguments");
    }
    else if (operation->starts_with("dereferencing") || operation->starts_with("indexing") ||
             operation->starts_with("accessing field")) {
        notes.emplace_back("the compiler cannot prove that a raw pointer addresses a live value of its type");
    }
    // The call's own location is the token after its callee, so the report points at the callee instead.
    const SourceLocation location = AstCast<const CallExpr>(&expression)
                                      ? CallSiteLocation(static_cast<const CallExpr &>(expression))
                                      : expression.location;
    EmitError(location, std::format("{} requires 'unsafe'", *operation), std::move(notes),
              "wrap it in 'unsafe { ... }' and say why it holds in a '// Safety:' comment above it");
}

void AnalysisContext::BeginUnsafeBlock(const BlockExpr &expression) {
    UnsafeBlockFrame frame;
    frame.block = &expression;
    frame.diagnosticStart = diags.size();
    frame.nested = InUnsafeContext();
    if (frame.nested) {
        EmitWarning(
            expression.location, "unnecessary 'unsafe' block",
            {unsafeFunction && unsafeBlockFrames.empty()
                 ? std::format("the body of unsafe function '{}' is an unsafe context already", unsafeFunction->name)
                 : std::string("it is inside another 'unsafe' block")},
            "remove 'unsafe' and keep the block");
    }
    unsafeBlockFrames.push_back(frame);
}

void AnalysisContext::EndUnsafeBlock(const BlockExpr &expression) {
    const UnsafeBlockFrame frame = unsafeBlockFrames.back();
    unsafeBlockFrames.pop_back();
    if (!unsafeBlockFrames.empty()) {
        unsafeBlockFrames.back().operations += frame.operations;
    }
    if (frame.nested || frame.operations != 0) {
        return;
    }
    // A block whose contents did not check cleanly may hold an operation whose type could not be told.
    const bool failed = std::any_of(diags.begin() + static_cast<std::ptrdiff_t>(frame.diagnosticStart), diags.end(),
                                    [](const SemanticDiagnostic &diagnostic) {
                                        return diagnostic.severity == SemanticDiagnostic::Severity::Error;
                                    });
    if (!failed) {
        EmitWarning(expression.location, "unnecessary 'unsafe' block",
                    {"it contains no operation that requires 'unsafe'"}, "remove 'unsafe' and keep the block");
    }
}

void AnalysisContext::ReportUnusedUnsafeBlockValue(const BlockExpr &expression, const TypeRef &yielded) {
    if (yielded.IsUnknown() || yielded.IsUnit() || yielded.IsOpaque() || !expression.block) {
        return;
    }
    const ExprStmt *tail = YieldedStatement(*expression.block);
    if (!tail) {
        return;
    }
    EmitError(tail->location,
              std::format("this 'unsafe' block yields a value of type '{}' that is not used", yielded.DisplayString()),
              {"an 'unsafe' block written as a statement yields nothing, so its last expression needs a ';'"},
              "add ';' or use the value");
}
} // namespace Rux::SemanticDetail
