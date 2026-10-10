#include "Optimization/LirScalarCombines.h"

#include "Optimization/ConstantEvaluator.h"

#include <algorithm>
#include <bit>
#include <unordered_map>

namespace Rux::Optimization {
namespace {
bool Combine(LirFunc &function) {
    std::unordered_map<LirReg, LirInstr> definitions;
    LirReg next = 0;
    for (const auto &parameter : function.params) {
        definitions[parameter.reg] = {};
        next = std::max(next, parameter.reg + 1);
    }
    for (const auto &block : function.blocks) {
        for (const auto &instruction : block.instrs) {
            if (instruction.dst != LirNoReg) {
                if (!definitions.emplace(instruction.dst, instruction).second) {
                    return false;
                }
                next = std::max(next, instruction.dst + 1);
            }
        }
    }
    const auto constant = [&](const LirReg reg) -> std::optional<std::uint64_t> {
        const auto found = definitions.find(reg);
        if (found == definitions.end() || found->second.op != LirOpcode::Const || !found->second.type.IsInteger()) {
            return std::nullopt;
        }
        const auto value = ParseConstant(found->second.strArg, found->second.type);
        return value && value->Width() <= 64 ? std::optional{value->RawBits()} : std::nullopt;
    };
    bool changed = false;
    for (auto &block : function.blocks) {
        std::vector<LirInstr> output;
        for (auto instruction : block.instrs) {
            // Keep a deliberately narrow contract: no signed arithmetic, wide integers or floating identities.
            const bool word =
                instruction.type.kind == TypeRef::Kind::UInt32 || instruction.type.kind == TypeRef::Kind::UInt64;
            if (word && instruction.srcs.size() == 2 && instruction.op == LirOpcode::Or) {
                const auto left = definitions.find(instruction.srcs[0]);
                const auto right = definitions.find(instruction.srcs[1]);
                if (left != definitions.end() && right != definitions.end()) {
                    const auto *shiftLeft = &left->second;
                    const auto *shiftRight = &right->second;
                    if (shiftRight->op == LirOpcode::Shl) {
                        std::swap(shiftLeft, shiftRight);
                    }
                    if (shiftLeft->op == LirOpcode::Shl &&
                        (shiftRight->op == LirOpcode::Shr || shiftRight->op == LirOpcode::Lshr) &&
                        shiftLeft->srcs.size() == 2 && shiftRight->srcs.size() == 2 &&
                        shiftLeft->srcs[0] == shiftRight->srcs[0] && shiftLeft->type == instruction.type &&
                        shiftRight->type == instruction.type) {
                        const auto a = constant(shiftLeft->srcs[1]);
                        const auto b = constant(shiftRight->srcs[1]);
                        const auto width = instruction.type.kind == TypeRef::Kind::UInt32 ? 32U : 64U;
                        if (a && b && *a > 0 && *a < width && *b > 0 && *b < width && *a + *b == width) {
                            instruction.op = LirOpcode::RotateRight;
                            instruction.srcs = {shiftLeft->srcs[0]};
                            instruction.strArg = std::to_string(*b);
                            changed = true;
                        }
                    }
                }
            }
            if (word && instruction.srcs.size() == 2 &&
                (instruction.op == LirOpcode::Div || instruction.op == LirOpcode::Mod)) {
                const auto divisor = constant(instruction.srcs[1]);
                if (divisor && std::has_single_bit(*divisor)) {
                    LirInstr immediate;
                    immediate.op = LirOpcode::Const;
                    immediate.dst = next++;
                    immediate.type = instruction.type;
                    immediate.strArg =
                        std::to_string(instruction.op == LirOpcode::Div ? std::countr_zero(*divisor) : *divisor - 1);
                    instruction.srcs[1] = immediate.dst;
                    instruction.op = instruction.op == LirOpcode::Div ? LirOpcode::Lshr : LirOpcode::And;
                    output.push_back(std::move(immediate));
                    changed = true;
                }
            }
            output.push_back(std::move(instruction));
        }
        block.instrs = std::move(output);
    }
    return changed;
}
} // namespace

std::string_view LirScalarCombines::Name() const noexcept {
    return "lir-scalar-combines";
}

PassChange LirScalarCombines::Run(LirPackage &package, const PassContext &) {
    bool changed = false;
    for (auto &module : package.modules) {
        for (auto &function : module.funcs) {
            changed = Combine(function) || changed;
        }
    }
    return changed ? PassChange::Changed : PassChange::None;
}
} // namespace Rux::Optimization
