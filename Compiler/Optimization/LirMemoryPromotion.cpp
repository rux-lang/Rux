#include "Optimization/LirMemoryPromotion.h"

#include "Ir/Lir/ControlFlow.h"
#include "Ir/Lir/RegisterTable.h"
#include "Optimization/ReferenceTypes.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Rux::Optimization {
namespace {
using Copies = LirRegisterTable<LirReg>;

LirReg Resolve(const Copies &copies, LirReg reg) {
    for (std::size_t depth = 0; depth < copies.Size(); ++depth) {
        const LirReg *next = copies.Find(reg);
        if (next == nullptr || *next == reg) {
            break;
        }
        reg = *next;
    }
    return reg;
}

void RewriteUses(LirFunc &function, const Copies &copies) {
    for (auto &block : function.blocks) {
        for (auto &instruction : block.instrs) {
            for (auto &source : instruction.srcs) {
                source = Resolve(copies, source);
            }
            for (auto &[source, predecessor] : instruction.phiPreds) {
                static_cast<void>(predecessor);
                source = Resolve(copies, source);
            }
        }
        if (block.term) {
            block.term->cond = Resolve(copies, block.term->cond);
            if (block.term->retVal) {
                block.term->retVal = Resolve(copies, *block.term->retVal);
            }
        }
    }
}

bool Scalar(const TypeRef &type) {
    return type.IsInteger() || type.IsChar() || type.IsBool() || type.IsFloat() ||
           type->kind == TypeRef::Kind::Pointer || type->kind == TypeRef::Kind::Func ||
           (type->kind == TypeRef::Kind::Reference && type->inner.size() == 1 &&
            (type->inner[0].IsInteger() || type->inner[0].IsChar() || type->inner[0].IsBool() ||
             type->inner[0].IsFloat()));
}

/// The spelling of a slot type's zero, which a path that reads the slot before any store reads instead. Source
/// definite assignment rules out such a read; lowering still leaves paths that look like one, such as the fallthrough
/// after the last arm of an exhaustive match, and garbage is all a real one could have read.
std::optional<std::string> ZeroOf(const TypeRef &type) {
    if (type.IsFloat())
        return "0.0";
    if (type.IsInteger() || type.IsChar() || type.IsBool() || type->kind == TypeRef::Kind::Pointer)
        return "0";
    return std::nullopt;
}

bool Promote(LirFunc &function, const ConcreteReferenceTypes &references) {
    if (function.isExtern || function.isAsm || function.blocks.empty()) {
        return false;
    }
    const LirControlFlow cfg(function);
    // Cleanup runs before promotion. Unreachable predecessors must not introduce undefined phi inputs.
    if (std::ranges::find(cfg.reachable, false) != cfg.reachable.end()) {
        return false;
    }

    // The type each register is defined with, pointing at the function's own parameters and instructions, which do
    // not change until every slot is decided. An alloca defines the address of its slot's type, which is marked
    // rather than built.
    struct Defined {
        const TypeRef *type = nullptr;
        bool address = false;
    };

    const std::size_t registers = RegisterCountHint(function);
    LirRegisterTable<Defined> types(registers);
    // Ordered by register: promotion numbers its new registers slot by slot in this order.
    std::map<LirReg, const TypeRef *> slots;
    LirReg next = 0;
    for (const auto &parameter : function.params) {
        types.Insert(parameter.reg, {&parameter.type, false});
        next = std::max(next, parameter.reg + 1);
    }
    for (const auto &block : function.blocks) {
        for (const auto &instruction : block.instrs) {
            if (instruction.dst == LirNoReg) {
                continue;
            }
            const auto &type = instruction.type;
            if (!types.Insert(instruction.dst, {&type, instruction.op == LirOpcode::Alloca})) {
                return false; // Legacy non-SSA fixtures keep their memory behavior.
            }
            next = std::max(next, instruction.dst + 1);
            if (instruction.op == LirOpcode::Alloca && instruction.strArg.empty() &&
                (Scalar(type) || references.Contains(type))) {
                slots.emplace(instruction.dst, &type);
            }
        }
    }
    const auto storedType = [&](const LirReg value, const TypeRef &slot) {
        const Defined *defined = types.Find(value);
        if (defined == nullptr)
            return false;
        return defined->address ? slot == TypeRef::MakePointer(*defined->type) : *defined->type == slot;
    };
    for (const auto &block : function.blocks) {
        for (const auto &instruction : block.instrs) {
            for (std::size_t index = 0; index < instruction.srcs.size(); ++index) {
                const LirReg source = instruction.srcs[index];
                const auto slot = slots.find(source);
                if (slot == slots.end()) {
                    continue;
                }
                const bool load = instruction.op == LirOpcode::Load && instruction.srcs.size() == 1 && index == 0 &&
                                  instruction.strArg.empty();
                const bool store = instruction.op == LirOpcode::Store && instruction.srcs.size() == 2 && index == 1 &&
                                   storedType(instruction.srcs[0], *slot->second);
                if ((!load && !store) || instruction.isVolatile || instruction.type != *slot->second) {
                    slots.erase(slot);
                }
            }
            for (const auto &[source, predecessor] : instruction.phiPreds) {
                static_cast<void>(predecessor);
                slots.erase(source);
            }
        }
        if (block.term) {
            slots.erase(block.term->cond);
            if (block.term->retVal) {
                slots.erase(*block.term->retVal);
            }
        }
    }
    // Bound construction for generated functions; an unpromoted slot retains its original semantics.
    const std::size_t count = function.blocks.size();
    if (slots.empty() || slots.size() > 2000000 / count) {
        return false;
    }

    // Every slot is decided against the function as it stands, then all promoted slots are rewritten in one walk. Each
    // slot reads and writes only its own address, so deciding one never depends on another having been rewritten.
    enum class Access : std::uint8_t {
        Alloca,
        Store,
        Load
    };

    struct Event {
        std::uint32_t block;
        Access access;
    };

    struct Slot {
        LirReg reg;
        TypeRef type;
        std::vector<Event> events;
        bool promoted = false;
        LirReg zero = LirNoReg;
        std::vector<LirReg> incoming;
        // The slot's value at the end of each block; during the rewrite, its value so far in the block.
        std::vector<LirReg> outgoing;
    };

    std::vector<Slot> promoted;
    promoted.reserve(slots.size());
    LirRegisterTable<std::uint32_t> slotIndex(registers);
    for (const auto &[reg, type] : slots) {
        slotIndex.Insert(reg, static_cast<std::uint32_t>(promoted.size()));
        auto &slot = promoted.emplace_back();
        slot.reg = reg;
        slot.type = *type;
    }
    // The slot an instruction accesses and how, if it accesses one.
    const auto accessed = [&](const LirInstr &instruction) -> std::optional<std::pair<std::uint32_t, Access>> {
        LirReg reg = LirNoReg;
        Access access{};
        if (instruction.op == LirOpcode::Alloca) {
            reg = instruction.dst;
            access = Access::Alloca;
        }
        else if (instruction.op == LirOpcode::Store && instruction.srcs.size() == 2) {
            reg = instruction.srcs[1];
            access = Access::Store;
        }
        else if (instruction.op == LirOpcode::Load && instruction.srcs.size() == 1) {
            reg = instruction.srcs[0];
            access = Access::Load;
        }
        if (reg == LirNoReg)
            return std::nullopt;
        const std::uint32_t *index = slotIndex.Find(reg);
        if (index == nullptr)
            return std::nullopt;
        return std::pair{*index, access};
    };
    for (std::uint32_t block = 0; block < count; ++block)
        for (const auto &instruction : function.blocks[block].instrs)
            if (const auto access = accessed(instruction))
                promoted[access->first].events.push_back({block, access->second});

    bool changed = false;
    for (auto &slot : promoted) {
        std::vector<bool> liveIn(count), initializedIn(count, true), initializedOut(count, true);
        std::vector<int> lastWrite(count, -1);
        std::vector<bool> readsBeforeWrite(count, false);
        for (const auto &event : slot.events) {
            if (event.access == Access::Alloca) {
                lastWrite[event.block] = 0;
            }
            else if (event.access == Access::Store) {
                lastWrite[event.block] = 1;
            }
            else if (lastWrite[event.block] == -1) {
                readsBeforeWrite[event.block] = true;
            }
        }
        bool again = true;
        while (again) {
            again = false;
            for (std::size_t block = count; block-- > 0;) {
                const bool out =
                    std::ranges::any_of(cfg.successors[block], [&](const auto successor) { return liveIn[successor]; });
                const bool in = readsBeforeWrite[block] || (lastWrite[block] == -1 && out);
                again = again || in != liveIn[block];
                liveIn[block] = in;
            }
        }
        again = true;
        while (again) {
            again = false;
            for (std::size_t block = 0; block < count; ++block) {
                const bool in = block != 0 && !cfg.predecessors[block].empty() &&
                                std::ranges::all_of(cfg.predecessors[block], [&](const auto predecessor) {
                                    return initializedOut[predecessor];
                                });
                const bool out = lastWrite[block] == -1 ? in : lastWrite[block] == 1;
                again = again || out != initializedOut[block];
                initializedIn[block] = in;
                initializedOut[block] = out;
            }
        }
        bool valid = true;
        std::uint32_t current = ~std::uint32_t{};
        bool initialized = false;
        for (const auto &event : slot.events) {
            if (event.block != current) {
                current = event.block;
                initialized = initializedIn[current];
            }
            if (event.access == Access::Alloca) {
                initialized = false;
            }
            else if (event.access == Access::Store) {
                initialized = true;
            }
            else if (!initialized) {
                valid = false;
            }
        }
        if (!valid) {
            if (!ZeroOf(slot.type) || liveIn[0])
                continue;
            slot.zero = next++;
        }
        slot.incoming.assign(count, LirNoReg);
        for (std::size_t block = 0; block < count; ++block) {
            if (liveIn[block]) {
                slot.incoming[block] = next++;
            }
        }
        slot.outgoing = slot.incoming;
        slot.promoted = true;
        changed = true;
    }
    if (!changed)
        return false;

    Copies copies(registers);
    for (std::uint32_t block = 0; block < count; ++block) {
        std::erase_if(function.blocks[block].instrs, [&](const LirInstr &instruction) {
            const auto access = accessed(instruction);
            if (!access || !promoted[access->first].promoted)
                return false;
            auto &slot = promoted[access->first];
            auto &value = slot.outgoing[block];
            if (access->second == Access::Alloca)
                value = slot.zero;
            else if (access->second == Access::Store)
                value = instruction.srcs[0];
            else
                copies.Assign(instruction.dst, value);
            return true;
        });
    }
    // Each slot's zero and phis go in front of the earlier slots', as if every slot had inserted its own at the start
    // of the block in turn.
    std::vector<std::vector<LirInstr>> prefixes(count);
    {
        std::vector<std::size_t> sizes(count);
        for (const auto &slot : promoted) {
            if (!slot.promoted)
                continue;
            if (slot.zero != LirNoReg)
                ++sizes[0];
            for (std::size_t block = 0; block < count; ++block)
                if (slot.incoming[block] != LirNoReg)
                    ++sizes[block];
        }
        for (std::size_t block = 0; block < count; ++block)
            prefixes[block].reserve(sizes[block]);
    }
    for (const auto &slot : promoted) {
        if (!slot.promoted)
            continue;
        if (slot.zero != LirNoReg) {
            LirInstr constant;
            constant.op = LirOpcode::Const;
            constant.dst = slot.zero;
            constant.type = slot.type;
            constant.strArg = *ZeroOf(slot.type);
            prefixes[0].push_back(std::move(constant));
        }
        for (std::size_t block = 0; block < count; ++block) {
            if (slot.incoming[block] == LirNoReg) {
                continue;
            }
            LirInstr phi;
            phi.dst = slot.incoming[block];
            phi.type = slot.type;
            phi.op = LirOpcode::Phi;
            for (const auto predecessor : cfg.predecessors[block]) {
                phi.phiPreds.emplace_back(
                    slot.outgoing[predecessor] == LirNoReg ? slot.zero : slot.outgoing[predecessor], predecessor);
            }
            prefixes[block].push_back(std::move(phi));
        }
    }
    for (std::size_t block = 0; block < count; ++block) {
        if (prefixes[block].empty())
            continue;
        auto &instructions = function.blocks[block].instrs;
        instructions.insert(instructions.begin(), std::make_move_iterator(prefixes[block].rbegin()),
                            std::make_move_iterator(prefixes[block].rend()));
    }
    RewriteUses(function, copies);
    // Collapse forwarding phis locally, including self edges. Real loop recurrences keep their merge.
    Copies forwarded(registers);
    bool again = true;
    while (again) {
        again = false;
        for (auto &block : function.blocks) {
            std::erase_if(block.instrs, [&](const LirInstr &instruction) {
                if (instruction.op != LirOpcode::Phi) {
                    return false;
                }
                LirReg value = LirNoReg;
                for (const auto &[source, predecessor] : instruction.phiPreds) {
                    static_cast<void>(predecessor);
                    const auto resolved = Resolve(forwarded, source);
                    if (resolved == instruction.dst) {
                        continue;
                    }
                    if (value != LirNoReg && resolved != value) {
                        return false;
                    }
                    value = resolved;
                }
                if (value == LirNoReg) {
                    return false;
                }
                forwarded.Assign(instruction.dst, value);
                again = true;
                return true;
            });
        }
    }
    RewriteUses(function, forwarded);
    return true;
}
} // namespace

std::string_view LirMemoryPromotion::Name() const noexcept {
    return "lir-memory-promotion";
}

void LirMemoryPromotion::Prepare(const LirPackage &package) {
    references.emplace(package);
}

PassChange LirMemoryPromotion::RunFunction(LirFunc &function, const std::size_t, const PassContext &) const {
    return Promote(function, *references) ? PassChange::Changed : PassChange::None;
}
} // namespace Rux::Optimization
