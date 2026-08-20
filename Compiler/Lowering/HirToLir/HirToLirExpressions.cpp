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
        if (e->object->type.kind == TypeRef::Kind::Array && e->object->type.arrayLength && e->field == "length") {
            return EmitConst(std::to_string(*e->object->type.arrayLength), TypeRef::MakeUInt());
        }
        LirReg base = e->object->type.kind == TypeRef::Kind::Pointer ? LowerExpr(*e->object) : LowerLValue(*e->object);
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
    if (e.op == TokenKind::Assign && (IsInterfaceType(e.type) || IsSliceType(e.type))) {
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
    if (e.op != TokenKind::Assign) {
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
        ci.srcs.push_back(LowerExpr(*arg));
    }
    Emit(std::move(ci));
    return dst;
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
        // Slice types are 16-byte {data, length} structs. The callee
        // expects a POINTER to the struct (not the 16-byte value, which
        // wouldn't fit in a single ABI register), so take the lvalue.
        if (IsSliceType(arg->type)) {
            argRegs.push_back(LowerLValue(*arg));
        }
        else {
            argRegs.push_back(LowerExpr(*arg));
        }
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
    if (IsArrayType(e.object->type)) {
        data = LowerLValue(*e.object);
        collectionLength = EmitConst(std::to_string(e.object->type.arrayLength.value_or(0)), indexType);
    }
    else {
        const LirReg objectSlot = LowerLValue(*e.object);
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
        LirReg base = e->object->type.kind == TypeRef::Kind::Pointer ? LowerExpr(*e->object) : LowerLValue(*e->object);
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
