// Basic-block and statement lowering.

#include "Lowering/HirToLir/HirToLirContext.h"

#include <utility>

namespace Rux::HirToLirDetail {

void HirToLirContext::LowerStmt(const HirStmt &stmt) {
    if (auto *s = dynamic_cast<const HirLetStmt *>(&stmt)) {
        LirReg slot = EmitAlloca(s->type);
        if (!s->pattern) {
            locals[s->name] = slot;
        }
        // A declaration with no initializer owns nothing yet. Marking it live would have the scope destroy whatever
        // its storage happened to hold, which for a droppable type means calling a destructor on garbage. It becomes
        // live when something assigns to it, which is where the assignment's own cleanup marks it.
        if (s->init) {
            StoreExprIntoSlot(*s->init, slot, s->type);
            // Marked live only once the initializer has been stored: an initializer that consumed another binding
            // has to have cleared that one first, and one that left the function never reaches this at all.
            if (!IsTerminated()) {
                MarkBindingLive(s->bindingId, true);
            }
        }
        else {
            MarkBindingLive(s->bindingId, false);
        }
        if (s->pattern) {
            BindLetPattern(*s->pattern, slot, s->type);
        }
        return;
    }

    if (auto *s = dynamic_cast<const HirDropStmt *>(&stmt)) {
        EmitCleanup(s->action);
        return;
    }

    if (auto *s = dynamic_cast<const HirExprStmt *>(&stmt)) {
        LowerExpr(*s->expr);
        return;
    }

    if (auto *s = dynamic_cast<const HirReturnStmt *>(&stmt)) {
        if (s->value) {
            LirReg val = LowerExpr(**s->value);
            // The returned value is in hand and its binding is already released, so every cleanup below leaves it
            // alone and destroys only what the function still owns.
            EmitActivePartialCleanups();
            EmitCleanups(s->cleanups);
            TypeRef retType = builder->Function().returnType;
            if (val != LirNoReg && !retType.IsUnknown() && (*s->value)->type != retType) {
                LirReg casted = NewReg();
                LirInstr cast;
                cast.dst = casted;
                cast.op = LirOpcode::Cast;
                cast.type = retType;
                cast.srcs = {val};
                cast.strArg = (*s->value)->type.ToString();
                Emit(std::move(cast));
                val = casted;
            }
            Return({val}, retType);
        }
        else {
            EmitActivePartialCleanups();
            EmitCleanups(s->cleanups);
            Return(std::nullopt, TypeRef::MakeOpaque());
        }
        return;
    }

    if (auto *s = dynamic_cast<const HirBreakStmt *>(&stmt)) {
        EmitCleanups(s->cleanups);
        if (!s->label.empty()) {
            Jump(labelTargets.at(s->label).breakTarget);
        }
        else {
            Jump(breakTarget);
        }
        return;
    }

    if (auto *s = dynamic_cast<const HirContinueStmt *>(&stmt)) {
        EmitCleanups(s->cleanups);
        if (!s->label.empty()) {
            Jump(labelTargets.at(s->label).continueTarget);
        }
        else {
            Jump(continueTarget);
        }
        return;
    }

    if (auto *s = dynamic_cast<const HirIfStmt *>(&stmt)) {
        LowerIf(*s);
        return;
    }

    if (auto *s = dynamic_cast<const HirWhileStmt *>(&stmt)) {
        LowerWhile(*s);
        return;
    }

    if (auto *s = dynamic_cast<const HirDoWhileStmt *>(&stmt)) {
        LowerDoWhile(*s);
        return;
    }

    if (auto *s = dynamic_cast<const HirLoopStmt *>(&stmt)) {
        LowerLoop(*s);
        return;
    }

    if (auto *s = dynamic_cast<const HirForStmt *>(&stmt)) {
        LowerFor(*s);
        return;
    }

    if (auto *s = dynamic_cast<const HirScopeStmt *>(&stmt)) {
        LowerBlock(s->block);
        return;
    }
    if (auto *s = dynamic_cast<const HirMatchStmt *>(&stmt)) {
        LowerMatch(*s);
        return;
    }

    if (auto *s = dynamic_cast<const HirLocalDecl *>(&stmt)) {
        if (s->hasConstant) {
            localConsts[s->constantName] = {s->constantValue.get(), s->constantType};
        }
        return;
    }
}

// Control-flow lowering
void HirToLirContext::LowerIf(const HirIfStmt &s) {
    std::uint32_t mergeBlock = NewBlock("if.merge");

    // Pre-allocate blocks for each else-if condition so we know their
    // indices before emitting the branch for the preceding condition.
    std::vector<std::uint32_t> elifCondBlocks;
    elifCondBlocks.reserve(s.elseIfs.size());
    for (std::size_t i = 0; i < s.elseIfs.size(); ++i) {
        elifCondBlocks.push_back(NewBlock(std::format("if.elif{}", i)));
    }
    std::uint32_t elseBlock = s.elseBlock ? NewBlock("if.else") : mergeBlock;
    // Main condition
    const LirReg cond0 = LowerExpr(*s.condition);
    const std::uint32_t thenBb0 = NewBlock("if.then");
    const std::uint32_t fall0 = s.elseIfs.empty() ? elseBlock : elifCondBlocks[0];
    Branch(cond0, thenBb0, fall0);
    SetBlock(thenBb0);
    LowerBlock(s.thenBlock);
    if (!IsTerminated()) {
        Jump(mergeBlock);
    }
    // Else-if chain
    for (std::size_t i = 0; i < s.elseIfs.size(); ++i) {
        SetBlock(elifCondBlocks[i]);
        const LirReg elifCond = LowerExpr(*s.elseIfs[i].condition);
        const std::uint32_t elifThen = NewBlock(std::format("if.elif.then{}", i));
        const std::uint32_t nextFall = (i + 1 < s.elseIfs.size()) ? elifCondBlocks[i + 1] : elseBlock;
        Branch(elifCond, elifThen, nextFall);
        SetBlock(elifThen);
        LowerBlock(s.elseIfs[i].block);
        if (!IsTerminated()) {
            Jump(mergeBlock);
        }
    }
    // Else block
    if (s.elseBlock) {
        SetBlock(elseBlock);
        LowerBlock(*s.elseBlock);
        if (!IsTerminated()) {
            Jump(mergeBlock);
        }
    }
    SetBlock(mergeBlock);
}

void HirToLirContext::LowerWhile(const HirWhileStmt &s) {
    std::uint32_t condBlock = NewBlock("while.cond");
    std::uint32_t bodyBlock = NewBlock("while.body");
    std::uint32_t afterBlock = NewBlock("while.after");
    if (!IsTerminated()) {
        Jump(condBlock);
    }
    SetBlock(condBlock);
    const LirReg cond = LowerExpr(*s.condition);
    Branch(cond, bodyBlock, afterBlock);
    const std::uint32_t savedBreak = breakTarget;
    const std::uint32_t savedContinue = continueTarget;
    breakTarget = afterBlock;
    continueTarget = condBlock;
    if (!s.label.empty()) {
        labelTargets[s.label] = {afterBlock, condBlock};
    }
    SetBlock(bodyBlock);
    LowerBlock(s.body);
    if (!s.label.empty()) {
        labelTargets.erase(s.label);
    }
    if (!IsTerminated()) {
        Jump(condBlock);
    }
    breakTarget = savedBreak;
    continueTarget = savedContinue;
    SetBlock(afterBlock);
}

void HirToLirContext::LowerDoWhile(const HirDoWhileStmt &s) {
    std::uint32_t bodyBlock = NewBlock("do.body");
    std::uint32_t condBlock = NewBlock("do.cond");
    std::uint32_t afterBlock = NewBlock("do.after");
    if (!IsTerminated()) {
        Jump(bodyBlock);
    }
    SetBlock(bodyBlock);
    const std::uint32_t savedBreak = breakTarget;
    const std::uint32_t savedContinue = continueTarget;
    breakTarget = afterBlock;
    continueTarget = condBlock;
    if (!s.label.empty()) {
        labelTargets[s.label] = {afterBlock, condBlock};
    }
    LowerBlock(s.body);
    if (!s.label.empty()) {
        labelTargets.erase(s.label);
    }
    breakTarget = savedBreak;
    continueTarget = savedContinue;
    if (!IsTerminated()) {
        Jump(condBlock);
    }
    SetBlock(condBlock);
    const LirReg cond = LowerExpr(*s.condition);
    Branch(cond, bodyBlock, afterBlock);
    SetBlock(afterBlock);
}

void HirToLirContext::LowerLoop(const HirLoopStmt &s) {
    std::uint32_t bodyBlock = NewBlock("loop.body");
    std::uint32_t afterBlock = NewBlock("loop.after");
    if (!IsTerminated()) {
        Jump(bodyBlock);
    }
    SetBlock(bodyBlock);
    const std::uint32_t savedBreak = breakTarget;
    const std::uint32_t savedContinue = continueTarget;
    breakTarget = afterBlock;
    continueTarget = bodyBlock;
    if (!s.label.empty()) {
        labelTargets[s.label] = {afterBlock, bodyBlock};
    }
    LowerBlock(s.body);
    if (!s.label.empty()) {
        labelTargets.erase(s.label);
    }
    breakTarget = savedBreak;
    continueTarget = savedContinue;
    if (!IsTerminated()) {
        Jump(bodyBlock);
    }
    SetBlock(afterBlock);
}

void HirToLirContext::LowerFor(const HirForStmt &s) {
    const bool isRange = s.iterable->type.IsIterableRange();
    const TypeRef elemType = (isRange && !s.iterable->type.inner.empty()) ? s.iterable->type.inner[0] : s.varType;

    // When the loop reuses a variable already in scope, its storage is the
    // outer variable's slot, so the loop's mutations persist afterwards.
    // Otherwise the loop gets a fresh slot. In both cases the name is bound
    // to `slot` only *after* the iterable is lowered below, so that a
    // self-referential bound like `for k in k..7` reads the pre-loop value
    // of the outer `k` rather than the induction slot mid-initialization.
    LirReg slot = s.reusesOuterVar ? locals.at(s.variable) : EmitAlloca(s.varType);

    if (isRange) {
        // Get a slot holding the range struct
        LirReg iterSlot;
        if (auto *re = dynamic_cast<const HirRangeExpr *>(s.iterable.get())) {
            iterSlot = LowerRange(*re);
        }
        else {
            iterSlot = LowerLValue(*s.iterable);
        }

        // The iterable has been lowered in the enclosing scope; now bind the
        // loop variable name to its induction slot for the loop body.
        locals[s.variable] = slot;

        // i = range.start
        LirReg loPtr = EmitFieldPtr(iterSlot, "start", elemType);
        LirReg loVal = EmitLoad(loPtr, elemType);
        EmitStore(loVal, slot, elemType);

        LirReg hiPtr = LirNoReg;
        if (s.iterable->type.RangeHasEnd()) {
            hiPtr = EmitFieldPtr(iterSlot, "end", elemType);
        }

        std::uint32_t condBlock = NewBlock("for.cond");
        std::uint32_t bodyBlock = NewBlock("for.body");
        std::uint32_t stepBlock = NewBlock("for.step");
        std::uint32_t afterBlock = NewBlock("for.after");

        if (!IsTerminated()) {
            Jump(condBlock);
        }
        SetBlock(condBlock);
        LirReg iVal = EmitLoad(slot, elemType);
        LirReg cond;
        if (s.iterable->type.RangeHasEnd()) {
            const LirReg hiCondVal = EmitLoad(hiPtr, elemType);
            cond = EmitBinary(s.iterable->type.IsInclusiveRange() ? LirOpcode::CmpLe : LirOpcode::CmpLt, iVal,
                              hiCondVal, TypeRef::MakeBool());
        }
        else {
            // RangeFrom<T> is unbounded; the loop exits only through
            // break/return or another terminating statement in its body.
            cond = EmitConst("true", TypeRef::MakeBool());
        }
        Branch(cond, bodyBlock, afterBlock);

        std::uint32_t savedBreak = breakTarget;
        std::uint32_t savedContinue = continueTarget;
        breakTarget = afterBlock;
        continueTarget = stepBlock;
        if (!s.label.empty()) {
            labelTargets[s.label] = {afterBlock, stepBlock};
        }

        SetBlock(bodyBlock);
        LowerBlock(s.body);

        if (!IsTerminated()) {
            Jump(stepBlock);
        }
        SetBlock(stepBlock);
        LirReg iCur = EmitLoad(slot, elemType);
        LirReg one = EmitConst("1", elemType);
        LirReg iNext = EmitBinary(LirOpcode::Add, iCur, one, elemType);
        EmitStore(iNext, slot, elemType);
        if (!IsTerminated()) {
            Jump(condBlock);
        }

        if (!s.label.empty()) {
            labelTargets.erase(s.label);
        }
        breakTarget = savedBreak;
        continueTarget = savedContinue;
        SetBlock(afterBlock);
        return;
    }

    if (IsSliceType(s.iterable->type) || IsArrayType(s.iterable->type)) {
        const TypeRef dataType = TypeRef::MakePointer(elemType);
        LirReg iterSlot = LowerLValue(*s.iterable);

        // The iterable has been lowered in the enclosing scope; now bind the
        // loop variable name to its induction slot for the loop body.
        locals[s.variable] = slot;

        LirReg dataPtr = iterSlot;
        LirReg length = LirNoReg;
        if (IsSliceType(s.iterable->type)) {
            LirReg dataFieldPtr = EmitFieldPtr(iterSlot, "data", dataType);
            dataPtr = EmitLoad(dataFieldPtr, dataType);
            LirReg lenFieldPtr = EmitFieldPtr(iterSlot, "length", TypeRef::MakeUInt64());
            length = EmitLoad(lenFieldPtr, TypeRef::MakeUInt64());
        }
        else {
            length = EmitConst(std::to_string(s.iterable->type.arrayLength.value_or(0)), TypeRef::MakeUInt64());
        }

        LirReg idxSlot = EmitAlloca(TypeRef::MakeUInt64());
        LirReg zero = EmitConst("0", TypeRef::MakeUInt64());
        EmitStore(zero, idxSlot, TypeRef::MakeUInt64());

        std::uint32_t condBlock = NewBlock("for.cond");
        std::uint32_t bodyBlock = NewBlock("for.body");
        std::uint32_t stepBlock = NewBlock("for.step");
        std::uint32_t afterBlock = NewBlock("for.after");

        if (!IsTerminated()) {
            Jump(condBlock);
        }
        SetBlock(condBlock);
        LirReg idx = EmitLoad(idxSlot, TypeRef::MakeUInt64());
        LirReg cond = EmitBinary(LirOpcode::CmpLt, idx, length, TypeRef::MakeBool());
        Branch(cond, bodyBlock, afterBlock);

        std::uint32_t savedBreak = breakTarget;
        std::uint32_t savedContinue = continueTarget;
        breakTarget = afterBlock;
        continueTarget = stepBlock;
        if (!s.label.empty()) {
            labelTargets[s.label] = {afterBlock, stepBlock};
        }

        SetBlock(bodyBlock);
        LirReg elemPtr = EmitIndexPtr(dataPtr, idx, elemType);
        LirReg elemVal = EmitLoad(elemPtr, elemType);
        EmitStore(elemVal, slot, elemType);
        LowerBlock(s.body);

        if (!IsTerminated()) {
            Jump(stepBlock);
        }
        SetBlock(stepBlock);
        LirReg idxCur = EmitLoad(idxSlot, TypeRef::MakeUInt64());
        LirReg one = EmitConst("1", TypeRef::MakeUInt64());
        LirReg idxNext = EmitBinary(LirOpcode::Add, idxCur, one, TypeRef::MakeUInt64());
        EmitStore(idxNext, idxSlot, TypeRef::MakeUInt64());
        if (!IsTerminated()) {
            Jump(condBlock);
        }

        if (!s.label.empty()) {
            labelTargets.erase(s.label);
        }
        breakTarget = savedBreak;
        continueTarget = savedContinue;
        SetBlock(afterBlock);
        return;
    }
}

} // namespace Rux::HirToLirDetail
