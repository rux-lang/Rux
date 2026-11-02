#include "Ir/Lir/LirCheckedArithmetic.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Rux {
namespace {
/// Hands out register numbers above every one the function already uses.
class RegisterSource {
public:
    explicit RegisterSource(const LirFunc &function) {
        for (const auto &parameter : function.params) {
            next = std::max(next, parameter.reg + 1);
        }
        for (const auto &block : function.blocks) {
            for (const auto &instruction : block.instrs) {
                if (instruction.dst != LirNoReg) {
                    next = std::max(next, instruction.dst + 1);
                }
            }
        }
    }

    [[nodiscard]] LirReg Take() noexcept {
        return next++;
    }

private:
    LirReg next = 0;
};

class Expansion {
public:
    Expansion(std::vector<LirInstr> &output, RegisterSource &registers)
        : out(output)
        , regs(registers) {
    }

    LirReg Emit(const LirOpcode op, const TypeRef &type, LirRegs sources, std::string argument = {},
                const bool defines = true) {
        LirInstr instruction;
        instruction.dst = defines ? regs.Take() : LirNoReg;
        instruction.op = op;
        instruction.type = type;
        instruction.srcs = std::move(sources);
        instruction.strArg = std::move(argument);
        const LirReg result = instruction.dst;
        out.push_back(std::move(instruction));
        return result;
    }

    /// An instruction defining a register the function already names.
    void Define(const LirReg dst, const LirOpcode op, const TypeRef &type, LirRegs sources, std::string argument = {}) {
        LirInstr instruction;
        instruction.dst = dst;
        instruction.op = op;
        instruction.type = type;
        instruction.srcs = std::move(sources);
        instruction.strArg = std::move(argument);
        out.push_back(std::move(instruction));
    }

    LirReg Constant(const std::string &literal, const TypeRef &type) {
        return Emit(LirOpcode::Const, type, {}, literal);
    }

    LirReg Convert(const LirReg value, const TypeRef &from, const TypeRef &to) {
        return Emit(LirOpcode::Cast, to, {value}, from.ToString());
    }

private:
    std::vector<LirInstr> &out;
    RegisterSource &regs;
};

/// The type twice as wide as an integer of at most 64 bits, of its signedness, that every product of two of its values
/// fits.
TypeRef DoubleWidth(const TypeRef &type) {
    const bool isSigned = type.IsSigned();
    if (type.SizeInBytes().value_or(8) <= 4) {
        return TypeRef::MakePrimitive(isSigned ? TypeRef::Kind::Int64 : TypeRef::Kind::UInt64);
    }
    return TypeRef::MakePrimitive(isSigned ? TypeRef::Kind::Int128 : TypeRef::Kind::UInt128);
}

/// Emits the wrapping result of `checked` into the register it defined and returns a boolean that holds when the
/// result was exact.
LirReg ExpandOperation(const LirInstr &checked, Expansion &emit) {
    const TypeRef &type = checked.type;
    const TypeRef flag = TypeRef::MakeBool();
    const LirReg left = checked.srcs[0];
    const LirReg right = checked.srcs[1];
    if (checked.op == LirOpcode::CheckedMul) {
        const TypeRef wide = DoubleWidth(type);
        const LirReg product =
            emit.Emit(LirOpcode::Mul, wide, {emit.Convert(left, type, wide), emit.Convert(right, type, wide)});
        emit.Define(checked.dst, LirOpcode::Cast, type, {product}, wide.ToString());
        const LirReg widened = emit.Convert(checked.dst, type, wide);
        return emit.Emit(LirOpcode::CmpEq, flag, {widened, product});
    }
    const LirOpcode plain = UncheckedOpcode(checked.op);
    const LirReg result = checked.dst;
    emit.Define(result, plain, type, {left, right});
    if (type.IsSigned()) {
        // A signed sum overflowed when both operands' signs differ from the result's; a difference when the operands
        // differ in sign and the result's differs from the left one's.
        const LirReg first = emit.Emit(LirOpcode::Xor, type, {left, plain == LirOpcode::Add ? result : right});
        const LirReg second = emit.Emit(LirOpcode::Xor, type, {plain == LirOpcode::Add ? right : left, result});
        const LirReg both = emit.Emit(LirOpcode::And, type, {first, second});
        return emit.Emit(LirOpcode::CmpGe, flag, {both, emit.Constant("0", type)});
    }
    // An unsigned sum that wrapped is below its left operand; a difference wrapped exactly when the right one is
    // larger.
    return plain == LirOpcode::Add ? emit.Emit(LirOpcode::CmpGe, flag, {result, left})
                                   : emit.Emit(LirOpcode::CmpGe, flag, {left, right});
}

/// The block that reports `checked`'s failure and stops, as a lowered `Panic` does.
LirBlock FailureBlock(const LirInstr &checked, RegisterSource &registers, const std::size_t ordinal) {
    LirBlock block;
    block.label = "overflow.fail" + std::to_string(ordinal);
    Expansion emit(block.instrs, registers);
    const TypeRef element = TypeRef::MakeChar8();
    const TypeRef slice = TypeRef::MakeSlice(element);
    const TypeRef dataPointer = TypeRef::MakePointer(TypeRef::MakePointer(element));
    const TypeRef lengthPointer = TypeRef::MakePointer(TypeRef::MakeUInt64());
    const LirReg slot = emit.Emit(LirOpcode::Alloca, slice, {});
    const LirReg text = emit.Emit(LirOpcode::StringAddr, TypeRef::MakePointer(element), {}, checked.strArg);
    const LirReg data = emit.Emit(LirOpcode::FieldPtr, dataPointer, {slot}, "data");
    emit.Emit(LirOpcode::Store, TypeRef::MakePointer(element), {text, data}, {}, false);
    const LirReg length = emit.Constant(std::to_string(checked.strArg.size()), TypeRef::MakeUInt64());
    const LirReg lengthField = emit.Emit(LirOpcode::FieldPtr, lengthPointer, {slot}, "length");
    emit.Emit(LirOpcode::Store, TypeRef::MakeUInt64(), {length, lengthField}, {}, false);
    LirInstr panic;
    panic.op = LirOpcode::Panic;
    panic.type = TypeRef::MakeOpaque();
    panic.srcs = {slot};
    panic.strArg = checked.strArg;
    panic.source = checked.source;
    block.instrs.push_back(std::move(panic));
    LirTerminator stop;
    stop.kind = LirTermKind::Unreachable;
    block.term = std::move(stop);
    return block;
}

void RenamePredecessor(LirFunc &function, const std::uint32_t from, const std::uint32_t to) {
    const auto rename = [&](const std::uint32_t target) {
        for (auto &instruction : function.blocks[target].instrs) {
            for (auto &[source, predecessor] : instruction.phiPreds) {
                static_cast<void>(source);
                if (predecessor == from) {
                    predecessor = to;
                }
            }
        }
    };
    const auto &term = function.blocks[to].term;
    if (!term) {
        return;
    }
    switch (term->kind) {
    case LirTermKind::Jump:
        rename(term->trueTarget);
        break;
    case LirTermKind::Branch:
        rename(term->trueTarget);
        if (term->falseTarget != term->trueTarget) {
            rename(term->falseTarget);
        }
        break;
    case LirTermKind::Switch: {
        std::vector<std::uint32_t> targets{term->defaultTarget};
        for (const auto &entry : term->cases) {
            if (!std::ranges::contains(targets, entry.target)) {
                targets.push_back(entry.target);
            }
        }
        for (const std::uint32_t target : targets) {
            rename(target);
        }
        break;
    }
    case LirTermKind::Return:
    case LirTermKind::Unreachable:
        break;
    }
}
} // namespace

bool HasCheckedArithmetic(const LirFunc &function) noexcept {
    return std::ranges::any_of(function.blocks, [](const LirBlock &block) {
        return std::ranges::any_of(block.instrs,
                                   [](const LirInstr &instruction) { return IsOverflowCheckedOpcode(instruction.op); });
    });
}

void ExpandCheckedArithmetic(LirFunc &function) {
    RegisterSource registers(function);
    std::size_t ordinal = 0;
    // Blocks appended while splitting are visited in turn, so a block holding several checks is split at each.
    for (std::uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        auto &instructions = function.blocks[blockIndex].instrs;
        const auto found = std::ranges::find_if(
            instructions, [](const LirInstr &instruction) { return IsOverflowCheckedOpcode(instruction.op); });
        if (found == instructions.end()) {
            continue;
        }
        const LirInstr checked = std::move(*found);
        std::vector<LirInstr> tail(std::make_move_iterator(found + 1), std::make_move_iterator(instructions.end()));
        instructions.erase(found, instructions.end());

        std::vector<LirInstr> expansion;
        Expansion emit(expansion, registers);
        const LirReg exact = ExpandOperation(checked, emit);
        for (auto &instruction : expansion) {
            function.blocks[blockIndex].instrs.push_back(std::move(instruction));
        }

        const auto continuation = static_cast<std::uint32_t>(function.blocks.size());
        const auto failure = continuation + 1;
        LirBlock rest;
        rest.label = function.blocks[blockIndex].label + ".ok" + std::to_string(ordinal);
        rest.instrs = std::move(tail);
        rest.term = std::move(function.blocks[blockIndex].term);
        LirTerminator branch;
        branch.kind = LirTermKind::Branch;
        branch.cond = exact;
        branch.trueTarget = continuation;
        branch.falseTarget = failure;
        function.blocks[blockIndex].term = std::move(branch);
        LirBlock fail = FailureBlock(checked, registers, ordinal++);
        function.blocks.push_back(std::move(rest));
        function.blocks.push_back(std::move(fail));
        RenamePredecessor(function, blockIndex, continuation);
    }
}
} // namespace Rux
