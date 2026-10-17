#pragma once

#include "Ir/Lir/Lir.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace Rux {
/// A value per register of one function. Registers are numbered from zero upwards in a function, so a table by number
/// holds them without a node per register; a number far past the function's size, which only hand-built IR uses, goes
/// to a short list instead so a stray large number cannot make the table huge. Nothing is allocated until a register
/// gets a value: an empty standard hash map allocates, and passes build these tables for every function.
template <typename Value>
class LirRegisterTable {
public:
    /// @param registers roughly how many registers the function defines, usually its instruction count
    explicit LirRegisterTable(const std::size_t registers)
        : expected(registers)
        , limit(registers * 4 + 64) {
    }

    [[nodiscard]] const Value *Find(const LirReg reg) const {
        if (reg < limit) {
            if (reg >= table.size() || !table[reg])
                return nullptr;
            return &*table[reg];
        }
        for (const auto &[key, value] : sparse)
            if (key == reg)
                return &value;
        return nullptr;
    }

    [[nodiscard]] Value *Find(const LirReg reg) {
        return const_cast<Value *>(std::as_const(*this).Find(reg));
    }

    [[nodiscard]] bool Contains(const LirReg reg) const {
        return Find(reg) != nullptr;
    }

    /// The value of a register that has one.
    [[nodiscard]] const Value &At(const LirReg reg) const {
        const Value *value = Find(reg);
        assert(value != nullptr && "register has no value in the table");
        return *value;
    }

    /// Record `value` for `reg` unless it already has one.
    ///
    /// @return false when `reg` already had a value, which is kept
    bool Insert(const LirReg reg, Value value) {
        if (reg >= limit) {
            if (Find(reg) != nullptr)
                return false;
            sparse.emplace_back(reg, std::move(value));
            ++count;
            return true;
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
            if (Value *existing = Find(reg)) {
                *existing = std::move(value);
                return;
            }
            sparse.emplace_back(reg, std::move(value));
            ++count;
            return;
        }
        auto &slot = Slot(reg);
        count += slot ? 0 : 1;
        slot = std::move(value);
    }

    /// The value of `reg`, given a default one first if it has none.
    Value &Ref(const LirReg reg) {
        if (Value *existing = Find(reg))
            return *existing;
        Insert(reg, Value{});
        return *Find(reg);
    }

    /// Forget the value of `reg`, if it has one.
    void Erase(const LirReg reg) {
        if (reg >= limit) {
            for (auto entry = sparse.begin(); entry != sparse.end(); ++entry) {
                if (entry->first == reg) {
                    sparse.erase(entry);
                    --count;
                    return;
                }
            }
            return;
        }
        if (reg < table.size() && table[reg]) {
            table[reg].reset();
            --count;
        }
    }

    /// Calls `visit(reg, value)` for every register with a value: those numbered in the table in increasing order,
    /// then the rest in the order they were recorded.
    template <typename Visit>
    void ForEach(Visit visit) const {
        for (std::size_t reg = 0; reg < table.size(); ++reg)
            if (table[reg])
                visit(static_cast<LirReg>(reg), *table[reg]);
        for (const auto &[reg, value] : sparse)
            visit(reg, value);
    }

    /// How many registers have a value.
    [[nodiscard]] std::size_t Size() const {
        return count;
    }

private:
    std::optional<Value> &Slot(const LirReg reg) {
        if (reg >= table.size()) {
            // Registers usually arrive in increasing order, so the first one makes room for the expected count rather
            // than letting the table grow (and move every value) step by step.
            if (table.capacity() == 0)
                table.reserve(std::max(expected + 1, static_cast<std::size_t>(reg) + 1));
            table.resize(static_cast<std::size_t>(reg) + 1);
        }
        return table[reg];
    }

    std::size_t expected;
    std::size_t limit;
    std::size_t count = 0;
    std::vector<std::optional<Value>> table;
    std::vector<std::pair<LirReg, Value>> sparse;
};

/// The number of instructions and parameters of a function: the size a register table of it is built for.
inline std::size_t RegisterCountHint(const LirFunc &function) {
    std::size_t count = function.params.size();
    for (const auto &block : function.blocks)
        count += block.instrs.size();
    return count;
}
} // namespace Rux
