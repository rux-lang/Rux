#include "Optimization/LirMemoryPromotion.h"

#include "Ir/Lir/ControlFlow.h"
#include "Optimization/ReferenceTypes.h"

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Rux::Optimization {
namespace {
using Types = std::unordered_map<LirReg, TypeRef>;
using Copies = std::unordered_map<LirReg, LirReg>;

LirReg Resolve(const Copies &copies, LirReg reg) {
    for (std::size_t depth = 0; depth < copies.size(); ++depth) {
        const auto next = copies.find(reg);
        if (next == copies.end() || next->second == reg) {
            break;
        }
        reg = next->second;
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
           type.kind == TypeRef::Kind::Pointer || type.kind == TypeRef::Kind::Func ||
           (type.kind == TypeRef::Kind::Reference && type.inner.size() == 1 &&
            (type.inner[0].IsInteger() || type.inner[0].IsChar() || type.inner[0].IsBool() || type.inner[0].IsFloat()));
}

/// The spelling of a slot type's zero, which a path that reads the slot before any store reads instead. Source
/// definite assignment rules out such a read; lowering still leaves paths that look like one, such as the fallthrough
/// after the last arm of an exhaustive match, and garbage is all a real one could have read.
std::optional<std::string> ZeroOf(const TypeRef &type) {
    if (type.IsFloat())
        return "0.0";
    if (type.IsInteger() || type.IsChar() || type.IsBool() || type.kind == TypeRef::Kind::Pointer)
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
    Types types;
    std::map<LirReg, TypeRef> slots;
    LirReg next = 0;
    for (const auto &parameter : function.params) {
        types.emplace(parameter.reg, parameter.type);
        next = std::max(next, parameter.reg + 1);
    }
    for (const auto &block : function.blocks) {
        for (const auto &instruction : block.instrs) {
            if (instruction.dst == LirNoReg) {
                continue;
            }
            const auto &type = instruction.type;
            if (!types.emplace(instruction.dst, instruction.op == LirOpcode::Alloca ? TypeRef::MakePointer(type) : type)
                     .second) {
                return false; // Legacy non-SSA fixtures keep their memory behavior.
            }
            next = std::max(next, instruction.dst + 1);
            if (instruction.op == LirOpcode::Alloca && instruction.strArg.empty() &&
                (Scalar(type) || references.Contains(type))) {
                slots.emplace(instruction.dst, type);
            }
        }
    }
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
                                   types.contains(instruction.srcs[0]) && types.at(instruction.srcs[0]) == slot->second;
                if ((!load && !store) || instruction.isVolatile || instruction.type != slot->second) {
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
    if (slots.size() > 2000000 / count) {
        return false;
    }
    bool changed = false;
    for (const auto &[slot, type] : slots) {
        std::vector<bool> liveIn(count), initializedIn(count, true), initializedOut(count, true);
        std::vector<int> lastWrite(count, -1);
        std::vector<bool> readsBeforeWrite(count, false);
        for (std::size_t block = 0; block < count; ++block) {
            for (const auto &instruction : function.blocks[block].instrs) {
                if (instruction.op == LirOpcode::Alloca && instruction.dst == slot) {
                    lastWrite[block] = 0;
                }
                else if (instruction.op == LirOpcode::Store && instruction.srcs.size() == 2 &&
                         instruction.srcs[1] == slot) {
                    lastWrite[block] = 1;
                }
                else if (instruction.op == LirOpcode::Load && instruction.srcs.size() == 1 &&
                         instruction.srcs[0] == slot && lastWrite[block] == -1) {
                    readsBeforeWrite[block] = true;
                }
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
        for (std::size_t block = 0; block < count; ++block) {
            bool initialized = initializedIn[block];
            for (const auto &instruction : function.blocks[block].instrs) {
                if (instruction.op == LirOpcode::Alloca && instruction.dst == slot) {
                    initialized = false;
                }
                else if (instruction.op == LirOpcode::Store && instruction.srcs.size() == 2 &&
                         instruction.srcs[1] == slot) {
                    initialized = true;
                }
                else if (instruction.op == LirOpcode::Load && instruction.srcs.size() == 1 &&
                         instruction.srcs[0] == slot && !initialized) {
                    valid = false;
                }
            }
        }
        LirReg zero = LirNoReg;
        if (!valid) {
            const auto spelling = ZeroOf(type);
            if (!spelling || liveIn[0])
                continue;
            LirInstr constant;
            constant.op = LirOpcode::Const;
            constant.dst = next++;
            constant.type = type;
            constant.strArg = *spelling;
            zero = constant.dst;
            types[zero] = type;
            function.blocks[0].instrs.insert(function.blocks[0].instrs.begin(), std::move(constant));
        }
        std::vector<LirReg> incoming(count, LirNoReg), outgoing(count, LirNoReg);
        for (std::size_t block = 0; block < count; ++block) {
            if (liveIn[block]) {
                incoming[block] = next++;
                types[incoming[block]] = type;
            }
        }
        Copies copies;
        for (std::size_t block = 0; block < count; ++block) {
            auto value = incoming[block];
            std::erase_if(function.blocks[block].instrs, [&](const LirInstr &instruction) {
                if (instruction.op == LirOpcode::Alloca && instruction.dst == slot) {
                    value = zero;
                    return true;
                }
                if (instruction.op == LirOpcode::Store && instruction.srcs.size() == 2 && instruction.srcs[1] == slot) {
                    value = instruction.srcs[0];
                    return true;
                }
                if (instruction.op == LirOpcode::Load && instruction.srcs.size() == 1 && instruction.srcs[0] == slot) {
                    copies[instruction.dst] = value;
                    return true;
                }
                return false;
            });
            outgoing[block] = value;
        }
        for (std::size_t block = 0; block < count; ++block) {
            if (incoming[block] == LirNoReg) {
                continue;
            }
            LirInstr phi;
            phi.dst = incoming[block];
            phi.type = type;
            phi.op = LirOpcode::Phi;
            for (const auto predecessor : cfg.predecessors[block]) {
                phi.phiPreds.emplace_back(outgoing[predecessor] == LirNoReg ? zero : outgoing[predecessor],
                                          predecessor);
            }
            function.blocks[block].instrs.insert(function.blocks[block].instrs.begin(), std::move(phi));
        }
        RewriteUses(function, copies);
        changed = true;
    }
    // Collapse forwarding phis locally, including self edges. Real loop recurrences keep their merge.
    Copies copies;
    bool again = changed;
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
                    const auto resolved = Resolve(copies, source);
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
                copies[instruction.dst] = value;
                again = true;
                return true;
            });
        }
    }
    RewriteUses(function, copies);
    return changed;
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
