#pragma once

#include "CodeGen/IntegerLiteral.h"
#include "Ir/Lir/Lir.h"

#include <unordered_map>
#include <unordered_set>

namespace Rux {
/// Constants with a unique definition. Values with legacy multiple definitions remain register operands.
inline std::unordered_map<LirReg, std::uint64_t> ScalarImmediates(const LirFunc &function) {
    std::unordered_map<LirReg, std::uint64_t> result;
    std::unordered_set<LirReg> seen, repeated;
    for (const auto &parameter : function.params) {
        seen.insert(parameter.reg);
    }
    for (const auto &block : function.blocks) {
        for (const auto &instruction : block.instrs) {
            if (instruction.dst == LirNoReg) {
                continue;
            }
            if (!seen.insert(instruction.dst).second) {
                repeated.insert(instruction.dst);
            }
            if (instruction.op == LirOpcode::Const && instruction.type.IsInteger()) {
                if (const auto value = ParseIntegerLiteralBits(instruction.strArg)) {
                    result[instruction.dst] = *value;
                }
            }
        }
    }
    for (const auto reg : repeated) {
        result.erase(reg);
    }
    return result;
}
} // namespace Rux
