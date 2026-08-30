// Scalar, lvalue, and call expression lowering.

#include "Lowering/HirToLir/HirToLirContext.h"

#include <cassert>
#include <format>
#include <utility>

namespace Rux::HirToLirDetail {

LirReg HirToLirContext::LowerExprValue(const HirExpr &expr) {
    if (auto *e = dynamic_cast<const HirLiteralExpr *>(&expr)) {
        if (IsStringSliceLiteral(*e)) {
            return LowerStringLiteralSlice(*e);
        }
        return EmitConst(e->value, e->type);
    }
    if (auto *e = dynamic_cast<const HirVarExpr *>(&expr)) {
        if (const auto it = localConsts.find(e->name); it != localConsts.end()) {
            const TypeRef &constType = it->second.type.IsUnknown() ? e->type : it->second.type;
            LirReg value = LowerExpr(*it->second.value);
            return EmitCastIfNeeded(value, it->second.value->type, constType);
        }
        if (const auto it = globalConsts.find(e->name); it != globalConsts.end()) {
            LirReg value = LowerExpr(*it->second->value);
            return EmitCastIfNeeded(value, it->second->value->type, it->second->type);
        }
        auto it = locals.find(e->name);
        if (it != locals.end()) {
            if (IsInterfaceType(e->type)) {
                return it->second; // fat-ptr address lives in the slot
            }
            return EmitLoad(it->second, e->type);
        }
        // A function referenced by name (not called) evaluates to its
        // address, i.e. a function pointer.
        if (funcNames.contains(e->name)) {
            return EmitGlobalAddr(SymbolFor(e->name));
        }
        return EmitNamedLoad(e->name, e->type);
    }
    if (dynamic_cast<const HirSelfExpr *>(&expr)) {
        auto it = locals.find("self");
        if (it != locals.end()) {
            if (IsInterfaceType(expr.type)) {
                return it->second;
            }
            return EmitLoad(it->second, expr.type);
        }
        return EmitNamedLoad("self", expr.type);
    }
    if (auto *e = dynamic_cast<const HirPathExpr *>(&expr)) {
        std::string path;
        for (std::size_t i = 0; i < e->segments.size(); ++i) {
            if (i) {
                path += "::";
            }
            path += e->segments[i];
        }
        if (const auto it = globalConsts.find(path); it != globalConsts.end()) {
            LirReg value = LowerExpr(*it->second->value);
            return EmitCastIfNeeded(value, it->second->value->type, it->second->type);
        }
        // Module-qualified function referenced by name → its address. The
        // binary symbol is the final path segment (e.g. Math::Add → "Add").
        if (funcNames.contains(e->segments.back())) {
            return EmitGlobalAddr(SymbolFor(e->segments.back()));
        }
        return EmitNamedLoad(path, e->type);
    }
    if (auto *e = dynamic_cast<const HirUnaryExpr *>(&expr)) {
        return LowerUnary(*e);
    }
    if (auto *e = dynamic_cast<const HirPostfixExpr *>(&expr)) {
        return LowerPostfix(*e);
    }
    if (auto *e = dynamic_cast<const HirBinaryExpr *>(&expr)) {
        return LowerBinary(*e);
    }
    if (auto *e = dynamic_cast<const HirVariantEqualityExpr *>(&expr)) {
        return LowerVariantEquality(*e);
    }
    if (auto *e = dynamic_cast<const HirCopyExpr *>(&expr)) {
        return LowerCopy(*e);
    }
    if (auto *e = dynamic_cast<const HirMoveExpr *>(&expr)) {
        return LowerMove(*e);
    }
    if (auto *e = dynamic_cast<const HirAssignExpr *>(&expr)) {
        return LowerAssign(*e);
    }
    if (auto *e = dynamic_cast<const HirTernaryExpr *>(&expr)) {
        return LowerTernary(*e);
    }
    if (auto *e = dynamic_cast<const HirMatchExpr *>(&expr)) {
        return LowerMatchExpr(*e);
    }
    if (auto *e = dynamic_cast<const HirEnumConstructExpr *>(&expr)) {
        return LowerEnumConstruct(*e);
    }
    if (auto *e = dynamic_cast<const HirCallExpr *>(&expr)) {
        return LowerCall(*e);
    }
    if (auto *e = dynamic_cast<const HirCoerceToInterfaceExpr *>(&expr)) {
        return LowerCoerceToInterface(*e);
    }
    if (auto *e = dynamic_cast<const HirArrayToSliceExpr *>(&expr)) {
        return LowerArrayToSlice(*e);
    }
    if (auto *e = dynamic_cast<const HirInterfaceCallExpr *>(&expr)) {
        return LowerInterfaceCall(*e);
    }
    if (auto *e = dynamic_cast<const HirIndexExpr *>(&expr)) {
        if (e->index->type.IsRange()) {
            return LowerRangeIndex(*e);
        }
        LirReg idx = LowerExpr(*e->index);
        LirReg sliceBase = LowerSliceDataPtr(*e->object, e->type);
        LirReg ptr = EmitIndexPtr(sliceBase, idx, e->type);
        if (IsInterfaceType(e->type)) {
            return ptr;
        }
        return EmitLoad(ptr, e->type);
    }
    if (auto *e = dynamic_cast<const HirFieldExpr *>(&expr)) {
        const TypeRef &objectValueType =
            e->object->type.kind == TypeRef::Kind::Reference && !e->object->type.inner.empty()
                ? e->object->type.inner.front()
                : e->object->type;
        if (objectValueType.kind == TypeRef::Kind::Array && objectValueType.arrayLength && e->field == "length") {
            return EmitConst(std::to_string(*objectValueType.arrayLength), TypeRef::MakeUInt());
        }
        const bool indirect =
            e->object->type.kind == TypeRef::Kind::Pointer || e->object->type.kind == TypeRef::Kind::Reference;
        LirReg base = indirect ? LowerExpr(*e->object) : LowerLValue(*e->object);
        LirReg ptr = EmitFieldPtr(base, e->field, e->type);
        // An interface value is a 16-byte fat pointer that is handled by its address, the same way a local or an
        // element of one is. Loading it would take eight of its sixteen bytes and pass that as the address, which is
        // what made a call through an interface stored in a struct jump into the data pointer.
        if (IsInterfaceType(e->type)) {
            return ptr;
        }
        return EmitLoad(ptr, e->type);
    }
    if (auto *e = dynamic_cast<const HirStructInitExpr *>(&expr)) {
        return LowerStructInit(*e);
    }
    if (auto *e = dynamic_cast<const HirArrayExpr *>(&expr)) {
        return LowerArray(*e);
    }
    if (auto *e = dynamic_cast<const HirTupleExpr *>(&expr)) {
        return LowerTuple(*e);
    }
    if (auto *e = dynamic_cast<const HirCastExpr *>(&expr)) {
        return EmitCast(LowerExpr(*e->operand), e->operand->type, e->type);
    }
    if (auto *e = dynamic_cast<const HirIsExpr *>(&expr)) {
        // Should only be reached for interface types (rejected by
        // sema). Return false as a safe fallback.
        LowerExpr(*e->operand);
        return EmitConst("false", TypeRef::MakeBool());
    }
    if (auto *e = dynamic_cast<const HirBlockExpr *>(&expr)) {
        LowerBlock(e->block);
        if (IsTerminated()) {
            return LirNoReg;
        }
        // A block carrying a trailing expression evaluates to it. Blocks lowered from source carry none and keep
        // producing a placeholder, so this changes nothing a program can already write.
        if (e->value) {
            return LowerExpr(*e->value);
        }
        return EmitConst("0", e->type);
    }
    if (auto *e = dynamic_cast<const HirRangeExpr *>(&expr)) {
        return LowerRange(*e);
    }
    return EmitConst("0", expr.type);
}

LirReg HirToLirContext::LowerPostfix(const HirPostfixExpr &e) {
    const LirReg ptr = LowerLValue(*e.operand);
    const LirReg old_val = EmitLoad(ptr, e.type);
    LirOpcode op = LirOpcode::Add;
    switch (e.op) {
    case TokenKind::PlusPlus:
        op = LirOpcode::Add;
        break;
    case TokenKind::MinusMinus:
        op = LirOpcode::Sub;
        break;
    default:
        BuilderFailure("postfix operator has no LIR opcode mapping");
        break;
    }
    const LirReg new_val = IsPointerArithmetic(e.type) ? EmitPointerStep(old_val, e.type, op == LirOpcode::Add)
                                                       : EmitBinary(op, old_val, EmitConst("1", e.type), e.type);
    EmitStore(new_val, ptr, e.type);
    return old_val;
}

LirReg HirToLirContext::LowerUnary(const HirUnaryExpr &e) {
    using TK = TokenKind;
    switch (e.op) {
    case TK::Minus:
        return EmitUnary(LirOpcode::Neg, LowerExpr(*e.operand), e.type);
    case TK::Bang:
        return EmitUnary(LirOpcode::Not, LowerExpr(*e.operand), e.type);
    case TK::Tilde:
        return EmitUnary(LirOpcode::BitNot, LowerExpr(*e.operand), e.type);
    case TK::Star: {
        // Dereference: the operand evaluates to a pointer; load through
        // it.
        LirReg ptr = LowerExpr(*e.operand);
        return EmitLoad(ptr, e.type);
    }
    case TK::At: {
        return LowerLValue(*e.operand);
    }
    case TK::PlusPlus:
    case TK::MinusMinus: {
        const LirReg ptr = LowerLValue(*e.operand);
        const LirReg old_val = EmitLoad(ptr, e.type);
        const LirOpcode aop = (e.op == TK::PlusPlus) ? LirOpcode::Add : LirOpcode::Sub;
        const LirReg new_val = IsPointerArithmetic(e.type) ? EmitPointerStep(old_val, e.type, e.op == TK::PlusPlus)
                                                           : EmitBinary(aop, old_val, EmitConst("1", e.type), e.type);
        EmitStore(new_val, ptr, e.type);
        return new_val;
    }
    default:
        BuilderFailure("unary operator has no LIR opcode mapping");
        return LirNoReg;
    }
}

LirReg HirToLirContext::EmitVariantPayloadEquality(const HirVariantEqualityPayload &payload, const LirReg left,
                                                   const LirReg right) {
    using Operation = HirVariantEqualityPayload::Operation;
    if (payload.operation == Operation::Builtin) {
        return EmitBinary(LirOpcode::CmpEq, EmitLoad(left, payload.type), EmitLoad(right, payload.type),
                          TypeRef::MakeBool());
    }
    if (payload.operation == Operation::Custom) {
        const bool receiverIndirect = payload.customReceiverType.kind == TypeRef::Kind::Pointer ||
                                      payload.customReceiverType.kind == TypeRef::Kind::Reference;
        const bool argumentIndirect = payload.customArgumentType.kind == TypeRef::Kind::Pointer ||
                                      payload.customArgumentType.kind == TypeRef::Kind::Reference;
        LirInstr call;
        call.dst = NewReg();
        call.op = LirOpcode::Call;
        call.type = TypeRef::MakeBool();
        call.strArg = SymbolFor(payload.customCallee);
        call.srcs.push_back(receiverIndirect ? left : EmitLoad(left, payload.type));
        call.srcs.push_back(argumentIndirect ? right : EmitLoad(right, payload.type));
        const LirReg result = call.dst;
        Emit(std::move(call));
        return result;
    }
    if (payload.operation == Operation::Variant) {
        return EmitVariantEquality(payload.type, payload.variantCases, left, right);
    }

    LirReg equal = EmitConst("true", TypeRef::MakeBool());
    if (payload.operation == Operation::Tuple) {
        for (std::size_t index = 0; index < payload.elements.size(); ++index) {
            const HirVariantEqualityPayload &element = payload.elements[index];
            const LirReg leftElement = EmitFieldPtr(left, std::to_string(index), element.type);
            const LirReg rightElement = EmitFieldPtr(right, std::to_string(index), element.type);
            equal = EmitBinary(LirOpcode::And, equal, EmitVariantPayloadEquality(element, leftElement, rightElement),
                               TypeRef::MakeBool());
        }
        return equal;
    }
    if (payload.operation == Operation::Array && !payload.elements.empty()) {
        const HirVariantEqualityPayload &element = payload.elements.front();
        for (std::uint64_t index = 0; index < payload.type.arrayLength.value_or(0); ++index) {
            const LirReg offset = EmitConst(std::to_string(index), TypeRef::MakeUInt64());
            const LirReg leftElement = EmitIndexPtr(left, offset, element.type);
            const LirReg rightElement = EmitIndexPtr(right, offset, element.type);
            equal = EmitBinary(LirOpcode::And, equal, EmitVariantPayloadEquality(element, leftElement, rightElement),
                               TypeRef::MakeBool());
        }
    }
    return equal;
}

LirReg HirToLirContext::EmitVariantPayloadsEquality(const std::vector<HirVariantEqualityPayload> &payloads,
                                                    const LirReg left, const LirReg right, const TypeRef &tagType) {
    LirReg equal = EmitConst("true", TypeRef::MakeBool());
    std::uint64_t offset = tagType.SizeInBytes().value_or(8);
    for (const HirVariantEqualityPayload &payload : payloads) {
        const std::uint64_t size = payload.type.SizeInBytes().value_or(8);
        const std::uint64_t alignment = size > 0 ? std::min<std::uint64_t>(size, 8) : 1;
        offset = (offset + alignment - 1) / alignment * alignment;
        const LirReg byteOffset = EmitConst(std::to_string(offset), TypeRef::MakeUInt64());
        const LirReg leftBytes = EmitIndexPtr(left, byteOffset, TypeRef::MakeChar8());
        const LirReg rightBytes = EmitIndexPtr(right, byteOffset, TypeRef::MakeChar8());
        const TypeRef payloadPointer = TypeRef::MakePointer(payload.type);
        const LirReg leftPayload = EmitCast(leftBytes, TypeRef::MakePointer(TypeRef::MakeChar8()), payloadPointer);
        const LirReg rightPayload = EmitCast(rightBytes, TypeRef::MakePointer(TypeRef::MakeChar8()), payloadPointer);
        equal = EmitBinary(LirOpcode::And, equal, EmitVariantPayloadEquality(payload, leftPayload, rightPayload),
                           TypeRef::MakeBool());
        offset += size;
    }
    return equal;
}

LirReg HirToLirContext::EmitVariantEquality(const TypeRef &type, const std::vector<HirVariantEqualityCase> &cases,
                                            const LirReg left, const LirReg right) {
    const TypeRef tagType = EnumTagType(type);
    const LirReg leftTag = EmitLoad(left, tagType);
    const LirReg rightTag = EmitLoad(right, tagType);
    const std::uint32_t sharedTagBlock = NewBlock("variant.eq.shared-tag");
    const std::uint32_t differentTagBlock = NewBlock("variant.eq.different-tag");
    const std::uint32_t mergeBlock = NewBlock("variant.eq.merge");
    Branch(EmitBinary(LirOpcode::CmpEq, leftTag, rightTag, TypeRef::MakeBool()), sharedTagBlock, differentTagBlock);

    std::vector<std::pair<LirReg, std::uint32_t>> results;
    SetBlock(differentTagBlock);
    results.emplace_back(EmitConst("false", TypeRef::MakeBool()), builder->CurrentBlock());
    Jump(mergeBlock);

    SetBlock(sharedTagBlock);
    for (const HirVariantEqualityCase &variantCase : cases) {
        const std::uint32_t caseBlock = NewBlock("variant.eq.case");
        const std::uint32_t nextBlock = NewBlock("variant.eq.next");
        Branch(EmitBinary(LirOpcode::CmpEq, leftTag, EmitConst(variantCase.discriminant, tagType), TypeRef::MakeBool()),
               caseBlock, nextBlock);
        SetBlock(caseBlock);
        const LirReg caseEqual = EmitVariantPayloadsEquality(variantCase.payloads, left, right, tagType);
        results.emplace_back(caseEqual, builder->CurrentBlock());
        Jump(mergeBlock);
        SetBlock(nextBlock);
    }

    // Equal invalid tags cannot arise in a checked program, but keeping a concrete fallback makes this CFG total and
    // avoids treating corrupt storage as equal.
    results.emplace_back(EmitConst("false", TypeRef::MakeBool()), builder->CurrentBlock());
    Jump(mergeBlock);
    SetBlock(mergeBlock);
    const LirReg result = NewReg();
    LirInstr phi;
    phi.dst = result;
    phi.op = LirOpcode::Phi;
    phi.type = TypeRef::MakeBool();
    phi.phiPreds = std::move(results);
    Emit(std::move(phi));
    return result;
}

LirReg HirToLirContext::LowerVariantEquality(const HirVariantEqualityExpr &e) {
    const auto stableStorage = [&](const HirExpr &operand) {
        const auto *unary = dynamic_cast<const HirUnaryExpr *>(&operand);
        const bool addressable =
            dynamic_cast<const HirVarExpr *>(&operand) || dynamic_cast<const HirSelfExpr *>(&operand) ||
            dynamic_cast<const HirFieldExpr *>(&operand) || dynamic_cast<const HirIndexExpr *>(&operand) ||
            (unary && unary->op == TokenKind::Star);
        if (addressable) {
            return LowerLValue(operand);
        }
        const LirReg slot = EmitAlloca(e.variantType);
        StoreExprIntoSlot(operand, slot, e.variantType);
        return slot;
    };
    const LirReg left = stableStorage(*e.left);
    const LirReg right = stableStorage(*e.right);
    const LirReg equal = EmitVariantEquality(e.variantType, e.cases, left, right);
    return e.negated ? EmitUnary(LirOpcode::Not, equal, TypeRef::MakeBool()) : equal;
}

LirReg HirToLirContext::LowerBinary(const HirBinaryExpr &e) {
    using TK = TokenKind;
    // Short-circuit operators: branch to avoid evaluating the
    // right-hand side.
    if (e.op == TK::AmpAmp || e.op == TK::PipePipe) {
        LirReg lhs = LowerExpr(*e.left);
        std::uint32_t rhsBlock = NewBlock(e.op == TK::AmpAmp ? "land.rhs" : "lor.rhs");
        std::uint32_t shortBlock = NewBlock(e.op == TK::AmpAmp ? "land.short" : "lor.short");
        std::uint32_t mergeBlock = NewBlock(e.op == TK::AmpAmp ? "land.merge" : "lor.merge");
        if (e.op == TK::AmpAmp) {
            Branch(lhs, rhsBlock, shortBlock); // false → skip rhs
        }
        else {
            Branch(lhs, shortBlock, rhsBlock); // true  → skip rhs
        }
        // Short-circuit path: result is the known constant.
        SetBlock(shortBlock);
        LirReg shortVal = EmitConst(e.op == TK::AmpAmp ? "false" : "true", TypeRef::MakeBool());
        // The block a side ends in is not the block it started in: an operand
        // that is itself a short-circuit -- the `b && c` of `a || (b && c)` --
        // opens blocks of its own and leaves the cursor on its own merge. That
        // block, not the one entered here, is what reaches the phi below, and
        // naming the wrong predecessor is what let the phi take the value from
        // an edge that was never followed.
        const std::uint32_t shortBlockIdx = builder->CurrentBlock();
        Jump(mergeBlock);
        // Right-hand side path.
        SetBlock(rhsBlock);
        LirReg rhs = LowerExpr(*e.right);
        const std::uint32_t rhsBlockIdx = builder->CurrentBlock();
        Jump(mergeBlock);
        // Join with a phi.
        SetBlock(mergeBlock);
        LirReg result = NewReg();
        LirInstr phi;
        phi.dst = result;
        phi.op = LirOpcode::Phi;
        phi.type = TypeRef::MakeBool();
        phi.phiPreds = {{shortVal, shortBlockIdx}, {rhs, rhsBlockIdx}};
        Emit(std::move(phi));
        return result;
    }
    LirReg lhs = LowerExpr(*e.left);
    LirReg rhs = LowerExpr(*e.right);

    // A comparison loads both of its operands at the width of the left one,
    // so a right operand of a narrower type would be read past the slot it
    // sits in and take in whatever follows it: `'A' == c8'A'` compared four
    // bytes against a byte and came out false. Widen the narrower side to the
    // type of the other, which is the implicit conversion the language
    // already allows between these types.
    if (IsComparison(e.op)) {
        const TypeRef &leftType = e.left->type;
        const TypeRef &rightType = e.right->type;
        if (IsScalar(leftType) && IsScalar(rightType) && leftType != rightType) {
            const auto leftSize = leftType.SizeInBytes();
            const auto rightSize = rightType.SizeInBytes();
            if (leftSize && rightSize) {
                if (*leftSize > *rightSize) {
                    rhs = EmitCast(rhs, rightType, leftType);
                }
                else if (*rightSize > *leftSize) {
                    lhs = EmitCast(lhs, leftType, rightType);
                }
            }
        }
    }

    // An arithmetic or bitwise operator reads both operands at its own width, for the same reason a comparison does,
    // so one that arrived narrower is read past the storage it occupies. Below 64 bits that went unnoticed, because
    // the operand and the operation share a register either way; at 128 bits and above the operation reads limbs the
    // operand never wrote, and an untyped `1` beside an `int128` took in whatever followed it. Widen each side to the
    // operation's type, which is the implicit conversion the language already allows here.
    //
    // A shift is left alone: its right operand is a count rather than a second value of the left's type, and the back
    // ends already read it at its own width.
    const bool countsOperand = e.op == TK::LessLess || e.op == TK::GreaterGreater || e.op == TK::GreaterGreaterGreater;
    if (!IsComparison(e.op) && !countsOperand && IsScalar(e.type) && !IsPointerArithmetic(e.type)) {
        lhs = EmitCastIfNeeded(lhs, e.left->type, e.type);
        rhs = EmitCastIfNeeded(rhs, e.right->type, e.type);
    }

    // Pointer arithmetic offsets by whole elements, so the integer operand is scaled by the pointee size. IndexPtr
    // carries the element type and is scaled in the back end, which is the only place a struct pointee can be sized.
    if ((e.op == TK::Plus || e.op == TK::Minus) && IsPointerArithmetic(e.type)) {
        if (e.left->type.kind == TypeRef::Kind::Pointer) {
            // ptr + int, or ptr - int with the count negated first.
            const LirReg index =
                e.op == TK::Minus ? EmitBinary(LirOpcode::Sub, EmitConst("0", e.right->type), rhs, e.right->type) : rhs;
            return EmitPointerOffset(lhs, index, e.type);
        }
        // int + ptr. Subtraction never lands here: `int - ptr` is not a pointer.
        return EmitPointerOffset(rhs, lhs, e.type);
    }

    return EmitBinary(RequireOpcode(CheckedLirBuilder::BinaryOpcode(e.op)), lhs, rhs, e.type);
}

LirReg HirToLirContext::LowerAssign(const HirAssignExpr &e) {
    const bool simpleAssignment = e.op == TokenKind::Assign || e.op == TokenKind::MoveArrow;
    if (simpleAssignment && (IsInterfaceType(e.type) || IsSliceType(e.type))) {
        const LirReg ptr = LowerLValue(*e.target);
        StoreExprIntoSlot(*e.value, ptr, e.type);
        return ptr;
    }

    LirReg val = LowerExpr(*e.value);
    // Whatever the target held is destroyed once the new value exists and before it is stored, so an assignment from
    // a value the target itself owns part of still reads live storage. The store below then re-establishes ownership.
    if (e.overwriteCleanup) {
        EmitCleanup(*e.overwriteCleanup);
    }
    if (!simpleAssignment) {
        // Compound assignment: load current value, compute, then store.
        const LirReg current = LowerExpr(*e.target);
        if (IsPointerArithmetic(e.type)) {
            // `ptr += n` and `ptr -= n` offset by whole elements, scaled in the back end as above.
            const LirReg index = e.op == TokenKind::MinusAssign
                                   ? EmitBinary(LirOpcode::Sub, EmitConst("0", e.value->type), val, e.value->type)
                                   : val;
            val = EmitPointerOffset(current, index, e.type);
        }
        else {
            // The same widening the binary form does: `wide += 1` computes at the place's width, not the literal's.
            val = EmitBinary(RequireOpcode(CheckedLirBuilder::CompoundOpcode(e.op)), current,
                             e.op == TokenKind::LessLessAssign || e.op == TokenKind::GreaterGreaterAssign ||
                                     e.op == TokenKind::GreaterGreaterGreaterAssign
                                 ? val
                                 : EmitCastIfNeeded(val, e.value->type, e.type),
                             e.type);
        }
    }
    else {
        val = EmitCastIfNeeded(val, e.value->type, e.type);
    }
    const LirReg ptr = LowerLValue(*e.target);
    EmitStore(val, ptr, e.type, e.isVolatile);
    if (e.overwriteCleanup) {
        MarkBindingLive(e.overwriteCleanup->bindingId, true);
    }
    return val;
}

LirReg HirToLirContext::LowerInterfaceCall(const HirInterfaceCallExpr &e) {
    const TypeRef ptrType = TypeRef::MakePointer(TypeRef::MakeOpaque());

    // fat_ptr_addr = the 8-byte value that IS the fat ptr address
    LirReg fatPtrAddr = LowerExpr(*e.fatPtrExpr);

    // Load data_ptr = fat_ptr[0]
    LirReg i0 = EmitConst("0", TypeRef::MakeUInt64());
    LirReg dataField = EmitIndexPtr(fatPtrAddr, i0, TypeRef::MakeUInt64());
    LirReg dataPtr = EmitLoad(dataField, ptrType);

    // Load vtbl_ptr = fat_ptr[1]
    LirReg i1 = EmitConst("1", TypeRef::MakeUInt64());
    LirReg vtblField = EmitIndexPtr(fatPtrAddr, i1, TypeRef::MakeUInt64());
    LirReg vtblPtr = EmitLoad(vtblField, ptrType);

    // Load fn_ptr = vtbl_ptr[methodIdx]
    LirReg midx = EmitConst(std::to_string(e.methodIdx), TypeRef::MakeUInt64());
    LirReg fnSlot = EmitIndexPtr(vtblPtr, midx, TypeRef::MakeUInt64());
    LirReg fnPtr = EmitLoad(fnSlot, ptrType);

    // CallIndirect(fn_ptr, data_ptr, args...)
    const LirReg dst = e.type.IsOpaque() ? LirNoReg : NewReg();
    LirInstr ci;
    ci.dst = dst;
    ci.type = e.type;
    ci.op = LirOpcode::CallIndirect;
    ci.callConv = CallingConvention::Default;
    ci.srcs = {fnPtr, dataPtr};
    for (const auto &arg : e.args) {
        ci.srcs.push_back(LowerArgument(*arg));
    }
    Emit(std::move(ci));
    return dst;
}

LirReg HirToLirContext::LowerArgument(const HirExpr &argument) {
    // A slice parameter is lowered as a pointer to the {data, length} pair, so the argument is the pair's address. The
    // implementation behind a vtable is an ordinary method with that same parameter, so an interface call passes the
    // address too: handing over the 16-byte value put `data` and `length` in two registers on System V and AAPCS64,
    // and the callee read `data` as the pointer to the pair. Win64 passes such a value by reference to a copy, which
    // is the one ABI where the two happened to agree.
    if (IsSliceType(argument.type)) {
        return LowerLValue(argument);
    }
    // An array or tuple parameter is a value, like a struct parameter, and the back ends place a value by its own
    // size: on System V an aggregate wider than sixteen bytes goes to the caller's stack slots. A literal lowers to
    // the slot it was built in, and a slot is a pointer — which is what a caller handed over for `Start([...], 64)`,
    // so the callee read the pointer as the array's first word and the array's stack slots as whatever the caller
    // happened to leave there. Win64 and AAPCS64 pass a wide aggregate by reference to a copy, so a pointer was
    // indistinguishable from the value there. Loading the value makes the argument what the parameter is.
    // A range is the same story at sixteen bytes. One held in a variable was loaded on the way to the call and
    // arrived as the value the callee reads, but one written at the call site — `v[2..5]` — handed over the address
    // of its slot, which System V and AAPCS64 placed as the first of the two registers the callee reads the pair
    // from, leaving `end` whatever the second register happened to hold.
    if (argument.type.kind == TypeRef::Kind::Array || argument.type.kind == TypeRef::Kind::Tuple ||
        argument.type.IsRange()) {
        return EmitLoad(LowerLValue(argument), argument.type);
    }
    return LowerExpr(argument);
}

LirReg HirToLirContext::LowerCall(const HirCallExpr &e) {
    if (const auto *callee = dynamic_cast<const HirVarExpr *>(e.callee.get())) {
        if (callee->name == "__builtin_debug_assert_disabled") {
            return LirNoReg;
        }
        if (callee->name == "__builtin_assert") {
            if (e.args.size() != 2) {
                return LirNoReg;
            }

            const LirReg condition = LowerExpr(*e.args[0]);
            const LirReg message = LowerLValue(*e.args[1]);
            std::string messageText;
            if (const auto *literal = dynamic_cast<const HirLiteralExpr *>(e.args[1].get());
                literal && IsStringSliceLiteral(*literal)) {
                messageText = literal->value;
            }

            LirInstr assertion;
            assertion.op = LirOpcode::Assert;
            assertion.type = TypeRef::MakeOpaque();
            assertion.srcs = {condition, message};
            assertion.strArg = std::move(messageText);
            assertion.sourceFile = e.sourceFile;
            assertion.sourceFunction = e.sourceFunction;
            assertion.sourceLine = e.sourceLine;
            assertion.sourceColumn = e.sourceColumn;
            Emit(std::move(assertion));
            return LirNoReg;
        }
        if (callee->name == "__builtin_panic") {
            if (e.args.size() != 1) {
                return LirNoReg;
            }

            const LirReg message = LowerLValue(*e.args[0]);
            std::string messageText;
            if (const auto *literal = dynamic_cast<const HirLiteralExpr *>(e.args[0].get());
                literal && IsStringSliceLiteral(*literal)) {
                messageText = literal->value;
            }

            LirInstr panic;
            panic.op = LirOpcode::Panic;
            panic.type = TypeRef::MakeOpaque();
            panic.srcs = {message};
            panic.strArg = std::move(messageText);
            panic.sourceFile = e.sourceFile;
            panic.sourceFunction = e.sourceFunction;
            panic.sourceLine = e.sourceLine;
            panic.sourceColumn = e.sourceColumn;
            Emit(std::move(panic));
            Unreachable();
            return LirNoReg;
        }
    }

    std::vector<LirReg> argRegs;
    argRegs.reserve(e.args.size());
    for (const auto &arg : e.args) {
        argRegs.push_back(LowerArgument(*arg));
    }
    const LirReg dst = e.type.IsOpaque() ? LirNoReg : NewReg();
    LirInstr ci;
    ci.dst = dst;
    ci.type = e.type;
    ci.srcs = std::move(argRegs);
    if (auto *v = dynamic_cast<const HirVarExpr *>(e.callee.get()); v && !locals.contains(v->name)) {
        // Direct call to a named function. A same-named local would be a
        // function-pointer variable and is handled by the indirect path.
        ci.op = LirOpcode::Call;
        auto it = funcConvs.find(v->name);
        if (it != funcConvs.end()) {
            ci.callConv = it->second;
        }
        SetCVariadicCallMetadata(ci, v->name, e);
        // The convention is keyed by the Rux name; the call itself has to
        // relocate against the imported symbol.
        ci.strArg = SymbolFor(v->name);
    }
    else if (auto *p = dynamic_cast<const HirPathExpr *>(e.callee.get())) {
        ci.op = LirOpcode::Call;
        // Module qualifiers are compile-time only; the binary symbol is
        // just the final segment (e.g. Math::Add → "Add").
        const std::string &callee = p->segments.back();
        auto it = funcConvs.find(callee);
        if (it != funcConvs.end()) {
            ci.callConv = it->second;
        }
        SetCVariadicCallMetadata(ci, callee, e);
        ci.strArg = SymbolFor(callee);
    }
    else {
        // Function pointer / indirect call: evaluate callee first.
        ci.op = LirOpcode::CallIndirect;
        LirReg fp = LowerExpr(*e.callee);
        ci.srcs.insert(ci.srcs.begin(), fp);
    }
    Emit(std::move(ci));
    if (e.isNoReturn) {
        Unreachable();
    }
    return dst;
}

LirReg HirToLirContext::LowerSliceDataPtr(const HirExpr &object, const TypeRef &elemType) {
    if (IsArrayType(object.type)) {
        return LowerLValue(object);
    }
    if (!IsSliceType(object.type)) {
        return LowerExpr(object);
    }
    LirReg slicePtr = LowerLValue(object);
    LirReg dataField = EmitFieldPtr(slicePtr, "data", TypeRef::MakePointer(elemType));
    return EmitLoad(dataField, TypeRef::MakePointer(elemType));
}

LirReg HirToLirContext::LowerRangeIndex(const HirIndexExpr &e) {
    const TypeRef elemType = SliceElementTypeFromType(e.type);
    const TypeRef dataType = TypeRef::MakePointer(elemType);
    const TypeRef indexType = TypeRef::MakeUInt64();

    // Evaluate the collection once and obtain its data pointer and length.
    LirReg data;
    LirReg collectionLength;
    const TypeRef &collectionType = e.object->type.kind == TypeRef::Kind::Reference && !e.object->type.inner.empty()
                                      ? e.object->type.inner.front()
                                      : e.object->type;
    if (IsArrayType(collectionType)) {
        data = e.object->type.kind == TypeRef::Kind::Reference ? LowerExpr(*e.object) : LowerLValue(*e.object);
        collectionLength = EmitConst(std::to_string(collectionType.arrayLength.value_or(0)), indexType);
    }
    else {
        const LirReg objectSlot =
            e.object->type.kind == TypeRef::Kind::Reference ? LowerExpr(*e.object) : LowerLValue(*e.object);
        const LirReg dataField = EmitFieldPtr(objectSlot, "data", dataType);
        data = EmitLoad(dataField, dataType);
        const LirReg lengthField = EmitFieldPtr(objectSlot, "length", indexType);
        collectionLength = EmitLoad(lengthField, indexType);
    }

    const TypeRef &rangeType = e.index->type;
    LirReg rangeSlot = LirNoReg;
    if (rangeType.RangeHasStart() || rangeType.RangeHasEnd()) {
        if (const auto *literal = dynamic_cast<const HirRangeExpr *>(e.index.get())) {
            rangeSlot = LowerRange(*literal);
        }
        else {
            rangeSlot = LowerLValue(*e.index);
        }
    }

    LirReg start = EmitConst("0", indexType);
    if (rangeType.RangeHasStart()) {
        const TypeRef boundType = rangeType.inner.empty() ? indexType : rangeType.inner[0];
        const LirReg startField = EmitFieldPtr(rangeSlot, "start", boundType);
        start = EmitCastIfNeeded(EmitLoad(startField, boundType), boundType, indexType);
    }

    LirReg end = collectionLength;
    if (rangeType.RangeHasEnd()) {
        const TypeRef boundType = rangeType.inner.empty() ? indexType : rangeType.inner[0];
        const LirReg endField = EmitFieldPtr(rangeSlot, "end", boundType);
        end = EmitCastIfNeeded(EmitLoad(endField, boundType), boundType, indexType);
        if (rangeType.IsInclusiveRange()) {
            end = EmitBinary(LirOpcode::Add, end, EmitConst("1", indexType), indexType);
        }
    }

    const LirReg sliceData = EmitIndexPtr(data, start, elemType);
    const LirReg sliceLength = EmitBinary(LirOpcode::Sub, end, start, indexType);
    const LirReg slot = EmitAlloca(e.type);
    const LirReg resultDataField = EmitFieldPtr(slot, "data", dataType);
    EmitStore(sliceData, resultDataField, dataType);
    const LirReg resultLengthField = EmitFieldPtr(slot, "length", indexType);
    EmitStore(sliceLength, resultLengthField, indexType);
    return slot;
}

/// Returns the pointer register for an lvalue expression.
LirReg HirToLirContext::LowerLValue(const HirExpr &expr) {
    // A receiver taken by value is a local like any other, and its slot is where a write through it has to land. This
    // never came up while every receiver was a pointer, because a write went through the pointer rather than to `self`.
    if (dynamic_cast<const HirSelfExpr *>(&expr)) {
        if (const auto it = locals.find("self"); it != locals.end()) {
            return it->second;
        }
    }
    if (auto *e = dynamic_cast<const HirVarExpr *>(&expr)) {
        auto it = locals.find(e->name);
        if (it != locals.end()) {
            return it->second;
        }
        // A slice-typed constant lives in read-only data under its own
        // name, so indexing or reading .length works off that address. A
        // local const has no symbol, so its initializer is materialized.
        if (const auto constIt = localConsts.find(e->name); constIt != localConsts.end()) {
            return LowerLValue(*constIt->second.value);
        }
        if (const auto constIt = globalConsts.find(e->name); constIt != globalConsts.end()) {
            if (IsSliceType(constIt->second->type) || IsArrayType(constIt->second->type)) {
                return EmitGlobalAddr(e->name, constIt->second->type);
            }
            return LowerLValue(*constIt->second->value);
        }
        // Global variable address.
        LirReg ptr = NewReg();
        LirInstr i;
        i.dst = ptr;
        i.op = LirOpcode::Load;
        i.type = TypeRef::MakePointer(e->type);
        i.strArg = "&" + e->name;
        Emit(std::move(i));
        return ptr;
    }
    if (auto *e = dynamic_cast<const HirArrayExpr *>(&expr)) {
        LirReg slot = EmitAlloca(e->type);
        StoreArrayInit(*e, slot);
        return slot;
    }
    if (auto *e = dynamic_cast<const HirArrayToSliceExpr *>(&expr)) {
        return LowerArrayToSlice(*e);
    }
    if (auto *e = dynamic_cast<const HirTupleExpr *>(&expr)) {
        LirReg slot = EmitAlloca(e->type);
        StoreTupleInit(*e, slot);
        return slot;
    }
    if (auto *e = dynamic_cast<const HirRangeExpr *>(&expr)) {
        return LowerRange(*e);
    }
    if (auto *e = dynamic_cast<const HirTernaryExpr *>(&expr)) {
        LirReg slot = EmitAlloca(e->type);
        StoreTernaryInit(*e, slot, e->type);
        return slot;
    }
    if (auto *e = dynamic_cast<const HirFieldExpr *>(&expr)) {
        const bool indirect =
            e->object->type.kind == TypeRef::Kind::Pointer || e->object->type.kind == TypeRef::Kind::Reference;
        LirReg base = indirect ? LowerExpr(*e->object) : LowerLValue(*e->object);
        return EmitFieldPtr(base, e->field, e->type);
    }

    if (auto *e = dynamic_cast<const HirIndexExpr *>(&expr)) {
        if (e->index->type.IsRange()) {
            return LowerRangeIndex(*e);
        }
        LirReg idx = LowerExpr(*e->index);
        LirReg sliceBase = LowerSliceDataPtr(*e->object, e->type);
        return EmitIndexPtr(sliceBase, idx, e->type);
    }
    if (auto *e = dynamic_cast<const HirUnaryExpr *>(&expr)) {
        if (e->op == TokenKind::Star) {
            return LowerExpr(*e->operand); // pointer dereference
        }
    }
    if (auto *e = dynamic_cast<const HirLiteralExpr *>(&expr)) {
        // String literals: return the alloca slot directly instead of
        // spilling through the 16-byte fallback (which would misread
        // the alloca vreg's 8-byte pointer slot as the slice value).
        if (IsStringSliceLiteral(*e)) {
            return LowerStringLiteralSlice(*e);
        }
        LirReg slot = EmitAlloca(expr.type);
        EmitStore(EmitConst(e->value, e->type), slot, expr.type);
        return slot;
    }
    // Non-addressable fallback: spill to a temp slot.
    LirReg val = LowerExpr(expr);
    LirReg slot = EmitAlloca(expr.type);
    EmitStore(val, slot, expr.type);
    return slot;
}

} // namespace Rux::HirToLirDetail
