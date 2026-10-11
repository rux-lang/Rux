#include "Optimization/LirLoopUnrolling.h"

#include "Ir/Lir/NaturalLoops.h"
#include "Optimization/ConstantEvaluator.h"

#include <algorithm>
#include <optional>
#include <unordered_map>

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
        if (head.label.ends_with(".unroll2.header"))
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
        const auto preparation = static_cast<std::uint32_t>(function.blocks.size());
        const auto newHeader = preparation + 1;
        const auto newBody = newHeader + 1;
        LirBlock prepare, setup, doubled;
        prepare.label = head.label + ".unroll2.prepare";
        setup.label = head.label + ".unroll2.header";
        doubled.label = source.label + ".unroll2.body";
        std::unordered_map<LirReg, LirReg> values;
        for (const auto &original : phis) {
            auto copy = original;
            copy.dst = next++;
            values.emplace(original.dst, copy.dst);
            setup.instrs.push_back(std::move(copy));
        }
        const auto headerValues = values;
        const auto mapped = [&](const LirReg value) { return values.contains(value) ? values.at(value) : value; };
        for (int iteration = 0; iteration < 2; ++iteration) {
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
        // Otherwise end is initial + floor((limit-initial)/2)*2 and cannot exceed limit or wrap.
        const auto remaining = emit(prepare, LirOpcode::Sub, phi->type, {limit, initial});
        const auto mask =
            emit(prepare, LirOpcode::Const, phi->type, {}, signedInduction ? "-2" : "18446744073709551614");
        const auto evenCount = emit(prepare, LirOpcode::And, phi->type, {remaining, mask});
        const auto end = emit(prepare, LirOpcode::Add, phi->type, {initial, evenCount});
        const auto inRange = emit(prepare, LirOpcode::CmpLt, TypeRef::MakeBool(), {initial, limit});
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
} // namespace

std::string_view LirLoopUnrolling::Name() const noexcept {
    return "lir-loop-unrolling";
}

PassChange LirLoopUnrolling::Run(LirPackage &package, const PassContext &context) {
    if (context.iteration != 0)
        return PassChange::None;
    bool changed = false;
    // The unrolled copy is skipped by its label and the original keeps a third header predecessor, which no round
    // accepts, so repeating reaches the function's other loops without unrolling one twice.
    for (auto &module : package.modules)
        for (auto &function : module.funcs)
            for (int round = 0; round < 8 && Unroll(function); ++round)
                changed = true;
    return changed ? PassChange::Changed : PassChange::None;
}
} // namespace Rux::Optimization
