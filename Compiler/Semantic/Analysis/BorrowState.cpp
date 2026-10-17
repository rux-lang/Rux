#include "Semantic/Analysis/AnalysisContext.h"

#include <algorithm>
#include <format>
#include <functional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Rux::SemanticDetail {
namespace {
using NameSet = std::unordered_set<std::string>;
using LiveAfterMap = std::unordered_map<const Stmt *, NameSet>;
using LastUseOffsets = std::unordered_map<std::string, std::uint32_t>;
using LastUseMap = std::unordered_map<const Stmt *, LastUseOffsets>;

void AddAll(NameSet &destination, const NameSet &source) {
    destination.insert(source.begin(), source.end());
}

void CollectExpressionUses(const Expr &expression, NameSet &uses);
NameSet AnalyzeBlockLiveness(const Block &block, NameSet liveAfter, LiveAfterMap &result);
void RecordBlockLastUses(const Block &block, LastUseMap &result);

void CollectPatternUses(const Pattern &pattern, NameSet &uses) {
    if (const auto *guarded = AstCast<const GuardedPattern>(&pattern)) {
        CollectPatternUses(*guarded->inner, uses);
        CollectExpressionUses(*guarded->guard, uses);
    }
    else if (const auto *enumeration = AstCast<const EnumPattern>(&pattern)) {
        for (const auto &argument : enumeration->args) {
            CollectPatternUses(*argument, uses);
        }
        for (const auto &argument : enumeration->namedArgs) {
            CollectPatternUses(*argument.pattern, uses);
        }
    }
    else if (const auto *structure = AstCast<const StructPattern>(&pattern)) {
        for (const auto &field : structure->fields) {
            CollectPatternUses(*field.pattern, uses);
        }
    }
    else if (const auto *tuple = AstCast<const TuplePattern>(&pattern)) {
        for (const auto &element : tuple->elements) {
            CollectPatternUses(*element, uses);
        }
    }
}

void CollectExpressionUses(const Expr &expression, NameSet &uses) {
    if (const auto *identifier = AstCast<const IdentExpr>(&expression)) {
        uses.insert(identifier->name);
    }
    else if (AstCast<const SelfExpr>(&expression)) {
        uses.insert("self");
    }
    else if (const auto *intrinsic = AstCast<const IntrinsicExpr>(&expression)) {
        for (const auto &argument : intrinsic->args) {
            CollectExpressionUses(*argument, uses);
        }
    }
    else if (const auto *unary = AstCast<const UnaryExpr>(&expression)) {
        CollectExpressionUses(*unary->operand, uses);
    }
    else if (const auto *propagation = AstCast<const TryExpr>(&expression)) {
        CollectExpressionUses(*propagation->operand, uses);
    }
    else if (const auto *mapped = AstCast<const MappedTryExpr>(&expression)) {
        CollectExpressionUses(*mapped->operand, uses);
        if (mapped->mapper) {
            CollectExpressionUses(*mapped->mapper, uses);
        }
    }
    else if (const auto *construct = AstCast<const NativeConstructExpr>(&expression)) {
        if (construct->operand) {
            CollectExpressionUses(*construct->operand, uses);
        }
    }
    else if (const auto *diverge = AstCast<const DivergeExpr>(&expression)) {
        if (diverge->value) {
            CollectExpressionUses(*diverge->value, uses);
        }
    }
    else if (const auto *recovery = AstCast<const CatchExpr>(&expression)) {
        CollectExpressionUses(*recovery->subject, uses);
        for (const auto &arm : recovery->arms) {
            CollectPatternUses(*arm.pattern, uses);
            CollectExpressionUses(*arm.body, uses);
        }
    }
    else if (const auto *postfix = AstCast<const PostfixExpr>(&expression)) {
        CollectExpressionUses(*postfix->operand, uses);
    }
    else if (const auto *binary = AstCast<const BinaryExpr>(&expression)) {
        CollectExpressionUses(*binary->left, uses);
        CollectExpressionUses(*binary->right, uses);
    }
    else if (const auto *assignment = AstCast<const AssignExpr>(&expression)) {
        CollectExpressionUses(*assignment->target, uses);
        CollectExpressionUses(*assignment->value, uses);
    }
    else if (const auto *conditional = AstCast<const TernaryExpr>(&expression)) {
        CollectExpressionUses(*conditional->condition, uses);
        CollectExpressionUses(*conditional->thenExpr, uses);
        CollectExpressionUses(*conditional->elseExpr, uses);
    }
    else if (const auto *range = AstCast<const RangeExpr>(&expression)) {
        if (range->lo) {
            CollectExpressionUses(*range->lo, uses);
        }
        if (range->hi) {
            CollectExpressionUses(*range->hi, uses);
        }
    }
    else if (const auto *call = AstCast<const CallExpr>(&expression)) {
        CollectExpressionUses(*call->callee, uses);
        for (const auto &argument : call->args) {
            CollectExpressionUses(*argument, uses);
        }
    }
    else if (const auto *index = AstCast<const IndexExpr>(&expression)) {
        CollectExpressionUses(*index->object, uses);
        CollectExpressionUses(*index->index, uses);
    }
    else if (const auto *field = AstCast<const FieldExpr>(&expression)) {
        CollectExpressionUses(*field->object, uses);
    }
    else if (const auto *initializer = AstCast<const StructInitExpr>(&expression)) {
        for (const auto &initializedField : initializer->fields) {
            CollectExpressionUses(*initializedField.value, uses);
        }
    }
    else if (const auto *array = AstCast<const ArrayExpr>(&expression)) {
        for (const auto &element : array->elements) {
            CollectExpressionUses(*element, uses);
        }
    }
    else if (const auto *repeat = AstCast<const ArrayRepeatExpr>(&expression)) {
        CollectExpressionUses(*repeat->value, uses);
    }
    else if (const auto *spread = AstCast<const SpreadExpr>(&expression)) {
        CollectExpressionUses(*spread->operand, uses);
    }
    else if (const auto *tuple = AstCast<const TupleExpr>(&expression)) {
        for (const auto &element : tuple->elements) {
            CollectExpressionUses(*element, uses);
        }
    }
    else if (const auto *cast = AstCast<const CastExpr>(&expression)) {
        CollectExpressionUses(*cast->operand, uses);
    }
    else if (const auto *test = AstCast<const IsExpr>(&expression)) {
        CollectExpressionUses(*test->operand, uses);
    }
    else if (const auto *block = AstCast<const BlockExpr>(&expression)) {
        LiveAfterMap ignored;
        AddAll(uses, AnalyzeBlockLiveness(*block->block, {}, ignored));
    }
    else if (const auto *match = AstCast<const MatchExpr>(&expression)) {
        CollectExpressionUses(*match->subject, uses);
        for (const auto &arm : match->arms) {
            CollectPatternUses(*arm.pattern, uses);
            CollectExpressionUses(*arm.body, uses);
        }
    }
}

NameSet ExpressionUses(const Expr *expression) {
    NameSet uses;
    if (expression) {
        CollectExpressionUses(*expression, uses);
    }
    return uses;
}

NameSet AnalyzeStatementLiveness(const Stmt &statement, const NameSet &liveAfter, LiveAfterMap &result) {
    result.insert_or_assign(&statement, liveAfter);
    NameSet live = liveAfter;
    if (const auto *expression = AstCast<const ExprStmt>(&statement)) {
        AddAll(live, ExpressionUses(expression->expr.get()));
    }
    else if (const auto *binding = AstCast<const LetStmt>(&statement)) {
        live.erase(binding->name);
        AddAll(live, ExpressionUses(binding->init.get()));
    }
    else if (const auto *conditional = AstCast<const IfStmt>(&statement)) {
        NameSet branches;
        if (conditional->elseBlock) {
            AddAll(branches, AnalyzeBlockLiveness(*conditional->elseBlock, liveAfter, result));
        }
        else {
            AddAll(branches, liveAfter);
        }
        for (auto branch = conditional->elseIfs.rbegin(); branch != conditional->elseIfs.rend(); ++branch) {
            AddAll(branches, AnalyzeBlockLiveness(*branch->block, liveAfter, result));
            AddAll(branches, ExpressionUses(branch->condition.get()));
        }
        AddAll(branches, AnalyzeBlockLiveness(*conditional->thenBlock, liveAfter, result));
        AddAll(branches, ExpressionUses(conditional->condition.get()));
        AddAll(live, branches);
    }
    else if (const auto *whileLoop = AstCast<const WhileStmt>(&statement)) {
        NameSet loopLive = liveAfter;
        AddAll(loopLive, ExpressionUses(whileLoop->condition.get()));
        for (;;) {
            NameSet next = liveAfter;
            AddAll(next, ExpressionUses(whileLoop->condition.get()));
            AddAll(next, AnalyzeBlockLiveness(*whileLoop->body, loopLive, result));
            if (next == loopLive) {
                break;
            }
            loopLive = std::move(next);
        }
        live = std::move(loopLive);
    }
    else if (const auto *doWhileLoop = AstCast<const DoWhileStmt>(&statement)) {
        NameSet loopLive = liveAfter;
        AddAll(loopLive, ExpressionUses(doWhileLoop->condition.get()));
        for (;;) {
            NameSet next = liveAfter;
            AddAll(next, ExpressionUses(doWhileLoop->condition.get()));
            AddAll(next, AnalyzeBlockLiveness(*doWhileLoop->body, loopLive, result));
            if (next == loopLive) {
                break;
            }
            loopLive = std::move(next);
        }
        live = std::move(loopLive);
    }
    else if (const auto *infiniteLoop = AstCast<const LoopStmt>(&statement)) {
        NameSet loopLive = liveAfter;
        for (;;) {
            NameSet next = liveAfter;
            AddAll(next, AnalyzeBlockLiveness(*infiniteLoop->body, loopLive, result));
            if (next == loopLive) {
                break;
            }
            loopLive = std::move(next);
        }
        live = std::move(loopLive);
    }
    else if (const auto *forLoop = AstCast<const ForStmt>(&statement)) {
        NameSet loopLive = liveAfter;
        for (;;) {
            NameSet next = liveAfter;
            NameSet body = AnalyzeBlockLiveness(*forLoop->body, loopLive, result);
            body.erase(forLoop->variable);
            AddAll(next, body);
            AddAll(next, ExpressionUses(forLoop->iterable.get()));
            if (next == loopLive) {
                break;
            }
            loopLive = std::move(next);
        }
        live = std::move(loopLive);
    }
    else if (const auto *match = AstCast<const MatchStmt>(&statement)) {
        for (const auto &arm : match->arms) {
            NameSet armLive = liveAfter;
            AddAll(armLive, ExpressionUses(arm.body.get()));
            CollectPatternUses(*arm.pattern, armLive);
            AddAll(live, armLive);
        }
        AddAll(live, ExpressionUses(match->subject.get()));
    }
    else if (const auto *returned = AstCast<const ReturnStmt>(&statement)) {
        live.clear();
        AddAll(live, ExpressionUses(returned->value ? returned->value->get() : nullptr));
    }
    return live;
}

NameSet AnalyzeBlockLiveness(const Block &block, NameSet liveAfter, LiveAfterMap &result) {
    for (auto statement = block.stmts.rbegin(); statement != block.stmts.rend(); ++statement) {
        liveAfter = AnalyzeStatementLiveness(**statement, liveAfter, result);
    }
    return liveAfter;
}

void RecordExpressionLastUses(const Expr &expression, LastUseOffsets &offsets, LastUseMap &result) {
    if (const auto *identifier = AstCast<const IdentExpr>(&expression)) {
        auto &offset = offsets[identifier->name];
        offset = std::max(offset, identifier->location.offset);
    }
    else if (AstCast<const SelfExpr>(&expression)) {
        auto &offset = offsets["self"];
        offset = std::max(offset, expression.location.offset);
    }
    else if (const auto *intrinsic = AstCast<const IntrinsicExpr>(&expression)) {
        for (const auto &argument : intrinsic->args) {
            RecordExpressionLastUses(*argument, offsets, result);
        }
    }
    else if (const auto *unary = AstCast<const UnaryExpr>(&expression)) {
        RecordExpressionLastUses(*unary->operand, offsets, result);
    }
    else if (const auto *propagation = AstCast<const TryExpr>(&expression)) {
        RecordExpressionLastUses(*propagation->operand, offsets, result);
    }
    else if (const auto *mapped = AstCast<const MappedTryExpr>(&expression)) {
        RecordExpressionLastUses(*mapped->operand, offsets, result);
        if (mapped->mapper) {
            RecordExpressionLastUses(*mapped->mapper, offsets, result);
        }
    }
    else if (const auto *construct = AstCast<const NativeConstructExpr>(&expression)) {
        if (construct->operand) {
            RecordExpressionLastUses(*construct->operand, offsets, result);
        }
    }
    else if (const auto *diverge = AstCast<const DivergeExpr>(&expression)) {
        if (diverge->value) {
            RecordExpressionLastUses(*diverge->value, offsets, result);
        }
    }
    else if (const auto *recovery = AstCast<const CatchExpr>(&expression)) {
        RecordExpressionLastUses(*recovery->subject, offsets, result);
        for (const auto &arm : recovery->arms) {
            RecordExpressionLastUses(*arm.body, offsets, result);
        }
    }
    else if (const auto *postfix = AstCast<const PostfixExpr>(&expression)) {
        RecordExpressionLastUses(*postfix->operand, offsets, result);
    }
    else if (const auto *binary = AstCast<const BinaryExpr>(&expression)) {
        RecordExpressionLastUses(*binary->left, offsets, result);
        RecordExpressionLastUses(*binary->right, offsets, result);
    }
    else if (const auto *assignment = AstCast<const AssignExpr>(&expression)) {
        RecordExpressionLastUses(*assignment->target, offsets, result);
        RecordExpressionLastUses(*assignment->value, offsets, result);
    }
    else if (const auto *conditional = AstCast<const TernaryExpr>(&expression)) {
        RecordExpressionLastUses(*conditional->condition, offsets, result);
        RecordExpressionLastUses(*conditional->thenExpr, offsets, result);
        RecordExpressionLastUses(*conditional->elseExpr, offsets, result);
    }
    else if (const auto *range = AstCast<const RangeExpr>(&expression)) {
        if (range->lo) {
            RecordExpressionLastUses(*range->lo, offsets, result);
        }
        if (range->hi) {
            RecordExpressionLastUses(*range->hi, offsets, result);
        }
    }
    else if (const auto *call = AstCast<const CallExpr>(&expression)) {
        RecordExpressionLastUses(*call->callee, offsets, result);
        for (const auto &argument : call->args) {
            RecordExpressionLastUses(*argument, offsets, result);
        }
    }
    else if (const auto *index = AstCast<const IndexExpr>(&expression)) {
        RecordExpressionLastUses(*index->object, offsets, result);
        RecordExpressionLastUses(*index->index, offsets, result);
    }
    else if (const auto *field = AstCast<const FieldExpr>(&expression)) {
        RecordExpressionLastUses(*field->object, offsets, result);
    }
    else if (const auto *initializer = AstCast<const StructInitExpr>(&expression)) {
        for (const auto &initializedField : initializer->fields) {
            RecordExpressionLastUses(*initializedField.value, offsets, result);
        }
    }
    else if (const auto *array = AstCast<const ArrayExpr>(&expression)) {
        for (const auto &element : array->elements) {
            RecordExpressionLastUses(*element, offsets, result);
        }
    }
    else if (const auto *repeat = AstCast<const ArrayRepeatExpr>(&expression)) {
        RecordExpressionLastUses(*repeat->value, offsets, result);
    }
    else if (const auto *spread = AstCast<const SpreadExpr>(&expression)) {
        RecordExpressionLastUses(*spread->operand, offsets, result);
    }
    else if (const auto *tuple = AstCast<const TupleExpr>(&expression)) {
        for (const auto &element : tuple->elements) {
            RecordExpressionLastUses(*element, offsets, result);
        }
    }
    else if (const auto *cast = AstCast<const CastExpr>(&expression)) {
        RecordExpressionLastUses(*cast->operand, offsets, result);
    }
    else if (const auto *test = AstCast<const IsExpr>(&expression)) {
        RecordExpressionLastUses(*test->operand, offsets, result);
    }
    else if (const auto *block = AstCast<const BlockExpr>(&expression)) {
        RecordBlockLastUses(*block->block, result);
    }
    else if (const auto *match = AstCast<const MatchExpr>(&expression)) {
        RecordExpressionLastUses(*match->subject, offsets, result);
        for (const auto &arm : match->arms) {
            RecordExpressionLastUses(*arm.body, offsets, result);
        }
    }
}

void RemoveBlockUsesFromOffsets(const Block &block, LastUseOffsets &offsets) {
    LiveAfterMap ignored;
    const NameSet uses = AnalyzeBlockLiveness(block, {}, ignored);
    for (const std::string &name : uses) {
        offsets.erase(name);
    }
}

void RecordStatementLastUses(const Stmt &statement, LastUseMap &result) {
    LastUseOffsets &offsets = result[&statement];
    if (const auto *expression = AstCast<const ExprStmt>(&statement)) {
        RecordExpressionLastUses(*expression->expr, offsets, result);
    }
    else if (const auto *binding = AstCast<const LetStmt>(&statement)) {
        if (binding->init) {
            RecordExpressionLastUses(*binding->init, offsets, result);
        }
    }
    else if (const auto *conditional = AstCast<const IfStmt>(&statement)) {
        RecordExpressionLastUses(*conditional->condition, offsets, result);
        RecordBlockLastUses(*conditional->thenBlock, result);
        RemoveBlockUsesFromOffsets(*conditional->thenBlock, offsets);
        for (const auto &branch : conditional->elseIfs) {
            RecordExpressionLastUses(*branch.condition, offsets, result);
            RecordBlockLastUses(*branch.block, result);
            RemoveBlockUsesFromOffsets(*branch.block, offsets);
        }
        if (conditional->elseBlock) {
            RecordBlockLastUses(*conditional->elseBlock, result);
            RemoveBlockUsesFromOffsets(*conditional->elseBlock, offsets);
        }
    }
    else if (const auto *whileLoop = AstCast<const WhileStmt>(&statement)) {
        RecordExpressionLastUses(*whileLoop->condition, offsets, result);
        RecordBlockLastUses(*whileLoop->body, result);
        RemoveBlockUsesFromOffsets(*whileLoop->body, offsets);
    }
    else if (const auto *doWhileLoop = AstCast<const DoWhileStmt>(&statement)) {
        RecordBlockLastUses(*doWhileLoop->body, result);
        RecordExpressionLastUses(*doWhileLoop->condition, offsets, result);
        RemoveBlockUsesFromOffsets(*doWhileLoop->body, offsets);
    }
    else if (const auto *infiniteLoop = AstCast<const LoopStmt>(&statement)) {
        RecordBlockLastUses(*infiniteLoop->body, result);
        RemoveBlockUsesFromOffsets(*infiniteLoop->body, offsets);
    }
    else if (const auto *forLoop = AstCast<const ForStmt>(&statement)) {
        RecordExpressionLastUses(*forLoop->iterable, offsets, result);
        RecordBlockLastUses(*forLoop->body, result);
        RemoveBlockUsesFromOffsets(*forLoop->body, offsets);
    }
    else if (const auto *match = AstCast<const MatchStmt>(&statement)) {
        RecordExpressionLastUses(*match->subject, offsets, result);
        for (const auto &arm : match->arms) {
            RecordExpressionLastUses(*arm.body, offsets, result);
        }
    }
    else if (const auto *returned = AstCast<const ReturnStmt>(&statement); returned && returned->value) {
        RecordExpressionLastUses(**returned->value, offsets, result);
    }
}

void RecordBlockLastUses(const Block &block, LastUseMap &result) {
    for (const auto &statement : block.stmts) {
        RecordStatementLastUses(*statement, result);
    }
}

} // namespace

bool AnalysisContext::BorrowPlacesOverlap(const BorrowPlace &left, const BorrowPlace &right) {
    if (!left.root || !right.root) {
        return true;
    }
    if (left.root != right.root) {
        return false;
    }
    const std::size_t count = std::min(left.projections.size(), right.projections.size());
    for (std::size_t index = 0; index < count; ++index) {
        const MovePlace::Projection &a = left.projections[index];
        const MovePlace::Projection &b = right.projections[index];
        if (a.kind != b.kind) {
            return false;
        }
        if (a.kind == MovePlace::Projection::Kind::Field && a.value != b.value) {
            return false;
        }
        if (a.kind == MovePlace::Projection::Kind::Index && !a.value.empty() && !b.value.empty() &&
            a.value != b.value) {
            return false;
        }
    }
    return true;
}

bool AnalysisContext::SameBorrowPlace(const BorrowPlace &left, const BorrowPlace &right) {
    if (left.root != right.root || left.projections.size() != right.projections.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.projections.size(); ++index) {
        if (left.projections[index].kind != right.projections[index].kind ||
            left.projections[index].value != right.projections[index].value) {
            return false;
        }
    }
    return true;
}

std::string AnalysisContext::BorrowPlace::Display() const {
    std::string text = root ? root->name : "<unknown storage>";
    for (const MovePlace::Projection &projection : projections) {
        text += projection.Display();
    }
    return text;
}

void AnalysisContext::PrepareBorrowAnalysis(const FuncDecl &function) {
    currentBorrowStatement = nullptr;
    activeBorrows.clear();
    endedBorrowProvenance.clear();
    pendingCallBorrows.clear();
    borrowLiveAfter.clear();
    borrowLastUseOffsets.clear();
    if (function.body) {
        AnalyzeBlockLiveness(*function.body, {}, borrowLiveAfter);
        RecordBlockLastUses(*function.body, borrowLastUseOffsets);
    }
}

void AnalysisContext::FinishBorrowAnalysis() {
    currentBorrowStatement = nullptr;
    activeBorrows.clear();
    endedBorrowProvenance.clear();
    pendingCallBorrows.clear();
    borrowLiveAfter.clear();
    borrowLastUseOffsets.clear();
}

void AnalysisContext::ExpireBorrowAtLastUse(const Symbol &symbol, const SourceLocation location) {
    if (symbol.type->kind != TypeRef::Kind::Reference || !currentBorrowStatement) {
        return;
    }
    const auto live = borrowLiveAfter.find(currentBorrowStatement);
    if (live != borrowLiveAfter.end() && live->second.contains(symbol.name)) {
        return;
    }
    const auto statementUses = borrowLastUseOffsets.find(currentBorrowStatement);
    if (statementUses == borrowLastUseOffsets.end()) {
        return;
    }
    const auto lastUse = statementUses->second.find(symbol.name);
    if (lastUse == statementUses->second.end() || lastUse->second != location.offset) {
        return;
    }
    if (const auto active = activeBorrows.find(&symbol); active != activeBorrows.end()) {
        endedBorrowProvenance.insert_or_assign(&symbol, active->second);
        activeBorrows.erase(active);
    }
}

void AnalysisContext::ExpireDeadBorrowsAfter(const Stmt &statement) {
    const auto live = borrowLiveAfter.find(&statement);
    if (live == borrowLiveAfter.end()) {
        return;
    }
    std::erase_if(activeBorrows,
                  [&](const auto &entry) { return entry.first && !live->second.contains(entry.first->name); });
}

void AnalysisContext::EndBorrowScope(const Scope &scope) {
    for (const auto &[_, symbol] : scope.Table()) {
        activeBorrows.erase(&symbol);
    }
}

const Symbol *AnalysisContext::ReferenceSourceAlias(const Expr &expression) const {
    const Expr *root = &expression;
    while (true) {
        if (const auto *field = AstCast<const FieldExpr>(root)) {
            root = field->object.get();
            continue;
        }
        if (const auto *index = AstCast<const IndexExpr>(root)) {
            // An operator index produces a fresh value, so no reference the object was reached through aliases it.
            if (IsIndexOperatorCall(*index)) {
                return nullptr;
            }
            root = index->object.get();
            continue;
        }
        break;
    }
    if (const auto *identifier = AstCast<const IdentExpr>(root)) {
        const Symbol *symbol = currentScope->Lookup(identifier->name);
        return symbol && symbol->type->kind == TypeRef::Kind::Reference ? symbol : nullptr;
    }
    if (AstCast<const SelfExpr>(root)) {
        const Symbol *symbol = currentScope->Lookup("self");
        return symbol && symbol->type->kind == TypeRef::Kind::Reference ? symbol : nullptr;
    }
    return nullptr;
}

std::optional<AnalysisContext::BorrowPlace> AnalysisContext::ResolveBorrowPlace(const Expr &expression) const {
    const MovePlace analyzed = AnalyzeMovePlace(expression);
    const Symbol *root = nullptr;
    if (analyzed.rootKind == MovePlace::RootKind::Named) {
        root = currentScope->Lookup(analyzed.rootName);
    }
    else if (analyzed.rootKind == MovePlace::RootKind::Self) {
        root = currentScope->Lookup("self");
    }
    else if (analyzed.rootKind == MovePlace::RootKind::Dereference && analyzed.rootName.starts_with('$')) {
        root = currentScope->Lookup(analyzed.rootName.substr(1));
    }
    if (!root) {
        return std::nullopt;
    }

    BorrowPlace place{root, analyzed.projections};
    if (root->type->kind == TypeRef::Kind::Reference) {
        if (const auto provenance = activeBorrows.find(root); provenance != activeBorrows.end()) {
            place = provenance->second.place;
            place.projections.insert(place.projections.end(), analyzed.projections.begin(), analyzed.projections.end());
        }
        else if (const auto endedProvenance = endedBorrowProvenance.find(root);
                 endedProvenance != endedBorrowProvenance.end()) {
            place = endedProvenance->second.place;
            place.projections.insert(place.projections.end(), analyzed.projections.begin(), analyzed.projections.end());
        }
    }
    return place;
}

bool AnalysisContext::ReportBorrowConflict(const BorrowPlace &place, const bool exclusive, const Symbol *parentAlias,
                                           const SourceLocation location, const std::string_view action) const {
    for (const auto &[alias, active] : activeBorrows) {
        if (alias == parentAlias || !BorrowPlacesOverlap(place, active.place) || (!exclusive && !active.exclusive)) {
            continue;
        }
        EmitError(
            location,
            std::format("cannot {} '{}' while it is {} borrowed", action, place.Display(),
                        active.exclusive ? "exclusively" : "immutably"),
            {std::format("borrow '{}' begins at {}:{}", alias ? alias->name : "<temporary>", active.location.line,
                         active.location.column)},
            std::format("use '{}' for the access or wait until its last use", alias ? alias->name : "the borrow"));
        return true;
    }
    return false;
}

void AnalysisContext::RegisterReferenceBinding(const Symbol &alias, const Expr &initializer,
                                               const TypeRef &referenceType) {
    const auto place = ResolveBorrowPlace(initializer);
    if (!place || referenceType->inner.empty()) {
        return;
    }
    const Symbol *parent = ReferenceSourceAlias(initializer);
    const bool exclusive = referenceType->inner.front().isMut;
    if (ReportBorrowConflict(*place, exclusive, parent, initializer.location,
                             exclusive ? "borrow exclusively" : "borrow")) {
        return;
    }
    activeBorrows.insert_or_assign(&alias, ActiveBorrow{&alias, parent, *place, exclusive, initializer.location});
}

void AnalysisContext::RegisterReferenceAssignment(const Expr &target, const Expr &initializer,
                                                  const TypeRef &referenceType) {
    const auto *identifier = AstCast<const IdentExpr>(&target);
    Symbol *alias = identifier ? currentScope->Lookup(identifier->name) : nullptr;
    if (!alias || alias->type->kind != TypeRef::Kind::Reference) {
        return;
    }
    const auto place = ResolveBorrowPlace(initializer);
    const Symbol *parent = ReferenceSourceAlias(initializer);
    activeBorrows.erase(alias);
    if (!place || referenceType->inner.empty()) {
        return;
    }
    const bool exclusive = referenceType->inner.front().isMut;
    if (!ReportBorrowConflict(*place, exclusive, parent, initializer.location,
                              exclusive ? "borrow exclusively" : "borrow")) {
        activeBorrows.insert_or_assign(alias, ActiveBorrow{alias, parent, *place, exclusive, initializer.location});
    }
}

void AnalysisContext::CheckBorrowedRead(const Symbol &symbol, const SourceLocation location) {
    BorrowPlace place{&symbol, {}};
    bool ownExclusive = false;
    if (const auto own = activeBorrows.find(&symbol); own != activeBorrows.end()) {
        place = own->second.place;
        ownExclusive = own->second.exclusive;
    }
    for (const auto &[alias, active] : activeBorrows) {
        if (alias == &symbol || !BorrowPlacesOverlap(place, active.place) || (!ownExclusive && !active.exclusive)) {
            continue;
        }
        EmitError(location,
                  std::format("cannot read '{}' while '{}' holds an exclusive borrow", place.Display(),
                              alias ? alias->name : "<temporary>"),
                  {std::format("exclusive borrow begins at {}:{}", active.location.line, active.location.column)},
                  std::format("read through '{}' or wait until its last use", alias ? alias->name : "the borrow"));
        return;
    }
}

void AnalysisContext::CheckBorrowedPlaceRead(const Expr &expression, const SourceLocation location) {
    const auto place = ResolveBorrowPlace(expression);
    if (!place) {
        return;
    }
    const Symbol *sourceAlias = ReferenceSourceAlias(expression);
    bool ownExclusive = false;
    if (sourceAlias) {
        if (const auto own = activeBorrows.find(sourceAlias); own != activeBorrows.end()) {
            ownExclusive = own->second.exclusive;
        }
        else if (const auto ended = endedBorrowProvenance.find(sourceAlias); ended != endedBorrowProvenance.end()) {
            ownExclusive = ended->second.exclusive;
        }
    }
    for (const auto &[alias, active] : activeBorrows) {
        if (alias == sourceAlias || !BorrowPlacesOverlap(*place, active.place) ||
            (!ownExclusive && !active.exclusive)) {
            continue;
        }
        EmitError(location,
                  std::format("cannot read '{}' while '{}' holds an exclusive borrow", place->Display(),
                              alias ? alias->name : "<temporary>"),
                  {std::format("exclusive borrow begins at {}:{}", active.location.line, active.location.column)},
                  std::format("read through '{}' or wait until its last use", alias ? alias->name : "the borrow"));
        return;
    }
}

void AnalysisContext::CheckBorrowedMutation(const Expr &target, const SourceLocation location) {
    const auto place = ResolveBorrowPlace(target);
    if (!place) {
        return;
    }
    const Symbol *sourceAlias = ReferenceSourceAlias(target);
    static_cast<void>(ReportBorrowConflict(*place, true, sourceAlias, location, "modify"));
}

bool AnalysisContext::CheckBorrowedMove(const Expr &expression, const SourceLocation location) {
    const auto place = ResolveBorrowPlace(expression);
    if (!place) {
        return true;
    }
    return !ReportBorrowConflict(*place, true, ReferenceSourceAlias(expression), location, "move");
}

void AnalysisContext::BeginReceiverReferenceBorrow(const CallExpr &call, const Expr &receiver,
                                                   const TypeRef &referenceType) {
    const auto place = ResolveBorrowPlace(receiver);
    if (!place || referenceType->inner.empty()) {
        return;
    }
    const Symbol *parent = ReferenceSourceAlias(receiver);
    const bool exclusive = referenceType->inner.front().isMut;
    if (!ReportBorrowConflict(*place, exclusive, parent, receiver.location,
                              exclusive ? "borrow exclusively" : "borrow")) {
        pendingCallBorrows[&call].push_back({nullptr, parent, *place, exclusive, receiver.location});
    }
}

void AnalysisContext::ValidateCallReferenceBorrows(const CallExpr &call, const std::vector<TypeRef> &parameterTypes) {
    std::vector<ActiveBorrow> temporaries;
    if (auto pending = pendingCallBorrows.find(&call); pending != pendingCallBorrows.end()) {
        temporaries = std::move(pending->second);
        pendingCallBorrows.erase(pending);
    }
    const std::size_t count = std::min(call.args.size(), parameterTypes.size());
    for (std::size_t index = 0; index < count; ++index) {
        const TypeRef &parameter = parameterTypes[index];
        if (parameter->kind != TypeRef::Kind::Reference || parameter->inner.empty()) {
            continue;
        }
        if (RejectSubsetViewUse(*call.args[index], call.args[index]->location, "be passed to a reference parameter")) {
            continue;
        }
        const auto place = ResolveBorrowPlace(*call.args[index]);
        if (!place) {
            continue;
        }
        const Symbol *parent = ReferenceSourceAlias(*call.args[index]);
        const bool exclusive = parameter->inner.front().isMut;
        bool conflict = ReportBorrowConflict(*place, exclusive, parent, call.args[index]->location,
                                             exclusive ? "borrow exclusively" : "borrow");
        for (const ActiveBorrow &temporary : temporaries) {
            if (!conflict && BorrowPlacesOverlap(*place, temporary.place) && (exclusive || temporary.exclusive)) {
                EmitError(call.args[index]->location,
                          std::format("call arguments create overlapping {} borrows of '{}'",
                                      exclusive || temporary.exclusive ? "exclusive" : "immutable", place->Display()),
                          {std::format("an earlier argument borrows the same storage at {}:{}", temporary.location.line,
                                       temporary.location.column)},
                          "split the accesses into non-overlapping calls");
                conflict = true;
            }
        }
        if (!conflict) {
            temporaries.push_back({nullptr, parent, *place, exclusive, call.args[index]->location});
        }
    }
}

AnalysisContext::BorrowSnapshot AnalysisContext::SaveBorrows() const {
    BorrowSnapshot snapshot;
    snapshot.reserve(activeBorrows.size());
    for (const auto &[_, borrow] : activeBorrows) {
        snapshot.push_back(borrow);
    }
    return snapshot;
}

void AnalysisContext::RestoreBorrows(const BorrowSnapshot &snapshot) {
    activeBorrows.clear();
    for (const ActiveBorrow &borrow : snapshot) {
        if (borrow.alias) {
            activeBorrows.insert_or_assign(borrow.alias, borrow);
        }
    }
}

AnalysisContext::BorrowSnapshot AnalysisContext::MergeBorrows(const std::span<const BorrowSnapshot> snapshots) {
    BorrowSnapshot merged;
    for (const BorrowSnapshot &snapshot : snapshots) {
        for (const ActiveBorrow &borrow : snapshot) {
            const auto existing = std::ranges::find(merged, borrow.alias, &ActiveBorrow::alias);
            if (existing == merged.end()) {
                merged.push_back(borrow);
            }
            else if (!SameBorrowPlace(existing->place, borrow.place)) {
                existing->place = {};
                existing->exclusive = existing->exclusive || borrow.exclusive;
            }
        }
    }
    return merged;
}

AnalysisContext::BorrowSnapshot AnalysisContext::ProjectBorrows(const BorrowSnapshot &source,
                                                                const BorrowSnapshot &shape) {
    BorrowSnapshot projected;
    for (const ActiveBorrow &expected : shape) {
        const auto found = std::ranges::find(source, expected.alias, &ActiveBorrow::alias);
        projected.push_back(found == source.end() ? expected : *found);
    }
    return projected;
}

bool AnalysisContext::TypeStoresReference(const TypeRef &type) {
    if (type->kind == TypeRef::Kind::Reference) {
        return true;
    }
    // A native sum, optional, or fallible stores its payloads the way a tuple stores its elements, so one holding a
    // reference anywhere is reference-storing as a whole.
    if (type->kind == TypeRef::Kind::Array || type->kind == TypeRef::Kind::Tuple || type.IsSum() || type.IsOptional() ||
        type.IsFallible()) {
        return std::ranges::any_of(type->inner, [&](const TypeRef &element) { return TypeStoresReference(element); });
    }
    if (type->kind != TypeRef::Kind::Named) {
        return false;
    }

    const std::string base = BaseTypeName(type->name);
    if (!referenceStorageChecks.insert(type->name).second) {
        return false;
    }
    const auto finish = [&] { referenceStorageChecks.erase(type->name); };
    const std::vector<TypeRef> arguments = ParseTypeArgsFromTypeName(type->name);
    if (const auto structure = structDecls.find(base); structure != structDecls.end()) {
        std::unordered_map<std::string, TypeRef> substitutions;
        for (std::size_t index = 0; index < std::min(arguments.size(), structure->second->typeParams.size()); ++index) {
            substitutions.emplace(structure->second->typeParams[index].name, arguments[index]);
        }
        const bool stores = std::ranges::any_of(structure->second->fields, [&](const StructDecl::Field &field) {
            return TypeStoresReference(ResolveTypeWithSubstitution(*field.type, substitutions));
        });
        finish();
        return stores;
    }
    if (const auto enumeration = enumDecls.find(base); enumeration != enumDecls.end()) {
        if (!enumeration->second->IsVariant()) {
            finish();
            return false;
        }
        std::unordered_map<std::string, TypeRef> substitutions;
        for (std::size_t index = 0; index < std::min(arguments.size(), enumeration->second->typeParams.size());
             ++index) {
            substitutions.emplace(enumeration->second->typeParams[index].name, arguments[index]);
        }
        for (const EnumDecl::Variant &variant : enumeration->second->variants) {
            for (const auto &field : variant.fields) {
                if (TypeStoresReference(ResolveTypeWithSubstitution(*field, substitutions))) {
                    finish();
                    return true;
                }
            }
            for (const auto &field : variant.namedFields) {
                if (TypeStoresReference(ResolveTypeWithSubstitution(*field.type, substitutions))) {
                    finish();
                    return true;
                }
            }
        }
    }
    if (const auto unionType = unionDecls.find(base); unionType != unionDecls.end()) {
        const bool stores = std::ranges::any_of(unionType->second->fields, [&](const UnionDecl::Field &field) {
            return TypeStoresReference(ResolveType(*field.type));
        });
        finish();
        return stores;
    }
    finish();
    return false;
}

void AnalysisContext::ValidateStoredType(const TypeRef &type, const SourceLocation location,
                                         const std::string_view subject) {
    if (TypeStoresReference(type)) {
        EmitError(location, std::format("{} cannot store reference type '{}'", subject, type.ToString()),
                  {"references are non-owning aliases and cannot escape into aggregate storage"},
                  "store the owned value or a raw pointer when an address must outlive the borrow");
    }
}
} // namespace Rux::SemanticDetail

namespace Rux::SemanticDetail {
bool AnalysisContext::TypeHoldsView(const TypeRef &type) {
    if (type.IsSlice()) {
        return true;
    }
    if (type->kind == TypeRef::Kind::Array || type->kind == TypeRef::Kind::Tuple || type.IsSum() || type.IsOptional() ||
        type.IsFallible()) {
        return std::ranges::any_of(type->inner, [&](const TypeRef &element) { return TypeHoldsView(element); });
    }
    if (type->kind != TypeRef::Kind::Named) {
        return false;
    }
    if (const auto known = viewHoldingTypes.find(type->name); known != viewHoldingTypes.end()) {
        return known->second;
    }
    if (!viewHoldingChecks.insert(type->name).second) {
        return false;
    }
    const std::string base = BaseTypeName(type->name);
    const std::vector<TypeRef> arguments = ParseTypeArgsFromTypeName(type->name);
    const auto substitutionsFor = [&](const std::vector<TypeParameter> &parameters) {
        std::unordered_map<std::string, TypeRef> substitutions;
        for (std::size_t index = 0; index < std::min(arguments.size(), parameters.size()); ++index) {
            substitutions.emplace(parameters[index].name, arguments[index]);
        }
        return substitutions;
    };
    bool holds = false;
    if (const auto structure = structDecls.find(base); structure != structDecls.end()) {
        const auto substitutions = substitutionsFor(structure->second->typeParams);
        holds = std::ranges::any_of(structure->second->fields, [&](const StructDecl::Field &field) {
            return TypeHoldsView(ResolveTypeWithSubstitution(*field.type, substitutions));
        });
    }
    else if (const auto enumeration = enumDecls.find(base); enumeration != enumDecls.end()) {
        const auto substitutions = substitutionsFor(enumeration->second->typeParams);
        for (const EnumDecl::Variant &variant : enumeration->second->variants) {
            for (const auto &field : variant.fields) {
                holds = holds || TypeHoldsView(ResolveTypeWithSubstitution(*field, substitutions));
            }
            for (const auto &field : variant.namedFields) {
                holds = holds || TypeHoldsView(ResolveTypeWithSubstitution(*field.type, substitutions));
            }
        }
    }
    else if (const auto unionType = unionDecls.find(base); unionType != unionDecls.end()) {
        holds = std::ranges::any_of(unionType->second->fields, [&](const UnionDecl::Field &field) {
            return TypeHoldsView(ResolveType(*field.type));
        });
    }
    viewHoldingChecks.erase(type->name);
    viewHoldingTypes.emplace(type->name, holds);
    return holds;
}

namespace {
/// The local whose own storage `expression` names: the local itself, or a part of it reached by fields and indexes,
/// never through a pointer, a reference or a view, which reach storage the local does not hold.
const Symbol *LocalStorageRoot(const Expr &expression, Scope &scope, const std::unordered_set<const Symbol *> &locals,
                               const std::function<TypeRef(const Expr &)> &typeOf) {
    if (const auto *identifier = AstCast<const IdentExpr>(&expression)) {
        const Symbol *symbol = scope.Lookup(identifier->name);
        if (symbol == nullptr || symbol->kind != Symbol::Kind::Var || !locals.contains(symbol)) {
            return nullptr;
        }
        const TypeRef &type = symbol->type;
        const bool indirect =
            type->kind == TypeRef::Kind::Reference || type->kind == TypeRef::Kind::Pointer || type.IsSlice();
        return indirect ? nullptr : symbol;
    }
    const Expr *object = nullptr;
    if (const auto *field = AstCast<const FieldExpr>(&expression)) {
        object = field->object.get();
    }
    else if (const auto *index = AstCast<const IndexExpr>(&expression);
             index && !AstCast<const RangeExpr>(index->index.get())) {
        object = index->object.get();
    }
    if (object == nullptr) {
        return nullptr;
    }
    const TypeRef objectType = typeOf(*object);
    if (objectType->kind == TypeRef::Kind::Reference || objectType->kind == TypeRef::Kind::Pointer ||
        objectType.IsSlice()) {
        return nullptr;
    }
    return LocalStorageRoot(*object, scope, locals, typeOf);
}

void AddUnique(std::vector<const Symbol *> &into, const Symbol *symbol) {
    if (!std::ranges::contains(into, symbol)) {
        into.push_back(symbol);
    }
}
} // namespace

std::vector<const Symbol *> AnalysisContext::LocalArraysViewed(const Expr &expression, const TypeRef &targetType) {
    std::vector<const Symbol *> viewed;
    const std::function<TypeRef(const Expr &)> typeOf = [this](const Expr &part) {
        return CheckedExpressionType(part);
    };
    const auto addAll = [&](const std::vector<const Symbol *> &symbols) {
        for (const Symbol *symbol : symbols) {
            AddUnique(viewed, symbol);
        }
    };
    if (const auto *move = AstCast<const MoveExpr>(&expression)) {
        return LocalArraysViewed(*move->operand, targetType);
    }
    if (const auto *ternary = AstCast<const TernaryExpr>(&expression)) {
        addAll(LocalArraysViewed(*ternary->thenExpr, targetType));
        addAll(LocalArraysViewed(*ternary->elseExpr, targetType));
        return viewed;
    }
    const TypeRef type = typeOf(expression);
    // A fixed array where a view is expected is viewed in place, as `return buffer` from a function returning a slice.
    if (type->kind == TypeRef::Kind::Array && targetType.IsSlice()) {
        if (const Symbol *root = LocalStorageRoot(expression, *currentScope, functionLocals, typeOf)) {
            AddUnique(viewed, root);
        }
        return viewed;
    }
    if (const auto *index = AstCast<const IndexExpr>(&expression)) {
        if (AstCast<const RangeExpr>(index->index.get())) {
            // Slicing an array the function holds views its storage; slicing a view views what that view does.
            if (typeOf(*index->object)->kind == TypeRef::Kind::Array) {
                if (const Symbol *root = LocalStorageRoot(*index->object, *currentScope, functionLocals, typeOf)) {
                    AddUnique(viewed, root);
                }
                return viewed;
            }
            return LocalArraysViewed(*index->object, TypeRef::MakeUnknown());
        }
        return TypeHoldsView(type) ? LocalArraysViewed(*index->object, TypeRef::MakeUnknown()) : viewed;
    }
    if (const auto *field = AstCast<const FieldExpr>(&expression)) {
        return TypeHoldsView(type) ? LocalArraysViewed(*field->object, TypeRef::MakeUnknown()) : viewed;
    }
    if (const auto *identifier = AstCast<const IdentExpr>(&expression)) {
        const Symbol *symbol = currentScope->Lookup(identifier->name);
        if (const auto sources = symbol ? viewSources.find(symbol) : viewSources.end(); sources != viewSources.end()) {
            addAll(sources->second);
        }
        return viewed;
    }
    if (const auto *structure = AstCast<const StructInitExpr>(&expression)) {
        for (const StructInitExpr::Field &field : structure->fields) {
            addAll(LocalArraysViewed(*field.value, StructFieldType(type, field.name)));
        }
        return viewed;
    }
    // A call's result may view whatever the views it was handed view: `spec.WithOptions(options[..])` returns a spec
    // holding that slice. Nothing in a signature says which argument a returned view comes from, so every argument
    // holding views, and a method's receiver, is taken to be one, as a single borrowed input is in lifetime elision.
    if (const auto *call = AstCast<const CallExpr>(&expression); call && TypeHoldsView(type)) {
        for (const ExprPtr &argument : call->args) {
            if (TypeHoldsView(typeOf(*argument))) {
                addAll(LocalArraysViewed(*argument, TypeRef::MakeUnknown()));
            }
        }
        if (const auto *method = AstCast<const FieldExpr>(call->callee.get());
            method && TypeHoldsView(typeOf(*method->object))) {
            addAll(LocalArraysViewed(*method->object, TypeRef::MakeUnknown()));
        }
        return viewed;
    }
    if (const auto *array = AstCast<const ArrayExpr>(&expression)) {
        const bool sequence = targetType->kind == TypeRef::Kind::Array || targetType.IsSlice();
        const TypeRef element =
            sequence && !targetType->inner.empty() ? targetType->inner.front() : TypeRef::MakeUnknown();
        for (const ExprPtr &item : array->elements) {
            addAll(LocalArraysViewed(*item, element));
        }
        return viewed;
    }
    if (const auto *tuple = AstCast<const TupleExpr>(&expression)) {
        for (std::size_t index = 0; index < tuple->elements.size(); ++index) {
            const TypeRef element = targetType->kind == TypeRef::Kind::Tuple && index < targetType->inner.size()
                                      ? targetType->inner[index]
                                      : TypeRef::MakeUnknown();
            addAll(LocalArraysViewed(*tuple->elements[index], element));
        }
    }
    return viewed;
}

void AnalysisContext::RecordViewSources(const Symbol &local, const Expr &value, const TypeRef &type) {
    if (type.IsUnknown() || !TypeHoldsView(type)) {
        return;
    }
    const std::vector<const Symbol *> viewed = LocalArraysViewed(value, type);
    if (viewed.empty()) {
        return;
    }
    // Every store adds to what the local may view rather than replacing it: a branch that stores another view leaves
    // the first one possible on the path that skipped it.
    auto &sources = viewSources[&local];
    for (const Symbol *symbol : viewed) {
        AddUnique(sources, symbol);
    }
}

void AnalysisContext::CheckViewEscape(const Expr &value, const TypeRef &type, const SourceLocation location,
                                      const std::string_view what) {
    if (type.IsUnknown() || !TypeHoldsView(type)) {
        return;
    }
    const std::vector<const Symbol *> viewed = LocalArraysViewed(value, type);
    if (viewed.empty()) {
        return;
    }
    const Symbol &array = *viewed.front();
    const std::string function = currentFunctionDecl ? currentFunctionDecl->name : std::string("the function");
    EmitError(location,
              std::format("{} views local array '{}', which is freed when '{}' returns", what, array.name, function),
              {std::format("'{}' is declared at {}:{}", array.name, array.location.line, array.location.column),
               "a slice is a view of storage it does not own, and that storage must outlive it"},
              "return an owned container such as a 'Vector', or take the storage as a parameter");
}

bool AnalysisContext::MakesFrameInterfaceValue(const Expr &value, const TypeRef &valueType, const TypeRef &targetType) {
    const auto isInterface = [this](const TypeRef &type) {
        return !type.IsUnknown() && type->kind == TypeRef::Kind::Named && interfaceDecls.contains(type->name);
    };
    // An optional interface value holds one in place, so it carries the same storage when present.
    const auto holdsInterface = [&](const TypeRef &type) {
        return isInterface(type) ||
               (!type.IsUnknown() && type.IsOptional() && type->inner.size() == 1 && isInterface(type->inner.front()));
    };
    if (!holdsInterface(targetType) || valueType.IsUnknown()) {
        return false;
    }
    if (!holdsInterface(valueType)) {
        return true;
    }
    const auto *identifier = AstCast<const IdentExpr>(&value);
    const Symbol *symbol = identifier ? currentScope->Lookup(identifier->name) : nullptr;
    return symbol != nullptr && frameInterfaceLocals.contains(symbol);
}

bool AnalysisContext::IsHolderCoercion(const TypeRef &valueType, const TypeRef &targetType) {
    const auto isInterface = [this](const TypeRef &type) {
        return !type.IsUnknown() && type->kind == TypeRef::Kind::Named && interfaceDecls.contains(type->name);
    };
    return isInterface(targetType) && !valueType.IsUnknown() && !isInterface(valueType) &&
           MayNeedDestruction(valueType);
}

void AnalysisContext::RecordInterfaceHolder(const Expr &value, const LetStmt &owner, std::string place,
                                            const TypeRef &type) {
    InterfaceHolderSite site{&owner, std::move(place), type};
    auto &holders = interfaceHolders[&owner];
    if (std::ranges::none_of(holders, [&](const InterfaceHolderSite &holder) {
            return holder.place == site.place && holder.type == site.type;
        })) {
        holders.push_back(site);
    }
    interfaceHolderSites.insert_or_assign(&value, std::move(site));
}

void AnalysisContext::CheckViewAssignment(const Expr &target, const Expr &value, const TypeRef &targetType,
                                          const SourceLocation location) {
    const bool makesFrameInterface = MakesFrameInterfaceValue(value, CheckedExpressionType(value), targetType);
    if (targetType.IsUnknown() || (!TypeHoldsView(targetType) && !makesFrameInterface)) {
        return;
    }
    // Walk to the name the target is part of, noting whether the way there leaves the local's own storage.
    const Expr *root = &target;
    bool throughIndirection = false;
    bool throughIndex = false;
    std::vector<std::string_view> fields;
    const auto indirect = [this](const Expr &object, const bool viewIsIndirect) {
        const TypeRef objectType = CheckedExpressionType(object);
        return objectType->kind == TypeRef::Kind::Pointer || objectType->kind == TypeRef::Kind::Reference ||
               (viewIsIndirect && objectType.IsSlice());
    };
    while (true) {
        if (const auto *field = AstCast<const FieldExpr>(root)) {
            throughIndirection = throughIndirection || indirect(*field->object, false);
            fields.push_back(field->field);
            root = field->object.get();
        }
        else if (const auto *index = AstCast<const IndexExpr>(root)) {
            throughIndirection = throughIndirection || indirect(*index->object, true);
            throughIndex = true;
            root = index->object.get();
        }
        else if (const auto *unary = AstCast<const UnaryExpr>(root); unary && unary->op == TokenKind::Star) {
            throughIndirection = true;
            root = unary->operand.get();
        }
        else {
            break;
        }
    }
    const auto *identifier = AstCast<const IdentExpr>(root);
    const Symbol *symbol = identifier                    ? currentScope->Lookup(identifier->name)
                         : AstCast<const SelfExpr>(root) ? currentScope->Lookup("self")
                                                         : nullptr;
    if (symbol == nullptr || symbol->kind != Symbol::Kind::Var) {
        return;
    }
    const bool local = functionLocals.contains(symbol);
    const bool keptInCall = local && !throughIndirection && symbol->type->kind != TypeRef::Kind::Reference &&
                            symbol->type->kind != TypeRef::Kind::Pointer;
    if (makesFrameInterface) {
        // A local keeps it, as a view of a local array; storage that outlives the call cannot hold it.
        if (keptInCall) {
            frameInterfaceLocals.insert(symbol);
            // The value is kept with the local it is stored in, so it lives as long as the local and the next value of
            // its type stored at the same place replaces it. An element of an array has no place of its own here.
            const auto owner = localDeclarations.find(symbol);
            if (!throughIndex && owner != localDeclarations.end() &&
                IsHolderCoercion(CheckedExpressionType(value), targetType)) {
                std::string place;
                for (auto name = fields.rbegin(); name != fields.rend(); ++name) {
                    place += place.empty() ? std::string(*name) : std::format(".{}", *name);
                }
                RecordInterfaceHolder(value, *owner->second, std::move(place), CheckedExpressionType(value));
            }
        }
        else {
            const std::string function =
                currentFunctionDecl ? std::format("'{}'", currentFunctionDecl->name) : "the function";
            EmitError(location,
                      std::format("cannot store an interface value made in {} where it outlives the call", function),
                      {std::format("it points at storage {} gives up when it returns", function)},
                      "store the concrete value, and make the interface value where it is used");
        }
        if (!TypeHoldsView(targetType)) {
            return;
        }
    }
    if (local && !throughIndirection && symbol->type->kind != TypeRef::Kind::Reference) {
        RecordViewSources(*symbol, value, targetType);
        return;
    }
    // A module variable, or storage reached through a pointer or a reference, outlives the call.
    CheckViewEscape(value, targetType, location, "stored value");
}
} // namespace Rux::SemanticDetail
