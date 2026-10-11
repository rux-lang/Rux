#pragma once

// Addresses of module constants. A constant lives in read-only data, so nothing the program does can change what a
// load through such an address reads: the load is a pure function of its address, whatever stores, calls or ordering
// operations run around it.

#include "Ir/Lir/Lir.h"

#include <string>
#include <unordered_set>

namespace Rux::Optimization {
[[nodiscard]] inline std::unordered_set<std::string> ConstantNames(const LirPackage &package) {
    std::unordered_set<std::string> names;
    for (const auto &module : package.modules)
        for (const auto &constant : module.consts)
            names.insert(constant.name);
    return names;
}

/// Registers of `function` that address part of a module constant: the constant itself, or a field of one.
[[nodiscard]] inline std::unordered_set<LirReg> ConstantAddresses(const LirFunc &function,
                                                                  const std::unordered_set<std::string> &names) {
    std::unordered_set<LirReg> addresses;
    if (names.empty())
        return addresses;
    for (bool changed = true; changed;) {
        changed = false;
        for (const auto &block : function.blocks) {
            for (const auto &instruction : block.instrs) {
                if (instruction.dst == LirNoReg || instruction.isVolatile || addresses.contains(instruction.dst))
                    continue;
                const bool global = instruction.op == LirOpcode::GlobalAddr && names.contains(instruction.strArg);
                const bool field = instruction.op == LirOpcode::FieldPtr && instruction.srcs.size() == 1 &&
                                   addresses.contains(instruction.srcs[0]);
                if (global || field) {
                    addresses.insert(instruction.dst);
                    changed = true;
                }
            }
        }
    }
    return addresses;
}
} // namespace Rux::Optimization
