#include "Semantic/Detail/SemanticAnalyzerContext.h"

#include <algorithm>
#include <format>
#include <ranges>
#include <unordered_set>
#include <utility>

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
    if (const auto *guarded = dynamic_cast<const GuardedPattern *>(&pattern)) {
        CollectPatternUses(*guarded->inner, uses);
        CollectExpressionUses(*guarded->guard, uses);
    }
    else if (const auto *enumeration = dynamic_cast<const EnumPattern *>(&pattern)) {
        for (const auto &argument : enumeration->args) {
            CollectPatternUses(*argument, uses);
        }
        for (const auto &argument : enumeration->namedArgs) {
            CollectPatternUses(*argument.pattern, uses);
        }
    }
    else if (const auto *structure = dynamic_cast<const StructPattern *>(&pattern)) {
        for (const auto &field : structure->fields) {
            CollectPatternUses(*field.pattern, uses);
        }
    }
    else if (const auto *tuple = dynamic_cast<const TuplePattern *>(&pattern)) {
        for (const auto &element : tuple->elements) {
            CollectPatternUses(*element, uses);
        }
    }
}

void CollectExpressionUses(const Expr &expression, NameSet &uses) {
    if (const auto *identifier = dynamic_cast<const IdentExpr *>(&expression)) {
        uses.insert(identifier->name);
    }
    else if (dynamic_cast<const SelfExpr *>(&expression)) {
        uses.insert("self");
    }
    else if (const auto *intrinsic = dynamic_cast<const IntrinsicExpr *>(&expression)) {
        for (const auto &argument : intrinsic->args) {
            CollectExpressionUses(*argument, uses);
        }
    }
    else if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expression)) {
        CollectExpressionUses(*unary->operand, uses);
    }
    else if (const auto *propagation = dynamic_cast<const TryExpr *>(&expression)) {
        CollectExpressionUses(*propagation->operand, uses);
    }
    else if (const auto *postfix = dynamic_cast<const PostfixExpr *>(&expression)) {
        CollectExpressionUses(*postfix->operand, uses);
    }
    else if (const auto *binary = dynamic_cast<const BinaryExpr *>(&expression)) {
        CollectExpressionUses(*binary->left, uses);
        CollectExpressionUses(*binary->right, uses);
    }
    else if (const auto *assignment = dynamic_cast<const AssignExpr *>(&expression)) {
        CollectExpressionUses(*assignment->target, uses);
        CollectExpressionUses(*assignment->value, uses);
    }
    else if (const auto *conditional = dynamic_cast<const TernaryExpr *>(&expression)) {
        CollectExpressionUses(*conditional->condition, uses);
        CollectExpressionUses(*conditional->thenExpr, uses);
        CollectExpressionUses(*conditional->elseExpr, uses);
    }
    else if (const auto *range = dynamic_cast<const RangeExpr *>(&expression)) {
        if (range->lo) {
            CollectExpressionUses(*range->lo, uses);
        }
        if (range->hi) {
            CollectExpressionUses(*range->hi, uses);
        }
    }
    else if (const auto *call = dynamic_cast<const CallExpr *>(&expression)) {
        CollectExpressionUses(*call->callee, uses);
        for (const auto &argument : call->args) {
            CollectExpressionUses(*argument, uses);
        }
    }
    else if (const auto *index = dynamic_cast<const IndexExpr *>(&expression)) {
        CollectExpressionUses(*index->object, uses);
        CollectExpressionUses(*index->index, uses);
    }
    else if (const auto *field = dynamic_cast<const FieldExpr *>(&expression)) {
        CollectExpressionUses(*field->object, uses);
    }
    else if (const auto *initializer = dynamic_cast<const StructInitExpr *>(&expression)) {
        for (const auto &initializedField : initializer->fields) {
            CollectExpressionUses(*initializedField.value, uses);
        }
    }
    else if (const auto *array = dynamic_cast<const ArrayExpr *>(&expression)) {
        for (const auto &element : array->elements) {
            CollectExpressionUses(*element, uses);
        }
    }
    else if (const auto *repeat = dynamic_cast<const ArrayRepeatExpr *>(&expression)) {
        CollectExpressionUses(*repeat->value, uses);
    }
    else if (const auto *spread = dynamic_cast<const SpreadExpr *>(&expression)) {
        CollectExpressionUses(*spread->operand, uses);
    }
    else if (const auto *tuple = dynamic_cast<const TupleExpr *>(&expression)) {
        for (const auto &element : tuple->elements) {
            CollectExpressionUses(*element, uses);
        }
    }
    else if (const auto *cast = dynamic_cast<const CastExpr *>(&expression)) {
        CollectExpressionUses(*cast->operand, uses);
    }
    else if (const auto *test = dynamic_cast<const IsExpr *>(&expression)) {
        CollectExpressionUses(*test->operand, uses);
    }
    else if (const auto *block = dynamic_cast<const BlockExpr *>(&expression)) {
        LiveAfterMap ignored;
        AddAll(uses, AnalyzeBlockLiveness(*block->block, {}, ignored));
    }
    else if (const auto *match = dynamic_cast<const MatchExpr *>(&expression)) {
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
    if (const auto *expression = dynamic_cast<const ExprStmt *>(&statement)) {
        AddAll(live, ExpressionUses(expression->expr.get()));
    }
    else if (const auto *binding = dynamic_cast<const LetStmt *>(&statement)) {
        live.erase(binding->name);
        AddAll(live, ExpressionUses(binding->init.get()));
    }
    else if (const auto *conditional = dynamic_cast<const IfStmt *>(&statement)) {
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
    else if (const auto *whileLoop = dynamic_cast<const WhileStmt *>(&statement)) {
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
    else if (const auto *doWhileLoop = dynamic_cast<const DoWhileStmt *>(&statement)) {
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
    else if (const auto *infiniteLoop = dynamic_cast<const LoopStmt *>(&statement)) {
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
    else if (const auto *forLoop = dynamic_cast<const ForStmt *>(&statement)) {
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
    else if (const auto *match = dynamic_cast<const MatchStmt *>(&statement)) {
        for (const auto &arm : match->arms) {
            NameSet armLive = liveAfter;
            AddAll(armLive, ExpressionUses(arm.body.get()));
            CollectPatternUses(*arm.pattern, armLive);
            AddAll(live, armLive);
        }
        AddAll(live, ExpressionUses(match->subject.get()));
    }
    else if (const auto *returned = dynamic_cast<const ReturnStmt *>(&statement)) {
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
    if (const auto *identifier = dynamic_cast<const IdentExpr *>(&expression)) {
        auto &offset = offsets[identifier->name];
        offset = std::max(offset, identifier->location.offset);
    }
    else if (dynamic_cast<const SelfExpr *>(&expression)) {
        auto &offset = offsets["self"];
        offset = std::max(offset, expression.location.offset);
    }
    else if (const auto *intrinsic = dynamic_cast<const IntrinsicExpr *>(&expression)) {
        for (const auto &argument : intrinsic->args) {
            RecordExpressionLastUses(*argument, offsets, result);
        }
    }
    else if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expression)) {
        RecordExpressionLastUses(*unary->operand, offsets, result);
    }
    else if (const auto *propagation = dynamic_cast<const TryExpr *>(&expression)) {
        RecordExpressionLastUses(*propagation->operand, offsets, result);
    }
    else if (const auto *postfix = dynamic_cast<const PostfixExpr *>(&expression)) {
        RecordExpressionLastUses(*postfix->operand, offsets, result);
    }
    else if (const auto *binary = dynamic_cast<const BinaryExpr *>(&expression)) {
        RecordExpressionLastUses(*binary->left, offsets, result);
        RecordExpressionLastUses(*binary->right, offsets, result);
    }
    else if (const auto *assignment = dynamic_cast<const AssignExpr *>(&expression)) {
        RecordExpressionLastUses(*assignment->target, offsets, result);
        RecordExpressionLastUses(*assignment->value, offsets, result);
    }
    else if (const auto *conditional = dynamic_cast<const TernaryExpr *>(&expression)) {
        RecordExpressionLastUses(*conditional->condition, offsets, result);
        RecordExpressionLastUses(*conditional->thenExpr, offsets, result);
        RecordExpressionLastUses(*conditional->elseExpr, offsets, result);
    }
    else if (const auto *range = dynamic_cast<const RangeExpr *>(&expression)) {
        if (range->lo) {
            RecordExpressionLastUses(*range->lo, offsets, result);
        }
        if (range->hi) {
            RecordExpressionLastUses(*range->hi, offsets, result);
        }
    }
    else if (const auto *call = dynamic_cast<const CallExpr *>(&expression)) {
        RecordExpressionLastUses(*call->callee, offsets, result);
        for (const auto &argument : call->args) {
            RecordExpressionLastUses(*argument, offsets, result);
        }
    }
    else if (const auto *index = dynamic_cast<const IndexExpr *>(&expression)) {
        RecordExpressionLastUses(*index->object, offsets, result);
        RecordExpressionLastUses(*index->index, offsets, result);
    }
    else if (const auto *field = dynamic_cast<const FieldExpr *>(&expression)) {
        RecordExpressionLastUses(*field->object, offsets, result);
    }
    else if (const auto *initializer = dynamic_cast<const StructInitExpr *>(&expression)) {
        for (const auto &initializedField : initializer->fields) {
            RecordExpressionLastUses(*initializedField.value, offsets, result);
        }
    }
    else if (const auto *array = dynamic_cast<const ArrayExpr *>(&expression)) {
        for (const auto &element : array->elements) {
            RecordExpressionLastUses(*element, offsets, result);
        }
    }
    else if (const auto *repeat = dynamic_cast<const ArrayRepeatExpr *>(&expression)) {
        RecordExpressionLastUses(*repeat->value, offsets, result);
    }
    else if (const auto *spread = dynamic_cast<const SpreadExpr *>(&expression)) {
        RecordExpressionLastUses(*spread->operand, offsets, result);
    }
    else if (const auto *tuple = dynamic_cast<const TupleExpr *>(&expression)) {
        for (const auto &element : tuple->elements) {
            RecordExpressionLastUses(*element, offsets, result);
        }
    }
    else if (const auto *cast = dynamic_cast<const CastExpr *>(&expression)) {
        RecordExpressionLastUses(*cast->operand, offsets, result);
    }
    else if (const auto *test = dynamic_cast<const IsExpr *>(&expression)) {
        RecordExpressionLastUses(*test->operand, offsets, result);
    }
    else if (const auto *block = dynamic_cast<const BlockExpr *>(&expression)) {
        RecordBlockLastUses(*block->block, result);
    }
    else if (const auto *match = dynamic_cast<const MatchExpr *>(&expression)) {
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
    if (const auto *expression = dynamic_cast<const ExprStmt *>(&statement)) {
        RecordExpressionLastUses(*expression->expr, offsets, result);
    }
    else if (const auto *binding = dynamic_cast<const LetStmt *>(&statement)) {
        if (binding->init) {
            RecordExpressionLastUses(*binding->init, offsets, result);
        }
    }
    else if (const auto *conditional = dynamic_cast<const IfStmt *>(&statement)) {
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
    else if (const auto *whileLoop = dynamic_cast<const WhileStmt *>(&statement)) {
        RecordExpressionLastUses(*whileLoop->condition, offsets, result);
        RecordBlockLastUses(*whileLoop->body, result);
        RemoveBlockUsesFromOffsets(*whileLoop->body, offsets);
    }
    else if (const auto *doWhileLoop = dynamic_cast<const DoWhileStmt *>(&statement)) {
        RecordBlockLastUses(*doWhileLoop->body, result);
        RecordExpressionLastUses(*doWhileLoop->condition, offsets, result);
        RemoveBlockUsesFromOffsets(*doWhileLoop->body, offsets);
    }
    else if (const auto *infiniteLoop = dynamic_cast<const LoopStmt *>(&statement)) {
        RecordBlockLastUses(*infiniteLoop->body, result);
        RemoveBlockUsesFromOffsets(*infiniteLoop->body, offsets);
    }
    else if (const auto *forLoop = dynamic_cast<const ForStmt *>(&statement)) {
        RecordExpressionLastUses(*forLoop->iterable, offsets, result);
        RecordBlockLastUses(*forLoop->body, result);
        RemoveBlockUsesFromOffsets(*forLoop->body, offsets);
    }
    else if (const auto *match = dynamic_cast<const MatchStmt *>(&statement)) {
        RecordExpressionLastUses(*match->subject, offsets, result);
        for (const auto &arm : match->arms) {
            RecordExpressionLastUses(*arm.body, offsets, result);
        }
    }
    else if (const auto *returned = dynamic_cast<const ReturnStmt *>(&statement); returned && returned->value) {
        RecordExpressionLastUses(**returned->value, offsets, result);
    }
}

void RecordBlockLastUses(const Block &block, LastUseMap &result) {
    for (const auto &statement : block.stmts) {
        RecordStatementLastUses(*statement, result);
    }
}

} // namespace

bool SemanticAnalyzerContext::BorrowPlacesOverlap(const BorrowPlace &left, const BorrowPlace &right) {
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

bool SemanticAnalyzerContext::SameBorrowPlace(const BorrowPlace &left, const BorrowPlace &right) {
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

std::string SemanticAnalyzerContext::BorrowPlace::Display() const {
    std::string text = root ? root->name : "<unknown storage>";
    for (const MovePlace::Projection &projection : projections) {
        text += projection.Display();
    }
    return text;
}

void SemanticAnalyzerContext::PrepareBorrowAnalysis(const FuncDecl &function) {
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

void SemanticAnalyzerContext::FinishBorrowAnalysis() {
    currentBorrowStatement = nullptr;
    activeBorrows.clear();
    endedBorrowProvenance.clear();
    pendingCallBorrows.clear();
    borrowLiveAfter.clear();
    borrowLastUseOffsets.clear();
}

void SemanticAnalyzerContext::ExpireBorrowAtLastUse(const Symbol &symbol, const SourceLocation location) {
    if (symbol.type.kind != TypeRef::Kind::Reference || !currentBorrowStatement) {
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

void SemanticAnalyzerContext::ExpireDeadBorrowsAfter(const Stmt &statement) {
    const auto live = borrowLiveAfter.find(&statement);
    if (live == borrowLiveAfter.end()) {
        return;
    }
    std::erase_if(activeBorrows,
                  [&](const auto &entry) { return entry.first && !live->second.contains(entry.first->name); });
}

void SemanticAnalyzerContext::EndBorrowScope(const Scope &scope) {
    for (const auto &[_, symbol] : scope.Table()) {
        activeBorrows.erase(&symbol);
    }
}

const Symbol *SemanticAnalyzerContext::ReferenceSourceAlias(const Expr &expression) const {
    const Expr *root = &expression;
    while (true) {
        if (const auto *field = dynamic_cast<const FieldExpr *>(root)) {
            root = field->object.get();
            continue;
        }
        if (const auto *index = dynamic_cast<const IndexExpr *>(root)) {
            // An operator index produces a fresh value, so no reference the object was reached through aliases it.
            if (IsIndexOperatorCall(*index)) {
                return nullptr;
            }
            root = index->object.get();
            continue;
        }
        break;
    }
    if (const auto *identifier = dynamic_cast<const IdentExpr *>(root)) {
        const Symbol *symbol = currentScope->Lookup(identifier->name);
        return symbol && symbol->type.kind == TypeRef::Kind::Reference ? symbol : nullptr;
    }
    if (dynamic_cast<const SelfExpr *>(root)) {
        const Symbol *symbol = currentScope->Lookup("self");
        return symbol && symbol->type.kind == TypeRef::Kind::Reference ? symbol : nullptr;
    }
    return nullptr;
}

std::optional<SemanticAnalyzerContext::BorrowPlace>
SemanticAnalyzerContext::ResolveBorrowPlace(const Expr &expression) const {
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
    if (root->type.kind == TypeRef::Kind::Reference) {
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

bool SemanticAnalyzerContext::ReportBorrowConflict(const BorrowPlace &place, const bool exclusive,
                                                   const Symbol *parentAlias, const SourceLocation location,
                                                   const std::string_view action) const {
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

void SemanticAnalyzerContext::RegisterReferenceBinding(const Symbol &alias, const Expr &initializer,
                                                       const TypeRef &referenceType) {
    const auto place = ResolveBorrowPlace(initializer);
    if (!place || referenceType.inner.empty()) {
        return;
    }
    const Symbol *parent = ReferenceSourceAlias(initializer);
    const bool exclusive = referenceType.inner.front().isMut;
    if (ReportBorrowConflict(*place, exclusive, parent, initializer.location,
                             exclusive ? "borrow exclusively" : "borrow")) {
        return;
    }
    activeBorrows.insert_or_assign(&alias, ActiveBorrow{&alias, parent, *place, exclusive, initializer.location});
}

void SemanticAnalyzerContext::RegisterReferenceAssignment(const Expr &target, const Expr &initializer,
                                                          const TypeRef &referenceType) {
    const auto *identifier = dynamic_cast<const IdentExpr *>(&target);
    Symbol *alias = identifier ? currentScope->Lookup(identifier->name) : nullptr;
    if (!alias || alias->type.kind != TypeRef::Kind::Reference) {
        return;
    }
    const auto place = ResolveBorrowPlace(initializer);
    const Symbol *parent = ReferenceSourceAlias(initializer);
    activeBorrows.erase(alias);
    if (!place || referenceType.inner.empty()) {
        return;
    }
    const bool exclusive = referenceType.inner.front().isMut;
    if (!ReportBorrowConflict(*place, exclusive, parent, initializer.location,
                              exclusive ? "borrow exclusively" : "borrow")) {
        activeBorrows.insert_or_assign(alias, ActiveBorrow{alias, parent, *place, exclusive, initializer.location});
    }
}

void SemanticAnalyzerContext::CheckBorrowedRead(const Symbol &symbol, const SourceLocation location) {
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

void SemanticAnalyzerContext::CheckBorrowedPlaceRead(const Expr &expression, const SourceLocation location) {
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

void SemanticAnalyzerContext::CheckBorrowedMutation(const Expr &target, const SourceLocation location) {
    const auto place = ResolveBorrowPlace(target);
    if (!place) {
        return;
    }
    const Symbol *sourceAlias = ReferenceSourceAlias(target);
    static_cast<void>(ReportBorrowConflict(*place, true, sourceAlias, location, "modify"));
}

bool SemanticAnalyzerContext::CheckBorrowedMove(const Expr &expression, const SourceLocation location) {
    const auto place = ResolveBorrowPlace(expression);
    if (!place) {
        return true;
    }
    return !ReportBorrowConflict(*place, true, ReferenceSourceAlias(expression), location, "move");
}

void SemanticAnalyzerContext::BeginReceiverReferenceBorrow(const CallExpr &call, const Expr &receiver,
                                                           const TypeRef &referenceType) {
    const auto place = ResolveBorrowPlace(receiver);
    if (!place || referenceType.inner.empty()) {
        return;
    }
    const Symbol *parent = ReferenceSourceAlias(receiver);
    const bool exclusive = referenceType.inner.front().isMut;
    if (!ReportBorrowConflict(*place, exclusive, parent, receiver.location,
                              exclusive ? "borrow exclusively" : "borrow")) {
        pendingCallBorrows[&call].push_back({nullptr, parent, *place, exclusive, receiver.location});
    }
}

void SemanticAnalyzerContext::ValidateCallReferenceBorrows(const CallExpr &call,
                                                           const std::vector<TypeRef> &parameterTypes) {
    std::vector<ActiveBorrow> temporaries;
    if (auto pending = pendingCallBorrows.find(&call); pending != pendingCallBorrows.end()) {
        temporaries = std::move(pending->second);
        pendingCallBorrows.erase(pending);
    }
    const std::size_t count = std::min(call.args.size(), parameterTypes.size());
    for (std::size_t index = 0; index < count; ++index) {
        const TypeRef &parameter = parameterTypes[index];
        if (parameter.kind != TypeRef::Kind::Reference || parameter.inner.empty()) {
            continue;
        }
        const auto place = ResolveBorrowPlace(*call.args[index]);
        if (!place) {
            continue;
        }
        const Symbol *parent = ReferenceSourceAlias(*call.args[index]);
        const bool exclusive = parameter.inner.front().isMut;
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

SemanticAnalyzerContext::BorrowSnapshot SemanticAnalyzerContext::SaveBorrows() const {
    BorrowSnapshot snapshot;
    snapshot.reserve(activeBorrows.size());
    for (const auto &[_, borrow] : activeBorrows) {
        snapshot.push_back(borrow);
    }
    return snapshot;
}

void SemanticAnalyzerContext::RestoreBorrows(const BorrowSnapshot &snapshot) {
    activeBorrows.clear();
    for (const ActiveBorrow &borrow : snapshot) {
        if (borrow.alias) {
            activeBorrows.insert_or_assign(borrow.alias, borrow);
        }
    }
}

SemanticAnalyzerContext::BorrowSnapshot
SemanticAnalyzerContext::MergeBorrows(const std::span<const BorrowSnapshot> snapshots) {
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

SemanticAnalyzerContext::BorrowSnapshot SemanticAnalyzerContext::ProjectBorrows(const BorrowSnapshot &source,
                                                                                const BorrowSnapshot &shape) {
    BorrowSnapshot projected;
    for (const ActiveBorrow &expected : shape) {
        const auto found = std::ranges::find(source, expected.alias, &ActiveBorrow::alias);
        projected.push_back(found == source.end() ? expected : *found);
    }
    return projected;
}

bool SemanticAnalyzerContext::TypeStoresReference(const TypeRef &type) {
    if (type.kind == TypeRef::Kind::Reference) {
        return true;
    }
    if (type.kind == TypeRef::Kind::Array || type.kind == TypeRef::Kind::Tuple) {
        return std::ranges::any_of(type.inner, [&](const TypeRef &element) { return TypeStoresReference(element); });
    }
    if (type.kind != TypeRef::Kind::Named) {
        return false;
    }

    const std::string base = BaseTypeName(type.name);
    if (!referenceStorageChecks.insert(type.name).second) {
        return false;
    }
    const auto finish = [&] { referenceStorageChecks.erase(type.name); };
    const std::vector<TypeRef> arguments = ParseTypeArgsFromTypeName(type.name);
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

void SemanticAnalyzerContext::ValidateStoredType(const TypeRef &type, const SourceLocation location,
                                                 const std::string_view subject) {
    if (TypeStoresReference(type)) {
        EmitError(location, std::format("{} cannot store reference type '{}'", subject, type.ToString()),
                  {"references are non-owning aliases and cannot escape into aggregate storage"},
                  "store the owned value or a raw pointer when an address must outlive the borrow");
    }
}
} // namespace Rux::SemanticDetail
