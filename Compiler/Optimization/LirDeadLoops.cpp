#include "Optimization/LirDeadLoops.h"

#include "Ir/Lir/NaturalLoops.h"
#include "Ir/Lir/RegisterTable.h"
#include "Optimization/ConstantEvaluator.h"

namespace Rux::Optimization {
namespace {
/// An operation with no effect beyond its result: no memory write, call, trap or allocation.
bool Inert(const LirInstr &instruction) {
    if (instruction.isVolatile)
        return false;
    switch (instruction.op) {
    case LirOpcode::Const:
    case LirOpcode::Phi:
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
    case LirOpcode::Not:
    case LirOpcode::BitNot:
    case LirOpcode::CmpEq:
    case LirOpcode::CmpNe:
    case LirOpcode::CmpLt:
    case LirOpcode::CmpLe:
    case LirOpcode::CmpGt:
    case LirOpcode::CmpGe:
        return true;
    // A load whose value nothing reads is no more observable than the machine code that drops it.
    case LirOpcode::Load:
        return instruction.strArg.empty();
    default:
        return false;
    }
}

bool Word(const TypeRef &type) {
    return type->kind == TypeRef::Kind::UInt || type->kind == TypeRef::Kind::UInt64;
}

bool Remove(LirFunc &function) {
    if (function.isExtern || function.isAsm || function.blocks.size() < 3 || function.blocks.size() > 4096)
        return false;
    const LirControlFlow cfg(function);
    const LirDominance dominance(cfg);
    const LirNaturalLoops loops(cfg, dominance);
    if (!loops.complete)
        return false;
    const std::size_t registers = RegisterCountHint(function);
    LirRegisterTable<std::uint32_t> owners(registers);
    LirRegisterTable<const LirInstr *> definitions(registers);
    for (const auto &parameter : function.params)
        owners.Insert(parameter.reg, 0);
    for (std::uint32_t block = 0; block < function.blocks.size(); ++block)
        for (const auto &instruction : function.blocks[block].instrs)
            if (instruction.dst != LirNoReg) {
                owners.Insert(instruction.dst, block);
                definitions.Insert(instruction.dst, &instruction);
            }
    const auto get = [&](const LirReg reg) -> const LirInstr * {
        const auto *found = definitions.Find(reg);
        return found == nullptr ? nullptr : *found;
    };
    for (const auto &[header, members] : loops.members) {
        if (header == 0)
            continue;
        // Nothing inside has an effect, and every way out leads to one block that merges no values.
        bool inert = true;
        auto exit = LirDominance::Invalid;
        for (const auto block : members) {
            const auto &term = function.blocks[block].term;
            if (!term || (term->kind != LirTermKind::Jump && term->kind != LirTermKind::Branch) ||
                !std::ranges::all_of(function.blocks[block].instrs, Inert)) {
                inert = false;
                break;
            }
            for (const auto successor : cfg.successors[block]) {
                if (members.contains(successor))
                    continue;
                if (exit != LirDominance::Invalid && exit != successor)
                    inert = false;
                exit = successor;
            }
        }
        if (!inert || exit == LirDominance::Invalid ||
            std::ranges::any_of(function.blocks[exit].instrs,
                                [](const LirInstr &instruction) { return instruction.op == LirOpcode::Phi; }))
            continue;
        // No value the loop computes is read outside it.
        const auto local = [&](const LirReg reg) {
            const auto *found = owners.Find(reg);
            return found != nullptr && members.contains(*found) && definitions.Contains(reg);
        };
        bool escapes = false;
        for (std::uint32_t block = 0; block < function.blocks.size() && !escapes; ++block) {
            if (members.contains(block))
                continue;
            for (const auto &instruction : function.blocks[block].instrs) {
                escapes =
                    escapes || std::ranges::any_of(instruction.srcs, local) ||
                    std::ranges::any_of(instruction.phiPreds, [&](const auto &edge) { return local(edge.first); });
            }
            const auto &term = *function.blocks[block].term;
            escapes = escapes || local(term.cond) || (term.retVal && local(*term.retVal));
        }
        if (escapes)
            continue;
        // It ends: one back edge, and a test that every iteration passes on its way there compares an unsigned
        // count, stepped by one along that edge, with a limit fixed before the loop. The count cannot pass the limit
        // without the test failing, and below the limit a step of one cannot wrap.
        auto latch = LirDominance::Invalid;
        std::size_t outside = 0;
        for (const auto predecessor : cfg.predecessors[header]) {
            if (!members.contains(predecessor))
                ++outside;
            else if (latch == LirDominance::Invalid)
                latch = predecessor;
            else
                latch = LirDominance::Invalid - 1;
        }
        if (outside == 0 || latch == LirDominance::Invalid || latch == LirDominance::Invalid - 1)
            continue;
        bool ends = false;
        for (const auto block : members) {
            const auto &term = *function.blocks[block].term;
            if (ends || term.kind != LirTermKind::Branch || !members.contains(term.trueTarget) ||
                members.contains(term.falseTarget) || !dominance.Dominates(block, latch))
                continue;
            const auto *test = get(term.cond);
            if (!test || test->op != LirOpcode::CmpLt || test->srcs.size() != 2)
                continue;
            const auto *counter = get(test->srcs[0]);
            const auto *limit = owners.Find(test->srcs[1]);
            if (!counter || counter->op != LirOpcode::Phi || *owners.Find(counter->dst) != header ||
                !Word(counter->type) || limit == nullptr || members.contains(*limit))
                continue;
            for (const auto &[value, predecessor] : counter->phiPreds) {
                if (predecessor != latch)
                    continue;
                const auto *step = get(value);
                const auto *one = step && step->op == LirOpcode::Add && step->srcs.size() == 2 &&
                                          step->srcs[0] == counter->dst && step->type == counter->type
                                    ? get(step->srcs[1])
                                    : nullptr;
                if (one && one->op == LirOpcode::Const) {
                    const auto amount = ParseConstant(one->strArg, one->type);
                    ends = amount && amount->RawBits() == 1;
                }
            }
        }
        if (!ends)
            continue;
        // Every entry into the loop now enters its exit instead.
        for (const auto predecessor : cfg.predecessors[header]) {
            if (members.contains(predecessor))
                continue;
            auto &term = *function.blocks[predecessor].term;
            if (term.trueTarget == header)
                term.trueTarget = exit;
            if (term.kind == LirTermKind::Branch && term.falseTarget == header)
                term.falseTarget = exit;
            if (term.kind == LirTermKind::Branch && term.trueTarget == term.falseTarget) {
                term.kind = LirTermKind::Jump;
                term.cond = LirNoReg;
                term.falseTarget = 0;
            }
        }
        return true;
    }
    return false;
}
} // namespace

std::string_view LirDeadLoops::Name() const noexcept {
    return "lir-dead-loop-elimination";
}

PassChange LirDeadLoops::RunFunction(LirFunc &function, const std::size_t, const PassContext &) const {
    if (!MayHaveLoops(function))
        return PassChange::None;
    bool changed = false;
    // A removed loop is unreachable, and cleanup deletes it; no round sees it again.
    for (int round = 0; round < 8 && Remove(function); ++round)
        changed = true;
    return changed ? PassChange::Changed : PassChange::None;
}
} // namespace Rux::Optimization
