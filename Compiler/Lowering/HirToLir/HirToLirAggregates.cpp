// Aggregate initialization, pattern, and match lowering.

#include "Lowering/HirToLir/HirToLirContext.h"
#include "Semantic/PrimitiveCatalog.h"

#include <cassert>
#include <format>
#include <utility>

namespace Rux::HirToLirDetail {

void HirToLirContext::StoreEnumConstructIntoSlot(const HirEnumConstructExpr &e, const LirReg slot) {
    if (!IsAggregateEnumType(e.type)) {
        auto &payloadSlots = enumPayloadSlots[slot];
        payloadSlots.clear();
        payloadSlots.reserve(e.payloads.size());

        LirReg packed = EmitConst(e.discriminant, TypeRef::MakeInt64());
        for (std::size_t i = 0; i < e.payloads.size(); ++i) {
            const auto &payloadExpr = e.payloads[i];
            LirReg payload = LowerExpr(*payloadExpr);
            const LirReg payloadSlot = EmitAlloca(payloadExpr->type);
            EmitStore(payload, payloadSlot, payloadExpr->type);
            payloadSlots.push_back(payloadSlot);

            // The established compact enum representation has room for
            // one payload in the upper 32 bits. Wider generic enums use
            // the aggregate path below instead.
            if (i == 0) {
                if (payloadExpr->type.kind != TypeRef::Kind::Int64 && payloadExpr->type.kind != TypeRef::Kind::Int) {
                    payload = EmitCast(payload, payloadExpr->type, TypeRef::MakeInt64());
                }
                const LirReg shift = EmitConst("32", TypeRef::MakeInt64());
                const LirReg shifted = EmitBinary(LirOpcode::Shl, payload, shift, TypeRef::MakeInt64());
                packed = EmitBinary(LirOpcode::Or, shifted, packed, TypeRef::MakeInt64());
            }
        }
        EmitStore(packed, slot, e.type);
        return;
    }

    LirReg tag = EmitConst(e.discriminant, TypeRef::MakeInt64());
    EmitStore(tag, slot, TypeRef::MakeInt64());

    auto &payloadSlots = enumPayloadSlots[slot];
    payloadSlots.clear();
    payloadSlots.reserve(e.payloads.size());
    std::uint64_t offset = 8;
    for (const auto &payloadExpr : e.payloads) {
        const std::uint64_t size = payloadExpr->type.SizeInBytes().value_or(8);
        const std::uint64_t align = size > 0 ? std::min<std::uint64_t>(size, 8) : 1;
        offset = (offset + align - 1) / align * align;
        const LirReg offsetReg = EmitConst(std::to_string(offset), TypeRef::MakeUInt64());
        const LirReg payloadSlot = EmitIndexPtr(slot, offsetReg, TypeRef::MakeChar8());
        const LirReg payload = LowerExpr(*payloadExpr);
        EmitStore(payload, payloadSlot, payloadExpr->type);
        payloadSlots.push_back(payloadSlot);
        offset += size;
    }
}

void HirToLirContext::LowerMatch(const HirMatchStmt &s) {
    LirReg subjectSlot = LirNoReg;
    const std::vector<LirReg> *subjectPayload = nullptr;
    if (auto *subjectVar = dynamic_cast<const HirVarExpr *>(s.subject.get())) {
        if (const auto localIt = locals.find(subjectVar->name); localIt != locals.end()) {
            subjectSlot = localIt->second;
            if (const auto payloadIt = enumPayloadSlots.find(localIt->second); payloadIt != enumPayloadSlots.end()) {
                subjectPayload = &payloadIt->second;
            }
        }
    }
    if (subjectSlot == LirNoReg && IsAggregateEnumType(s.subject->type)) {
        subjectSlot = EmitAlloca(s.subject->type);
        StoreExprIntoSlot(*s.subject, subjectSlot, s.subject->type);
    }
    const LirReg subjectVal = subjectSlot != LirNoReg && IsAggregateEnumType(s.subject->type)
                                ? EmitLoad(subjectSlot, TypeRef::MakeInt64())
                                : LowerExpr(*s.subject);
    // Reading an aggregate subject straight out of its slot skips the one place consumption is normally recorded, so
    // a subject handed over to the arms would still be destroyed as well. Clearing it here covers both paths.
    ClearConsumedBinding(*s.subject);
    const std::uint32_t mergeBlock = NewBlock("match.merge");
    if (s.arms.empty()) {
        if (!IsTerminated()) {
            Jump(mergeBlock);
        }
        SetBlock(mergeBlock);
        return;
    }
    for (std::size_t i = 0; i < s.arms.size(); ++i) {
        const auto &arm = s.arms[i];
        const bool isLast = (i + 1 == s.arms.size());
        std::uint32_t bodyBlock = NewBlock(std::format("match.arm{}", i));
        std::uint32_t nextBlock = isLast ? mergeBlock : NewBlock(std::format("match.next{}", i));
        LirReg matched = LowerPattern(*arm.pattern, subjectVal, s.subject->type, subjectPayload, subjectSlot);
        Branch(matched, bodyBlock, nextBlock);
        SetBlock(bodyBlock);
        LowerExpr(*arm.body);
        // A pattern binding owns whatever it matched out of the subject, and the arm is the whole of its life.
        EmitCleanups(arm.cleanups);
        if (!IsTerminated()) {
            Jump(mergeBlock);
        }
        if (!isLast) {
            SetBlock(nextBlock);
        }
    }
    SetBlock(mergeBlock);
}

/// Pattern lowering.
///
/// Returns a bool register: 1 if the pattern matches `subjectVal`. Side-effects: binds pattern variables into locals.
void HirToLirContext::BindLetPattern(const HirPattern &pat, LirReg subjectPtr, const TypeRef &subjectType) {
    if (dynamic_cast<const HirWildcardPattern *>(&pat)) {
        return;
    }

    if (auto *p = dynamic_cast<const HirBindingPattern *>(&pat)) {
        const TypeRef bindType = p->type.IsUnknown() ? subjectType : p->type;
        LirReg bindSlot = EmitAlloca(bindType);
        locals[p->name] = bindSlot;
        LirReg val = EmitLoad(subjectPtr, bindType);
        EmitStore(val, bindSlot, bindType);
        MarkBindingLive(p->bindingId, true);
        return;
    }

    if (auto *p = dynamic_cast<const HirTuplePattern *>(&pat)) {
        for (std::size_t i = 0; i < p->elements.size(); ++i) {
            TypeRef elemType = TypeRef::MakeUnknown();
            if (subjectType.kind == TypeRef::Kind::Tuple && i < subjectType.inner.size()) {
                elemType = subjectType.inner[i];
            }
            LirReg elemPtr = EmitFieldPtr(subjectPtr, std::to_string(i), elemType);
            BindLetPattern(*p->elements[i], elemPtr, elemType);
        }
    }
}

LirReg HirToLirContext::LowerPattern(const HirPattern &pat, LirReg subjectVal, const TypeRef &subjectType,
                                     const std::vector<LirReg> *enumPayload, LirReg subjectSlot) {
    if (dynamic_cast<const HirWildcardPattern *>(&pat)) {
        return EmitConst("1", TypeRef::MakeBool());
    }
    if (auto *p = dynamic_cast<const HirLiteralPattern *>(&pat)) {
        LirReg lit = EmitConst(p->value, p->type);
        return EmitBinary(LirOpcode::CmpEq, subjectVal, lit, TypeRef::MakeBool());
    }
    if (auto *p = dynamic_cast<const HirBindingPattern *>(&pat)) {
        LirReg bindSlot = EmitAlloca(p->type);
        locals[p->name] = bindSlot;
        EmitStore(subjectVal, bindSlot, p->type);
        // Live once it holds something. A binding that does not own what it took carries no identifier at all, so
        // this is a no-op for it and no cleanup was recorded for it either.
        MarkBindingLive(p->bindingId, true);
        return EmitConst("1", TypeRef::MakeBool());
    }
    if (auto *p = dynamic_cast<const HirRangePattern *>(&pat)) {
        LirReg lo = LirNoReg, hi = LirNoReg;
        if (auto *lit = dynamic_cast<const HirLiteralPattern *>(p->lo.get())) {
            lo = EmitConst(lit->value, subjectType);
        }
        else {
            lo = EmitConst("0", subjectType);
        }
        if (auto *lit = dynamic_cast<const HirLiteralPattern *>(p->hi.get())) {
            hi = EmitConst(lit->value, subjectType);
        }
        else {
            hi = EmitConst("0", subjectType);
        }
        const LirReg cmpLo = EmitBinary(LirOpcode::CmpLe, lo, subjectVal, TypeRef::MakeBool());
        const LirOpcode hiOp = p->inclusive ? LirOpcode::CmpLe : LirOpcode::CmpLt;
        const LirReg cmpHi = EmitBinary(hiOp, subjectVal, hi, TypeRef::MakeBool());
        return EmitBinary(LirOpcode::And, cmpLo, cmpHi, TypeRef::MakeBool());
    }

    // Enum, struct, tuple patterns: lower payload bindings, then emit a
    // placeholder true. Full structural matching requires runtime
    // support beyond what this IR stage provides.
    if (auto *p = dynamic_cast<const HirEnumPattern *>(&pat)) {
        // Read the tag back at the width it was stored. A C-like enum is stored at its declared base type, so reading
        // it as a full word picked up whatever happened to sit beside it, and the mask below only cleared that when
        // the base type was at least as wide as the mask -- which is why matching an int8- or int16-based enum
        // matched nothing at all. Widening afterwards keeps the comparison below unchanged.
        const TypeRef tagType = EnumTagType(subjectType);
        LirReg tagValue = subjectVal;
        if (subjectSlot != LirNoReg) {
            tagValue = EmitLoad(subjectSlot, tagType);
            tagValue = EmitCastIfNeeded(tagValue, tagType, TypeRef::MakeInt64());
        }
        // The mask strips a packed payload from beside the tag. A tag narrower than the mask has nothing packed
        // beside it, and masking a sign-extended negative discriminant would only corrupt it.
        if ((!p->unitDiscriminants.empty() || p->discriminant) &&
            tagType.SizeInBytes().value_or(8) >= sizeof(std::uint32_t)) {
            LirReg mask = EmitConst("4294967295", TypeRef::MakeInt64());
            tagValue = EmitBinary(LirOpcode::And, tagValue, mask, TypeRef::MakeInt64());
        }
        for (std::size_t i = 0; i < p->args.size(); ++i) {
            const auto &arg = p->args[i];
            if (auto *bp = dynamic_cast<const HirBindingPattern *>(arg.get())) {
                const TypeRef bindType = bp->type.IsUnknown() ? subjectType : bp->type;
                const LirReg bindSlot = EmitAlloca(bindType);
                locals[bp->name] = bindSlot;
                LirReg payload = LirNoReg;
                const std::size_t payloadIndex = i < p->argIndices.size() ? p->argIndices[i] : i;
                if (enumPayload && payloadIndex < enumPayload->size()) {
                    payload = EmitLoad((*enumPayload)[payloadIndex], bindType);
                }
                else if (subjectSlot != LirNoReg) {
                    std::uint64_t offset = 8;
                    for (std::size_t fieldIndex = 0; fieldIndex < payloadIndex && fieldIndex < p->args.size();
                         ++fieldIndex) {
                        TypeRef fieldType = TypeRef::MakeUnknown();
                        if (const auto *fieldBinding =
                                dynamic_cast<const HirBindingPattern *>(p->args[fieldIndex].get())) {
                            fieldType = fieldBinding->type;
                        }
                        else if (const auto *fieldLiteral =
                                     dynamic_cast<const HirLiteralPattern *>(p->args[fieldIndex].get())) {
                            fieldType = fieldLiteral->type;
                        }
                        const std::uint64_t fieldSize = fieldType.SizeInBytes().value_or(8);
                        const std::uint64_t fieldAlign = fieldSize > 0 ? std::min<std::uint64_t>(fieldSize, 8) : 1;
                        offset = (offset + fieldAlign - 1) / fieldAlign * fieldAlign;
                        offset += fieldSize;
                    }
                    const std::uint64_t bindSize = bindType.SizeInBytes().value_or(8);
                    const std::uint64_t bindAlign = bindSize > 0 ? std::min<std::uint64_t>(bindSize, 8) : 1;
                    offset = (offset + bindAlign - 1) / bindAlign * bindAlign;
                    const LirReg offsetReg = EmitConst(std::to_string(offset), TypeRef::MakeUInt64());
                    const LirReg payloadPtr = EmitIndexPtr(subjectSlot, offsetReg, TypeRef::MakeChar8());
                    payload = EmitLoad(payloadPtr, bindType);
                }
                else {
                    LirReg shift = EmitConst("32", TypeRef::MakeInt64());
                    payload = EmitBinary(LirOpcode::Shr, subjectVal, shift, TypeRef::MakeInt64());
                }
                EmitStore(payload, bindSlot, bindType);
                MarkBindingLive(bp->bindingId, true);
            }
        }
        if (p->hasPayload) {
            if (p->discriminant) {
                LirReg lit = EmitConst(*p->discriminant, TypeRef::MakeInt64());
                return EmitBinary(LirOpcode::CmpEq, tagValue, lit, TypeRef::MakeBool());
            }
            return EmitConst("1", TypeRef::MakeBool());
        }
        if (p->discriminant) {
            LirReg lit = EmitConst(*p->discriminant, TypeRef::MakeInt64());
            return EmitBinary(LirOpcode::CmpEq, tagValue, lit, TypeRef::MakeBool());
        }
        return EmitConst("1", TypeRef::MakeBool());
    }

    if (auto *p = dynamic_cast<const HirStructPattern *>(&pat)) {
        for (const auto &f : p->fields) {
            if (auto *bp = dynamic_cast<const HirBindingPattern *>(f.pattern.get())) {
                LirReg bindSlot = EmitAlloca(bp->type);
                locals[bp->name] = bindSlot;
            }
        }
        return EmitConst("1", TypeRef::MakeBool());
    }

    if (auto *p = dynamic_cast<const HirTuplePattern *>(&pat)) {
        for (const auto &elem : p->elements) {
            if (auto *bp = dynamic_cast<const HirBindingPattern *>(elem.get())) {
                LirReg bindSlot = EmitAlloca(bp->type);
                locals[bp->name] = bindSlot;
            }
        }
        return EmitConst("1", TypeRef::MakeBool());
    }

    if (auto *p = dynamic_cast<const HirGuardedPattern *>(&pat)) {
        LirReg inner = LowerPattern(*p->inner, subjectVal, subjectType, enumPayload, subjectSlot);
        LirReg guard = LowerExpr(*p->guard);
        return EmitBinary(LirOpcode::And, inner, guard, TypeRef::MakeBool());
    }

    return EmitConst("1", TypeRef::MakeBool()); // wildcard fallback
}

// Expression lowering
// Returns the register holding the expression's value.
// For void expressions the return value is LirNoReg.

TypeRef HirToLirContext::SliceElementTypeFromType(const TypeRef &type) {
    if (type.kind == TypeRef::Kind::Named) {
        if (type.name == "Slice<char16>") {
            return TypeRef::MakeChar16();
        }
        if (type.name == "Slice<char32>") {
            return TypeRef::MakeChar32();
        }
        constexpr std::string_view prefix = "Slice<";
        if (type.name.starts_with(prefix) && type.name.ends_with(">")) {
            const std::string elemName = type.name.substr(prefix.size(), type.name.size() - prefix.size() - 1);
            if (const auto primitive = PrimitiveTypeFromName(elemName)) {
                return *primitive;
            }
            return TypeRef::MakeNamed(elemName);
        }
    }
    return TypeRef::MakeChar8();
}

void HirToLirContext::CopySliceValue(LirReg srcSlot, LirReg dstSlot, const TypeRef &sliceType) {
    const TypeRef elemType = SliceElementTypeFromType(sliceType);
    const TypeRef dataType = TypeRef::MakePointer(elemType);

    const LirReg srcDataPtr = EmitFieldPtr(srcSlot, "data", dataType);
    const LirReg data = EmitLoad(srcDataPtr, dataType);
    const LirReg dstDataPtr = EmitFieldPtr(dstSlot, "data", dataType);
    EmitStore(data, dstDataPtr, dataType);

    const LirReg srcLenPtr = EmitFieldPtr(srcSlot, "length", TypeRef::MakeUInt64());
    const LirReg len = EmitLoad(srcLenPtr, TypeRef::MakeUInt64());
    const LirReg dstLenPtr = EmitFieldPtr(dstSlot, "length", TypeRef::MakeUInt64());
    EmitStore(len, dstLenPtr, TypeRef::MakeUInt64());
}

void HirToLirContext::StoreTernaryInit(const HirTernaryExpr &e, LirReg slot, const TypeRef &type) {
    LirReg cond = LowerExpr(*e.condition);
    const std::uint32_t thenBlock = NewBlock("ternary.store.then");
    const std::uint32_t elseBlock = NewBlock("ternary.store.else");
    const std::uint32_t mergeBlock = NewBlock("ternary.store.merge");
    Branch(cond, thenBlock, elseBlock);

    SetBlock(thenBlock);
    StoreExprIntoSlot(*e.thenExpr, slot, type);
    Jump(mergeBlock);

    SetBlock(elseBlock);
    StoreExprIntoSlot(*e.elseExpr, slot, type);
    Jump(mergeBlock);

    SetBlock(mergeBlock);
}

void HirToLirContext::StoreExprValueIntoSlot(const HirExpr &expr, LirReg slot, const TypeRef &type) {
    if (auto *init = dynamic_cast<const HirStructInitExpr *>(&expr)) {
        StoreStructInit(*init, slot);
        return;
    }
    if (auto *arrayExpr = dynamic_cast<const HirArrayExpr *>(&expr)) {
        StoreArrayInit(*arrayExpr, slot);
        return;
    }
    if (auto *initTupleExpr = dynamic_cast<const HirTupleExpr *>(&expr)) {
        StoreTupleInit(*initTupleExpr, slot);
        return;
    }
    if (auto *initRangeExpr = dynamic_cast<const HirRangeExpr *>(&expr)) {
        StoreRangeInit(*initRangeExpr, slot);
        return;
    }
    if (auto *initTernaryExpr = dynamic_cast<const HirTernaryExpr *>(&expr)) {
        StoreTernaryInit(*initTernaryExpr, slot, type);
        return;
    }
    if (auto *initMatchExpr = dynamic_cast<const HirMatchExpr *>(&expr)) {
        StoreMatchInit(*initMatchExpr, slot, type);
        return;
    }
    if (auto *initEnumExpr = dynamic_cast<const HirEnumConstructExpr *>(&expr)) {
        StoreEnumConstructIntoSlot(*initEnumExpr, slot);
        return;
    }
    if (auto *initBlockExpr = dynamic_cast<const HirBlockExpr *>(&expr)) {
        LowerBlock(initBlockExpr->block);
        // A block that left the function reaches no slot. Storing anything after its terminator would put an
        // instruction in a block the verifier has already closed.
        if (IsTerminated()) {
            return;
        }
        if (initBlockExpr->value) {
            StoreExprIntoSlot(*initBlockExpr->value, slot, type);
        }
        return;
    }
    if (auto *initLitExpr = dynamic_cast<const HirLiteralExpr *>(&expr);
        initLitExpr && IsStringSliceLiteral(*initLitExpr)) {
        StoreStringLiteralSlice(*initLitExpr, slot);
        return;
    }
    if (auto *coerce = dynamic_cast<const HirArrayToSliceExpr *>(&expr)) {
        StoreArrayToSlice(*coerce, slot);
        return;
    }
    if (IsSliceType(type)) {
        const LirReg src = LowerLValue(expr);
        CopySliceValue(src, slot, type);
        return;
    }

    if (auto *coerce = dynamic_cast<const HirCoerceToInterfaceExpr *>(&expr)) {
        StoreCoerceToInterface(*coerce, slot);
        return;
    }

    if (IsInterfaceType(type)) {
        // Copy the 16-byte fat pointer {data, vtable} field by field.
        const LirReg srcBase = LowerExpr(expr); // returns fat-ptr address
        const TypeRef ptrType = TypeRef::MakePointer(TypeRef::MakeOpaque());
        LirReg i0 = EmitConst("0", TypeRef::MakeUInt64());
        LirReg srcData = EmitIndexPtr(srcBase, i0, TypeRef::MakeUInt64());
        LirReg dataVal = EmitLoad(srcData, ptrType);
        LirReg dstData = EmitIndexPtr(slot, i0, TypeRef::MakeUInt64());
        EmitStore(dataVal, dstData, ptrType);
        LirReg i1 = EmitConst("1", TypeRef::MakeUInt64());
        LirReg srcVtbl = EmitIndexPtr(srcBase, i1, TypeRef::MakeUInt64());
        LirReg vtblVal = EmitLoad(srcVtbl, ptrType);
        LirReg dstVtbl = EmitIndexPtr(slot, i1, TypeRef::MakeUInt64());
        EmitStore(vtblVal, dstVtbl, ptrType);
        return;
    }

    const LirReg val = LowerExpr(expr);
    EmitStore(EmitCastIfNeeded(val, expr.type, type), slot, type);
}

LirReg HirToLirContext::LowerTernary(const HirTernaryExpr &e) {
    LirReg cond = LowerExpr(*e.condition);
    const std::uint32_t thenBlock = NewBlock("ternary.then");
    const std::uint32_t elseBlock = NewBlock("ternary.else");
    const std::uint32_t mergeBlock = NewBlock("ternary.merge");
    Branch(cond, thenBlock, elseBlock);
    SetBlock(thenBlock);
    LirReg thenVal = LowerExpr(*e.thenExpr);
    // The block that reaches the merge is the one the arm ends in, which is the one it started in only when the arm
    // built no control flow of its own. An arm holding another conditional ends somewhere further on, and naming
    // the block it started in gave the merge a phi listing a block that does not branch to it and none of the
    // blocks that do -- a program the verifier refuses rather than one that runs wrongly, but only because the
    // verifier is there to catch it.
    const std::uint32_t thenIdx = builder->CurrentBlock();
    Jump(mergeBlock);
    SetBlock(elseBlock);
    LirReg elseVal = LowerExpr(*e.elseExpr);
    const std::uint32_t elseIdx = builder->CurrentBlock();
    Jump(mergeBlock);
    SetBlock(mergeBlock);
    LirReg result = NewReg();
    LirInstr phi;
    phi.dst = result;
    phi.op = LirOpcode::Phi;
    phi.type = e.type;
    phi.phiPreds = {{thenVal, thenIdx}, {elseVal, elseIdx}};
    Emit(std::move(phi));
    return result;
}

void HirToLirContext::StoreMatchInit(const HirMatchExpr &e, LirReg slot, const TypeRef &type) {
    LirReg subjectSlot = LirNoReg;
    const std::vector<LirReg> *subjectPayload = nullptr;
    if (auto *subjectVar = dynamic_cast<const HirVarExpr *>(e.subject.get())) {
        if (const auto localIt = locals.find(subjectVar->name); localIt != locals.end()) {
            subjectSlot = localIt->second;
            if (const auto payloadIt = enumPayloadSlots.find(localIt->second); payloadIt != enumPayloadSlots.end()) {
                subjectPayload = &payloadIt->second;
            }
        }
    }
    if (subjectSlot == LirNoReg && IsAggregateEnumType(e.subject->type)) {
        subjectSlot = EmitAlloca(e.subject->type);
        StoreExprIntoSlot(*e.subject, subjectSlot, e.subject->type);
    }
    const LirReg subjectVal = subjectSlot != LirNoReg && IsAggregateEnumType(e.subject->type)
                                ? EmitLoad(subjectSlot, TypeRef::MakeInt64())
                                : LowerExpr(*e.subject);
    ClearConsumedBinding(*e.subject);
    const std::uint32_t mergeBlock = NewBlock("match.expr.store.merge");
    if (e.arms.empty()) {
        if (!IsTerminated()) {
            Jump(mergeBlock);
        }
        SetBlock(mergeBlock);
        return;
    }

    for (std::size_t i = 0; i < e.arms.size(); ++i) {
        const auto &arm = e.arms[i];
        const bool isLast = (i + 1 == e.arms.size());
        const std::uint32_t bodyBlock = NewBlock(std::format("match.expr.store.arm{}", i));
        const std::uint32_t nextBlock = isLast ? mergeBlock : NewBlock(std::format("match.expr.store.next{}", i));
        const LirReg matched = LowerPattern(*arm.pattern, subjectVal, e.subject->type, subjectPayload, subjectSlot);
        Branch(matched, bodyBlock, nextBlock);
        SetBlock(bodyBlock);
        StoreExprIntoSlot(*arm.body, slot, type);
        if (!IsTerminated()) {
            Jump(mergeBlock);
        }
        if (!isLast) {
            SetBlock(nextBlock);
        }
    }

    SetBlock(mergeBlock);
}

LirReg HirToLirContext::LowerMatchExpr(const HirMatchExpr &e) {
    const LirReg slot = EmitAlloca(e.type);
    StoreMatchInit(e, slot, e.type);
    return EmitLoad(slot, e.type);
}

/// Fill an existing 16-byte fat-pointer slot with {&concrete, &vtable}.
void HirToLirContext::StoreCoerceToInterface(const HirCoerceToInterfaceExpr &e, LirReg slot) {
    LirReg val = LowerExpr(*e.value);
    LirReg concreteSlot = EmitAlloca(e.value->type);
    EmitStore(val, concreteSlot, e.value->type);

    const TypeRef ptrType = TypeRef::MakePointer(TypeRef::MakeOpaque());
    LirReg i0 = EmitConst("0", TypeRef::MakeUInt64());
    LirReg dataField = EmitIndexPtr(slot, i0, TypeRef::MakeUInt64());
    EmitStore(concreteSlot, dataField, ptrType);

    LirReg i1 = EmitConst("1", TypeRef::MakeUInt64());
    LirReg vtblField = EmitIndexPtr(slot, i1, TypeRef::MakeUInt64());
    if (!e.vtableLabel.empty()) {
        LirReg vtblAddr = EmitGlobalAddr(e.vtableLabel);
        EmitStore(vtblAddr, vtblField, ptrType);
    }
    else {
        LirReg zero = EmitConst("0", TypeRef::MakeUInt64());
        EmitStore(zero, vtblField, ptrType);
    }
}

/// Wrap a concrete value into a {data_ptr, vtable_ptr} fat pointer. Returns the alloca slot whose data region IS the
/// 16-byte fat pointer.
LirReg HirToLirContext::LowerCoerceToInterface(const HirCoerceToInterfaceExpr &e) {
    LirReg slot = EmitAlloca(e.type); // Named("X") → 16-byte data region
    StoreCoerceToInterface(e, slot);
    return slot;
}

void HirToLirContext::StoreArrayToSlice(const HirArrayToSliceExpr &e, LirReg slot) {
    const LirReg data = LowerLValue(*e.value);
    const TypeRef dataType = TypeRef::MakePointer(e.elementType);
    const LirReg dataField = EmitFieldPtr(slot, "data", dataType);
    EmitStore(data, dataField, dataType);
    const LirReg length = EmitConst(std::to_string(e.length), TypeRef::MakeUInt64());
    const LirReg lengthField = EmitFieldPtr(slot, "length", TypeRef::MakeUInt64());
    EmitStore(length, lengthField, TypeRef::MakeUInt64());
}

LirReg HirToLirContext::LowerArrayToSlice(const HirArrayToSliceExpr &e) {
    const LirReg slot = EmitAlloca(e.type);
    StoreArrayToSlice(e, slot);
    return slot;
}

/// Call a method through an interface fat pointer via vtable dispatch.
LirReg HirToLirContext::LowerEnumConstruct(const HirEnumConstructExpr &e) {
    const LirReg slot = EmitAlloca(e.type);
    StoreEnumConstructIntoSlot(e, slot);
    return EmitLoad(slot, e.type);
}

void HirToLirContext::StoreRangeInit(const HirRangeExpr &e, LirReg slot) {
    const TypeRef elemType = e.type.inner.empty() ? TypeRef::MakeInt64() : e.type.inner[0];
    // Endpoints may be narrower than the range element type (e.g. a uint32
    // bound in a Range<int>). Widen them to the element type so the store
    // writes the full field; otherwise the unwritten high bits are garbage.
    if (e.lo) {
        const LirReg loVal = EmitCastIfNeeded(LowerExpr(*e.lo), e.lo->type, elemType);
        const LirReg loPtr = EmitFieldPtr(slot, "start", elemType);
        EmitStore(loVal, loPtr, elemType);
    }
    if (e.hi) {
        const LirReg hiVal = EmitCastIfNeeded(LowerExpr(*e.hi), e.hi->type, elemType);
        const LirReg hiPtr = EmitFieldPtr(slot, "end", elemType);
        EmitStore(hiVal, hiPtr, elemType);
    }
}

LirReg HirToLirContext::LowerRange(const HirRangeExpr &e) {
    const LirReg slot = EmitAlloca(e.type);
    StoreRangeInit(e, slot);
    return slot;
}

LirReg HirToLirContext::LowerStructInit(const HirStructInitExpr &e) {
    const LirReg slot = EmitAlloca(e.type);
    StoreStructInit(e, slot);
    return EmitLoad(slot, e.type);
}

void HirToLirContext::StoreStructInit(const HirStructInitExpr &e, LirReg slot) {
    for (const auto &f : e.fields) {
        const LirReg ptr = EmitFieldPtr(slot, f.name, f.value->type);
        StoreExprIntoSlot(*f.value, ptr, f.value->type);
    }
}

LirReg HirToLirContext::LowerArray(const HirArrayExpr &e) {
    const LirReg slot = EmitAlloca(e.type);
    StoreArrayInit(e, slot);
    return slot;
}

LirReg HirToLirContext::LowerTuple(const HirTupleExpr &e) {
    const LirReg slot = EmitAlloca(e.type);
    StoreTupleInit(e, slot);
    return slot;
}

void HirToLirContext::StoreTupleInit(const HirTupleExpr &e, LirReg slot) {
    for (std::size_t i = 0; i < e.elements.size(); ++i) {
        const LirReg ptr = EmitFieldPtr(slot, std::to_string(i), e.elements[i]->type);
        StoreExprIntoSlot(*e.elements[i], ptr, e.elements[i]->type);
    }
}

LirReg HirToLirContext::LowerStringLiteralSlice(const HirLiteralExpr &e) {
    const LirReg slot = EmitAlloca(e.type);
    StoreStringLiteralSlice(e, slot);
    return slot;
}

void HirToLirContext::StoreStringLiteralSlice(const HirLiteralExpr &e, LirReg slot) {
    const TypeRef elemType = StringSliceElementType(e);
    const LirReg data = EmitStringAddr(e.value, elemType);
    LirReg dataField = EmitFieldPtr(slot, "data", TypeRef::MakePointer(elemType));
    EmitStore(data, dataField, TypeRef::MakePointer(elemType));
    LirReg len = EmitConst(std::to_string(e.value.size()), TypeRef::MakeUInt64());
    LirReg lenField = EmitFieldPtr(slot, "length", TypeRef::MakeUInt64());
    EmitStore(len, lenField, TypeRef::MakeUInt64());
}

void HirToLirContext::StoreArrayInit(const HirArrayExpr &e, LirReg slot) {
    TypeRef elemType = e.elementType;
    if (elemType.IsUnknown() && !e.elements.empty()) {
        elemType = e.elements.front()->type;
    }
    if (IsArrayType(e.type)) {
        for (std::size_t i = 0; i < e.elements.size(); ++i) {
            const LirReg idx = EmitConst(std::to_string(i), TypeRef::MakeUInt64());
            const LirReg ptr = EmitIndexPtr(slot, idx, elemType);
            StoreExprIntoSlot(*e.elements[i], ptr, elemType);
        }
        return;
    }
    LirReg data = EmitAlloca(elemType, e.elements.size());
    for (std::size_t i = 0; i < e.elements.size(); ++i) {
        LirReg idx = EmitConst(std::to_string(i), TypeRef::MakeUInt64());
        LirReg ptr = EmitIndexPtr(data, idx, elemType);
        StoreExprIntoSlot(*e.elements[i], ptr, elemType);
    }
    LirReg dataField = EmitFieldPtr(slot, "data", TypeRef::MakePointer(elemType));
    EmitStore(data, dataField, TypeRef::MakePointer(elemType));
    LirReg len = EmitConst(std::to_string(e.elements.size()), TypeRef::MakeUInt64());
    LirReg lenField = EmitFieldPtr(slot, "length", TypeRef::MakeUInt64());
    EmitStore(len, lenField, TypeRef::MakeUInt64());
}

} // namespace Rux::HirToLirDetail
