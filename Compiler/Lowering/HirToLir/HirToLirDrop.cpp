// Destruction lowering: one glue function per droppable type, and the conditional calls that reach it.
//
// Analysis decides *where* a value is destroyed and records a `DropGluePlan` saying *how*. This file is the part that
// makes either observable: it turns each plan into an ordinary function taking the value's address, and turns each
// recorded cleanup into a call to that function guarded by the binding's drop flag.
//
// The flag is what keeps a moved value from being destroyed where it used to live. A binding that owns something sets
// its flag when it is initialized and clears it when an expression consumes it; a cleanup runs the glue only when the
// flag is still set. Straight-line moves are already rejected statically, so the flag exists for the paths that are
// not straight: a value moved away inside one arm of a branch and left alone in another.

#include "Lowering/HirToLir/HirToLirContext.h"

#include <algorithm>
#include <format>
#include <utility>

namespace Rux::HirToLirDetail {

/// The whole package's glue, in the order the plans were sorted, so a build produces the same symbols every time.
std::vector<LirFunc> HirToLirContext::SynthesizeDropGlue(const std::vector<DropGluePlan> &plans) {
    std::vector<LirFunc> functions;
    functions.reserve(plans.size());
    for (const DropGluePlan &plan : plans) {
        functions.push_back(SynthesizeDropGlueFunc(plan));
    }
    return functions;
}

/// One `void (*T)` function that destroys the value at its argument.
///
/// The parameter is the address rather than the value: destruction reads storage that is about to go away, and a
/// by-value parameter would be one more thing needing destruction. The address is used directly instead of being
/// spilled to a slot, the same way a slice or interface parameter already is.
LirFunc HirToLirContext::SynthesizeDropGlueFunc(const DropGluePlan &plan) {
    currentFunction = plan.symbol;
    locals.clear();
    localConsts.clear();
    enumPayloadSlots.clear();
    dropFlags.clear();

    LirFunc function;
    function.name = plan.symbol;
    function.returnType = TypeRef::MakeOpaque();
    CheckedLirBuilder functionBuilder(function);
    builder = &functionBuilder;
    SetBlock(NewBlock("entry"));

    const LirReg address = builder->DefineParameter();
    function.params.push_back({address, TypeRef::MakePointer(plan.type), "value"});
    EmitDropGlueSteps(plan.steps, address);
    if (!IsTerminated()) {
        Return(std::nullopt, TypeRef::MakeOpaque());
    }
    builder = nullptr;
    return function;
}

void HirToLirContext::EmitDropGlueSteps(const std::vector<DropGlueStep> &steps, const LirReg base) {
    for (const DropGlueStep &step : steps) {
        if (IsTerminated()) {
            return;
        }
        EmitDropGlueStep(step, base);
    }
}

void HirToLirContext::EmitDropGlueStep(const DropGlueStep &step, const LirReg base) {
    switch (step.kind) {
    case DropGlueStep::Kind::InvokeDrop:
        // A type whose `Drop` lowering could not name has nothing to call. The steps around it still run, so the
        // fields of a half-resolved type are released even when its own body is missing.
        if (!step.dropSymbol.empty()) {
            EmitDropGlueCall(step.dropSymbol, base);
        }
        return;
    case DropGlueStep::Kind::Field:
        EmitDropGlueSteps(step.children, EmitFieldPtr(base, step.name, step.type));
        return;
    case DropGlueStep::Kind::TupleElement:
        EmitDropGlueSteps(step.children, EmitFieldPtr(base, std::to_string(step.ordinal), step.type));
        return;
    case DropGlueStep::Kind::ArrayElements:
        EmitDropGlueArrayElements(step, base);
        return;
    case DropGlueStep::Kind::EnumVariant:
        EmitDropGlueEnumVariant(step, base);
        return;
    }
}

/// Destroy every element of an array, last first.
///
/// The count is known here, but the loop is emitted rather than unrolled: an array of a thousand elements would
/// otherwise put a thousand copies of its element's destruction in the glue.
void HirToLirContext::EmitDropGlueArrayElements(const DropGlueStep &step, const LirReg base) {
    if (step.count == 0) {
        return;
    }
    const TypeRef indexType = TypeRef::MakeUInt64();
    const LirReg index = EmitAlloca(indexType);
    EmitStore(EmitConst(std::to_string(step.count), indexType), index, indexType);

    const std::uint32_t condBlock = NewBlock("drop.array.cond");
    const std::uint32_t bodyBlock = NewBlock("drop.array.body");
    const std::uint32_t afterBlock = NewBlock("drop.array.after");
    Jump(condBlock);

    SetBlock(condBlock);
    const LirReg remaining = EmitLoad(index, indexType);
    const LirReg zero = EmitConst("0", indexType);
    Branch(EmitBinary(LirOpcode::CmpNe, remaining, zero, TypeRef::MakeBool()), bodyBlock, afterBlock);

    SetBlock(bodyBlock);
    // The counter names how many elements are left, so the one to destroy is the element below it. Counting down this
    // way keeps the loop's only comparison against zero and still visits the last element first.
    const LirReg one = EmitConst("1", indexType);
    const LirReg current = EmitBinary(LirOpcode::Sub, EmitLoad(index, indexType), one, indexType);
    EmitStore(current, index, indexType);
    EmitDropGlueSteps(step.children, EmitIndexPtr(base, current, step.type));
    if (!IsTerminated()) {
        Jump(condBlock);
    }
    SetBlock(afterBlock);
}

/// Destroy the payload of one enum variant, when the value stored is that variant.
///
/// Only the aggregate representation is decoded here -- a tag word followed by the payloads at their aligned offsets,
/// laid out exactly as construction writes them. A compact enum packs its payload into the upper half of a single
/// word, which nothing wider than four bytes survives, so a droppable payload never has a well-defined home there and
/// the glue leaves it alone rather than destroying a truncated copy of it.
void HirToLirContext::EmitDropGlueEnumVariant(const DropGlueStep &step, const LirReg base) {
    if (!IsAggregateEnumType(step.type)) {
        return;
    }
    const TypeRef tagType = TypeRef::MakeInt64();
    const LirReg tag = EmitLoad(base, tagType);
    const LirReg expected = EmitConst(step.discriminant, tagType);
    const std::uint32_t payloadBlock = NewBlock("drop.variant");
    const std::uint32_t afterBlock = NewBlock("drop.variant.after");
    Branch(EmitBinary(LirOpcode::CmpEq, tag, expected, TypeRef::MakeBool()), payloadBlock, afterBlock);

    SetBlock(payloadBlock);
    // Offsets are recomputed from the payload types the plan carries, by the same rule construction uses, because a
    // payload's home depends on the sizes of the payloads written before it -- including the ones nothing destroys.
    std::uint64_t offset = 8;
    for (std::size_t index = 0; index < step.payloadTypes.size(); ++index) {
        const std::uint64_t size = step.payloadTypes[index].SizeInBytes().value_or(8);
        const std::uint64_t alignment = size > 0 ? std::min<std::uint64_t>(size, 8) : 1;
        offset = (offset + alignment - 1) / alignment * alignment;
        for (const DropGlueStep &child : step.children) {
            if (child.ordinal != index || IsTerminated()) {
                continue;
            }
            const LirReg byteOffset = EmitConst(std::to_string(offset), TypeRef::MakeUInt64());
            const LirReg bytes = EmitIndexPtr(base, byteOffset, TypeRef::MakeChar8());
            // Counted in bytes to reach the payload, then said to be a pointer to what is actually there. The byte
            // pointer alone is enough to destroy a payload that is one droppable value, and wrong for a payload
            // that is an aggregate: a step reaching a field of it asks the back end for that field's offset, the
            // back end reads it from the type the register holds, and a pointer to bytes has no fields -- so every
            // field was destroyed at the front of the payload. A payload whose droppable field happened to sit
            // first was destroyed correctly, which is why this survived until an entry with a key before its value.
            const LirReg payload = EmitCast(bytes, TypeRef::MakePointer(TypeRef::MakeChar8()),
                                            TypeRef::MakePointer(step.payloadTypes[index]));
            EmitDropGlueSteps(child.children, payload);
        }
        offset += size;
    }
    if (!IsTerminated()) {
        Jump(afterBlock);
    }
    SetBlock(afterBlock);
}

void HirToLirContext::EmitDropGlueCall(const std::string &symbol, const LirReg address) {
    LirInstr call;
    call.dst = LirNoReg;
    call.op = LirOpcode::Call;
    call.type = TypeRef::MakeOpaque();
    call.srcs = {address};
    call.strArg = symbol;
    if (const auto convention = funcConvs.find(symbol); convention != funcConvs.end()) {
        call.callConv = convention->second;
    }
    Emit(std::move(call));
}

/// The flag slot for one binding, created on first mention.
///
/// A binding is only ever reached by cleanups within its own function, so the slot is allocated where it is first
/// needed rather than at the top of the function.
LirReg HirToLirContext::DropFlagSlot(const std::uint64_t bindingId) {
    if (const auto found = dropFlags.find(bindingId); found != dropFlags.end()) {
        return found->second;
    }
    const LirReg slot = EmitAlloca(TypeRef::MakeBool());
    dropFlags.emplace(bindingId, slot);
    return slot;
}

void HirToLirContext::MarkBindingLive(const std::uint64_t bindingId, const bool live) {
    if (bindingId == 0) {
        return;
    }
    const LirReg slot = DropFlagSlot(bindingId);
    EmitStore(EmitConst(live ? "true" : "false", TypeRef::MakeBool()), slot, TypeRef::MakeBool());
}

/// Clear the flag of whatever binding this expression took ownership from.
///
/// Called once the expression has been evaluated, never before: the value still has to be read out of the storage the
/// flag guards.
void HirToLirContext::ClearConsumedBinding(const HirExpr &expression) {
    if (expression.consumedBindingId != 0 && !IsTerminated()) {
        MarkBindingLive(expression.consumedBindingId, false);
    }
}

/// Every expression is lowered through here so that consumption is recorded in exactly one place, whichever of the
/// two ways the value was produced.
LirReg HirToLirContext::LowerExpr(const HirExpr &expression) {
    const LirReg value = LowerExprValue(expression);
    ClearConsumedBinding(expression);
    return value;
}

void HirToLirContext::StoreExprIntoSlot(const HirExpr &expression, const LirReg slot, const TypeRef &type) {
    StoreExprValueIntoSlot(expression, slot, type);
    ClearConsumedBinding(expression);
}

/// One recorded destruction: run the glue if the binding still owns its value, and record that it no longer does.
///
/// Clearing the flag afterwards matters for the paths that reach two cleanups for the same binding -- a `break` out of
/// a loop whose body scope and enclosing function both name it -- so the value is destroyed once rather than twice.
void HirToLirContext::EmitCleanup(const HirDropAction &action) {
    if (action.glueSymbol.empty() || IsTerminated()) {
        return;
    }
    LirReg address = LirNoReg;
    if (const auto local = locals.find(action.name); local != locals.end()) {
        address = local->second;
    }
    if (address == LirNoReg) {
        return;
    }
    if (action.bindingId == 0) {
        EmitDropGlueCall(action.glueSymbol, address);
        return;
    }

    const LirReg slot = DropFlagSlot(action.bindingId);
    const std::uint32_t dropBlock = NewBlock("drop.live");
    const std::uint32_t afterBlock = NewBlock("drop.done");
    Branch(EmitLoad(slot, TypeRef::MakeBool()), dropBlock, afterBlock);
    SetBlock(dropBlock);
    EmitDropGlueCall(action.glueSymbol, address);
    MarkBindingLive(action.bindingId, false);
    Jump(afterBlock);
    SetBlock(afterBlock);
}

void HirToLirContext::EmitCleanups(const std::vector<HirDropAction> &actions) {
    for (const HirDropAction &action : actions) {
        EmitCleanup(action);
    }
}

} // namespace Rux::HirToLirDetail
