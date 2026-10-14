#include "Optimization/LirIfConversion.h"

#include "Ir/Lir/NaturalLoops.h"
#include "Optimization/RegisterTable.h"

#include <unordered_map>
#include <unordered_set>

namespace Rux::Optimization {
namespace {
constexpr std::size_t SideLimit = 6;
constexpr std::size_t MergeLimit = 4;

/// An integer no wider than a machine word: the arithmetic below is single instructions on it.
bool Word(const TypeRef &type) {
    return type.IsInteger() && type.kind != TypeRef::Kind::Int128 && type.kind != TypeRef::Kind::UInt128;
}

/// Safe to run whether or not the branch would have reached it: no effect, no trap, no wide arithmetic.
bool Speculable(const LirInstr &instruction) {
    if (instruction.isVolatile)
        return false;
    switch (instruction.op) {
    case LirOpcode::Const:
        return Word(instruction.type) || instruction.type.IsBool();
    case LirOpcode::Cast:
        return (Word(instruction.type) || instruction.type.IsBool()) && !instruction.strArg.contains("128") &&
               (instruction.strArg.contains("int") || instruction.strArg.starts_with("bool"));
    case LirOpcode::Add:
    case LirOpcode::Sub:
    case LirOpcode::Mul:
    case LirOpcode::And:
    case LirOpcode::Or:
    case LirOpcode::Xor:
    case LirOpcode::BitNot:
        return Word(instruction.type);
    case LirOpcode::Not:
        return instruction.type.IsBool();
    case LirOpcode::CmpEq:
    case LirOpcode::CmpNe:
    case LirOpcode::CmpLt:
    case LirOpcode::CmpLe:
    case LirOpcode::CmpGt:
    case LirOpcode::CmpGe:
        return instruction.type.IsBool();
    default:
        return false;
    }
}

bool Convert(LirFunc &function) {
    if (function.isExtern || function.isAsm || function.blocks.size() < 3 || function.blocks.size() > 4096)
        return false;
    const LirControlFlow cfg(function);
    const LirDominance dominance(cfg);
    const LirNaturalLoops loops(cfg, dominance);
    if (!loops.complete)
        return false;
    std::unordered_set<std::uint32_t> looped;
    for (const auto &[header, members] : loops.members)
        looped.insert(members.begin(), members.end());

    // Both point at the function's own parameters and instructions, which stay put until a conversion, after which
    // the pass returns.
    const std::size_t registers = RegisterCountHint(function);
    RegisterTable<const TypeRef *> types(registers);
    RegisterTable<const LirInstr *> definitions(registers);
    LirReg next = 0;
    for (const auto &parameter : function.params) {
        types.Insert(parameter.reg, &parameter.type);
        next = std::max(next, parameter.reg + 1);
    }
    for (const auto &block : function.blocks)
        for (const auto &instruction : block.instrs)
            if (instruction.dst != LirNoReg) {
                types.Insert(instruction.dst, &instruction.type);
                definitions.Insert(instruction.dst, &instruction);
                next = std::max(next, instruction.dst + 1);
            }

    for (std::uint32_t head = 0; head < function.blocks.size(); ++head) {
        const auto &term = function.blocks[head].term;
        if (!term || term->kind != LirTermKind::Branch || !looped.contains(head))
            continue;
        const TypeRef *const *condition = types.Find(term->cond);
        if (condition == nullptr || !(*condition)->IsBool())
            continue;
        for (const bool taken : {true, false}) {
            const auto side = taken ? term->trueTarget : term->falseTarget;
            auto merge = taken ? term->falseTarget : term->trueTarget;
            // The other way may pass through an empty block, the remains of an empty arm; its values are the head's.
            auto through = head;
            const auto &other = function.blocks[merge];
            if (merge != head && merge != side && other.instrs.empty() && other.term &&
                other.term->kind == LirTermKind::Jump && cfg.predecessors[merge].size() == 1) {
                through = merge;
                merge = other.term->trueTarget;
            }
            if (side == head || merge == head || side == merge || through == merge ||
                cfg.predecessors[side].size() != 1 || cfg.predecessors[merge].size() != 2 ||
                !std::ranges::contains(cfg.predecessors[merge], side) ||
                !std::ranges::contains(cfg.predecessors[merge], through))
                continue;
            const auto &sideBlock = function.blocks[side];
            if (!sideBlock.term || sideBlock.term->kind != LirTermKind::Jump || sideBlock.term->trueTarget != merge ||
                !std::ranges::all_of(sideBlock.instrs, Speculable) ||
                std::ranges::count_if(sideBlock.instrs, [](const LirInstr &instruction) {
                    return instruction.op != LirOpcode::Const;
                }) > static_cast<std::ptrdiff_t>(SideLimit))
                continue;
            const auto &mergeBlock = function.blocks[merge];
            std::size_t phis = 0;
            bool words = true;
            for (const auto &instruction : mergeBlock.instrs) {
                if (instruction.op != LirOpcode::Phi)
                    break;
                ++phis;
                words = words && Word(instruction.type) && instruction.phiPreds.size() == 2;
            }
            if (!words || phis == 0 || phis > MergeLimit)
                continue;

            // The merged values, read before the side block's definitions move.
            struct Merged {
                LirReg dst;
                TypeRef type;
                LirReg kept;
                LirReg updated;
                LirReg addend = LirNoReg;
                bool one = false;
            };

            std::vector<Merged> merged;
            for (std::size_t index = 0; index < phis; ++index) {
                const auto &phi = mergeBlock.instrs[index];
                Merged value{phi.dst, phi.type, LirNoReg, LirNoReg};
                for (const auto &[reg, predecessor] : phi.phiPreds)
                    (predecessor == side ? value.updated : value.kept) = reg;
                const LirInstr *const *found = definitions.Find(value.updated);
                const auto *sum = found == nullptr ? nullptr : *found;
                if (sum && sum->op == LirOpcode::Add && sum->srcs.size() == 2 &&
                    sum->type.ToString() == phi.type.ToString() &&
                    (sum->srcs[0] == value.kept || sum->srcs[1] == value.kept)) {
                    value.addend = sum->srcs[0] == value.kept ? sum->srcs[1] : sum->srcs[0];
                    const LirInstr *const *addend = definitions.Find(value.addend);
                    value.one = addend != nullptr && (*addend)->op == LirOpcode::Const && (*addend)->strArg == "1";
                }
                merged.push_back(std::move(value));
            }

            const auto conditionReg = term->cond;
            const TypeRef conditionType = **condition;
            auto &instrs = function.blocks[head].instrs;
            for (auto &instruction : function.blocks[side].instrs)
                instrs.push_back(std::move(instruction));
            function.blocks[side].instrs.clear();
            const auto emit = [&](const LirOpcode op, const TypeRef &type, std::vector<LirReg> sources,
                                  std::string argument = {}, const LirReg dst = LirNoReg) {
                LirInstr instruction;
                instruction.op = op;
                instruction.dst = dst == LirNoReg ? next++ : dst;
                instruction.type = type;
                instruction.srcs = std::move(sources);
                instruction.strArg = std::move(argument);
                instrs.push_back(std::move(instruction));
                return instrs.back().dst;
            };
            // Per type: the condition as 0 or 1 when the side block would have run, and as all zeros or all ones.
            std::unordered_map<std::string, std::pair<LirReg, LirReg>> masks;
            const auto maskOf = [&](const TypeRef &type) {
                const auto key = type.ToString();
                if (const auto found = masks.find(key); found != masks.end())
                    return found->second;
                auto bit = emit(LirOpcode::Cast, type, {conditionReg}, conditionType.ToString());
                if (!taken)
                    bit = emit(LirOpcode::Xor, type, {bit, emit(LirOpcode::Const, type, {}, "1")});
                const auto all = emit(LirOpcode::Neg, type, {bit});
                return masks.emplace(key, std::pair{bit, all}).first->second;
            };
            for (const auto &value : merged) {
                const auto [bit, all] = maskOf(value.type);
                if (value.updated == value.kept)
                    emit(LirOpcode::Or, value.type, {value.kept, value.kept}, {}, value.dst);
                else if (value.one)
                    emit(LirOpcode::Add, value.type, {value.kept, bit}, {}, value.dst);
                else if (value.addend != LirNoReg)
                    emit(LirOpcode::Add, value.type,
                         {value.kept, emit(LirOpcode::And, value.type, {value.addend, all})}, {}, value.dst);
                else {
                    const auto difference = emit(LirOpcode::Xor, value.type, {value.kept, value.updated});
                    emit(LirOpcode::Xor, value.type, {value.kept, emit(LirOpcode::And, value.type, {difference, all})},
                         {}, value.dst);
                }
            }
            auto &mergeInstrs = function.blocks[merge].instrs;
            mergeInstrs.erase(mergeInstrs.begin(), mergeInstrs.begin() + static_cast<std::ptrdiff_t>(phis));
            auto &branch = *function.blocks[head].term;
            branch.kind = LirTermKind::Jump;
            branch.trueTarget = merge;
            branch.falseTarget = 0;
            branch.cond = LirNoReg;
            // The side block is unreachable now and still jumps to the merge, which has no phis left to name it.
            return true;
        }
    }
    return false;
}
} // namespace

std::string_view LirIfConversion::Name() const noexcept {
    return "lir-if-conversion";
}

PassChange LirIfConversion::RunFunction(LirFunc &function, const std::size_t, const PassContext &) const {
    // Only a branch inside a loop is converted.
    if (!MayHaveLoops(function))
        return PassChange::None;
    bool changed = false;
    for (int round = 0; round < 16 && Convert(function); ++round)
        changed = true;
    return changed ? PassChange::Changed : PassChange::None;
}
} // namespace Rux::Optimization
