// Aggregate initialization, pattern, and match lowering.

#include "Lowering/HirToLir/HirToLirContext.h"
#include "Types/PrimitiveCatalog.h"
#include "Unicode/Utf.h"

#include <algorithm>
#include <cassert>
#include <format>
#include <utility>

namespace Rux::HirToLirDetail {

void HirToLirContext::StoreEnumConstructIntoSlot(const HirEnumConstructExpr &e, const LirReg slot) {
    if (e.form != CaseTypeForm::Variant) {
        BuilderFailure("scalar enum reached tagged variant construction lowering");
    }
    if (!IsAggregateEnumType(e.type)) {
        enumPayloadSlots[slot].clear();
        enumPayloadSlots[slot].reserve(e.payloads.size());

        LirReg packed = EmitConst(e.discriminant, TypeRef::MakeInt64());
        for (std::size_t i = 0; i < e.payloads.size(); ++i) {
            const auto &payloadExpr = e.payloads[i];
            LirReg payload = LowerExpr(*payloadExpr);
            const LirReg payloadSlot = EmitAlloca(payloadExpr->type);
            EmitStore(payload, payloadSlot, payloadExpr->type);
            // Lowering the payload can register nested enum slots and rehash the map, so reacquire this entry rather
            // than retaining a reference across the recursive call.
            enumPayloadSlots[slot].push_back(payloadSlot);

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

    // A case may leave part or all of the maximum payload area unused. Initialize the complete representation before
    // writing the active tag and payload so passing the value never exposes stale stack bytes to another function or
    // target ABI.
    EmitStore(EmitConst("0", e.type), slot, e.type);
    const TypeRef tagType = EnumTagType(e.type);
    LirReg tag = EmitConst(e.discriminant, tagType);
    EmitStore(tag, slot, tagType);

    enumPayloadSlots[slot].clear();
    enumPayloadSlots[slot].reserve(e.payloads.size());
    std::uint64_t offset = tagType.SizeInBytes().value_or(8);
    for (std::size_t index = 0; index < e.payloads.size(); ++index) {
        const auto &payloadExpr = e.payloads[index];
        const auto [size, align] = TypeLayoutOf(payloadExpr->type);
        offset = (offset + align - 1) / align * align;
        const LirReg offsetReg = EmitConst(std::to_string(offset), TypeRef::MakeUInt64());
        const LirReg payloadSlot = EmitIndexPtr(slot, offsetReg, TypeRef::MakeChar8());
        PushPartialCleanupFrame(e.failureCleanups, index, slot);
        // A zero-sized payload has no data bytes to write, and the unit value has nothing to evaluate either.
        const auto *unit = dynamic_cast<const HirTupleExpr *>(payloadExpr.get());
        if (size != 0 || !unit || !unit->elements.empty()) {
            LirReg payload = LowerExpr(*payloadExpr);
            // A tuple or array literal lowers to the address of the storage it was built in, not to its value, so the
            // value is loaded from there before it is stored as the payload.
            const bool builtAtAddress = dynamic_cast<const HirTupleExpr *>(payloadExpr.get()) ||
                                        dynamic_cast<const HirArrayExpr *>(payloadExpr.get());
            if (size != 0 && builtAtAddress && !IsTerminated()) {
                payload = EmitLoad(payload, payloadExpr->type);
            }
            if (size != 0 && !IsTerminated()) {
                EmitStore(payload, payloadSlot, payloadExpr->type);
            }
        }
        PopPartialCleanupFrame();
        enumPayloadSlots[slot].push_back(payloadSlot);
        offset += size;
    }
}

LirReg HirToLirContext::BorrowedNativeSubjectSlot(const HirExpr &subject) {
    // A native subject read through a reference is matched where it lies, so a binding of one of its payloads names
    // the referenced storage rather than a copy of it.
    const auto *dereference = dynamic_cast<const HirUnaryExpr *>(&subject);
    const TypeRef &type = subject.type;
    if (!dereference || dereference->op != TokenKind::Star || !dereference->operand ||
        dereference->operand->type.kind != TypeRef::Kind::Reference ||
        !(type.IsSum() || type.IsOptional() || type.IsFallible())) {
        return LirNoReg;
    }
    return LowerExpr(*dereference->operand);
}

bool HirToLirContext::PatternBindsAnything(const HirPattern &pattern) {
    if (dynamic_cast<const HirBindingPattern *>(&pattern)) {
        return true;
    }
    if (const auto *enumeration = dynamic_cast<const HirEnumPattern *>(&pattern)) {
        return std::ranges::any_of(enumeration->args,
                                   [](const HirPatternPtr &argument) { return PatternBindsAnything(*argument); });
    }
    if (const auto *structure = dynamic_cast<const HirStructPattern *>(&pattern)) {
        return std::ranges::any_of(
            structure->fields, [](const HirStructPatternField &field) { return PatternBindsAnything(*field.pattern); });
    }
    if (const auto *tuple = dynamic_cast<const HirTuplePattern *>(&pattern)) {
        return std::ranges::any_of(tuple->elements,
                                   [](const HirPatternPtr &element) { return PatternBindsAnything(*element); });
    }
    if (const auto *guarded = dynamic_cast<const HirGuardedPattern *>(&pattern)) {
        return guarded->inner && PatternBindsAnything(*guarded->inner);
    }
    if (const auto *subset = dynamic_cast<const HirNativeSubsetPattern *>(&pattern)) {
        return subset->binding != nullptr;
    }
    return false;
}

LirReg HirToLirContext::LowerArmPattern(const HirPattern &pattern, const LirReg subjectValue,
                                        const TypeRef &subjectType, const std::vector<LirReg> *enumPayload,
                                        const LirReg subjectSlot, const bool consumed,
                                        std::vector<std::pair<LirReg, TypeRef>> &residual) {
    // An arm over a consumed subject takes what it binds; the rest of the active payload is its to destroy. An arm
    // that binds nothing leaves the whole subject, whose glue destroys only the active payload.
    if (consumed && subjectSlot != LirNoReg && !PatternBindsAnything(pattern)) {
        residual.emplace_back(subjectSlot, subjectType);
        return LowerPattern(pattern, subjectValue, subjectType, enumPayload, subjectSlot);
    }
    std::vector<std::pair<LirReg, TypeRef>> *const saved = residualPayloads;
    residualPayloads = consumed ? &residual : nullptr;
    const LirReg matched = LowerPattern(pattern, subjectValue, subjectType, enumPayload, subjectSlot);
    residualPayloads = saved;
    return matched;
}

void HirToLirContext::EmitResidualDrops(const std::vector<std::pair<LirReg, TypeRef>> &residual) {
    for (const auto &[address, type] : residual) {
        if (const auto glue = dropGlueSymbols.find(type.ToString()); glue != dropGlueSymbols.end()) {
            EmitDropGlueCall(glue->second, address);
        }
    }
}

LirReg HirToLirContext::LowerNativeSubsetPattern(const HirNativeSubsetPattern &pattern, const LirReg subjectVal,
                                                 const LirReg subjectSlot) {
    const TypeRef tagType = TypeRef::MakeInt64();
    const LirReg tag = subjectSlot != LirNoReg ? EmitLoad(subjectSlot, tagType) : subjectVal;
    LirReg matched = EmitConst("0", TypeRef::MakeBool());
    // Selected members keep their canonical order in the subset, so a member's subset tag is how many selected
    // members precede it in the subject.
    LirReg narrowedTag = EmitConst("0", tagType);
    for (const auto &[subjectTag, subsetTag] : pattern.tags) {
        const LirReg member = EmitConst(subjectTag, tagType);
        matched = EmitBinary(LirOpcode::Or, matched, EmitBinary(LirOpcode::CmpEq, tag, member, TypeRef::MakeBool()),
                             TypeRef::MakeBool());
        const LirReg above = EmitBinary(LirOpcode::CmpGt, tag, member, TypeRef::MakeBool());
        narrowedTag = EmitBinary(LirOpcode::Add, narrowedTag, EmitCast(above, TypeRef::MakeBool(), tagType), tagType);
    }
    const auto *binding = dynamic_cast<const HirBindingPattern *>(pattern.binding.get());
    if (!binding || subjectSlot == LirNoReg) {
        return matched;
    }

    // Only a matching subject is narrowed: a binding becomes live only when its pattern holds.
    const std::uint32_t bindBlock = NewBlock("native.subset.bind");
    const std::uint32_t mismatchBlock = NewBlock("native.subset.mismatch");
    const std::uint32_t mergeBlock = NewBlock("native.subset.merge");
    Branch(matched, bindBlock, mismatchBlock);

    SetBlock(bindBlock);
    // A member's payload sits at the same offset in every sum that holds it, and the subset is no larger than the
    // subject, so its bytes are read straight out of the subject and only the tag is rewritten.
    const LirReg narrowed = EmitAlloca(pattern.subsetType);
    EmitStore(EmitLoad(subjectSlot, pattern.subsetType), narrowed, pattern.subsetType);
    EmitStore(narrowedTag, narrowed, tagType);
    locals[binding->name] = narrowed;
    MarkBindingLive(binding->bindingId, true);
    const LirReg bound = EmitConst("1", TypeRef::MakeBool());
    const std::uint32_t bindPred = builder->CurrentBlock();
    Jump(mergeBlock);

    SetBlock(mismatchBlock);
    const LirReg mismatch = EmitConst("0", TypeRef::MakeBool());
    const std::uint32_t mismatchPred = builder->CurrentBlock();
    Jump(mergeBlock);

    SetBlock(mergeBlock);
    const LirReg result = NewReg();
    LirInstr phi;
    phi.dst = result;
    phi.op = LirOpcode::Phi;
    phi.type = TypeRef::MakeBool();
    phi.phiPreds = {{bound, bindPred}, {mismatch, mismatchPred}};
    Emit(std::move(phi));
    return result;
}

bool HirToLirContext::ArmsDestructure(const std::vector<HirMatchArm> &arms) {
    return std::ranges::any_of(arms, [](const HirMatchArm &arm) {
        const HirPattern *pattern = arm.pattern.get();
        while (const auto *guarded = dynamic_cast<const HirGuardedPattern *>(pattern)) {
            pattern = guarded->inner.get();
        }
        return dynamic_cast<const HirTuplePattern *>(pattern) || dynamic_cast<const HirStructPattern *>(pattern);
    });
}

LirReg HirToLirContext::MatchSubjectValue(const LirReg slot, const TypeRef &type) {
    return IsAggregateEnumType(type) ? EmitLoad(slot, EnumTagType(type)) : EmitLoad(slot, type);
}

LirReg HirToLirContext::LowerDestructuringPattern(const HirPattern &pattern, const LirReg subjectValue,
                                                  const TypeRef &subjectType, LirReg subjectSlot) {
    // The parts are read where they lie, so a subject that arrives only as a value is given storage first.
    if (subjectSlot == LirNoReg) {
        subjectSlot = EmitAlloca(subjectType);
        EmitStore(subjectValue, subjectSlot, subjectType);
    }
    std::optional<std::uint32_t> mismatchBlock;
    std::vector<PendingPatternBinding> bindings;
    EmitDestructuringChecks(pattern, subjectSlot, subjectType, mismatchBlock, bindings);

    // Only a pattern that holds as a whole binds: reached here, every refutable part has matched.
    for (const PendingPatternBinding &binding : bindings) {
        const TypeRef type = binding.pattern->type.IsUnknown() ? binding.type : binding.pattern->type;
        const LirReg value = binding.pattern->alias ? LirNoReg : EmitLoad(binding.address, type);
        static_cast<void>(LowerPattern(*binding.pattern, value, type, nullptr, binding.address));
    }
    const LirReg matched = EmitConst("1", TypeRef::MakeBool());
    if (!mismatchBlock) {
        return matched;
    }
    const std::uint32_t matchedPred = builder->CurrentBlock();
    const std::uint32_t mergeBlock = NewBlock("destructure.merge");
    Jump(mergeBlock);

    SetBlock(*mismatchBlock);
    const LirReg mismatch = EmitConst("0", TypeRef::MakeBool());
    const std::uint32_t mismatchPred = builder->CurrentBlock();
    Jump(mergeBlock);

    SetBlock(mergeBlock);
    const LirReg result = NewReg();
    LirInstr phi;
    phi.dst = result;
    phi.op = LirOpcode::Phi;
    phi.type = TypeRef::MakeBool();
    phi.phiPreds = {{matched, matchedPred}, {mismatch, mismatchPred}};
    Emit(std::move(phi));
    return result;
}

void HirToLirContext::EmitDestructuringChecks(const HirPattern &pattern, const LirReg address, const TypeRef &type,
                                              std::optional<std::uint32_t> &mismatch,
                                              std::vector<PendingPatternBinding> &bindings) {
    struct Part {
        const HirPattern *pattern;
        std::string field;
        TypeRef type;
    };

    std::vector<Part> parts;
    if (const auto *tuple = dynamic_cast<const HirTuplePattern *>(&pattern)) {
        for (std::size_t index = 0; index < tuple->elements.size(); ++index) {
            const TypeRef elementType = type.kind == TypeRef::Kind::Tuple && index < type.inner.size()
                                          ? type.inner[index]
                                          : TypeRef::MakeUnknown();
            parts.push_back({tuple->elements[index].get(), std::to_string(index), elementType});
        }
    }
    else if (const auto *structure = dynamic_cast<const HirStructPattern *>(&pattern)) {
        for (const HirStructPatternField &field : structure->fields) {
            parts.push_back({field.pattern.get(), field.name, field.type});
        }
    }

    // A field's offset comes from the layout its base register points at, and the address may be a byte offset into
    // a variant payload or a native case, so it is restated as the aggregate it holds.
    const LirReg base = EmitCast(address, TypeRef::MakePointer(TypeRef::MakeChar8()), TypeRef::MakePointer(type));
    for (const Part &part : parts) {
        const LirReg partAddress = EmitFieldPtr(base, part.field, part.type);
        // Over a consumed subject, a part the arm matches without binding has no other owner left.
        std::vector<std::pair<LirReg, TypeRef>> *const residual = residualPayloads;
        if (residual && !PatternBindsAnything(*part.pattern)) {
            residual->emplace_back(partAddress, part.type);
            residualPayloads = nullptr;
        }
        if (const auto *binding = dynamic_cast<const HirBindingPattern *>(part.pattern)) {
            bindings.push_back({binding, partAddress, part.type});
        }
        else if (dynamic_cast<const HirTuplePattern *>(part.pattern) ||
                 dynamic_cast<const HirStructPattern *>(part.pattern)) {
            EmitDestructuringChecks(*part.pattern, partAddress, part.type, mismatch, bindings);
        }
        else if (!dynamic_cast<const HirWildcardPattern *>(part.pattern)) {
            const LirReg matched =
                LowerPattern(*part.pattern, MatchSubjectValue(partAddress, part.type), part.type, nullptr, partAddress);
            if (!mismatch) {
                mismatch = NewBlock("destructure.mismatch");
            }
            const std::uint32_t next = NewBlock("destructure.next");
            Branch(matched, next, *mismatch);
            SetBlock(next);
        }
        residualPayloads = residual;
    }
}

void HirToLirContext::LowerMatch(const HirMatchStmt &s) {
    LirReg subjectSlot = BorrowedNativeSubjectSlot(*s.subject);
    const std::vector<LirReg> *subjectPayload = nullptr;
    if (auto *subjectVar = dynamic_cast<const HirVarExpr *>(s.subject.get())) {
        if (const auto localIt = locals.find(subjectVar->name); localIt != locals.end()) {
            subjectSlot = localIt->second;
            // An addressable variant finds its payload from the subject's own storage. Cached payload addresses
            // come from whichever construction last wrote the local, which need not be on this path.
            if (const auto payloadIt = enumPayloadSlots.find(localIt->second);
                payloadIt != enumPayloadSlots.end() && !IsAggregateEnumType(subjectVar->type)) {
                subjectPayload = &payloadIt->second;
            }
        }
    }
    const bool inPlace = IsAggregateEnumType(s.subject->type) || ArmsDestructure(s.arms);
    if (subjectSlot == LirNoReg && inPlace) {
        subjectSlot = EmitAlloca(s.subject->type);
        StoreExprIntoSlot(*s.subject, subjectSlot, s.subject->type);
    }
    const LirReg subjectVal =
        subjectSlot != LirNoReg && inPlace ? MatchSubjectValue(subjectSlot, s.subject->type) : LowerExpr(*s.subject);
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
    const std::optional<std::uint32_t> unmatchedBlock = UnmatchedBlock(s.exhaustive, s.arms);
    for (std::size_t i = 0; i < s.arms.size(); ++i) {
        const auto &arm = s.arms[i];
        const bool isLast = (i + 1 == s.arms.size());
        std::uint32_t bodyBlock = NewBlock(std::format("match.arm{}", i));
        std::uint32_t nextBlock =
            isLast ? unmatchedBlock.value_or(mergeBlock) : NewBlock(std::format("match.next{}", i));
        std::vector<std::pair<LirReg, TypeRef>> residual;
        LirReg matched = LowerArmPattern(*arm.pattern, subjectVal, s.subject->type, subjectPayload, subjectSlot,
                                         s.subject->consumption.has_value(), residual);
        Branch(matched, bodyBlock, nextBlock);
        SetBlock(bodyBlock);
        EmitResidualDrops(residual);
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
    EmitUnmatchedTrap(unmatchedBlock, *s.subject, s.location);
    SetBlock(mergeBlock);
}

/// The block an exhaustive match's last refutable arm falls to, or nothing when that edge reaches the merge as usual.
/// An irrefutable last arm never falls through, so it needs no trap.
std::optional<std::uint32_t> HirToLirContext::UnmatchedBlock(const bool exhaustive,
                                                             const std::vector<HirMatchArm> &arms) {
    if (!exhaustive || arms.empty()) {
        return std::nullopt;
    }
    const HirPattern &last = *arms.back().pattern;
    if (dynamic_cast<const HirWildcardPattern *>(&last) || dynamic_cast<const HirBindingPattern *>(&last)) {
        return std::nullopt;
    }
    return NewBlock("match.unmatched");
}

void HirToLirContext::EmitUnmatchedTrap(const std::optional<std::uint32_t> block, const HirExpr &subject,
                                        const SourceLocation &location) {
    if (!block) {
        return;
    }
    SetBlock(*block);
    EmitRuntimeTrap(std::format("no match arm matched value of '{}'", subject.type.ToString()), location);
}

/// Pattern lowering.
///
/// Returns a bool register: 1 if the pattern matches `subjectVal`. Side-effects: binds pattern variables into locals.
void HirToLirContext::BindLetPattern(const HirPattern &pat, LirReg subjectPtr, const TypeRef &subjectType) {
    if (const auto *wildcard = dynamic_cast<const HirWildcardPattern *>(&pat)) {
        if (!wildcard->discardGlue.empty()) {
            EmitDropGlueCall(wildcard->discardGlue, subjectPtr);
        }
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
        // A borrowed native payload is named where it lies, so writing through the binding writes the subject.
        if (p->alias && subjectSlot != LirNoReg) {
            // The payload's address is a byte offset into the subject; field access needs it typed as the payload.
            locals[p->name] =
                EmitCast(subjectSlot, TypeRef::MakePointer(TypeRef::MakeChar8()), TypeRef::MakePointer(p->type));
            return EmitConst("1", TypeRef::MakeBool());
        }
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
        // Only compact variants pack their payload above a 32-bit tag. Scalar enums use their entire declared base
        // type, while addressable variants keep the tag in a separate field.
        const bool addressableVariant = p->form == CaseTypeForm::Variant && IsAggregateEnumType(subjectType);
        if (p->form == CaseTypeForm::Variant && !addressableVariant && p->hasPayload) {
            LirReg mask = EmitConst("4294967295", TypeRef::MakeInt64());
            tagValue = EmitBinary(LirOpcode::And, tagValue, mask, TypeRef::MakeInt64());
        }

        LirReg tagMatches = EmitConst("1", TypeRef::MakeBool());
        if (p->discriminant) {
            const LirReg literal = EmitConst(*p->discriminant, TypeRef::MakeInt64());
            tagMatches = EmitBinary(LirOpcode::CmpEq, tagValue, literal, TypeRef::MakeBool());
        }
        if (p->form != CaseTypeForm::Variant || p->args.empty()) {
            return tagMatches;
        }

        // Do not inspect payload storage until the active tag is known to match. Besides being required for owners,
        // this makes a value returned across a call boundary decode exactly like one constructed in the current
        // function.
        const std::uint32_t payloadBlock = NewBlock("variant.pattern.payload");
        const std::uint32_t mismatchBlock = NewBlock("variant.pattern.mismatch");
        const std::uint32_t mergeBlock = NewBlock("variant.pattern.merge");
        Branch(tagMatches, payloadBlock, mismatchBlock);

        SetBlock(payloadBlock);
        LirReg payloadMatches = EmitConst("1", TypeRef::MakeBool());
        for (std::size_t i = 0; i < p->args.size(); ++i) {
            const auto &arg = p->args[i];
            const std::size_t payloadIndex = i < p->argIndices.size() ? p->argIndices[i] : i;
            const TypeRef payloadType =
                payloadIndex < p->payloadTypes.size() ? p->payloadTypes[payloadIndex] : TypeRef::MakeUnknown();
            LirReg payloadSlot = LirNoReg;
            LirReg payload = LirNoReg;
            if (enumPayload && payloadIndex < enumPayload->size()) {
                payloadSlot = (*enumPayload)[payloadIndex];
                payload = EmitLoad(payloadSlot, payloadType);
            }
            else if (addressableVariant && subjectSlot != LirNoReg) {
                std::uint64_t offset = tagType.SizeInBytes().value_or(8);
                for (std::size_t fieldIndex = 0; fieldIndex < payloadIndex && fieldIndex < p->payloadTypes.size();
                     ++fieldIndex) {
                    const auto [fieldSize, fieldAlign] = TypeLayoutOf(p->payloadTypes[fieldIndex]);
                    offset = (offset + fieldAlign - 1) / fieldAlign * fieldAlign;
                    offset += fieldSize;
                }
                const std::uint64_t payloadAlign = TypeLayoutOf(payloadType).alignment;
                offset = (offset + payloadAlign - 1) / payloadAlign * payloadAlign;
                const LirReg offsetReg = EmitConst(std::to_string(offset), TypeRef::MakeUInt64());
                payloadSlot = EmitIndexPtr(subjectSlot, offsetReg, TypeRef::MakeChar8());
                payload = EmitLoad(payloadSlot, payloadType);
            }
            else {
                const LirReg shift = EmitConst("32", TypeRef::MakeInt64());
                payload = EmitBinary(LirOpcode::Shr, subjectVal, shift, TypeRef::MakeInt64());
                payload = EmitCastIfNeeded(payload, TypeRef::MakeInt64(), payloadType);
            }
            // Over a consumed subject, a payload the arm matches without binding has no other owner left.
            std::vector<std::pair<LirReg, TypeRef>> *const residual = residualPayloads;
            const bool unbound = residual && payloadSlot != LirNoReg && !PatternBindsAnything(*arg);
            if (unbound) {
                residual->emplace_back(payloadSlot, payloadType);
                residualPayloads = nullptr;
            }
            const LirReg argumentMatches = LowerPattern(*arg, payload, payloadType, nullptr, payloadSlot);
            residualPayloads = residual;
            payloadMatches = EmitBinary(LirOpcode::And, payloadMatches, argumentMatches, TypeRef::MakeBool());
        }
        const std::uint32_t payloadPred = builder->CurrentBlock();
        Jump(mergeBlock);

        SetBlock(mismatchBlock);
        const LirReg mismatch = EmitConst("0", TypeRef::MakeBool());
        const std::uint32_t mismatchPred = builder->CurrentBlock();
        Jump(mergeBlock);

        SetBlock(mergeBlock);
        const LirReg result = NewReg();
        LirInstr phi;
        phi.dst = result;
        phi.op = LirOpcode::Phi;
        phi.type = TypeRef::MakeBool();
        phi.phiPreds = {{payloadMatches, payloadPred}, {mismatch, mismatchPred}};
        Emit(std::move(phi));
        return result;
    }

    if (dynamic_cast<const HirStructPattern *>(&pat) || dynamic_cast<const HirTuplePattern *>(&pat)) {
        return LowerDestructuringPattern(pat, subjectVal, subjectType, subjectSlot);
    }

    if (auto *p = dynamic_cast<const HirNativeSubsetPattern *>(&pat)) {
        return LowerNativeSubsetPattern(*p, subjectVal, subjectSlot);
    }

    if (auto *p = dynamic_cast<const HirGuardedPattern *>(&pat)) {
        const LirReg inner = LowerPattern(*p->inner, subjectVal, subjectType, enumPayload, subjectSlot);
        const std::uint32_t guardBlock = NewBlock("pattern.guard");
        const std::uint32_t mismatchBlock = NewBlock("pattern.guard.mismatch");
        const std::uint32_t mergeBlock = NewBlock("pattern.guard.merge");
        Branch(inner, guardBlock, mismatchBlock);
        SetBlock(guardBlock);
        const LirReg guard = LowerExpr(*p->guard);
        const std::uint32_t guardPred = builder->CurrentBlock();
        Jump(mergeBlock);
        SetBlock(mismatchBlock);
        const LirReg mismatch = EmitConst("0", TypeRef::MakeBool());
        const std::uint32_t mismatchPred = builder->CurrentBlock();
        Jump(mergeBlock);
        SetBlock(mergeBlock);
        const LirReg result = NewReg();
        LirInstr phi;
        phi.dst = result;
        phi.op = LirOpcode::Phi;
        phi.type = TypeRef::MakeBool();
        phi.phiPreds = {{guard, guardPred}, {mismatch, mismatchPred}};
        Emit(std::move(phi));
        return result;
    }

    return EmitConst("1", TypeRef::MakeBool()); // wildcard fallback
}

// Expression lowering
// Returns the register holding the expression's value.
// For void expressions the return value is LirNoReg.

TypeRef HirToLirContext::SliceElementTypeFromType(const TypeRef &type) {
    if (type.IsSlice() && !type.inner.empty()) {
        // The element type names storage here, never a place, so its writability is not part of the answer.
        TypeRef element = type.inner[0];
        element.isMut = false;
        return element;
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

    // An arm that never returns closes its own block and does not reach the merge.
    SetBlock(thenBlock);
    StoreExprIntoSlot(*e.thenExpr, slot, type);
    if (!IsTerminated()) {
        Jump(mergeBlock);
    }

    SetBlock(elseBlock);
    StoreExprIntoSlot(*e.elseExpr, slot, type);
    if (!IsTerminated()) {
        Jump(mergeBlock);
    }

    SetBlock(mergeBlock);
}

void HirToLirContext::StoreExprValueIntoSlot(const HirExpr &expr, LirReg slot, const TypeRef &type) {
    if (auto *copy = dynamic_cast<const HirCopyExpr *>(&expr)) {
        StoreCopyIntoSlot(*copy, slot);
        return;
    }
    if (auto *move = dynamic_cast<const HirMoveExpr *>(&expr)) {
        StoreMoveIntoSlot(*move, slot);
        return;
    }
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
    if (IsViewType(type)) {
        const LirReg src = LowerLValue(expr);
        // A diverging arm, such as a call to `Panic`, produced no view to copy.
        if (IsTerminated()) {
            return;
        }
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
        if (IsTerminated()) {
            return;
        }
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
    // A call that never returns, such as `Panic`, closes its block and produces no value to store.
    if (IsTerminated()) {
        return;
    }
    EmitStore(EmitCastIfNeeded(val, expr.type, type), slot, type);
}

void HirToLirContext::EmitCopyPlan(const HirCopyPlan &plan, const LirReg source, const LirReg destination) {
    if (plan.kind == HirCopyPlan::Kind::Custom) {
        LirInstr call;
        call.op = LirOpcode::Call;
        call.type = TypeRef::MakeOpaque();
        call.srcs = {destination, source};
        call.strArg = plan.customCallee;
        if (const auto convention = funcConvs.find(plan.customCallee); convention != funcConvs.end()) {
            call.callConv = convention->second;
        }
        Emit(std::move(call));
        return;
    }
    if (plan.kind == HirCopyPlan::Kind::Structure || plan.kind == HirCopyPlan::Kind::Tuple) {
        for (std::size_t index = 0; index < plan.components.size(); ++index) {
            const std::string name =
                plan.kind == HirCopyPlan::Kind::Structure ? plan.componentNames[index] : std::to_string(index);
            const HirCopyPlan &component = plan.components[index];
            EmitCopyPlan(component, EmitFieldPtr(source, name, component.type),
                         EmitFieldPtr(destination, name, component.type));
        }
        return;
    }
    if (plan.kind == HirCopyPlan::Kind::Array && !plan.components.empty()) {
        const HirCopyPlan &element = plan.components.front();
        for (std::uint64_t index = 0; index < plan.type.arrayLength.value_or(0); ++index) {
            const LirReg offset = EmitConst(std::to_string(index), TypeRef::MakeUInt64());
            EmitCopyPlan(element, EmitIndexPtr(source, offset, element.type),
                         EmitIndexPtr(destination, offset, element.type));
        }
        return;
    }
    if (plan.kind == HirCopyPlan::Kind::Enum) {
        if (plan.form != CaseTypeForm::Variant) {
            BuilderFailure("scalar enum reached variant copy-plan lowering");
        }
        // Preserve the tag, inactive storage, and every trivial payload first. The active variant then replaces only
        // the payloads that require a recursive or custom copy. Custom copy operations initialize scratch storage, so
        // the shallow bits temporarily present in the destination are never observed or destroyed.
        EmitStore(EmitLoad(source, plan.type), destination, plan.type);
        const TypeRef tagType = EnumTagType(plan.type);
        const LirReg tag = EmitLoad(source, tagType);
        const std::size_t variantCount = std::min(
            {plan.variantDiscriminants.size(), plan.variantPayloadTypes.size(), plan.variantComponents.size()});
        for (std::size_t variantIndex = 0; variantIndex < variantCount; ++variantIndex) {
            const auto &payloadTypes = plan.variantPayloadTypes[variantIndex];
            const auto &payloadPlans = plan.variantComponents[variantIndex];
            if (std::ranges::none_of(payloadPlans, [](const HirCopyPlan &component) {
                    return component.kind != HirCopyPlan::Kind::Trivial;
                })) {
                continue;
            }

            const std::uint32_t payloadBlock = NewBlock("copy.variant");
            const std::uint32_t afterBlock = NewBlock("copy.variant.after");
            Branch(EmitBinary(LirOpcode::CmpEq, tag, EmitConst(plan.variantDiscriminants[variantIndex], tagType),
                              TypeRef::MakeBool()),
                   payloadBlock, afterBlock);
            SetBlock(payloadBlock);
            std::uint64_t offset = tagType.SizeInBytes().value_or(8);
            const std::size_t payloadCount = std::min(payloadTypes.size(), payloadPlans.size());
            for (std::size_t payloadIndex = 0; payloadIndex < payloadCount; ++payloadIndex) {
                const TypeRef &payloadType = payloadTypes[payloadIndex];
                const auto [size, alignment] = TypeLayoutOf(payloadType);
                offset = (offset + alignment - 1) / alignment * alignment;
                const HirCopyPlan &component = payloadPlans[payloadIndex];
                if (component.kind != HirCopyPlan::Kind::Trivial) {
                    const LirReg byteOffset = EmitConst(std::to_string(offset), TypeRef::MakeUInt64());
                    const LirReg sourceBytes = EmitIndexPtr(source, byteOffset, TypeRef::MakeChar8());
                    const LirReg destinationBytes = EmitIndexPtr(destination, byteOffset, TypeRef::MakeChar8());
                    EmitCopyPlan(component,
                                 EmitCast(sourceBytes, TypeRef::MakePointer(TypeRef::MakeChar8()),
                                          TypeRef::MakePointer(payloadType)),
                                 EmitCast(destinationBytes, TypeRef::MakePointer(TypeRef::MakeChar8()),
                                          TypeRef::MakePointer(payloadType)));
                }
                offset += size;
            }
            if (!IsTerminated()) {
                Jump(afterBlock);
            }
            SetBlock(afterBlock);
        }
        return;
    }
    EmitStore(EmitLoad(source, plan.type), destination, plan.type);
}

void HirToLirContext::StoreCopyIntoSlot(const HirCopyExpr &expression, const LirReg slot) {
    const LirReg source = LowerLValue(*expression.value);
    EmitCopyPlan(expression.plan, source, slot);
}

LirReg HirToLirContext::LowerCopy(const HirCopyExpr &expression) {
    const LirReg slot = EmitAlloca(expression.type);
    StoreCopyIntoSlot(expression, slot);
    return expression.type.kind == TypeRef::Kind::Array ? slot : EmitLoad(slot, expression.type);
}

void HirToLirContext::EmitMovePlan(const HirMovePlan &plan, const LirReg source, const LirReg destination) {
    if (plan.kind == HirMovePlan::Kind::Custom) {
        LirInstr call;
        call.op = LirOpcode::Call;
        call.type = TypeRef::MakeOpaque();
        call.srcs = {destination, EmitLoad(source, plan.type)};
        call.strArg = plan.customCallee;
        if (const auto convention = funcConvs.find(plan.customCallee); convention != funcConvs.end()) {
            call.callConv = convention->second;
        }
        Emit(std::move(call));
        return;
    }
    if (plan.kind == HirMovePlan::Kind::Structure || plan.kind == HirMovePlan::Kind::Tuple) {
        for (std::size_t index = 0; index < plan.components.size(); ++index) {
            const std::string name =
                plan.kind == HirMovePlan::Kind::Structure ? plan.componentNames[index] : std::to_string(index);
            const HirMovePlan &component = plan.components[index];
            EmitMovePlan(component, EmitFieldPtr(source, name, component.type),
                         EmitFieldPtr(destination, name, component.type));
        }
        return;
    }
    if (plan.kind == HirMovePlan::Kind::Array && !plan.components.empty()) {
        const HirMovePlan &element = plan.components.front();
        for (std::uint64_t index = 0; index < plan.type.arrayLength.value_or(0); ++index) {
            const LirReg offset = EmitConst(std::to_string(index), TypeRef::MakeUInt64());
            EmitMovePlan(element, EmitIndexPtr(source, offset, element.type),
                         EmitIndexPtr(destination, offset, element.type));
        }
        return;
    }
    if (plan.kind == HirMovePlan::Kind::Variant) {
        if (plan.form != CaseTypeForm::Variant) {
            BuilderFailure("scalar enum reached variant move-plan lowering");
        }
        // Relocate all trivial bytes first. The active case then replaces only payload fields whose own move has
        // behavior, leaving inactive storage untouched and never invoking a move operation for the wrong case.
        EmitStore(EmitLoad(source, plan.type), destination, plan.type);
        const TypeRef tagType = EnumTagType(plan.type);
        const LirReg tag = EmitLoad(source, tagType);
        const std::size_t variantCount = std::min(
            {plan.variantDiscriminants.size(), plan.variantPayloadTypes.size(), plan.variantComponents.size()});
        for (std::size_t variantIndex = 0; variantIndex < variantCount; ++variantIndex) {
            const auto &payloadTypes = plan.variantPayloadTypes[variantIndex];
            const auto &payloadPlans = plan.variantComponents[variantIndex];
            if (std::ranges::none_of(payloadPlans, [](const HirMovePlan &component) {
                    return component.kind != HirMovePlan::Kind::Trivial;
                })) {
                continue;
            }

            const std::uint32_t payloadBlock = NewBlock("move.variant");
            const std::uint32_t afterBlock = NewBlock("move.variant.after");
            Branch(EmitBinary(LirOpcode::CmpEq, tag, EmitConst(plan.variantDiscriminants[variantIndex], tagType),
                              TypeRef::MakeBool()),
                   payloadBlock, afterBlock);
            SetBlock(payloadBlock);
            std::uint64_t offset = tagType.SizeInBytes().value_or(8);
            const std::size_t payloadCount = std::min(payloadTypes.size(), payloadPlans.size());
            for (std::size_t payloadIndex = 0; payloadIndex < payloadCount; ++payloadIndex) {
                const TypeRef &payloadType = payloadTypes[payloadIndex];
                const auto [size, alignment] = TypeLayoutOf(payloadType);
                offset = (offset + alignment - 1) / alignment * alignment;
                const HirMovePlan &component = payloadPlans[payloadIndex];
                if (component.kind != HirMovePlan::Kind::Trivial) {
                    const LirReg byteOffset = EmitConst(std::to_string(offset), TypeRef::MakeUInt64());
                    const LirReg sourceBytes = EmitIndexPtr(source, byteOffset, TypeRef::MakeChar8());
                    const LirReg destinationBytes = EmitIndexPtr(destination, byteOffset, TypeRef::MakeChar8());
                    EmitMovePlan(component,
                                 EmitCast(sourceBytes, TypeRef::MakePointer(TypeRef::MakeChar8()),
                                          TypeRef::MakePointer(payloadType)),
                                 EmitCast(destinationBytes, TypeRef::MakePointer(TypeRef::MakeChar8()),
                                          TypeRef::MakePointer(payloadType)));
                }
                offset += size;
            }
            if (!IsTerminated()) {
                Jump(afterBlock);
            }
            SetBlock(afterBlock);
        }
        return;
    }
    EmitStore(EmitLoad(source, plan.type), destination, plan.type);
}

void HirToLirContext::StoreMoveIntoSlot(const HirMoveExpr &expression, const LirReg slot) {
    const LirReg source = LowerLValue(*expression.value);
    EmitMovePlan(expression.plan, source, slot);
}

LirReg HirToLirContext::LowerMove(const HirMoveExpr &expression) {
    const LirReg slot = EmitAlloca(expression.type);
    StoreMoveIntoSlot(expression, slot);
    return expression.type.kind == TypeRef::Kind::Array ? slot : EmitLoad(slot, expression.type);
}

LirReg HirToLirContext::LowerTernary(const HirTernaryExpr &e) {
    // Slice arms can produce either a descriptor address (literals/ranges) or its value (locals/calls). Build the
    // selected descriptor in one slot before loading a value, so a phi never merges addresses as sixteen-byte data.
    if (IsViewType(e.type)) {
        const LirReg slot = EmitAlloca(e.type);
        StoreTernaryInit(e, slot, e.type);
        return EmitLoad(slot, e.type);
    }
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
    // An arm that never returns closes its own block, reaches no merge, and contributes no value to the phi.
    const std::uint32_t thenIdx = builder->CurrentBlock();
    const bool thenReaches = !IsTerminated();
    if (thenReaches) {
        Jump(mergeBlock);
    }
    SetBlock(elseBlock);
    LirReg elseVal = LowerExpr(*e.elseExpr);
    const std::uint32_t elseIdx = builder->CurrentBlock();
    const bool elseReaches = !IsTerminated();
    if (elseReaches) {
        Jump(mergeBlock);
    }
    SetBlock(mergeBlock);
    if (!thenReaches && !elseReaches) {
        Unreachable();
        return thenVal;
    }
    LirReg result = NewReg();
    LirInstr phi;
    phi.dst = result;
    phi.op = LirOpcode::Phi;
    phi.type = e.type;
    if (thenReaches) {
        phi.phiPreds.emplace_back(thenVal, thenIdx);
    }
    if (elseReaches) {
        phi.phiPreds.emplace_back(elseVal, elseIdx);
    }
    Emit(std::move(phi));
    return result;
}

void HirToLirContext::StoreMatchInit(const HirMatchExpr &e, LirReg slot, const TypeRef &type) {
    LirReg subjectSlot = BorrowedNativeSubjectSlot(*e.subject);
    const std::vector<LirReg> *subjectPayload = nullptr;
    if (auto *subjectVar = dynamic_cast<const HirVarExpr *>(e.subject.get())) {
        if (const auto localIt = locals.find(subjectVar->name); localIt != locals.end()) {
            subjectSlot = localIt->second;
            // An addressable variant finds its payload from the subject's own storage. Cached payload addresses
            // come from whichever construction last wrote the local, which need not be on this path.
            if (const auto payloadIt = enumPayloadSlots.find(localIt->second);
                payloadIt != enumPayloadSlots.end() && !IsAggregateEnumType(subjectVar->type)) {
                subjectPayload = &payloadIt->second;
            }
        }
    }
    const bool inPlace = IsAggregateEnumType(e.subject->type) || ArmsDestructure(e.arms);
    if (subjectSlot == LirNoReg && inPlace) {
        subjectSlot = EmitAlloca(e.subject->type);
        StoreExprIntoSlot(*e.subject, subjectSlot, e.subject->type);
    }
    const LirReg subjectVal =
        subjectSlot != LirNoReg && inPlace ? MatchSubjectValue(subjectSlot, e.subject->type) : LowerExpr(*e.subject);
    ClearConsumedBinding(*e.subject);
    const std::uint32_t mergeBlock = NewBlock("match.expr.store.merge");
    if (e.arms.empty()) {
        if (!IsTerminated()) {
            Jump(mergeBlock);
        }
        SetBlock(mergeBlock);
        return;
    }

    const std::optional<std::uint32_t> unmatchedBlock = UnmatchedBlock(e.exhaustive, e.arms);
    for (std::size_t i = 0; i < e.arms.size(); ++i) {
        const auto &arm = e.arms[i];
        const bool isLast = (i + 1 == e.arms.size());
        const std::uint32_t bodyBlock = NewBlock(std::format("match.expr.store.arm{}", i));
        const std::uint32_t nextBlock =
            isLast ? unmatchedBlock.value_or(mergeBlock) : NewBlock(std::format("match.expr.store.next{}", i));
        std::vector<std::pair<LirReg, TypeRef>> residual;
        const LirReg matched = LowerArmPattern(*arm.pattern, subjectVal, e.subject->type, subjectPayload, subjectSlot,
                                               e.subject->consumption.has_value(), residual);
        Branch(matched, bodyBlock, nextBlock);
        SetBlock(bodyBlock);
        EmitResidualDrops(residual);
        StoreExprIntoSlot(*arm.body, slot, type);
        if (!IsTerminated()) {
            EmitCleanups(arm.cleanups);
            Jump(mergeBlock);
        }
        if (!isLast) {
            SetBlock(nextBlock);
        }
    }

    EmitUnmatchedTrap(unmatchedBlock, *e.subject, e.location);
    SetBlock(mergeBlock);
}

LirReg HirToLirContext::LowerMatchExpr(const HirMatchExpr &e) {
    const LirReg slot = EmitAlloca(e.type);
    StoreMatchInit(e, slot, e.type);
    return EmitLoad(slot, e.type);
}

/// Fill an existing 16-byte fat-pointer slot with {&concrete, &vtable}.
void HirToLirContext::StoreCoerceToInterface(const HirCoerceToInterfaceExpr &e, LirReg slot) {
    LirReg concreteSlot = LirNoReg;
    if (e.borrowed) {
        concreteSlot = e.value->type.kind == TypeRef::Kind::Reference ? LowerExpr(*e.value) : LowerLValue(*e.value);
    }
    else {
        concreteSlot = EmitAlloca(e.value->type);
        // Use the same materialization as a local binding: aggregate expressions may produce storage addresses,
        // and the interface must own a complete concrete value for the duration of the call.
        StoreExprIntoSlot(*e.value, concreteSlot, e.value->type);
    }

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
    const TypeRef dataType = TypeRef::MakePointer(e.elementType);
    // An empty view is the same value however it was built: it addresses nothing, rather than the zero-sized array
    // it was written as, so a reader that tests the pointer sees no storage to read.
    const LirReg data = e.length == 0 ? EmitConst("0", dataType) : LowerLValue(*e.value);
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
    // bound in a int..int). Widen them to the element type so the store
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
    for (std::size_t index = 0; index < e.fields.size(); ++index) {
        const auto &f = e.fields[index];
        const LirReg ptr = EmitFieldPtr(slot, f.name, f.value->type);
        PushPartialCleanupFrame(e.failureCleanups, index, slot);
        StoreExprIntoSlot(*f.value, ptr, f.value->type);
        PopPartialCleanupFrame();
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
        PushPartialCleanupFrame(e.failureCleanups, i, slot);
        StoreExprIntoSlot(*e.elements[i], ptr, e.elements[i]->type);
        PopPartialCleanupFrame();
    }
}

LirReg HirToLirContext::LowerStringLiteralSlice(const HirLiteralExpr &e) {
    const LirReg slot = EmitAlloca(e.type);
    StoreStringLiteralSlice(e, slot);
    return slot;
}

/// How many code units of `elementType` a literal's UTF-8 value transcodes to. The length a view publishes counts
/// the units it actually holds, so a wider encoding does not report the byte count of the source spelling.
std::uint64_t HirToLirContext::StringLiteralLength(const std::string &value, const TypeRef &elementType) {
    const int unitBytes = elementType.kind == TypeRef::Kind::Char16 ? 2
                        : elementType.kind == TypeRef::Kind::Char32 ? 4
                                                                    : 1;
    // The lexer rejects source that is not valid UTF-8, so nothing valid reaches here without a count.
    return static_cast<std::uint64_t>(CodeUnitCount(value, unitBytes).value_or(0));
}

void HirToLirContext::StoreStringLiteralSlice(const HirLiteralExpr &e, LirReg slot) {
    const TypeRef elemType = StringSliceElementType(e);
    const LirReg data = EmitStringAddr(e.value, elemType);
    LirReg dataField = EmitFieldPtr(slot, "data", TypeRef::MakePointer(elemType));
    EmitStore(data, dataField, TypeRef::MakePointer(elemType));
    LirReg len = EmitConst(std::to_string(StringLiteralLength(e.value, elemType)), TypeRef::MakeUInt64());
    LirReg lenField = EmitFieldPtr(slot, "length", TypeRef::MakeUInt64());
    EmitStore(len, lenField, TypeRef::MakeUInt64());
}

void HirToLirContext::StoreArrayInit(const HirArrayExpr &e, LirReg slot) {
    TypeRef elemType = e.elementType;
    if (elemType.IsUnknown() && !e.elements.empty()) {
        elemType = e.elements.front()->type;
    }
    if (e.repeatedElement) {
        const LirReg source = EmitAlloca(elemType);
        StoreExprIntoSlot(*e.repeatedElement, source, elemType);
        if (e.repeatCount == 0) {
            if (!e.repeatElementDropGlue.empty()) {
                EmitDropGlueCall(e.repeatElementDropGlue, source);
            }
            return;
        }

        if (e.repeatCount == 1) {
            EmitCopyPlan(e.repeatCopyPlan, source, EmitIndexPtr(slot, EmitConst("0", TypeRef::MakeUInt64()), elemType));
            if (!e.repeatElementDropGlue.empty()) {
                EmitDropGlueCall(e.repeatElementDropGlue, source);
            }
            return;
        }

        const TypeRef indexType = TypeRef::MakeUInt64();
        const LirReg indexSlot = EmitAlloca(indexType);
        EmitStore(EmitConst("0", indexType), indexSlot, indexType);
        const std::uint32_t conditionBlock = NewBlock("array.repeat.cond");
        const std::uint32_t bodyBlock = NewBlock("array.repeat.body");
        const std::uint32_t stepBlock = NewBlock("array.repeat.step");
        const std::uint32_t afterBlock = NewBlock("array.repeat.after");
        Jump(conditionBlock);

        SetBlock(conditionBlock);
        const LirReg index = EmitLoad(indexSlot, indexType);
        Branch(EmitBinary(LirOpcode::CmpLt, index, EmitConst(std::to_string(e.repeatCount), indexType),
                          TypeRef::MakeBool()),
               bodyBlock, afterBlock);

        SetBlock(bodyBlock);
        EmitCopyPlan(e.repeatCopyPlan, source, EmitIndexPtr(slot, index, elemType));
        if (!IsTerminated()) {
            Jump(stepBlock);
        }

        SetBlock(stepBlock);
        const LirReg current = EmitLoad(indexSlot, indexType);
        EmitStore(EmitBinary(LirOpcode::Add, current, EmitConst("1", indexType), indexType), indexSlot, indexType);
        Jump(conditionBlock);

        SetBlock(afterBlock);
        if (!e.repeatElementDropGlue.empty()) {
            EmitDropGlueCall(e.repeatElementDropGlue, source);
        }
        return;
    }
    if (IsArrayType(e.type)) {
        for (std::size_t i = 0; i < e.elements.size(); ++i) {
            const LirReg idx = EmitConst(std::to_string(i), TypeRef::MakeUInt64());
            const LirReg ptr = EmitIndexPtr(slot, idx, elemType);
            PushPartialCleanupFrame(e.failureCleanups, i, slot);
            StoreExprIntoSlot(*e.elements[i], ptr, elemType);
            PopPartialCleanupFrame();
        }
        return;
    }
    LirReg data = EmitAlloca(elemType, e.elements.size());
    for (std::size_t i = 0; i < e.elements.size(); ++i) {
        LirReg idx = EmitConst(std::to_string(i), TypeRef::MakeUInt64());
        LirReg ptr = EmitIndexPtr(data, idx, elemType);
        PushPartialCleanupFrame(e.failureCleanups, i, data);
        StoreExprIntoSlot(*e.elements[i], ptr, elemType);
        PopPartialCleanupFrame();
    }
    LirReg dataField = EmitFieldPtr(slot, "data", TypeRef::MakePointer(elemType));
    EmitStore(data, dataField, TypeRef::MakePointer(elemType));
    LirReg len = EmitConst(std::to_string(e.elements.size()), TypeRef::MakeUInt64());
    LirReg lenField = EmitFieldPtr(slot, "length", TypeRef::MakeUInt64());
    EmitStore(len, lenField, TypeRef::MakeUInt64());
}

} // namespace Rux::HirToLirDetail
