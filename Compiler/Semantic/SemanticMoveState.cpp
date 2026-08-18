#include "Semantic/Detail/SemanticAnalyzerContext.h"

#include <format>
#include <utility>

namespace Rux::SemanticDetail {
Symbol *SemanticAnalyzerContext::Define(Symbol symbol) {
    const std::string name = symbol.name;
    if (!currentScope->Define(std::move(symbol), diags, currentFile)) {
        return nullptr;
    }
    return currentScope->LookupLocal(name);
}

Symbol *SemanticAnalyzerContext::DefineTrackedLocal(Symbol symbol, const bool initialized) {
    Symbol *defined = Define(std::move(symbol));
    if (defined) {
        moveStates.Declare(MoveStateTracker::Local(defined),
                           initialized ? MoveStateTracker::State::Initialized : MoveStateTracker::State::Uninitialized,
                           defined->location);
    }
    return defined;
}

const FuncDecl *SemanticAnalyzerContext::BeginTrackedFunction(const FuncDecl &function) {
    const FuncDecl *previousFunction = currentFunctionDecl;
    savedMoveStates.push_back(std::move(moveStates));
    moveStates.Reset();
    currentFunctionDecl = &function;
    return previousFunction;
}

void SemanticAnalyzerContext::EndTrackedFunction(const FuncDecl *previousFunction) {
    currentFunctionDecl = previousFunction;
    moveStates = std::move(savedMoveStates.back());
    savedMoveStates.pop_back();
}

void SemanticAnalyzerContext::CheckTrackedRead(const Symbol &symbol, const SourceLocation location) {
    const std::optional<MoveStateTracker::Issue> issue = moveStates.Read(MoveStateTracker::Local(&symbol));
    if (!issue) {
        return;
    }

    if (issue->kind == MoveStateTracker::IssueKind::Uninitialized) {
        EmitError(location, std::format("variable '{}' is used before it is initialized", symbol.name),
                  {std::format("'{}' was declared without an initializer at {}:{}", symbol.name,
                               issue->previousTransition.line, issue->previousTransition.column)},
                  std::format("assign a value to '{}' before this use", symbol.name));
        return;
    }

    EmitError(location, std::format("value '{}' is used after it was moved", symbol.name),
              {std::format("'{}' was moved at {}:{}", symbol.name, issue->previousTransition.line,
                           issue->previousTransition.column)},
              std::format("clone '{}' before moving it if both uses are required", symbol.name));
}

TypeRef SemanticAnalyzerContext::ReadTrackedSymbol(const Symbol &symbol, const SourceLocation location) {
    if (symbol.kind == Symbol::Kind::Var && !checkingPlainAssignmentTarget) {
        CheckTrackedRead(symbol, location);
    }
    return symbol.type;
}

void SemanticAnalyzerContext::RecordCheckedExpression(const Expr &expression, const TypeRef &type) {
    if (type.IsUnknown()) {
        return;
    }
    expressionTypes.insert_or_assign(&expression, type);
    if (!dynamic_cast<const IdentExpr *>(&expression)) {
        moveStates.Declare(MoveStateTracker::Temporary(&expression), MoveStateTracker::State::Initialized,
                           expression.location);
    }
}

void SemanticAnalyzerContext::MarkTrackedAssignment(const Expr &target, const SourceLocation location) {
    const Expr *root = &target;
    while (true) {
        if (const auto *field = dynamic_cast<const FieldExpr *>(root)) {
            root = field->object.get();
            continue;
        }
        if (const auto *index = dynamic_cast<const IndexExpr *>(root)) {
            root = index->object.get();
            continue;
        }
        break;
    }

    const auto *identifier = dynamic_cast<const IdentExpr *>(root);
    if (!identifier) {
        return;
    }
    if (Symbol *symbol = currentScope->Lookup(identifier->name); symbol && symbol->kind == Symbol::Kind::Var) {
        moveStates.Assign(MoveStateTracker::Local(symbol), location);
    }
}

std::optional<MoveStateTracker::Issue> SemanticAnalyzerContext::MoveTrackedExpression(const Expr &expression,
                                                                                      const SourceLocation location) {
    if (const auto *identifier = dynamic_cast<const IdentExpr *>(&expression)) {
        if (Symbol *symbol = currentScope->Lookup(identifier->name); symbol && symbol->kind == Symbol::Kind::Var) {
            return moveStates.Move(MoveStateTracker::Local(symbol), location);
        }
    }
    return moveStates.Move(MoveStateTracker::Temporary(&expression), location);
}
} // namespace Rux::SemanticDetail
