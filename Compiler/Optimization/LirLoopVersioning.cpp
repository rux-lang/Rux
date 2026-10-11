#include "Optimization/LirLoopVersioning.h"

#include "Ir/Lir/NaturalLoops.h"
#include "Optimization/ConstantEvaluator.h"

#include <functional>
#include <limits>
#include <optional>
#include <unordered_map>

namespace Rux::Optimization {
namespace {
bool Word(const TypeRef &type) {
    // Every supported target has 64-bit uint. Signed and narrowing conversions need separate proofs.
    return type.kind == TypeRef::Kind::UInt || type.kind == TypeRef::Kind::UInt64;
}

bool SignedWord(const TypeRef &type) {
    return type.kind == TypeRef::Kind::Int || type.kind == TypeRef::Kind::Int64;
}

struct Definition {
    const LirInstr *instruction;
    std::uint32_t block;
    TypeRef type;
};

struct Check {
    std::uint32_t block;
    LirReg offset;
    LirReg bound;
    /// For a signed induction value, a constant added to it; its first index is non-negative by construction.
    std::int64_t constantOffset = 0;
    /// For an index that never exceeds a constant, such as a byte used as a table index, that constant.
    std::optional<std::uint64_t> maximum = std::nullopt;
    /// For an index derived from another counter the loop advances by a constant, that counter's phi and the constant
    /// added to it.
    LirReg secondary = LirNoReg;
    std::uint64_t secondaryOffset = 0;
};

/// A header phi other than the induction value that every iteration advances by the same constant: in iteration n it
/// holds start + n * step.
struct Secondary {
    LirReg start;
    std::uint64_t step;
};

// Small enough that start + iterations * step + offset cannot wrap once start is at most 2^40 and the limit at most
// 2^32.
constexpr std::uint64_t MaximumStep = std::uint64_t{1} << 16;
constexpr std::uint64_t MaximumStart = std::uint64_t{1} << 40;
constexpr std::uint64_t MaximumIterations = std::uint64_t{1} << 32;

bool Version(LirFunc &function) {
    if (function.isExtern || function.isAsm || function.blocks.size() < 3 || function.blocks.size() > 2048)
        return false;
    const LirControlFlow cfg(function);
    const LirDominance dominance(cfg);
    const LirNaturalLoops loops(cfg, dominance);
    if (!loops.complete)
        return false;
    std::unordered_map<LirReg, Definition> definitions;
    LirReg next = 0;
    for (const auto &parameter : function.params) {
        if (!definitions.emplace(parameter.reg, Definition{nullptr, 0, parameter.type}).second)
            return false;
        next = std::max(next, parameter.reg + 1);
    }
    for (std::uint32_t block = 0; block < function.blocks.size(); ++block) {
        for (const auto &instruction : function.blocks[block].instrs) {
            if (instruction.dst == LirNoReg)
                continue;
            if (!definitions.emplace(instruction.dst, Definition{&instruction, block, instruction.type}).second)
                return false;
            next = std::max(next, instruction.dst + 1);
        }
    }
    const auto get = [&](const LirReg reg) -> const LirInstr * {
        const auto found = definitions.find(reg);
        return found == definitions.end() ? nullptr : found->second.instruction;
    };
    // A branch condition seen through phis with one incoming value, which cleanup leaves after threading a
    // short-circuit operand.
    const auto condition = [&](LirReg reg) -> const LirInstr * {
        for (std::size_t depth = 0; depth < 4; ++depth) {
            const auto *instruction = get(reg);
            if (!instruction || instruction->op != LirOpcode::Phi || instruction->phiPreds.size() != 1)
                return instruction;
            reg = instruction->phiPreds.front().first;
        }
        return nullptr;
    };
    const auto word = [&](const LirReg reg) {
        const auto found = definitions.find(reg);
        return found != definitions.end() && Word(found->second.type);
    };
    const auto strip = [&](LirReg reg) {
        for (std::size_t depth = 0; depth < 8; ++depth) {
            const auto *instruction = get(reg);
            if (!instruction || instruction->op != LirOpcode::Cast || instruction->isVolatile ||
                instruction->srcs.size() != 1 || !word(reg) || !word(instruction->srcs[0]))
                break;
            reg = instruction->srcs[0];
        }
        return reg;
    };
    for (const auto &[header, members] : loops.members) {
        if (header == 0 || members.size() > 48 || std::ranges::any_of(loops.members, [&](const auto &loop) {
                return loop.first != header && members.contains(loop.first);
            }))
            continue;
        // The controlling test leaves the loop unless the induction value is below the limit. A `for` loop tests in
        // its header; `while a && i < n` tests in a later block, which every iteration still passes because it
        // dominates the latch. Such tests lie on one dominator chain; the first one controls, and later ones
        // become checks.
        if (cfg.predecessors[header].size() != 2)
            continue;
        auto backEdge = LirDominance::Invalid;
        for (const auto predecessor : cfg.predecessors[header])
            if (members.contains(predecessor))
                backEdge = predecessor;
        if (backEdge == LirDominance::Invalid)
            continue;
        const LirInstr *test = nullptr;
        auto controlling = LirDominance::Invalid;
        for (const auto block : members) {
            const auto &branch = function.blocks[block].term;
            if (!branch || branch->kind != LirTermKind::Branch || !members.contains(branch->trueTarget) ||
                members.contains(branch->falseTarget) || !dominance.Dominates(block, backEdge))
                continue;
            const auto *comparison = condition(branch->cond);
            if (!comparison || comparison->op != LirOpcode::CmpLt || comparison->isVolatile ||
                comparison->srcs.size() != 2)
                continue;
            const auto *counter = get(comparison->srcs[0]);
            if (!counter || counter->op != LirOpcode::Phi || definitions.at(comparison->srcs[0]).block != header)
                continue;
            if (!test || dominance.Dominates(block, controlling)) {
                test = comparison;
                controlling = block;
            }
        }
        if (!test)
            continue;
        const auto &term = function.blocks[controlling].term;
        const auto induction = test->srcs[0];
        const auto limit = test->srcs[1];
        const auto *phi = get(induction);
        const auto signedWord = [&](const LirReg reg) {
            const auto found = definitions.find(reg);
            return found != definitions.end() && SignedWord(found->second.type);
        };
        // A signed induction value counts up by one from a non-negative constant, so every value it takes in the
        // loop is non-negative and below the limit, and converting it to an unsigned index changes nothing.
        const bool signedInduction =
            signedWord(induction) && signedWord(limit) && definitions.at(induction).type == definitions.at(limit).type;
        if ((!signedInduction && (!word(induction) || !word(limit))) || !phi || phi->op != LirOpcode::Phi ||
            phi->phiPreds.size() != 2 || definitions.at(induction).block != header ||
            cfg.predecessors[header].size() != 2)
            continue;
        auto preheader = LirDominance::Invalid;
        auto latch = LirDominance::Invalid;
        auto initial = LirNoReg;
        auto increment = LirNoReg;
        for (const auto &[value, predecessor] : phi->phiPreds) {
            if (members.contains(predecessor)) {
                latch = predecessor;
                increment = value;
            }
            else {
                preheader = predecessor;
                initial = value;
            }
        }
        if (preheader == LirDominance::Invalid || latch == LirDominance::Invalid ||
            cfg.successors[preheader].size() != 1 || !dominance.Dominates(preheader, header))
            continue;
        // The first index: zero for an unsigned induction with any stride, any non-negative constant otherwise.
        std::optional<std::int64_t> start;
        if (const auto *first = get(initial); first && first->op == LirOpcode::Const && !first->isVolatile &&
                                              first->srcs.empty() && first->type == phi->type) {
            if (const auto value = ParseConstant(first->strArg, first->type)) {
                const auto bits = static_cast<std::int64_t>(value->RawBits());
                if (bits >= 0)
                    start = bits;
            }
        }
        if (!start)
            continue;
        const auto *step = get(increment);
        if (!step || step->op != LirOpcode::Add || step->isVolatile || step->type != phi->type ||
            step->srcs.size() != 2 || step->srcs[0] != induction ||
            !dominance.Dominates(definitions.at(increment).block, latch))
            continue;
        const auto *strideInstruction = get(step->srcs[1]);
        if (!strideInstruction || strideInstruction->op != LirOpcode::Const || strideInstruction->isVolatile ||
            strideInstruction->type != phi->type || !strideInstruction->srcs.empty())
            continue;
        const auto strideValue = ParseConstant(strideInstruction->strArg, strideInstruction->type);
        if (!strideValue || strideValue->RawBits() == 0 || strideValue->RawBits() > 64)
            continue;
        const auto stride = strideValue->RawBits();
        // Strides other than one compute the last index from a zero start in unsigned arithmetic.
        if ((stride != 1 && (signedInduction || *start != 0)))
            continue;
        // A constant counts as invariant wherever it is defined: the guard recreates it in the preheader.
        const auto loopConstant = [&](const LirReg reg) {
            const auto *instruction = get(reg);
            return instruction && instruction->op == LirOpcode::Const && !instruction->isVolatile &&
                   instruction->srcs.empty() && members.contains(definitions.at(reg).block);
        };
        const auto invariant = [&](const LirReg reg) {
            const auto found = definitions.find(reg);
            return found != definitions.end() && Word(found->second.type) &&
                   ((!members.contains(found->second.block) && dominance.Dominates(found->second.block, preheader)) ||
                    loopConstant(reg));
        };
        const auto invariantLimit = [&](const LirReg reg) {
            const auto found = definitions.find(reg);
            return found != definitions.end() &&
                   ((!members.contains(found->second.block) && dominance.Dominates(found->second.block, preheader)) ||
                    loopConstant(reg));
        };
        if (signedInduction ? !invariantLimit(limit) : !invariant(limit))
            continue;
        const auto constantOf = [&](const LirReg reg) -> std::optional<std::uint64_t> {
            const auto *instruction = get(reg);
            if (!instruction || instruction->op != LirOpcode::Const || instruction->isVolatile ||
                !instruction->srcs.empty())
                return std::nullopt;
            const auto value = ParseConstant(instruction->strArg, instruction->type);
            return value ? std::optional<std::uint64_t>(value->RawBits()) : std::nullopt;
        };
        // An index that never exceeds a constant is below every bound larger than that constant: a zero-extended
        // byte or halfword, or a value masked by a constant.
        const auto maximumOf = [&](LirReg reg) -> std::optional<std::uint64_t> {
            reg = strip(reg);
            const auto *instruction = get(reg);
            if (!instruction || instruction->isVolatile)
                return std::nullopt;
            if (instruction->op == LirOpcode::Cast && instruction->srcs.size() == 1) {
                const auto found = definitions.find(instruction->srcs[0]);
                if (found == definitions.end())
                    return std::nullopt;
                if (found->second.type.kind == TypeRef::Kind::UInt8)
                    return 0xFF;
                if (found->second.type.kind == TypeRef::Kind::UInt16)
                    return 0xFFFF;
                return std::nullopt;
            }
            if (instruction->op == LirOpcode::And && instruction->srcs.size() == 2 && Word(instruction->type)) {
                const auto right = constantOf(instruction->srcs[1]);
                const auto left = constantOf(instruction->srcs[0]);
                if (right && *right < MaximumStart)
                    return right;
                if (left && *left < MaximumStart)
                    return left;
            }
            return std::nullopt;
        };
        // Other counters: header phis entered with an invariant value and advanced only by constant additions.
        std::unordered_map<LirReg, Secondary> secondaries;
        for (const auto &instruction : function.blocks[header].instrs) {
            if (instruction.op != LirOpcode::Phi || instruction.dst == induction || !Word(instruction.type) ||
                instruction.phiPreds.size() != 2)
                continue;
            auto entered = LirNoReg;
            auto advanced = LirNoReg;
            for (const auto &[value, predecessor] : instruction.phiPreds)
                (members.contains(predecessor) ? advanced : entered) = value;
            if (entered == LirNoReg || advanced == LirNoReg || !invariant(entered))
                continue;
            std::uint64_t advance = 0;
            auto reg = advanced;
            for (std::size_t depth = 0; depth < 16 && reg != instruction.dst && advance <= MaximumStep; ++depth) {
                const auto *add = get(reg);
                const auto amount = add && add->op == LirOpcode::Add && !add->isVolatile &&
                                            add->type == instruction.type && add->srcs.size() == 2
                                      ? constantOf(add->srcs[1])
                                      : std::nullopt;
                if (!amount || *amount > MaximumStep)
                    break;
                advance += *amount;
                reg = add->srcs[0];
            }
            if (reg == instruction.dst && advance <= MaximumStep)
                secondaries.emplace(instruction.dst, Secondary{entered, advance});
        }
        // An index as another counter plus constants.
        const auto secondaryIndex = [&](LirReg reg) -> std::optional<std::pair<LirReg, std::uint64_t>> {
            std::uint64_t offset = 0;
            for (std::size_t depth = 0; depth < 16 && offset <= MaximumStep; ++depth) {
                reg = strip(reg);
                if (secondaries.contains(reg))
                    return std::pair{reg, offset};
                const auto *add = get(reg);
                const auto amount =
                    add && add->op == LirOpcode::Add && !add->isVolatile && Word(add->type) && add->srcs.size() == 2
                        ? constantOf(add->srcs[1])
                        : std::nullopt;
                if (!amount || *amount > MaximumStep)
                    return std::nullopt;
                offset += *amount;
                reg = add->srcs[0];
            }
            return std::nullopt;
        };
        // The signed index expression a check compares, as the induction value plus a constant.
        const auto signedIndex = [&](LirReg reg) -> std::optional<std::int64_t> {
            const auto *cast = get(reg);
            if (!cast || cast->op != LirOpcode::Cast || cast->isVolatile || cast->srcs.size() != 1 ||
                !signedWord(cast->srcs[0]))
                return std::nullopt;
            reg = cast->srcs[0];
            if (reg == induction)
                return 0;
            const auto *arithmetic = get(reg);
            if (!arithmetic || (arithmetic->op != LirOpcode::Add && arithmetic->op != LirOpcode::Sub) ||
                arithmetic->isVolatile || arithmetic->srcs.size() != 2 || arithmetic->srcs[0] != induction)
                return std::nullopt;
            const auto *amount = get(arithmetic->srcs[1]);
            if (!amount || amount->op != LirOpcode::Const || amount->isVolatile || amount->type != phi->type)
                return std::nullopt;
            const auto value = ParseConstant(amount->strArg, amount->type);
            if (!value)
                return std::nullopt;
            const auto magnitude = static_cast<std::int64_t>(value->RawBits());
            if (magnitude < -(std::int64_t{1} << 40) || magnitude > (std::int64_t{1} << 40))
                return std::nullopt;
            return arithmetic->op == LirOpcode::Add ? magnitude : -magnitude;
        };
        bool eligible = true;
        std::size_t cost = 0;
        std::vector<Check> checks;
        for (const auto block : members) {
            // No side entries.
            for (const auto predecessor : cfg.predecessors[block])
                if (block != header && !members.contains(predecessor))
                    eligible = false;
            for (const auto &instruction : function.blocks[block].instrs) {
                ++cost;
                if (instruction.op == LirOpcode::Alloca || instruction.op == LirOpcode::Call ||
                    instruction.op == LirOpcode::CallIndirect || instruction.isVolatile)
                    eligible = false;
            }
            const auto &branch = function.blocks[block].term;
            // A check runs only after the controlling test passed in the same iteration, so the index is below the
            // limit there.
            if (block == controlling || !dominance.Dominates(term->trueTarget, block) || !branch ||
                branch->kind != LirTermKind::Branch || !members.contains(branch->trueTarget) ||
                members.contains(branch->falseTarget))
                continue;
            const auto *comparison = condition(branch->cond);
            if (!comparison || comparison->op != LirOpcode::CmpLt || comparison->isVolatile ||
                comparison->srcs.size() != 2 || !word(comparison->srcs[0]) || !invariant(comparison->srcs[1]))
                continue;
            if (const auto maximum = maximumOf(comparison->srcs[0])) {
                Check check{block, LirNoReg, comparison->srcs[1]};
                check.maximum = maximum;
                checks.push_back(check);
                continue;
            }
            if (const auto derived = secondaryIndex(comparison->srcs[0])) {
                Check check{block, LirNoReg, comparison->srcs[1]};
                check.secondary = derived->first;
                check.secondaryOffset = derived->second;
                checks.push_back(check);
                continue;
            }
            if (signedInduction) {
                // Only checks whose first index is already non-negative: below zero the conversion would wrap.
                if (const auto offset = signedIndex(comparison->srcs[0]); offset && *start + *offset >= 0)
                    checks.push_back({block, LirNoReg, comparison->srcs[1], *offset});
                continue;
            }
            const auto index = strip(comparison->srcs[0]);
            LirReg offset = LirNoReg;
            if (index != induction) {
                const auto *add = get(index);
                if (!add || add->op != LirOpcode::Add || add->isVolatile || !Word(add->type) || add->srcs.size() != 2)
                    continue;
                if (strip(add->srcs[0]) == induction && invariant(add->srcs[1]))
                    offset = add->srcs[1];
                else if (strip(add->srcs[1]) == induction && invariant(add->srcs[0]))
                    offset = add->srcs[0];
                else
                    continue;
            }
            checks.push_back({block, offset, comparison->srcs[1]});
        }
        if (!eligible || cost > 320 || checks.empty() || checks.size() > 16)
            continue;
        // Values the loop defines and code after it reads, directly or through a phi on an edge that leaves a block
        // outside the loop. Once the fast copy exists, both copies reach those readers.
        const auto local = [&](const LirReg reg) {
            const auto found = definitions.find(reg);
            return found != definitions.end() && members.contains(found->second.block);
        };
        std::vector<LirReg> liveOut;
        const auto note = [&](const LirReg reg) {
            if (local(reg) && std::ranges::find(liveOut, reg) == liveOut.end())
                liveOut.push_back(reg);
        };
        for (std::uint32_t block = 0; block < function.blocks.size(); ++block) {
            if (members.contains(block))
                continue;
            for (const auto &instruction : function.blocks[block].instrs) {
                for (const auto source : instruction.srcs)
                    note(source);
                for (const auto &[value, predecessor] : instruction.phiPreds)
                    if (!members.contains(predecessor))
                        note(value);
            }
            if (const auto &exit = function.blocks[block].term) {
                note(exit->cond);
                if (exit->retVal)
                    note(*exit->retVal);
            }
        }

        std::vector<std::uint32_t> ordered(members.begin(), members.end());
        std::ranges::sort(ordered);
        std::unordered_map<std::uint32_t, std::uint32_t> blocks;
        std::unordered_map<LirReg, LirReg> registers;
        const auto oldCount = static_cast<std::uint32_t>(function.blocks.size());
        for (std::uint32_t index = 0; index < ordered.size(); ++index) {
            blocks.emplace(ordered[index], oldCount + index);
            for (const auto &instruction : function.blocks[ordered[index]].instrs)
                if (instruction.dst != LirNoReg)
                    registers.emplace(instruction.dst, next++);
        }
        const auto mappedReg = [&](const LirReg reg) { return registers.contains(reg) ? registers.at(reg) : reg; };
        const auto mappedBlock = [&](const std::uint32_t block) {
            return blocks.contains(block) ? blocks.at(block) : block;
        };
        std::vector<LirBlock> clones;
        for (const auto block : ordered) {
            auto clone = function.blocks[block];
            clone.label += ".range.fast." + std::to_string(block);
            for (auto &instruction : clone.instrs) {
                instruction.dst = mappedReg(instruction.dst);
                for (auto &source : instruction.srcs)
                    source = mappedReg(source);
                for (auto &[source, predecessor] : instruction.phiPreds) {
                    source = mappedReg(source);
                    predecessor = mappedBlock(predecessor);
                }
            }
            if (auto &exit = clone.term) {
                if (std::ranges::any_of(checks, [&](const Check &check) { return check.block == block; })) {
                    exit->kind = LirTermKind::Jump;
                    exit->cond = LirNoReg;
                    exit->falseTarget = 0;
                }
                exit->cond = mappedReg(exit->cond);
                if (exit->retVal)
                    exit->retVal = mappedReg(*exit->retVal);
                exit->trueTarget = mappedBlock(exit->trueTarget);
                exit->falseTarget = mappedBlock(exit->falseTarget);
                exit->defaultTarget = mappedBlock(exit->defaultTarget);
                for (auto &entry : exit->cases)
                    entry.target = mappedBlock(entry.target);
            }
            clones.push_back(std::move(clone));
        }
        // Build non-trapping guards in the existing preheader. Subtraction may wrap when row > bound, but the
        // corresponding row <= bound conjunct rejects that case. No new loads, calls or panics are introduced.
        auto &setup = function.blocks[preheader].instrs;
        const auto emit = [&](const LirOpcode op, const TypeRef &type, std::vector<LirReg> sources,
                              const std::string &argument = "") {
            LirInstr instruction;
            instruction.op = op;
            instruction.dst = next++;
            instruction.type = type;
            instruction.srcs = std::move(sources);
            instruction.strArg = argument;
            setup.push_back(std::move(instruction));
            return setup.back().dst;
        };
        const auto widenFrom = [&](const LirReg value, const TypeRef &from) {
            return emit(LirOpcode::Cast, TypeRef::MakeUInt64(), {value}, from.ToString());
        };
        // The value as the preheader sees it: a constant defined inside the loop is recreated there.
        const auto outside = [&](const LirReg value) {
            const auto &definition = definitions.at(value);
            if (definition.instruction && definition.instruction->op == LirOpcode::Const &&
                members.contains(definition.block))
                return emit(LirOpcode::Const, definition.type, {}, definition.instruction->strArg);
            return value;
        };
        const auto widen = [&](const LirReg value) { return widenFrom(outside(value), definitions.at(value).type); };
        const auto limit64 = widen(limit);
        auto count = limit64;
        LirReg guard = LirNoReg;
        LirReg quotient = LirNoReg;
        if (stride != 1) {
            // The last visited index is floor((limit - 1) / stride) * stride. Reject limits for which the
            // increment could wrap: after wrapping, a non-power-of-two stride can visit different residues.
            // For an empty loop, these unsigned calculations may wrap but never access memory or trap.
            const auto one = emit(LirOpcode::Const, TypeRef::MakeUInt64(), {}, "1");
            const auto stepSize = emit(LirOpcode::Const, TypeRef::MakeUInt64(), {}, std::to_string(stride));
            const auto last = emit(LirOpcode::Sub, TypeRef::MakeUInt64(), {limit64, one});
            quotient = emit(LirOpcode::Div, TypeRef::MakeUInt64(), {last, stepSize});
            const auto lastIndex = emit(LirOpcode::Mul, TypeRef::MakeUInt64(), {quotient, stepSize});
            count = emit(LirOpcode::Add, TypeRef::MakeUInt64(), {lastIndex, one});
            const auto maximum = emit(LirOpcode::Const, TypeRef::MakeUInt64(), {},
                                      std::to_string(std::numeric_limits<std::uint64_t>::max() - (stride - 1)));
            guard = emit(LirOpcode::CmpLe, TypeRef::MakeBool(), {limit64, maximum});
        }
        const auto constant = [&](const std::uint64_t value) {
            return emit(LirOpcode::Const, TypeRef::MakeUInt64(), {}, std::to_string(value));
        };
        const auto conjoin = [&](LirReg &all, const LirReg term) {
            all = all == LirNoReg ? term : emit(LirOpcode::And, TypeRef::MakeBool(), {all, term});
        };
        // The number of iterations minus one, for counters derived from the induction value: the last index over
        // the stride, or limit - 1 - start for a unit stride. The limit is kept below 2^32 so that products with a
        // step cannot wrap; when the loop does not run at all, the value may wrap but nothing reads memory.
        LirReg iterations = LirNoReg;
        const auto lastIteration = [&] {
            if (iterations != LirNoReg)
                return iterations;
            if (stride != 1) {
                iterations = quotient;
            }
            else {
                const auto last = emit(LirOpcode::Sub, TypeRef::MakeUInt64(), {limit64, constant(1)});
                iterations =
                    emit(LirOpcode::Sub, TypeRef::MakeUInt64(), {last, constant(static_cast<std::uint64_t>(*start))});
            }
            conjoin(guard, emit(LirOpcode::CmpLe, TypeRef::MakeBool(), {limit64, constant(MaximumIterations)}));
            return iterations;
        };
        for (const auto &check : checks) {
            const auto bound = widen(check.bound);
            LirReg safe;
            if (check.maximum) {
                safe = emit(LirOpcode::CmpLt, TypeRef::MakeBool(), {constant(*check.maximum), bound});
            }
            else if (check.secondary != LirNoReg) {
                // In the last iteration the counter reaches start + (iterations - 1) * step, and the index adds a
                // constant to that; a start below 2^40 keeps the sum from wrapping.
                const auto &secondary = secondaries.at(check.secondary);
                const auto first = widen(secondary.start);
                const auto advance =
                    emit(LirOpcode::Mul, TypeRef::MakeUInt64(), {lastIteration(), constant(secondary.step)});
                const auto reach = emit(LirOpcode::Add, TypeRef::MakeUInt64(), {first, advance});
                const auto index =
                    emit(LirOpcode::Add, TypeRef::MakeUInt64(), {reach, constant(check.secondaryOffset)});
                safe = emit(LirOpcode::CmpLe, TypeRef::MakeBool(), {first, constant(MaximumStart)});
                conjoin(safe, emit(LirOpcode::CmpLt, TypeRef::MakeBool(), {index, bound}));
            }
            else if (signedInduction) {
                // Every index the loop computes is at most limit - 1 + offset, so limit + offset must not exceed
                // the bound. When the loop runs, limit exceeds the non-negative start, and limit + offset is
                // positive; a sum that overflows wraps to a negative value, which as an unsigned bound fails the
                // guard and keeps the checked loop.
                LirReg last = outside(limit);
                if (check.constantOffset != 0) {
                    const auto amount =
                        emit(LirOpcode::Const, definitions.at(limit).type, {}, std::to_string(check.constantOffset));
                    last = emit(LirOpcode::Add, definitions.at(limit).type, {last, amount});
                }
                safe =
                    emit(LirOpcode::CmpLe, TypeRef::MakeBool(), {widenFrom(last, definitions.at(limit).type), bound});
            }
            else if (check.offset == LirNoReg) {
                safe = emit(LirOpcode::CmpLe, TypeRef::MakeBool(), {count, bound});
            }
            else {
                const auto offset = widen(check.offset);
                const auto fits = emit(LirOpcode::CmpLe, TypeRef::MakeBool(), {offset, bound});
                const auto remaining = emit(LirOpcode::Sub, TypeRef::MakeUInt64(), {bound, offset});
                const auto lengthFits = emit(LirOpcode::CmpLe, TypeRef::MakeBool(), {count, remaining});
                safe = emit(LirOpcode::And, TypeRef::MakeBool(), {fits, lengthFits});
            }
            conjoin(guard, safe);
        }
        auto &entry = *function.blocks[preheader].term;
        entry.kind = LirTermKind::Branch;
        entry.cond = guard;
        entry.trueTarget = blocks.at(header);
        entry.falseTarget = header;
        for (auto &clone : clones)
            function.blocks.push_back(std::move(clone));
        // Update only real outgoing edges of the clone (eliminated check-failure edges get no phi operands).
        const LirControlFlow updated(function);
        for (const auto block : ordered) {
            const auto clone = blocks.at(block);
            for (const auto target : updated.successors[clone]) {
                if (target >= oldCount)
                    continue;
                for (auto &instruction : function.blocks[target].instrs) {
                    if (instruction.op != LirOpcode::Phi)
                        continue;
                    const auto edge = std::ranges::find_if(
                        instruction.phiPreds, [&](const auto &incoming) { return incoming.second == block; });
                    if (edge != instruction.phiPreds.end())
                        instruction.phiPreds.emplace_back(mappedReg(edge->first), clone);
                }
            }
        }
        // Each read after the loop takes the value of whichever copy reached it, merged by a phi where both meet. A
        // block of either copy holds that copy's value: the original program only reaches its readers through
        // blocks the definition dominates.
        for (const auto value : liveOut) {
            const auto fast = registers.at(value);
            std::unordered_map<std::uint32_t, LirReg> atStart;
            std::unordered_map<std::uint32_t, LirInstr> merges;
            std::function<LirReg(std::uint32_t)> startOf;
            const auto endOf = [&](const std::uint32_t block) {
                if (members.contains(block))
                    return value;
                return block >= oldCount ? fast : startOf(block);
            };
            startOf = [&](const std::uint32_t block) -> LirReg {
                if (const auto found = atStart.find(block); found != atStart.end())
                    return found->second;
                const auto &predecessors = updated.predecessors[block];
                if (predecessors.size() == 1) {
                    atStart.emplace(block, value);
                    return atStart[block] = endOf(predecessors.front());
                }
                if (predecessors.empty())
                    return value;
                LirInstr merge;
                merge.op = LirOpcode::Phi;
                merge.dst = next++;
                merge.type = definitions.at(value).type;
                const auto result = merge.dst;
                atStart.emplace(block, result);
                merges.emplace(block, std::move(merge));
                for (const auto predecessor : predecessors) {
                    const auto incoming = endOf(predecessor);
                    merges.at(block).phiPreds.emplace_back(incoming, predecessor);
                }
                return result;
            };
            for (std::uint32_t block = 0; block < oldCount; ++block) {
                if (members.contains(block) || !updated.reachable[block])
                    continue;
                for (auto &instruction : function.blocks[block].instrs) {
                    for (auto &source : instruction.srcs)
                        if (source == value)
                            source = startOf(block);
                    for (auto &[incoming, predecessor] : instruction.phiPreds)
                        if (incoming == value && !members.contains(predecessor) && predecessor < oldCount)
                            incoming = endOf(predecessor);
                }
                if (auto &exit = function.blocks[block].term) {
                    if (exit->cond == value)
                        exit->cond = startOf(block);
                    if (exit->retVal && *exit->retVal == value)
                        exit->retVal = startOf(block);
                }
            }
            std::vector<std::uint32_t> placed;
            for (const auto &merge : merges)
                placed.push_back(merge.first);
            std::ranges::sort(placed);
            for (const auto block : placed) {
                auto &instructions = function.blocks[block].instrs;
                instructions.insert(instructions.begin(), std::move(merges.at(block)));
            }
        }
        return true; // Bound code growth to one innermost loop per function and one invocation per pipeline run.
    }
    return false;
}
} // namespace

std::string_view LirLoopVersioning::Name() const noexcept {
    return "lir-loop-versioning";
}

PassChange LirLoopVersioning::Run(LirPackage &package, const PassContext &context) {
    if (context.iteration != 0)
        return PassChange::None;
    bool changed = false;
    // Each round versions one loop; a versioned loop's entry becomes a guard branch, which no later round accepts as
    // a preheader, so repeating reaches the function's other loops without revisiting one.
    for (auto &module : package.modules)
        for (auto &function : module.funcs)
            for (int round = 0; round < 8 && Version(function); ++round)
                changed = true;
    return changed ? PassChange::Changed : PassChange::None;
}
} // namespace Rux::Optimization
