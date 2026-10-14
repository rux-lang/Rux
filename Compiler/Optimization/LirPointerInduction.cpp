#include "Optimization/LirPointerInduction.h"

#include "Ir/Lir/NaturalLoops.h"

#include <algorithm>
#include <unordered_map>

namespace Rux::Optimization {
namespace {
LirInstr Instruction(const LirOpcode opcode, const LirReg result, const TypeRef &type,
                     std::vector<LirReg> operands = {}) {
    LirInstr instruction;
    instruction.op = opcode;
    instruction.dst = result;
    instruction.type = type;
    instruction.srcs = std::move(operands);
    return instruction;
}

bool Reduce(LirFunc &function) {
    if (function.isExtern || function.isAsm || function.blocks.size() < 4 || function.blocks.size() > 2048)
        return false;
    const LirControlFlow cfg(function);
    const LirDominance dominance(cfg);
    const LirNaturalLoops loops(cfg, dominance);
    if (!loops.complete)
        return false;
    std::unordered_map<LirReg, const LirInstr *> definitions;
    std::unordered_map<LirReg, std::uint32_t> owners;
    LirReg next = 0;
    std::size_t instructionCount = 0;
    for (const auto &parameter : function.params) {
        if (!definitions.emplace(parameter.reg, nullptr).second)
            return false;
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
    if (next > LirNoReg - 32)
        return false;
    const auto definition = [&](const LirReg value) -> const LirInstr * {
        const auto found = definitions.find(value);
        return found == definitions.end() ? nullptr : found->second;
    };
    std::size_t budget = 200000;
    for (const auto &[header, members] : loops.members) {
        const auto &head = function.blocks[header];
        if (members.size() != 2 || !head.term || head.term->kind != LirTermKind::Branch ||
            !members.contains(head.term->trueTarget) || members.contains(head.term->falseTarget) ||
            cfg.predecessors[header].size() != 2)
            continue;
        const auto body = head.term->trueTarget;
        const auto &source = function.blocks[body];
        if (body == header || !source.term || source.term->kind != LirTermKind::Jump ||
            source.term->trueTarget != header ||
            (cfg.predecessors[body].size() != 1 || cfg.predecessors[body][0] != header))
            continue;
        const auto preheader = cfg.predecessors[header][cfg.predecessors[header][0] == body ? 1 : 0];
        if (members.contains(preheader))
            continue;
        const auto *comparison = definition(head.term->cond);
        if (!comparison || comparison->op != LirOpcode::CmpLt || comparison->srcs.size() != 2)
            continue;
        const auto induction = comparison->srcs[0];
        const auto *phi = definition(induction);
        if (!phi || phi->op != LirOpcode::Phi || owners.at(induction) != header || phi->phiPreds.size() != 2 ||
            (phi->type.kind != TypeRef::Kind::UInt && phi->type.kind != TypeRef::Kind::UInt64))
            continue;
        const auto back = std::ranges::find_if(phi->phiPreds, [&](const auto &edge) { return edge.second == body; });
        const auto initial =
            std::ranges::find_if(phi->phiPreds, [&](const auto &edge) { return edge.second == preheader; });
        if (back == phi->phiPreds.end() || initial == phi->phiPreds.end())
            continue;
        const auto *increment = definition(back->first);
        if (!increment || increment->isVolatile || increment->op != LirOpcode::Add || increment->type != phi->type ||
            increment->srcs.size() != 2 || increment->srcs[0] != induction || owners.at(increment->dst) != body)
            continue;
        const auto *one = definition(increment->srcs[1]);
        if (!one || one->isVolatile || one->op != LirOpcode::Const || one->type != phi->type || one->strArg != "1")
            continue;
        const auto invariant = [&](const LirReg value) {
            return definitions.contains(value) &&
                   (!owners.contains(value) ||
                    (!members.contains(owners.at(value)) && dominance.Dominates(owners.at(value), preheader)));
        };

        struct Candidate {
            LirInstr address;
            LirReg offset;
        };

        std::vector<Candidate> candidates;
        for (const auto &address : source.instrs) {
            if (candidates.size() == 4)
                break;
            if (address.dst == LirNoReg || address.isVolatile || !address.strArg.empty() ||
                address.op != LirOpcode::IndexPtr || address.srcs.size() != 2 ||
                address.type.kind != TypeRef::Kind::Pointer || !invariant(address.srcs[0]))
                continue;
            const auto *index = definition(address.srcs[1]);
            if (!index || index->isVolatile || index->op != LirOpcode::Add || index->type != phi->type ||
                index->srcs.size() != 2 || owners.at(index->dst) != body)
                continue;
            const auto offset = index->srcs[0] == induction ? index->srcs[1]
                              : index->srcs[1] == induction ? index->srcs[0]
                                                            : LirNoReg;
            if (!invariant(offset))
                continue;
            // The new header value describes this iteration, not the last completed iteration at an exit.
            // Keep any address with an outside use unchanged, including captures through another phi.
            bool escapes = false;
            for (std::uint32_t block = 0; block < function.blocks.size() && !escapes; ++block) {
                if (block == body)
                    continue;
                for (const auto &use : function.blocks[block].instrs) {
                    if (budget-- == 0)
                        return false;
                    if (std::ranges::contains(use.srcs, address.dst) ||
                        std::ranges::any_of(use.phiPreds, [&](const auto &edge) { return edge.first == address.dst; }))
                        escapes = true;
                }
                const auto &term = function.blocks[block].term;
                if (term && (term->cond == address.dst || (term->retVal && *term->retVal == address.dst)))
                    escapes = true;
            }
            if (!escapes)
                candidates.push_back({address, offset});
        }
        if (candidates.empty())
            continue;
        // Copy definition-backed data before appending instructions invalidates the lookup pointers.
        const auto indexType = phi->type;
        const auto initialValue = initial->first;
        const auto step = increment->srcs[1];
        for (const auto &candidate : candidates) {
            const auto &address = candidate.address;
            const auto firstIndex = next++;
            const auto firstPointer = next++;
            const auto nextPointer = next++;
            auto &entry = function.blocks[preheader].instrs;
            entry.push_back(Instruction(LirOpcode::Add, firstIndex, indexType, {initialValue, candidate.offset}));
            entry.push_back(
                Instruction(LirOpcode::IndexPtr, firstPointer, address.type, {address.srcs[0], firstIndex}));
            auto pointerPhi = Instruction(LirOpcode::Phi, address.dst, address.type);
            pointerPhi.phiPreds = {{firstPointer, preheader}, {nextPointer, body}};
            auto &headerInstructions = function.blocks[header].instrs;
            headerInstructions.insert(headerInstructions.begin(), std::move(pointerPhi));
            auto &bodyInstructions = function.blocks[body].instrs;
            std::erase_if(bodyInstructions, [&](const auto &instruction) { return instruction.dst == address.dst; });
            bodyInstructions.push_back(
                Instruction(LirOpcode::IndexPtr, nextPointer, address.type, {address.dst, step}));
        }
        return true;
    }
    return false;
}
} // namespace

std::string_view LirPointerInduction::Name() const noexcept {
    return "lir-pointer-induction";
}

PassChange LirPointerInduction::RunFunction(LirFunc &function, const std::size_t, const PassContext &) const {
    if (!MayHaveLoops(function))
        return PassChange::None;
    bool changed = false;
    // A reduced loop has no index-based address left to reduce, so repeating reaches the function's other loops.
    for (int round = 0; round < 8 && Reduce(function); ++round)
        changed = true;
    return changed ? PassChange::Changed : PassChange::None;
}
} // namespace Rux::Optimization
