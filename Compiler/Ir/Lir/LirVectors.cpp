#include "Ir/Lir/LirVectors.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace Rux {
namespace {
[[nodiscard]] bool IsVector(const LirOpcode op) noexcept {
    switch (op) {
    case LirOpcode::VectorLoad:
    case LirOpcode::VectorStore:
    case LirOpcode::VectorSplat:
    case LirOpcode::VectorAdd:
    case LirOpcode::VectorSub:
    case LirOpcode::VectorMul:
    case LirOpcode::VectorDiv:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] LirOpcode ScalarOf(const LirOpcode op) noexcept {
    switch (op) {
    case LirOpcode::VectorAdd:
        return LirOpcode::Add;
    case LirOpcode::VectorSub:
        return LirOpcode::Sub;
    case LirOpcode::VectorMul:
        return LirOpcode::Mul;
    default:
        return LirOpcode::Div;
    }
}
} // namespace

bool HasVectorInstructions(const LirFunc &function) noexcept {
    return std::ranges::any_of(function.blocks, [](const LirBlock &block) {
        return std::ranges::any_of(block.instrs, [](const LirInstr &instruction) { return IsVector(instruction.op); });
    });
}

void Devectorize(LirFunc &function) {
    LirReg next = 0;
    for (const auto &parameter : function.params)
        next = std::max(next, parameter.reg + 1);
    for (const auto &block : function.blocks)
        for (const auto &instruction : block.instrs)
            if (instruction.dst != LirNoReg)
                next = std::max(next, instruction.dst + 1);
    // Each vector value as its two lanes.
    std::unordered_map<LirReg, std::pair<LirReg, LirReg>> lanes;
    for (auto &block : function.blocks) {
        std::vector<LirInstr> rewritten;
        rewritten.reserve(block.instrs.size());
        const auto emit = [&](const LirOpcode op, const TypeRef &type, std::vector<LirReg> sources, LirReg dst,
                              std::string argument = {}) {
            LirInstr instruction;
            instruction.op = op;
            instruction.type = type;
            instruction.dst = dst;
            instruction.srcs = std::move(sources);
            instruction.strArg = std::move(argument);
            rewritten.push_back(std::move(instruction));
            return dst;
        };
        const auto following = [&](const LirReg pointer, const TypeRef &element) {
            const auto one = emit(LirOpcode::Const, TypeRef::MakeUInt64(), {}, next++, "1");
            return emit(LirOpcode::IndexPtr, TypeRef::MakePointer(element), {pointer, one}, next++);
        };
        for (auto &instruction : block.instrs) {
            switch (instruction.op) {
            case LirOpcode::VectorLoad: {
                const auto pointer = instruction.srcs[0];
                const auto low = emit(LirOpcode::Load, instruction.type, {pointer}, next++);
                const auto high =
                    emit(LirOpcode::Load, instruction.type, {following(pointer, instruction.type)}, next++);
                lanes.emplace(instruction.dst, std::pair{low, high});
                break;
            }
            case LirOpcode::VectorStore: {
                const auto [low, high] = lanes.at(instruction.srcs[0]);
                const auto pointer = instruction.srcs[1];
                emit(LirOpcode::Store, instruction.type, {low, pointer}, LirNoReg);
                emit(LirOpcode::Store, instruction.type, {high, following(pointer, instruction.type)}, LirNoReg);
                break;
            }
            case LirOpcode::VectorSplat:
                lanes.emplace(instruction.dst, std::pair{instruction.srcs[0], instruction.srcs[0]});
                break;
            case LirOpcode::VectorAdd:
            case LirOpcode::VectorSub:
            case LirOpcode::VectorMul:
            case LirOpcode::VectorDiv: {
                const auto op = ScalarOf(instruction.op);
                const auto [leftLow, leftHigh] = lanes.at(instruction.srcs[0]);
                const auto [rightLow, rightHigh] = lanes.at(instruction.srcs[1]);
                const auto low = emit(op, instruction.type, {leftLow, rightLow}, next++);
                const auto high = emit(op, instruction.type, {leftHigh, rightHigh}, next++);
                lanes.emplace(instruction.dst, std::pair{low, high});
                break;
            }
            default:
                rewritten.push_back(std::move(instruction));
                break;
            }
        }
        block.instrs = std::move(rewritten);
    }
}
} // namespace Rux
