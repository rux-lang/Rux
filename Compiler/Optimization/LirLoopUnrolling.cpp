#include "Optimization/LirLoopUnrolling.h"

#include "Ir/Lir/NaturalLoops.h"
#include "Optimization/ConstantEvaluator.h"

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace Rux::Optimization {
namespace {
bool BodyOperation(const LirInstr &instruction) {
    if (instruction.isVolatile)
        return false;
    switch (instruction.op) {
    case LirOpcode::Const:
    case LirOpcode::Load:
    case LirOpcode::Store:
    case LirOpcode::FieldPtr:
    case LirOpcode::IndexPtr:
    case LirOpcode::Cast:
    case LirOpcode::Add:
    case LirOpcode::Sub:
    case LirOpcode::Mul:
    case LirOpcode::And:
    case LirOpcode::Or:
    case LirOpcode::Xor:
    case LirOpcode::Shl:
    case LirOpcode::Shr:
    case LirOpcode::Lshr:
    case LirOpcode::RotateRight:
    case LirOpcode::Neg:
    case LirOpcode::BitNot:
    // Duplicated in order, a trapping division traps at the same iteration it would have.
    case LirOpcode::Div:
    case LirOpcode::Mod:
    case LirOpcode::Not:
    case LirOpcode::Sqrt:
    case LirOpcode::GlobalAddr:
    case LirOpcode::CmpEq:
    case LirOpcode::CmpNe:
    case LirOpcode::CmpLt:
    case LirOpcode::CmpLe:
    case LirOpcode::CmpGt:
    case LirOpcode::CmpGe:
        return true;
    default:
        return false;
    }
}

/// How many times a count from one constant to below another runs, when that is at most sixteen.
std::optional<std::uint64_t> Trips(const LirInstr *first, const LirInstr *limit, const bool isSigned) {
    if (!first || !limit || first->op != LirOpcode::Const || limit->op != LirOpcode::Const)
        return std::nullopt;
    const auto start = ParseConstant(first->strArg, first->type);
    const auto end = ParseConstant(limit->strArg, limit->type);
    if (!start || !end)
        return std::nullopt;
    const auto from = start->RawBits();
    const auto to = end->RawBits();
    const bool runs = isSigned ? static_cast<std::int64_t>(from) < static_cast<std::int64_t>(to) : from < to;
    if (!runs)
        return std::nullopt;
    const auto count = to - from;
    return count <= 16 ? std::optional<std::uint64_t>(count) : std::nullopt;
}

bool Unroll(LirFunc &function) {
    if (function.isExtern || function.isAsm || function.blocks.size() < 4 || function.blocks.size() > 2048)
        return false;
    const LirControlFlow cfg(function);
    const LirDominance dominance(cfg);
    const LirNaturalLoops loops(cfg, dominance);
    if (!loops.complete)
        return false;
    std::unordered_map<LirReg, const LirInstr *> definitions;
    std::unordered_map<LirReg, std::uint32_t> owners;
    std::size_t instructionCount = 0;
    LirReg next = 0;
    for (const auto &parameter : function.params) {
        definitions.emplace(parameter.reg, nullptr);
        next = std::max(next, parameter.reg + 1);
    }
    for (std::uint32_t block = 0; block < function.blocks.size(); ++block) {
        instructionCount += function.blocks[block].instrs.size();
        if (instructionCount > 20000)
            return false;
        for (const auto &instruction : function.blocks[block].instrs) {
            if (instruction.dst == LirNoReg)
                continue;
            if (!definitions.emplace(instruction.dst, &instruction).second)
                return false;
            owners.emplace(instruction.dst, block);
            next = std::max(next, instruction.dst + 1);
        }
    }
    const auto definition = [&](const LirReg value) -> const LirInstr * {
        const auto found = definitions.find(value);
        return found == definitions.end() ? nullptr : found->second;
    };
    std::size_t workRemaining = 200000;
    for (const auto &[header, members] : loops.members) {
        const auto &head = function.blocks[header];
        // A loop this pass produced is not unrolled again.
        if (head.label.ends_with(".unroll2.header") || head.label.ends_with(".unroll4.header"))
            continue;
        if (members.size() != 2 || !head.term || head.term->kind != LirTermKind::Branch ||
            !members.contains(head.term->trueTarget) || members.contains(head.term->falseTarget) ||
            cfg.predecessors[header].size() != 2)
            continue;
        const auto body = head.term->trueTarget;
        const auto &source = function.blocks[body];
        if (body == header || !source.term || source.term->kind != LirTermKind::Jump ||
            source.term->trueTarget != header || cfg.predecessors[body] != std::vector<std::uint32_t>{header} ||
            source.instrs.size() < 4 || source.instrs.size() > 64 || !std::ranges::all_of(source.instrs, BodyOperation))
            continue;
        const auto preheader = cfg.predecessors[header][cfg.predecessors[header][0] == body ? 1 : 0];
        const auto &entry = function.blocks[preheader];
        if (members.contains(preheader) || !entry.term ||
            (entry.term->kind != LirTermKind::Jump && entry.term->kind != LirTermKind::Branch))
            continue;
        const auto *comparison = definition(head.term->cond);
        if (!comparison || comparison->op != LirOpcode::CmpLt || comparison->srcs.size() != 2 ||
            owners.at(comparison->dst) != header)
            continue;
        if (std::ranges::any_of(source.instrs, [&](const auto &i) {
                return std::ranges::find(i.srcs, comparison->dst) != i.srcs.end();
            }))
            continue;
        const auto induction = comparison->srcs[0];
        const auto limit = comparison->srcs[1];
        const auto *phi = definition(induction);
        const bool unsignedInduction =
            phi && (phi->type.kind == TypeRef::Kind::UInt || phi->type.kind == TypeRef::Kind::UInt64);
        const bool signedInduction =
            phi && (phi->type.kind == TypeRef::Kind::Int || phi->type.kind == TypeRef::Kind::Int64);
        if (!phi || phi->op != LirOpcode::Phi || owners.at(induction) != header ||
            (!unsignedInduction && !signedInduction) || (owners.contains(limit) && members.contains(owners.at(limit))))
            continue;
        std::vector<LirInstr> phis;
        bool valid = true;
        for (const auto &instruction : head.instrs) {
            if (instruction.isVolatile) {
                valid = false;
                break;
            }
            if (instruction.dst == comparison->dst)
                continue;
            if (instruction.op != LirOpcode::Phi || instruction.phiPreds.size() != 2 ||
                std::ranges::count_if(instruction.phiPreds, [&](const auto &edge) { return edge.second == body; }) !=
                    1 ||
                std::ranges::count_if(instruction.phiPreds,
                                      [&](const auto &edge) { return edge.second == preheader; }) != 1) {
                valid = false;
                break;
            }
            phis.push_back(instruction);
        }
        if (!valid || phis.empty() || phis.size() > 16)
            continue;
        // A signed count starts at a non-negative constant, so limit - initial cannot overflow when the loop runs.
        if (signedInduction) {
            const auto entryEdge =
                std::ranges::find_if(phi->phiPreds, [&](const auto &edge) { return edge.second != body; });
            const auto *first = entryEdge == phi->phiPreds.end() ? nullptr : definition(entryEdge->first);
            if (!first || first->op != LirOpcode::Const || first->type != phi->type || first->strArg.empty() ||
                first->strArg.starts_with('-'))
                continue;
        }
        const auto back = std::ranges::find_if(phi->phiPreds, [&](const auto &edge) { return edge.second == body; });
        if (back == phi->phiPreds.end())
            continue;
        const auto *increment = definition(back->first);
        if (!increment || increment->op != LirOpcode::Add || increment->type != phi->type ||
            increment->srcs.size() != 2 || increment->srcs[0] != induction || owners.at(increment->dst) != body)
            continue;
        const auto *one = definition(increment->srcs[1]);
        if (!one || one->op != LirOpcode::Const || one->type != phi->type || one->strArg != "1")
            continue;
        // Only header phis may export body-defined values. The original header remains the sole normal exit.
        const auto bodyValue = [&](const LirReg value) { return owners.contains(value) && owners.at(value) == body; };
        for (std::uint32_t block = 0; block < function.blocks.size() && valid; ++block) {
            if (block == body)
                continue;
            for (const auto &instruction : function.blocks[block].instrs) {
                if (workRemaining-- == 0)
                    return false;
                if (block == header && instruction.op == LirOpcode::Phi)
                    continue;
                if (std::ranges::any_of(instruction.srcs, bodyValue) ||
                    std::ranges::any_of(instruction.phiPreds, [&](const auto &edge) { return bodyValue(edge.first); }))
                    valid = false;
            }
            const auto &term = function.blocks[block].term;
            if (term && (bodyValue(term->cond) || (term->retVal && bodyValue(*term->retVal))))
                valid = false;
        }
        if (!valid)
            continue;
        const auto initial =
            std::ranges::find_if(phi->phiPreds, [&](const auto &edge) { return edge.second == preheader; })->first;
        // A count between two constants runs a known number of times; up to sixteen small iterations become one
        // straight block in the header's place, with the header's values after the last one flowing to the exit.
        if (const auto trips = Trips(definition(initial), definition(limit), signedInduction);
            trips && *trips * source.instrs.size() <= 256 &&
            std::ranges::none_of(function.blocks, [&](const LirBlock &block) {
                return std::ranges::any_of(block.instrs,
                                           [&](const LirInstr &instruction) {
                                               return std::ranges::find(instruction.srcs, comparison->dst) !=
                                                      instruction.srcs.end();
                                           }) ||
                       (&block != &head && block.term && block.term->cond == comparison->dst);
            })) {
            std::unordered_map<LirReg, LirReg> values;
            for (const auto &original : phis)
                values.emplace(original.dst, std::ranges::find_if(original.phiPreds, [&](const auto &edge) {
                                                 return edge.second == preheader;
                                             })->first);
            const auto mapped = [&](const LirReg value) { return values.contains(value) ? values.at(value) : value; };
            std::vector<LirInstr> flat;
            for (std::uint64_t iteration = 0; iteration < *trips; ++iteration) {
                for (const auto &original : source.instrs) {
                    auto copy = original;
                    for (auto &input : copy.srcs)
                        input = mapped(input);
                    if (copy.dst != LirNoReg) {
                        copy.dst = next++;
                        values[original.dst] = copy.dst;
                    }
                    flat.push_back(std::move(copy));
                }
                std::vector<std::pair<LirReg, LirReg>> updates;
                for (const auto &original : phis) {
                    const auto incoming =
                        std::ranges::find_if(original.phiPreds, [&](const auto &edge) { return edge.second == body; });
                    updates.emplace_back(original.dst, mapped(incoming->first));
                }
                for (const auto &[reg, value] : updates)
                    values[reg] = value;
            }
            // The header keeps its index and predecessors and becomes the straight block; the body is left
            // unreachable for cleanup.
            const auto exit = head.term->falseTarget;
            std::unordered_map<LirReg, LirReg> finals;
            for (const auto &original : phis)
                finals.emplace(original.dst, values.at(original.dst));
            auto &straight = function.blocks[header];
            straight.instrs = std::move(flat);
            straight.term.emplace();
            straight.term->kind = LirTermKind::Jump;
            straight.term->trueTarget = exit;
            auto &unused = function.blocks[body];
            unused.instrs.clear();
            unused.term.emplace();
            unused.term->kind = LirTermKind::Unreachable;
            const auto final = [&](LirReg &reg) {
                if (const auto found = finals.find(reg); found != finals.end())
                    reg = found->second;
            };
            for (std::uint32_t block = 0; block < function.blocks.size(); ++block) {
                if (block == header)
                    continue;
                for (auto &instruction : function.blocks[block].instrs) {
                    for (auto &input : instruction.srcs)
                        final(input);
                    for (auto &[input, predecessor] : instruction.phiPreds) {
                        static_cast<void>(predecessor);
                        final(input);
                    }
                }
                if (auto &term = function.blocks[block].term) {
                    final(term->cond);
                    if (term->retVal)
                        final(*term->retVal);
                }
            }
            return true;
        }

        // Pairing the two copies into two-lane vectors: every floating-point instruction of the body is a load, store
        // or arithmetic on float64 values the body computed or on scalars fixed before the loop, or, in a body with no
        // float64 values, every 64-bit integer value it loads and everything it computes from them is integer
        // arithmetic, bitwise logic or a shift by a constant; every address it reads or writes moves by one element
        // per iteration, and nothing else in the program reads a value it computes. The second copy's work then sits
        // in the upper lane of the first copy's.
        struct Access {
            std::size_t index;
            bool store;
            LirReg identity; // the pointer counter, or the invariant base an indexed address starts from
            bool counter;
            LirReg entry;            // the counter's value on entry, when it is one
            std::int64_t offset = 0; // elements past the count, for an indexed address
            LirReg address = LirNoReg;
        };

        const bool floating = std::ranges::any_of(
            source.instrs, [](const LirInstr &instruction) { return instruction.type.kind == TypeRef::Kind::Float64; });
        const auto element = [&](const TypeRef &type) {
            return floating ? type.kind == TypeRef::Kind::Float64
                            : type.kind == TypeRef::Kind::Int64 || type.kind == TypeRef::Kind::UInt64;
        };
        const auto pointerToDouble = [&](const TypeRef &type) {
            return type.kind == TypeRef::Kind::Pointer && type.inner.size() == 1 && element(type.inner[0]);
        };
        // A small integer known now, or once constants fold: the unroller runs before inlined arguments such as a
        // rotation's `64 - n` have been folded.
        const auto constantOf = [&](this const auto &self, const LirReg reg,
                                    const int depth = 0) -> std::optional<std::int64_t> {
            const auto *instruction = definition(reg);
            if (!instruction || !instruction->type.IsInteger() || depth > 4)
                return std::nullopt;
            if (instruction->op == LirOpcode::Cast && instruction->srcs.size() == 1)
                return self(instruction->srcs[0], depth + 1);
            if ((instruction->op == LirOpcode::Add || instruction->op == LirOpcode::Sub) &&
                instruction->srcs.size() == 2) {
                const auto left = self(instruction->srcs[0], depth + 1);
                const auto right = left ? self(instruction->srcs[1], depth + 1) : std::nullopt;
                if (!left || !right || *left < -4096 || *left > 4096 || *right < -4096 || *right > 4096)
                    return std::nullopt;
                return instruction->op == LirOpcode::Add ? *left + *right : *left - *right;
            }
            if (instruction->op != LirOpcode::Const)
                return std::nullopt;
            const auto value = ParseConstant(instruction->strArg, instruction->type);
            if (!value)
                return std::nullopt;
            return static_cast<std::int64_t>(value->RawBits());
        };
        // An index that is the count plus or minus a small constant.
        const auto offsetOf = [&](const LirReg reg) -> std::optional<std::int64_t> {
            if (reg == induction)
                return 0;
            const auto *instruction = definition(reg);
            if (!instruction || (instruction->op != LirOpcode::Add && instruction->op != LirOpcode::Sub) ||
                instruction->srcs.size() != 2 || instruction->srcs[0] != induction || instruction->type != phi->type)
                return std::nullopt;
            const auto amount = constantOf(instruction->srcs[1]);
            if (!amount || *amount < -1024 || *amount > 1024)
                return std::nullopt;
            return instruction->op == LirOpcode::Add ? *amount : -*amount;
        };
        const auto outsideLoop = [&](const LirReg reg) {
            return !owners.contains(reg) || (owners.at(reg) != header && owners.at(reg) != body);
        };
        const auto floatType = [&](const LirReg reg) -> std::optional<TypeRef> {
            if (const auto *instruction = definition(reg))
                return instruction->op == LirOpcode::Alloca ? TypeRef::MakePointer(instruction->type)
                                                            : instruction->type;
            for (const auto &parameter : function.params)
                if (parameter.reg == reg)
                    return parameter.type;
            return std::nullopt;
        };
        // A scalar both lanes share: fixed before the loop, or a constant the body defines, which the entry repeats.
        const auto shared = [&](const LirReg reg) {
            const auto type = floatType(reg);
            if (!type || !element(*type))
                return false;
            if (outsideLoop(reg))
                return true;
            const auto *instruction = definition(reg);
            return instruction && instruction->op == LirOpcode::Const && owners.at(reg) == body;
        };
        const auto addressOf = [&](const LirReg address) -> std::optional<Access> {
            const auto *instruction = definition(address);
            if (!instruction || !owners.contains(address) || !pointerToDouble(instruction->type))
                return std::nullopt;
            if (owners.at(address) == header && instruction->op == LirOpcode::Phi) {
                LirReg stepped = LirNoReg;
                LirReg entry = LirNoReg;
                for (const auto &[value, predecessor] : instruction->phiPreds)
                    (predecessor == body ? stepped : entry) = value;
                const auto *step = definition(stepped);
                const auto *one = step && step->op == LirOpcode::IndexPtr && step->srcs.size() == 2 &&
                                          step->srcs[0] == address && pointerToDouble(step->type)
                                    ? definition(step->srcs[1])
                                    : nullptr;
                if (!one || one->op != LirOpcode::Const || one->strArg != "1" || entry == LirNoReg)
                    return std::nullopt;
                return Access{0, false, address, true, entry};
            }
            if (owners.at(address) == body && instruction->op == LirOpcode::IndexPtr && instruction->srcs.size() == 2 &&
                outsideLoop(instruction->srcs[0]))
                if (const auto offset = offsetOf(instruction->srcs[1]))
                    return Access{0, false, instruction->srcs[0], false, LirNoReg, *offset, address};
            return std::nullopt;
        };
        std::vector<bool> paired(source.instrs.size(), false);
        std::unordered_set<LirReg> lanes;
        std::vector<Access> accesses;
        std::vector<LirReg> broadcasts;
        bool vectorize = true;
        const auto lane = [&](const LirReg reg) { return lanes.contains(reg); };
        for (std::size_t index = 0; index < source.instrs.size(); ++index) {
            const auto &instruction = source.instrs[index];
            if (!element(instruction.type) || instruction.isVolatile)
                continue;
            const auto &sources = instruction.srcs;
            if (floating) {
                switch (instruction.op) {
                case LirOpcode::Load:
                    vectorize = vectorize && instruction.strArg.empty() && sources.size() == 1;
                    break;
                case LirOpcode::Store:
                case LirOpcode::Add:
                case LirOpcode::Sub:
                case LirOpcode::Mul:
                case LirOpcode::Div:
                    vectorize = vectorize && sources.size() == 2;
                    break;
                default:
                    continue;
                }
            }
            else {
                // Integer lanes start at the loads and spread through what is computed from them.
                bool pair = false;
                switch (instruction.op) {
                case LirOpcode::Load:
                    pair = instruction.strArg.empty() && sources.size() == 1 && addressOf(sources[0]).has_value();
                    break;
                case LirOpcode::Store:
                    pair = sources.size() == 2 && lane(sources[0]);
                    break;
                case LirOpcode::Add:
                case LirOpcode::Sub:
                case LirOpcode::And:
                case LirOpcode::Or:
                case LirOpcode::Xor:
                    pair = sources.size() == 2 && (lane(sources[0]) || lane(sources[1]));
                    break;
                case LirOpcode::Shl:
                case LirOpcode::Shr: {
                    const auto count = sources.size() == 2 ? constantOf(sources[1]) : std::nullopt;
                    pair = lane(sources[0]) && count && *count >= 0 && *count < 64 &&
                           (instruction.op == LirOpcode::Shl || !instruction.type.IsSigned());
                    break;
                }
                case LirOpcode::RotateRight:
                    pair = sources.size() == 1 && lane(sources[0]);
                    break;
                default:
                    break;
                }
                if (!pair)
                    continue;
            }
            paired[index] = true;
            if (instruction.dst != LirNoReg)
                lanes.insert(instruction.dst);
        }
        const auto operand = [&](const LirReg reg) {
            if (lane(reg))
                return true;
            if (!shared(reg))
                return false;
            if (std::ranges::find(broadcasts, reg) == broadcasts.end())
                broadcasts.push_back(reg);
            return true;
        };
        for (std::size_t index = 0; index < source.instrs.size() && vectorize; ++index) {
            const auto &instruction = source.instrs[index];
            if (!paired[index]) {
                vectorize = std::ranges::none_of(instruction.srcs, lane);
                continue;
            }
            if (instruction.op == LirOpcode::Load || instruction.op == LirOpcode::Store) {
                const bool store = instruction.op == LirOpcode::Store;
                auto access = addressOf(instruction.srcs[store ? 1 : 0]);
                vectorize = access.has_value() && (!store || operand(instruction.srcs[0]));
                if (access) {
                    access->index = index;
                    access->store = store;
                    accesses.push_back(*access);
                }
                continue;
            }
            // A shift's count is the same scalar for both lanes; a rotation's is its literal.
            const bool counted = instruction.op == LirOpcode::Shl || instruction.op == LirOpcode::Shr ||
                                 instruction.op == LirOpcode::RotateRight;
            vectorize = operand(instruction.srcs[0]) && (counted || operand(instruction.srcs[1]));
        }
        // Two indexed accesses to one base, one element apart, would meet inside a pair when one of them writes:
        // the second iteration's would run beside the first's. Any other even distance leaves each pair independent.
        // An odd one does too, but then every vector load reads halves of two earlier vector stores, which the
        // processor cannot forward from its store buffer; such a loop (a message schedule reading back what it
        // wrote a few iterations earlier) runs faster as scalars.
        for (std::size_t later = 0; later < accesses.size() && vectorize; ++later)
            for (std::size_t earlier = 0; earlier < later; ++earlier) {
                const auto &x = accesses[earlier];
                const auto &y = accesses[later];
                if ((x.store || y.store) && !x.counter && !y.counter && x.identity == y.identity &&
                    (x.offset - y.offset) % 2 != 0)
                    vectorize = false;
            }
        vectorize = vectorize && std::ranges::any_of(accesses, [](const Access &access) { return access.store; }) &&
                    !lane(source.term->cond);
        for (std::uint32_t block = 0; block < function.blocks.size() && vectorize; ++block) {
            if (block == body)
                continue;
            for (const auto &instruction : function.blocks[block].instrs)
                vectorize =
                    vectorize && std::ranges::none_of(instruction.srcs, lane) &&
                    std::ranges::none_of(instruction.phiPreds, [&](const auto &edge) { return lane(edge.first); });
            const auto &term = function.blocks[block].term;
            vectorize = vectorize && (!term || (!lane(term->cond) && !(term->retVal && lane(*term->retVal))));
        }
        const auto preparation = static_cast<std::uint32_t>(function.blocks.size());
        const auto newHeader = preparation + 1;
        const auto newBody = newHeader + 1;
        LirBlock prepare, setup, doubled;
        // A paired loop runs two pairs per trip: four iterations as two vectors, which halves the loop's own
        // bookkeeping per element.
        const int copies = vectorize ? 4 : 2;
        const auto factor = std::to_string(copies);
        prepare.label = head.label + ".unroll" + factor + ".prepare";
        setup.label = head.label + ".unroll" + factor + ".header";
        doubled.label = source.label + ".unroll" + factor + ".body";
        std::unordered_map<LirReg, LirReg> values;
        for (const auto &original : phis) {
            auto copy = original;
            copy.dst = next++;
            values.emplace(original.dst, copy.dst);
            setup.instrs.push_back(std::move(copy));
        }
        const auto headerValues = values;
        const auto mapped = [&](const LirReg value) { return values.contains(value) ? values.at(value) : value; };
        for (int iteration = 0; iteration < copies; ++iteration) {
            for (const auto &original : source.instrs) {
                auto copy = original;
                for (auto &input : copy.srcs)
                    input = mapped(input);
                if (copy.dst != LirNoReg) {
                    copy.dst = next++;
                    values[original.dst] = copy.dst;
                }
                doubled.instrs.push_back(std::move(copy));
            }
            std::vector<std::pair<LirReg, LirReg>> updates;
            for (const auto &original : phis) {
                const auto incoming =
                    std::ranges::find_if(original.phiPreds, [&](const auto &edge) { return edge.second == body; });
                updates.emplace_back(original.dst, mapped(incoming->first));
            }
            for (const auto &[reg, value] : updates)
                values[reg] = value;
        }
        std::unordered_map<LirReg, LirReg> splats;
        if (vectorize) {
            for (const auto reg : broadcasts) {
                auto scalar = reg;
                if (!outsideLoop(reg)) {
                    auto copy = *definition(reg);
                    copy.dst = next++;
                    scalar = copy.dst;
                    prepare.instrs.push_back(std::move(copy));
                }
                LirInstr splat;
                splat.op = LirOpcode::VectorSplat;
                splat.type = *floatType(reg);
                splat.dst = next++;
                splat.srcs = {scalar};
                splats.emplace(reg, splat.dst);
                prepare.instrs.push_back(std::move(splat));
            }
            const auto size = source.instrs.size();
            std::vector<LirInstr> pairedBody;
            pairedBody.reserve(static_cast<std::size_t>(copies) * size);
            for (std::size_t pair = 0; pair < static_cast<std::size_t>(copies) / 2; ++pair) {
                const auto low = 2 * pair * size;
                std::unordered_map<LirReg, LirReg> vectors;
                const auto vectorOf = [&](const LirReg reg) {
                    return vectors.contains(reg) ? vectors.at(reg) : splats.at(reg);
                };
                for (std::size_t index = 0; index < size; ++index) {
                    if (!paired[index]) {
                        pairedBody.push_back(std::move(doubled.instrs[low + index]));
                        continue;
                    }
                    const auto &original = source.instrs[index];
                    const auto &first = doubled.instrs[low + index];
                    LirInstr vector;
                    vector.type = original.type;
                    switch (original.op) {
                    case LirOpcode::Load:
                        vector.op = LirOpcode::VectorLoad;
                        vector.srcs = {first.srcs[0]};
                        break;
                    case LirOpcode::Store:
                        vector.op = LirOpcode::VectorStore;
                        vector.srcs = {vectorOf(original.srcs[0]), first.srcs[1]};
                        break;
                    case LirOpcode::Shl:
                    case LirOpcode::Shr:
                        vector.op = original.op == LirOpcode::Shl ? LirOpcode::VectorShl : LirOpcode::VectorShr;
                        vector.srcs = {vectorOf(original.srcs[0]), first.srcs[1]};
                        break;
                    case LirOpcode::RotateRight:
                        vector.op = LirOpcode::VectorRotateRight;
                        vector.srcs = {vectorOf(original.srcs[0])};
                        vector.strArg = original.strArg;
                        break;
                    default:
                        vector.op = original.op == LirOpcode::Add ? LirOpcode::VectorAdd
                                  : original.op == LirOpcode::Sub ? LirOpcode::VectorSub
                                  : original.op == LirOpcode::Mul ? LirOpcode::VectorMul
                                  : original.op == LirOpcode::Div ? LirOpcode::VectorDiv
                                  : original.op == LirOpcode::And ? LirOpcode::VectorAnd
                                  : original.op == LirOpcode::Or  ? LirOpcode::VectorOr
                                                                  : LirOpcode::VectorXor;
                        vector.srcs = {vectorOf(original.srcs[0]), vectorOf(original.srcs[1])};
                        break;
                    }
                    if (original.dst != LirNoReg) {
                        vector.dst = next++;
                        vectors.emplace(original.dst, vector.dst);
                    }
                    pairedBody.push_back(std::move(vector));
                }
                for (std::size_t index = 0; index < size; ++index)
                    if (!paired[index])
                        pairedBody.push_back(std::move(doubled.instrs[low + size + index]));
            }
            doubled.instrs = std::move(pairedBody);
            doubled.label += ".paired";
        }
        for (std::size_t index = 0; index < phis.size(); ++index) {
            for (auto &edge : setup.instrs[index].phiPreds) {
                if (edge.second == body)
                    edge = {values.at(phis[index].dst), newBody};
                else
                    edge.second = preparation;
            }
        }
        const auto emit = [&](LirBlock &block, const LirOpcode op, const TypeRef &type, std::vector<LirReg> inputs,
                              const std::string &argument = "") {
            LirInstr instruction;
            instruction.op = op;
            instruction.type = type;
            instruction.dst = next++;
            instruction.srcs = std::move(inputs);
            instruction.strArg = argument;
            block.instrs.push_back(std::move(instruction));
            return block.instrs.back().dst;
        };
        // Subtraction may wrap for a descending/empty range; the entry guard then selects the unchanged loop.
        // Otherwise end is initial + floor((limit-initial)/k)*k, for the k copies made, and cannot exceed limit or
        // wrap.
        const auto remaining = emit(prepare, LirOpcode::Sub, phi->type, {limit, initial});
        const auto mask = emit(prepare, LirOpcode::Const, phi->type, {},
                               signedInduction ? "-" + factor
                                               : std::to_string(std::uint64_t{0} - static_cast<std::uint64_t>(copies)));
        const auto evenCount = emit(prepare, LirOpcode::And, phi->type, {remaining, mask});
        const auto end = emit(prepare, LirOpcode::Add, phi->type, {initial, evenCount});
        auto inRange = emit(prepare, LirOpcode::CmpLt, TypeRef::MakeBool(), {initial, limit});
        if (vectorize) {
            // Pairing moves each second-iteration access ahead of the first iteration's later ones. Where one of two
            // such accesses writes, they must not overlap: the second iteration's element of X, eight bytes past the
            // first's, must lie at least eight bytes from Y's in either direction. Both advance together, so their
            // distance on entry decides every iteration; a failing check runs the scalar loop instead.
            std::unordered_map<std::size_t, LirReg> entries;
            const auto entryOf = [&](const Access &access) {
                if (const auto found = entries.find(access.index); found != entries.end())
                    return found->second;
                auto start = initial;
                if (!access.counter && access.offset != 0) {
                    const auto amount = phi->type.IsSigned()
                                          ? std::to_string(access.offset)
                                          : std::to_string(static_cast<std::uint64_t>(access.offset));
                    start = emit(prepare, LirOpcode::Add, phi->type,
                                 {initial, emit(prepare, LirOpcode::Const, phi->type, {}, amount)});
                }
                const auto address = access.counter ? access.entry
                                                    : emit(prepare, LirOpcode::IndexPtr,
                                                           definition(access.address)->type, {access.identity, start});
                const auto pointerType = floatType(access.counter ? access.identity : address);
                const auto number = emit(prepare, LirOpcode::Cast, TypeRef::MakeUInt64(), {address},
                                         pointerType ? pointerType->ToString() : "*float64");
                entries.emplace(access.index, number);
                return number;
            };
            for (std::size_t later = 0; later < accesses.size(); ++later) {
                for (std::size_t earlier = 0; earlier < later; ++earlier) {
                    const auto &x = accesses[earlier];
                    const auto &y = accesses[later];
                    // One base shares its distances with every pair, checked above.
                    if ((!x.store && !y.store) || (x.identity == y.identity && x.counter == y.counter))
                        continue;
                    const auto shifted =
                        emit(prepare, LirOpcode::Add, TypeRef::MakeUInt64(),
                             {entryOf(x), emit(prepare, LirOpcode::Const, TypeRef::MakeUInt64(), {}, "15")});
                    const auto distance = emit(prepare, LirOpcode::Sub, TypeRef::MakeUInt64(), {shifted, entryOf(y)});
                    const auto apart =
                        emit(prepare, LirOpcode::CmpLe, TypeRef::MakeBool(),
                             {emit(prepare, LirOpcode::Const, TypeRef::MakeUInt64(), {}, "15"), distance});
                    inRange = emit(prepare, LirOpcode::And, TypeRef::MakeBool(), {inRange, apart});
                }
            }
        }
        prepare.term.emplace();
        prepare.term->kind = LirTermKind::Branch;
        prepare.term->cond = inRange;
        prepare.term->trueTarget = newHeader;
        prepare.term->falseTarget = header;
        const auto current = headerValues.at(induction);
        const auto guard = emit(setup, LirOpcode::CmpLt, TypeRef::MakeBool(), {current, end});
        setup.term.emplace();
        setup.term->kind = LirTermKind::Branch;
        setup.term->cond = guard;
        setup.term->trueTarget = newBody;
        setup.term->falseTarget = header;
        doubled.term.emplace();
        doubled.term->kind = LirTermKind::Jump;
        doubled.term->trueTarget = newHeader;
        auto &incoming = *function.blocks[preheader].term;
        if (incoming.trueTarget == header)
            incoming.trueTarget = preparation;
        if (incoming.kind == LirTermKind::Branch && incoming.falseTarget == header)
            incoming.falseTarget = preparation;
        for (auto &instruction : function.blocks[header].instrs) {
            if (instruction.op != LirOpcode::Phi)
                continue;
            for (auto &edge : instruction.phiPreds) {
                if (edge.second == preheader)
                    edge.second = preparation;
            }
            instruction.phiPreds.emplace_back(headerValues.at(instruction.dst), newHeader);
        }
        function.blocks.push_back(std::move(prepare));
        function.blocks.push_back(std::move(setup));
        function.blocks.push_back(std::move(doubled));
        return true;
    }
    return false;
}

/// Moves the constants a loop header defines to the one block entering it. An inlined loop keeps its constant limit in
/// its header, where the limit looks loop-defined and the count cannot be known.
bool HoistHeaderConstants(LirFunc &function) {
    if (function.isExtern || function.isAsm || function.blocks.size() < 3)
        return false;
    const LirControlFlow cfg(function);
    const LirDominance dominance(cfg);
    const LirNaturalLoops loops(cfg, dominance);
    if (!loops.complete)
        return false;
    bool changed = false;
    for (const auto &[header, members] : loops.members) {
        auto outside = LirDominance::Invalid;
        std::size_t entries = 0;
        for (const auto predecessor : cfg.predecessors[header])
            if (!members.contains(predecessor)) {
                outside = predecessor;
                ++entries;
            }
        if (entries != 1 || header == 0)
            continue;
        auto &instrs = function.blocks[header].instrs;
        const auto constant = [](const LirInstr &instruction) {
            return instruction.op == LirOpcode::Const && !instruction.isVolatile;
        };
        if (std::ranges::none_of(instrs, constant))
            continue;
        auto &entry = function.blocks[outside].instrs;
        std::vector<LirInstr> kept;
        for (auto &instruction : instrs)
            (constant(instruction) ? entry : kept).push_back(std::move(instruction));
        instrs = std::move(kept);
        changed = true;
    }
    return changed;
}
} // namespace

std::string_view LirLoopUnrolling::Name() const noexcept {
    return "lir-loop-unrolling";
}

PassChange LirLoopUnrolling::RunFunction(LirFunc &function, const std::size_t, const PassContext &) const {
    if (!MayHaveLoops(function))
        return PassChange::None;
    // The unrolled copy is skipped by its label and the original keeps a third header predecessor, which no round
    // accepts, so repeating reaches the function's other loops without unrolling one twice.
    bool changed = HoistHeaderConstants(function);
    for (int round = 0; round < 8 && Unroll(function); ++round)
        changed = true;
    return changed ? PassChange::Changed : PassChange::None;
}
} // namespace Rux::Optimization
