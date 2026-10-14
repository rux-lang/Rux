#pragma once

#include "Ir/Lir/Lir.h"

#include <cstddef>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Rux::Optimization {
/// A value per register of one function. Registers are numbered from zero upwards in a function, so a table by number
/// holds them without a node per register; a number far past the function's size, which only hand-built IR uses, goes
/// to a map instead so a stray large number cannot make the table huge.
template <typename Value>
class RegisterTable {
public:
    /// @param expected roughly how many registers the function defines, usually its instruction count
    explicit RegisterTable(const std::size_t expected)
        : limit(expected * 4 + 64) {
    }

    [[nodiscard]] const Value *Find(const LirReg reg) const {
        if (reg < limit) {
            if (reg >= table.size() || !table[reg])
                return nullptr;
            return &*table[reg];
        }
        const auto found = sparse.find(reg);
        return found == sparse.end() ? nullptr : &found->second;
    }

    [[nodiscard]] bool Contains(const LirReg reg) const {
        return Find(reg) != nullptr;
    }

    /// Record `value` for `reg` unless it already has one.
    ///
    /// @return false when `reg` already had a value, which is kept
    bool Insert(const LirReg reg, Value value) {
        if (reg >= limit) {
            const bool inserted = sparse.emplace(reg, std::move(value)).second;
            count += inserted ? 1 : 0;
            return inserted;
        }
        auto &slot = Slot(reg);
        if (slot)
            return false;
        slot = std::move(value);
        ++count;
        return true;
    }

    /// Record `value` for `reg`, replacing any earlier one.
    void Assign(const LirReg reg, Value value) {
        if (reg >= limit) {
            count += sparse.insert_or_assign(reg, std::move(value)).second ? 1 : 0;
            return;
        }
        auto &slot = Slot(reg);
        count += slot ? 0 : 1;
        slot = std::move(value);
    }

    /// How many registers have a value.
    [[nodiscard]] std::size_t Size() const {
        return count;
    }

private:
    std::optional<Value> &Slot(const LirReg reg) {
        if (reg >= table.size())
            table.resize(static_cast<std::size_t>(reg) + 1);
        return table[reg];
    }

    std::size_t limit;
    std::size_t count = 0;
    std::vector<std::optional<Value>> table;
    std::unordered_map<LirReg, Value> sparse;
};

/// The number of instructions and parameters of a function: the size a register table of it is built for.
inline std::size_t RegisterCountHint(const LirFunc &function) {
    std::size_t count = function.params.size();
    for (const auto &block : function.blocks)
        count += block.instrs.size();
    return count;
}
} // namespace Rux::Optimization
