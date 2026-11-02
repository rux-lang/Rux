#include "Optimization/LirConstantPropagation.h"

#include "Ir/Lir/Dominance.h"
#include "Optimization/ConstantEvaluator.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Rux::Optimization {
namespace {
using BlockIndex = std::uint32_t;

struct KnownValue {
    std::optional<TypedConstant> constant;
    std::optional<LirReg> copy;
};

using Values = std::unordered_map<LirReg, KnownValue>;

bool SameConstant(const TypedConstant &left, const TypedConstant &right) {
    return left.GetKind() == right.GetKind() && left.Width() == right.Width() && left.Bits() == right.Bits();
}

bool SameValue(const KnownValue &left, const KnownValue &right) {
    if (left.constant && right.constant) {
        return SameConstant(*left.constant, *right.constant);
    }
    return left.copy && right.copy && left.copy == right.copy;
}

/// Follow a chain of copies to whatever the register ultimately holds. The visited set is not defensive tidiness: a phi
/// cycle in a loop genuinely produces a copy chain that returns to its start.
KnownValue Resolve(const Values &values, const LirReg reg) {
    // Most chains end within a few steps, so the registers visited are kept in place until a long one needs a set.
    constexpr std::size_t kInline = 16;
    std::array<LirReg, kInline> recent{};
    std::size_t count = 0;
    // Built only when a chain outgrows the array: even an empty set allocates with some standard libraries.
    std::optional<std::unordered_set<LirReg>> visited;
    const auto visit = [&](const LirReg candidate) {
        if (count < kInline) {
            if (std::find(recent.begin(), recent.begin() + static_cast<std::ptrdiff_t>(count), candidate) !=
                recent.begin() + static_cast<std::ptrdiff_t>(count))
                return false;
            recent[count++] = candidate;
            return true;
        }
        if (!visited)
            visited.emplace(recent.begin(), recent.end());
        return visited->insert(candidate).second;
    };
    LirReg current = reg;
    while (visit(current)) {
        const auto found = values.find(current);
        if (found == values.end()) {
            return KnownValue{std::nullopt, current};
        }
        if (found->second.constant) {
            return found->second;
        }
        if (!found->second.copy || *found->second.copy == current) {
            return KnownValue{std::nullopt, current};
        }
        current = *found->second.copy;
    }
    const LirReg smallest = !visited
                              ? *std::min_element(recent.begin(), recent.begin() + static_cast<std::ptrdiff_t>(count))
                              : *std::ranges::min_element(*visited);
    return KnownValue{std::nullopt, smallest};
}

/// The one case a switch on a known subject reaches, or its default when no case value equals the subject.
///
/// @return nullopt when the subject is not known, or a case value cannot be read
std::optional<BlockIndex> KnownSwitchTarget(const LirTerminator &terminator, const Values &values) {
    const KnownValue subject = Resolve(values, terminator.cond);
    if (!subject.constant) {
        return std::nullopt;
    }
    for (const auto &switchCase : terminator.cases) {
        const auto value = ParseConstant(switchCase.value, terminator.retType);
        if (!value) {
            return std::nullopt;
        }
        if (SameConstant(*subject.constant, *value)) {
            return switchCase.target;
        }
    }
    return terminator.defaultTarget;
}

/// The successors a block's terminator can still transfer to once the constants known so far are honored: a branch on
/// a known boolean has one, a switch on a known subject has one, and anything else keeps them all.
std::vector<BlockIndex> SelectableSuccessors(const LirBlock &block, const Values &values) {
    std::vector<BlockIndex> successors;
    if (!block.term) {
        return successors;
    }
    const auto add = [&successors](const BlockIndex target) {
        if (!std::ranges::contains(successors, target)) {
            successors.push_back(target);
        }
    };
    const auto &terminator = *block.term;
    switch (terminator.kind) {
    case LirTermKind::Jump:
        add(terminator.trueTarget);
        break;
    case LirTermKind::Branch: {
        std::optional<bool> known;
        if (const KnownValue condition = Resolve(values, terminator.cond); condition.constant) {
            known = condition.constant->BooleanValue();
        }
        if (known) {
            add(*known ? terminator.trueTarget : terminator.falseTarget);
        }
        else {
            add(terminator.trueTarget);
            add(terminator.falseTarget);
        }
        break;
    }
    case LirTermKind::Switch:
        if (const auto selected = KnownSwitchTarget(terminator, values)) {
            add(*selected);
            break;
        }
        add(terminator.defaultTarget);
        for (const auto &switchCase : terminator.cases) {
            add(switchCase.target);
        }
        break;
    case LirTermKind::Return:
    case LirTermKind::Unreachable:
        break;
    }
    return successors;
}

/// The edges a walk from entry can still take once the branch conditions known so far are honored.
///
/// What a phi receives along any other edge comes from a block that will never run. Reading it anyway is what kept a
/// chain of `||` from settling: each join in the chain waited for CFG cleanup, which runs last in an iteration, to
/// delete the edge from the short-circuit block before the join could be known, so every comparison in the chain cost
/// the pipeline one whole iteration and seven of them exhausted the limit.
class LiveEdges {
public:
    LiveEdges(const LirFunc &function, const Values &values)
        : live(function.blocks.size(), false)
        , successors(function.blocks.size()) {
        std::vector<BlockIndex> pending;
        if (!function.blocks.empty()) {
            pending.push_back(0);
        }
        while (!pending.empty()) {
            const BlockIndex blockIndex = pending.back();
            pending.pop_back();
            if (live[blockIndex]) {
                continue;
            }
            live[blockIndex] = true;
            successors[blockIndex] = SelectableSuccessors(function.blocks[blockIndex], values);
            for (const BlockIndex target : successors[blockIndex]) {
                if (target < live.size() && !live[target]) {
                    pending.push_back(target);
                }
            }
        }
    }

    /// Whether control can still flow from `from` to `to`.
    [[nodiscard]] bool IsLive(const BlockIndex from, const BlockIndex to) const {
        return from < live.size() && live[from] && std::ranges::contains(successors[from], to);
    }

private:
    std::vector<bool> live;
    std::vector<std::vector<BlockIndex>> successors;
};

std::optional<TokenKind> UnaryToken(const LirOpcode opcode) {
    switch (opcode) {
    case LirOpcode::Neg:
        return TokenKind::Minus;
    case LirOpcode::Not:
        return TokenKind::Bang;
    case LirOpcode::BitNot:
        return TokenKind::Tilde;
    default:
        return std::nullopt;
    }
}

std::optional<TokenKind> BinaryToken(const LirOpcode opcode) {
    // A plain LIR operation wraps, as the source's wrapping operators do; a checked one folds only when the result
    // fits, so a check that would fire stays to fire.
    switch (opcode) {
    case LirOpcode::Add:
        return TokenKind::PlusPercent;
    case LirOpcode::Sub:
        return TokenKind::MinusPercent;
    case LirOpcode::Mul:
        return TokenKind::StarPercent;
    case LirOpcode::CheckedAdd:
        return TokenKind::Plus;
    case LirOpcode::CheckedSub:
        return TokenKind::Minus;
    case LirOpcode::CheckedMul:
        return TokenKind::Star;
    case LirOpcode::Div:
        return TokenKind::Slash;
    case LirOpcode::Mod:
        return TokenKind::Percent;
    case LirOpcode::And:
        return TokenKind::Amp;
    case LirOpcode::Or:
        return TokenKind::Pipe;
    case LirOpcode::Xor:
        return TokenKind::Caret;
    case LirOpcode::Shl:
        return TokenKind::LessLess;
    case LirOpcode::Shr:
        return TokenKind::GreaterGreater;
    case LirOpcode::Lshr:
        return TokenKind::GreaterGreaterGreater;
    case LirOpcode::CmpEq:
        return TokenKind::Equal;
    case LirOpcode::CmpNe:
        return TokenKind::BangEqual;
    case LirOpcode::CmpLt:
        return TokenKind::Less;
    case LirOpcode::CmpLe:
        return TokenKind::LessEqual;
    case LirOpcode::CmpGt:
        return TokenKind::Greater;
    case LirOpcode::CmpGe:
        return TokenKind::GreaterEqual;
    default:
        return std::nullopt;
    }
}

std::optional<KnownValue> Evaluate(const LirInstr &instruction, const Values &values, const LiveEdges &edges,
                                   const BlockIndex blockIndex, const std::unordered_set<LirReg> &floatRegisters) {
    if (instruction.op == LirOpcode::Const) {
        if (auto value = ParseConstant(instruction.strArg, instruction.type)) {
            return KnownValue{value, std::nullopt};
        }
        return std::nullopt;
    }
    if (instruction.op == LirOpcode::Phi) {
        // The one value every incoming edge `admits` agrees on.
        const auto join = [&](auto &&admits) -> std::optional<KnownValue> {
            std::optional<KnownValue> joined;
            for (const auto &[reg, predecessor] : instruction.phiPreds) {
                if (!admits(predecessor)) {
                    continue;
                }
                KnownValue incoming = Resolve(values, reg);
                if (joined && !SameValue(*joined, incoming)) {
                    return std::nullopt;
                }
                joined = incoming;
            }
            return joined;
        };
        // Over every predecessor first: a value the phi holds whichever way control arrives is safe to name from this
        // block, because whatever defines it dominates each predecessor and therefore the join.
        auto joined = join([](BlockIndex) { return true; });
        if (!joined) {
            // Over the live predecessors only, a constant is still safe: it is written in place and names nothing. A
            // copy is not, because until CFG cleanup has removed the dead predecessor the register it names does not
            // dominate this block.
            joined = join([&](const BlockIndex predecessor) { return edges.IsLive(predecessor, blockIndex); });
            if (joined && !joined->constant) {
                return std::nullopt;
            }
        }
        if (joined && joined->copy == instruction.dst) {
            return std::nullopt;
        }
        return joined;
    }
    if (instruction.op == LirOpcode::RotateRight && instruction.srcs.size() == 1) {
        const KnownValue operand = Resolve(values, instruction.srcs[0]);
        const auto amount = ParseConstant(instruction.strArg, TypeRef::MakeUInt32());
        if (operand.constant && amount && amount->RawBits() < operand.constant->Width() &&
            (instruction.type->kind == TypeRef::Kind::UInt32 || instruction.type->kind == TypeRef::Kind::UInt64)) {
            const auto bits = instruction.type->kind == TypeRef::Kind::UInt32
                                ? std::rotr(static_cast<std::uint32_t>(operand.constant->RawBits()),
                                            static_cast<int>(amount->RawBits()))
                                : std::rotr(operand.constant->RawBits(), static_cast<int>(amount->RawBits()));
            return KnownValue{ParseConstant(std::to_string(bits), instruction.type), std::nullopt};
        }
        return std::nullopt;
    }
    if (const auto token = UnaryToken(instruction.op); token && instruction.srcs.size() == 1) {
        const KnownValue operand = Resolve(values, instruction.srcs[0]);
        // A LIR negation wraps, as subtraction from zero does.
        if (operand.constant && instruction.op == LirOpcode::Neg &&
            (operand.constant->GetKind() == TypedConstant::Kind::SignedInteger ||
             operand.constant->GetKind() == TypedConstant::Kind::UnsignedInteger)) {
            if (const auto zero = ParseConstant("0", operand.constant->Type())) {
                if (auto result = EvaluateBinary(TokenKind::MinusPercent, *zero, *operand.constant)) {
                    return KnownValue{result, std::nullopt};
                }
            }
            return std::nullopt;
        }
        if (operand.constant) {
            if (auto result = EvaluateUnary(*token, *operand.constant)) {
                return KnownValue{result, std::nullopt};
            }
        }
        return std::nullopt;
    }
    if (const auto token = BinaryToken(instruction.op); token && instruction.srcs.size() == 2) {
        const KnownValue left = Resolve(values, instruction.srcs[0]);
        const KnownValue right = Resolve(values, instruction.srcs[1]);
        if (left.constant && right.constant) {
            if (auto result = EvaluateBinary(*token, *left.constant, *right.constant)) {
                return KnownValue{result, std::nullopt};
            }
        }

        const bool sameOperand =
            (instruction.srcs[0] == instruction.srcs[1]) || (left.copy && right.copy && *left.copy == *right.copy);
        const LirReg leftReg = left.copy ? *left.copy : instruction.srcs[0];
        const LirReg rightReg = right.copy ? *right.copy : instruction.srcs[1];

        // Self-identity & idempotence on same operand (for integers / booleans). A comparison's own type is always a
        // bool, so its operands are asked separately: a NaN is unequal to itself, which `x != x` is how one is found.
        const bool floatOperand =
            floatRegisters.contains(instruction.srcs[0]) || floatRegisters.contains(instruction.srcs[1]);
        if (sameOperand && !floatOperand && (instruction.type.IsInteger() || instruction.type.IsBool())) {
            switch (instruction.op) {
            case LirOpcode::Sub:
            case LirOpcode::CheckedSub:
            case LirOpcode::Xor:
                if (auto zero = ParseConstant("0", instruction.type)) {
                    return KnownValue{zero, std::nullopt};
                }
                break;
            case LirOpcode::And:
            case LirOpcode::Or:
                return KnownValue{std::nullopt, leftReg};
            case LirOpcode::CmpEq:
            case LirOpcode::CmpLe:
            case LirOpcode::CmpGe:
                if (auto t = ParseConstant("true", TypeRef::MakeBool())) {
                    return KnownValue{t, std::nullopt};
                }
                break;
            case LirOpcode::CmpNe:
            case LirOpcode::CmpLt:
            case LirOpcode::CmpGt:
                if (auto f = ParseConstant("false", TypeRef::MakeBool())) {
                    return KnownValue{f, std::nullopt};
                }
                break;
            default:
                break;
            }
        }

        // Identity with constant 0 / 1
        if (instruction.type.IsInteger() || instruction.type.IsBool()) {
            const bool rightIsZero = right.constant && right.constant->Bits().IsZero();
            const bool leftIsZero = left.constant && left.constant->Bits().IsZero();
            const bool rightIsOne =
                right.constant && right.constant->Bits() == WideInteger::FromUnsigned(1, right.constant->Width());
            const bool leftIsOne =
                left.constant && left.constant->Bits() == WideInteger::FromUnsigned(1, left.constant->Width());

            switch (instruction.op) {
            case LirOpcode::Add:
            case LirOpcode::CheckedAdd:
                if (rightIsZero)
                    return KnownValue{std::nullopt, leftReg};
                if (leftIsZero)
                    return KnownValue{std::nullopt, rightReg};
                break;
            case LirOpcode::Sub:
            case LirOpcode::CheckedSub:
                if (rightIsZero)
                    return KnownValue{std::nullopt, leftReg};
                break;
            case LirOpcode::Mul:
            case LirOpcode::CheckedMul:
                if (rightIsZero || leftIsZero) {
                    if (auto zero = ParseConstant("0", instruction.type)) {
                        return KnownValue{zero, std::nullopt};
                    }
                }
                if (rightIsOne)
                    return KnownValue{std::nullopt, leftReg};
                if (leftIsOne)
                    return KnownValue{std::nullopt, rightReg};
                break;
            case LirOpcode::Div:
                if (rightIsOne)
                    return KnownValue{std::nullopt, leftReg};
                break;
            case LirOpcode::And:
                if (rightIsZero || leftIsZero) {
                    if (auto zero = ParseConstant("0", instruction.type)) {
                        return KnownValue{zero, std::nullopt};
                    }
                }
                break;
            case LirOpcode::Or:
            case LirOpcode::Xor:
                if (rightIsZero)
                    return KnownValue{std::nullopt, leftReg};
                if (leftIsZero)
                    return KnownValue{std::nullopt, rightReg};
                break;
            case LirOpcode::Shl:
            case LirOpcode::Shr:
            case LirOpcode::Lshr:
                if (rightIsZero)
                    return KnownValue{std::nullopt, leftReg};
                if (leftIsZero) {
                    if (auto zero = ParseConstant("0", instruction.type)) {
                        return KnownValue{zero, std::nullopt};
                    }
                }
                break;
            default:
                break;
            }
        }
        return std::nullopt;
    }
    if (instruction.op == LirOpcode::Cast && instruction.srcs.size() == 1) {
        const KnownValue source = Resolve(values, instruction.srcs[0]);
        if (source.constant) {
            if (auto result = CastConstant(*source.constant, instruction.type)) {
                return KnownValue{result, std::nullopt};
            }
        }
    }
    return std::nullopt;
}

/// Work out what each register is known to hold, over the whole function before anything is rewritten. Separating the
/// two means the rewrite never reads facts it has already invalidated.
///
/// The facts only accumulate: a register goes from unknown to known and a known value never changes, so the edges the
/// facts prove dead only ever grow, and stopping at any point leaves nothing unsound behind.
Values Analyze(const LirFunc &function) {
    Values values;
    std::size_t definitions = function.params.size();
    std::unordered_set<LirReg> floatRegisters;
    for (const auto &parameter : function.params) {
        if (parameter.type.IsFloat()) {
            floatRegisters.insert(parameter.reg);
        }
    }
    for (const auto &block : function.blocks) {
        definitions += std::ranges::count_if(block.instrs,
                                             [](const LirInstr &instruction) { return instruction.dst != LirNoReg; });
        for (const auto &instruction : block.instrs) {
            if (instruction.dst != LirNoReg && instruction.type.IsFloat()) {
                floatRegisters.insert(instruction.dst);
            }
        }
    }

    for (std::size_t iteration = 0; iteration <= definitions; ++iteration) {
        const LiveEdges edges(function, values);
        bool changed = false;
        for (BlockIndex blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
            for (const auto &instruction : function.blocks[blockIndex].instrs) {
                if (instruction.dst == LirNoReg) {
                    continue;
                }
                const auto evaluated = Evaluate(instruction, values, edges, blockIndex, floatRegisters);
                const auto previous = values.find(instruction.dst);
                if (evaluated && (previous == values.end() || !SameValue(previous->second, *evaluated))) {
                    values[instruction.dst] = *evaluated;
                    changed = true;
                }
            }
        }
        if (!changed) {
            break;
        }
    }
    return values;
}

/// Whether an operand of this opcode may be replaced by an equal value. Restricted to instructions that only read their
/// sources: substituting into anything with an effect could change what the effect acts on.
bool IsPureUser(const LirOpcode opcode) {
    return UnaryToken(opcode).has_value() || BinaryToken(opcode).has_value() || opcode == LirOpcode::Cast ||
           opcode == LirOpcode::FieldPtr || opcode == LirOpcode::IndexPtr || opcode == LirOpcode::Phi;
}

bool RewriteCopy(LirReg &reg, const Values &values) {
    const KnownValue value = Resolve(values, reg);
    if (!value.copy || *value.copy == reg) {
        return false;
    }
    reg = *value.copy;
    return true;
}

bool RewriteFunction(LirFunc &function, const Values &values) {
    bool changed = false;
    for (auto &block : function.blocks) {
        for (auto &instruction : block.instrs) {
            if (instruction.op != LirOpcode::Const) {
                const auto value = values.find(instruction.dst);
                if (value != values.end() && value->second.constant) {
                    LirInstr constant;
                    constant.dst = instruction.dst;
                    constant.type = value->second.constant->Type();
                    constant.op = LirOpcode::Const;
                    constant.strArg = value->second.constant->ToLiteral();
                    instruction = std::move(constant);
                    changed = true;
                    continue;
                }
            }
            // A checked operation known to equal one of its operands cannot overflow, so it no longer needs its check
            // and dead-code elimination may remove it once its readers use the operand.
            if (IsOverflowCheckedOpcode(instruction.op)) {
                const auto value = values.find(instruction.dst);
                if (value != values.end() && value->second.copy) {
                    instruction.op = UncheckedOpcode(instruction.op);
                    instruction.strArg.clear();
                    changed = true;
                }
            }
            if (!IsPureUser(instruction.op)) {
                continue;
            }
            for (auto &source : instruction.srcs) {
                changed = RewriteCopy(source, values) || changed;
            }
            for (auto &[source, predecessor] : instruction.phiPreds) {
                static_cast<void>(predecessor);
                changed = RewriteCopy(source, values) || changed;
            }
        }
        if (block.term) {
            if (block.term->kind == LirTermKind::Branch || block.term->kind == LirTermKind::Switch) {
                changed = RewriteCopy(block.term->cond, values) || changed;
            }
            if (block.term->retVal) {
                changed = RewriteCopy(*block.term->retVal, values) || changed;
            }
        }
    }
    return changed;
}

/// The largest value an integer register of at most 64 bits can hold, when it is also known never to be negative.
std::optional<std::uint64_t> KnownMaximum(const LirReg reg,
                                          const std::unordered_map<LirReg, const LirInstr *> &definitions,
                                          const std::unordered_map<LirReg, TypeRef> &types,
                                          const std::uint32_t depth = 0) {
    const auto found = definitions.find(reg);
    if (found == definitions.end() || depth > 8) {
        return std::nullopt;
    }
    const LirInstr &instruction = *found->second;
    const auto width = static_cast<std::uint32_t>(instruction.type.SizeInBytes().value_or(0) * 8);
    if (!instruction.type.IsInteger() || width == 0 || width > 64) {
        return std::nullopt;
    }
    const bool isSigned = instruction.type.IsSigned();
    const std::uint32_t room = isSigned ? width - 1 : width;
    const std::uint64_t typeMaximum = room == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << room) - 1;
    const auto constant = [&](const LirReg source) -> std::optional<std::uint64_t> {
        const auto definition = definitions.find(source);
        if (definition == definitions.end() || definition->second->op != LirOpcode::Const) {
            return std::nullopt;
        }
        const auto value = ParseConstant(definition->second->strArg, definition->second->type);
        if (!value || value->Width() > 64 ||
            (value->GetKind() == TypedConstant::Kind::SignedInteger && value->Bits().IsNegative())) {
            return std::nullopt;
        }
        return value->RawBits();
    };
    const auto operand = [&](const std::size_t index) -> std::optional<std::uint64_t> {
        return index < instruction.srcs.size() ? KnownMaximum(instruction.srcs[index], definitions, types, depth + 1)
                                               : std::nullopt;
    };
    // An unsigned value bounded by nothing else is bounded by its type.
    const auto orType = [&](const std::optional<std::uint64_t> bound) -> std::optional<std::uint64_t> {
        if (bound) {
            return bound;
        }
        return isSigned ? std::nullopt : std::optional{typeMaximum};
    };
    // All ones up to the highest bit `value` sets, which bounds an or or an exclusive-or of values up to it.
    const auto filled = [](const std::uint64_t value) -> std::uint64_t {
        return value == 0 ? 0 : ~std::uint64_t{0} >> std::countl_zero(value);
    };
    switch (instruction.op) {
    case LirOpcode::Const:
        return constant(instruction.dst);
    case LirOpcode::Cast: {
        // A widening holds no more than its source when the source is never negative.
        const auto source = types.find(instruction.srcs.empty() ? LirNoReg : instruction.srcs[0]);
        if (source == types.end()) {
            return orType(std::nullopt);
        }
        const TypeRef &from = source->second;
        const auto fromBytes = from.SizeInBytes();
        if (!(from.IsInteger() || from.IsChar()) || !fromBytes || *fromBytes * 8 > width) {
            return orType(std::nullopt);
        }
        if (const auto bound = KnownMaximum(instruction.srcs[0], definitions, types, depth + 1)) {
            return bound;
        }
        if (from.IsInteger() && !from.IsSigned() && *fromBytes * 8 < width) {
            return (std::uint64_t{1} << (*fromBytes * 8)) - 1;
        }
        return orType(std::nullopt);
    }
    case LirOpcode::And: {
        // Either operand known not negative bounds the result.
        const auto left = operand(0);
        const auto right = operand(1);
        if (left && right) {
            return std::min(*left, *right);
        }
        return orType(left ? left : right);
    }
    case LirOpcode::Add:
    case LirOpcode::CheckedAdd:
    case LirOpcode::Mul:
    case LirOpcode::CheckedMul:
    case LirOpcode::Or:
    case LirOpcode::Xor: {
        const auto left = operand(0);
        const auto right = operand(1);
        if (!left || !right) {
            return orType(std::nullopt);
        }
        const bool sum = instruction.op == LirOpcode::Add || instruction.op == LirOpcode::CheckedAdd;
        const bool product = instruction.op == LirOpcode::Mul || instruction.op == LirOpcode::CheckedMul;
        const unsigned __int128 exact = sum     ? static_cast<unsigned __int128>(*left) + *right
                                      : product ? static_cast<unsigned __int128>(*left) * *right
                                                : filled(std::max(*left, *right));
        if (exact <= typeMaximum) {
            return static_cast<std::uint64_t>(exact);
        }
        // A checked result past the maximum stops the program; a wrapping one can be anything.
        return IsOverflowCheckedOpcode(instruction.op) && !isSigned ? std::optional{typeMaximum} : orType(std::nullopt);
    }
    case LirOpcode::Sub:
    case LirOpcode::CheckedSub: {
        // An unsigned difference that did not wrap is at most its left operand. A checked one never wraps, and a plain
        // one does not when its left operand is a constant no smaller than the right one can be.
        if (isSigned) {
            return std::nullopt;
        }
        if (instruction.op == LirOpcode::CheckedSub) {
            return orType(operand(0));
        }
        const auto minuend = constant(instruction.srcs.at(0));
        const auto right = operand(1);
        return minuend && right && *right <= *minuend ? minuend : orType(std::nullopt);
    }
    case LirOpcode::Mod: {
        // An unsigned remainder is below its divisor and at most its dividend.
        if (isSigned) {
            return std::nullopt;
        }
        std::optional<std::uint64_t> bound = operand(0);
        if (const auto right = operand(1); right && *right > 0) {
            bound = bound ? std::min(*bound, *right - 1) : *right - 1;
        }
        return orType(bound);
    }
    case LirOpcode::Lshr:
    case LirOpcode::Shr: {
        const auto count = instruction.srcs.size() == 2 ? constant(instruction.srcs[1]) : std::nullopt;
        if (isSigned) {
            // Only a logical shift by at least one clears a signed value's top bit.
            if (instruction.op != LirOpcode::Lshr || !count || *count == 0 || *count >= width) {
                return std::nullopt;
            }
            return (~std::uint64_t{0} >> (64 - width)) >> *count;
        }
        const auto shifted = orType(operand(0));
        if (shifted && count && *count < width) {
            return *shifted >> *count;
        }
        return shifted;
    }
    default:
        return orType(std::nullopt);
    }
}

/// Whether control reaching `block` has seen `left < right` hold, through a branch on a comparison that dominates it.
bool KnownLess(const LirFunc &function, const LirControlFlow &cfg, const LirDominance &dominance,
               const std::unordered_map<LirReg, const LirInstr *> &definitions,
               const std::unordered_map<LirReg, TypeRef> &types, std::uint32_t block, const LirReg left) {
    const auto immediate = dominance.ImmediateDominators();
    for (std::uint32_t depth = 0; depth < 64 && block < immediate.size(); ++depth) {
        const auto predecessors = cfg.predecessors[block];
        if (predecessors.size() == 1) {
            const auto &term = function.blocks[predecessors[0]].term;
            if (term && term->kind == LirTermKind::Branch && term->trueTarget != term->falseTarget) {
                const auto condition = definitions.find(term->cond);
                if (condition != definitions.end()) {
                    const LirInstr &comparison = *condition->second;
                    const bool taken = term->trueTarget == block;
                    // `left < right` when taken, or `left >= right` failing; the same from the other side.
                    const bool less = comparison.srcs.size() == 2 &&
                                      ((taken && comparison.op == LirOpcode::CmpLt && comparison.srcs[0] == left) ||
                                       (taken && comparison.op == LirOpcode::CmpGt && comparison.srcs[1] == left) ||
                                       (!taken && comparison.op == LirOpcode::CmpGe && comparison.srcs[0] == left) ||
                                       (!taken && comparison.op == LirOpcode::CmpLe && comparison.srcs[1] == left));
                    if (less) {
                        const auto other =
                            types.find(comparison.srcs[0] == left ? comparison.srcs[1] : comparison.srcs[0]);
                        const auto self = types.find(left);
                        // Both sides at one type, so the bound is a value of the type the step is taken in.
                        if (other != types.end() && self != types.end() && other->second == self->second) {
                            return true;
                        }
                    }
                }
            }
        }
        if (block == 0 || immediate[block] == LirDominance::Invalid || immediate[block] == block) {
            return false;
        }
        block = immediate[block];
    }
    return false;
}

/// Whether control reaching `block` has seen `value` differ from zero, through a branch that dominates it on a
/// comparison of `value`, or of the unsigned value it zero-extends, with zero.
bool KnownNonZero(const LirFunc &function, const LirControlFlow &cfg, const LirDominance &dominance,
                  const std::unordered_map<LirReg, const LirInstr *> &definitions,
                  const std::unordered_map<LirReg, TypeRef> &types, std::uint32_t block, const LirReg value) {
    std::vector<LirReg> names{value};
    if (const auto found = definitions.find(value);
        found != definitions.end() && found->second->op == LirOpcode::Cast && found->second->srcs.size() == 1) {
        const auto source = types.find(found->second->srcs[0]);
        const auto from = source == types.end() ? std::nullopt : source->second.SizeInBytes();
        const auto to = found->second->type.SizeInBytes();
        if (source != types.end() && source->second.IsInteger() && !source->second.IsSigned() && from && to &&
            *from <= *to)
            names.push_back(found->second->srcs[0]);
    }
    const auto zero = [&](const LirReg reg) {
        const auto found = definitions.find(reg);
        return found != definitions.end() && found->second->op == LirOpcode::Const &&
               (found->second->strArg == "0" || found->second->strArg == "0x0");
    };
    const auto immediate = dominance.ImmediateDominators();
    for (std::uint32_t depth = 0; depth < 64 && block < immediate.size(); ++depth) {
        const auto predecessors = cfg.predecessors[block];
        if (predecessors.size() == 1) {
            const auto &term = function.blocks[predecessors[0]].term;
            if (term && term->kind == LirTermKind::Branch && term->trueTarget != term->falseTarget) {
                const auto condition = definitions.find(term->cond);
                if (condition != definitions.end() && condition->second->srcs.size() == 2) {
                    const LirInstr &comparison = *condition->second;
                    const bool taken = term->trueTarget == block;
                    const LirReg left = comparison.srcs[0];
                    const LirReg right = comparison.srcs[1];
                    const auto named = [&](const LirReg reg) { return std::ranges::contains(names, reg); };
                    const bool against = (named(left) && zero(right)) || (named(right) && zero(left));
                    if (against && ((comparison.op == LirOpcode::CmpEq && !taken) ||
                                    (comparison.op == LirOpcode::CmpNe && taken))) {
                        return true;
                    }
                }
            }
        }
        if (block == 0 || immediate[block] == LirDominance::Invalid || immediate[block] == block) {
            return false;
        }
        block = immediate[block];
    }
    return false;
}

/// The bound on an unsigned `value` that control reaching `block` has seen it compare against a constant, through a
/// branch that dominates it: the largest value it can hold when `upper`, the smallest otherwise.
std::optional<std::uint64_t> BranchBound(const LirFunc &function, const LirControlFlow &cfg,
                                         const LirDominance &dominance,
                                         const std::unordered_map<LirReg, const LirInstr *> &definitions,
                                         std::uint32_t block, const LirReg value, const bool upper) {
    const auto constant = [&](const LirReg reg) -> std::optional<std::uint64_t> {
        const auto definition = definitions.find(reg);
        if (definition == definitions.end() || definition->second->op != LirOpcode::Const) {
            return std::nullopt;
        }
        const auto parsed = ParseConstant(definition->second->strArg, definition->second->type);
        if (!parsed || parsed->Width() > 64 || parsed->GetKind() != TypedConstant::Kind::UnsignedInteger) {
            return std::nullopt;
        }
        return parsed->RawBits();
    };
    const auto immediate = dominance.ImmediateDominators();
    for (std::uint32_t depth = 0; depth < 64 && block < immediate.size(); ++depth) {
        const auto predecessors = cfg.predecessors[block];
        if (predecessors.size() == 1) {
            const auto &term = function.blocks[predecessors[0]].term;
            if (term && term->kind == LirTermKind::Branch && term->trueTarget != term->falseTarget) {
                const auto condition = definitions.find(term->cond);
                if (condition != definitions.end() && condition->second->srcs.size() == 2) {
                    const LirInstr &comparison = *condition->second;
                    const bool taken = term->trueTarget == block;
                    // What held on this edge, as `value` against a constant: the comparison, negated on the false
                    // edge, and mirrored when the value is on the right.
                    LirOpcode op = comparison.op;
                    std::optional<std::uint64_t> limit;
                    if (comparison.srcs[0] == value) {
                        limit = constant(comparison.srcs[1]);
                    }
                    else if (comparison.srcs[1] == value) {
                        limit = constant(comparison.srcs[0]);
                        op = op == LirOpcode::CmpLt ? LirOpcode::CmpGt
                           : op == LirOpcode::CmpLe ? LirOpcode::CmpGe
                           : op == LirOpcode::CmpGt ? LirOpcode::CmpLt
                           : op == LirOpcode::CmpGe ? LirOpcode::CmpLe
                                                    : op;
                    }
                    if (limit && !taken) {
                        op = op == LirOpcode::CmpLt ? LirOpcode::CmpGe
                           : op == LirOpcode::CmpLe ? LirOpcode::CmpGt
                           : op == LirOpcode::CmpGt ? LirOpcode::CmpLe
                           : op == LirOpcode::CmpGe ? LirOpcode::CmpLt
                                                    : LirOpcode::CmpEq;
                    }
                    if (limit) {
                        if (upper && op == LirOpcode::CmpLt && *limit > 0) {
                            return *limit - 1;
                        }
                        if (upper && op == LirOpcode::CmpLe) {
                            return *limit;
                        }
                        if (!upper && op == LirOpcode::CmpGe) {
                            return *limit;
                        }
                        if (!upper && op == LirOpcode::CmpGt && *limit < ~std::uint64_t{0}) {
                            return *limit + 1;
                        }
                    }
                }
            }
        }
        if (block == 0 || immediate[block] == LirDominance::Invalid || immediate[block] == block) {
            return std::nullopt;
        }
        block = immediate[block];
    }
    return std::nullopt;
}

/// The largest constant a step from a known index may add: a slice length stays this far below the signed limit,
/// since no slice in an address space of at most 2^63 bytes holds that many elements of even one byte.
constexpr std::uint64_t MaximumIndexStep = std::uint64_t{1} << 16;

/// What a dominating comparison shows about a value: nothing, that it is not negative and below some unsigned bound,
/// or that it is below the length of a slice.
enum class IndexFact {
    None,
    BelowBound,
    BelowLength
};

/// Whether control reaching `block` has seen `value`, read as unsigned at its own width, compare below a bound, as a
/// bounds check does just before an index is used.
IndexFact KnownIndex(const LirFunc &function, const LirControlFlow &cfg, const LirDominance &dominance,
                     const std::unordered_map<LirReg, const LirInstr *> &definitions,
                     const std::unordered_map<LirReg, TypeRef> &types, std::uint32_t block, const LirReg value) {
    const auto self = types.find(value);
    if (self == types.end() || !self->second.IsInteger()) {
        return IndexFact::None;
    }
    // The value itself when it is unsigned, and every unsigned reading of it at the same width.
    const auto reads = [&](const LirReg reg) {
        if (reg == value) {
            return !self->second.IsSigned();
        }
        const auto found = definitions.find(reg);
        return found != definitions.end() && found->second->op == LirOpcode::Cast && found->second->srcs.size() == 1 &&
               found->second->srcs[0] == value && found->second->type.IsInteger() && !found->second->type.IsSigned() &&
               found->second->type.SizeInBytes() == self->second.SizeInBytes();
    };
    const auto length = [&](const LirReg reg) {
        const auto load = definitions.find(reg);
        if (load == definitions.end() || load->second->op != LirOpcode::Load || load->second->srcs.size() != 1) {
            return false;
        }
        const auto field = definitions.find(load->second->srcs[0]);
        return field != definitions.end() && field->second->op == LirOpcode::FieldPtr &&
               field->second->strArg == "length" && load->second->type.SizeInBytes() == self->second.SizeInBytes();
    };
    const auto immediate = dominance.ImmediateDominators();
    for (std::uint32_t depth = 0; depth < 64 && block < immediate.size(); ++depth) {
        const auto predecessors = cfg.predecessors[block];
        if (predecessors.size() == 1) {
            const auto &term = function.blocks[predecessors[0]].term;
            if (term && term->kind == LirTermKind::Branch && term->trueTarget != term->falseTarget) {
                const auto condition = definitions.find(term->cond);
                if (condition != definitions.end() && condition->second->srcs.size() == 2) {
                    const LirInstr &comparison = *condition->second;
                    const bool taken = term->trueTarget == block;
                    LirReg bound = LirNoReg;
                    if ((taken && comparison.op == LirOpcode::CmpLt) || (!taken && comparison.op == LirOpcode::CmpGe)) {
                        bound = reads(comparison.srcs[0]) ? comparison.srcs[1] : LirNoReg;
                    }
                    else if ((taken && comparison.op == LirOpcode::CmpGt) ||
                             (!taken && comparison.op == LirOpcode::CmpLe)) {
                        bound = reads(comparison.srcs[1]) ? comparison.srcs[0] : LirNoReg;
                    }
                    const auto boundType = types.find(bound);
                    if (bound != LirNoReg && boundType != types.end() && boundType->second.IsInteger() &&
                        !boundType->second.IsSigned()) {
                        return length(bound) ? IndexFact::BelowLength : IndexFact::BelowBound;
                    }
                }
            }
        }
        if (block == 0 || immediate[block] == LirDominance::Invalid || immediate[block] == block) {
            return IndexFact::None;
        }
        block = immediate[block];
    }
    return IndexFact::None;
}

/// Turns each overflow check that cannot fire into the plain operation, so dead-code elimination and the loop passes
/// treat it as ordinary arithmetic: a sum or product of operands whose known bits leave room for the result, and a
/// step of one up from a value known to be below another of its type, which is how a loop counter guarded by its
/// bound advances.
bool DemoteProvenChecks(LirFunc &function) {
    std::unordered_map<LirReg, const LirInstr *> definitions;
    std::unordered_map<LirReg, TypeRef> types;
    std::unordered_map<LirReg, std::uint32_t> blocks;
    bool any = false;
    for (const auto &parameter : function.params) {
        types.emplace(parameter.reg, parameter.type);
    }
    for (std::uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        for (const auto &instruction : function.blocks[blockIndex].instrs) {
            if (instruction.dst != LirNoReg) {
                definitions.emplace(instruction.dst, &instruction);
                types.emplace(instruction.dst, instruction.type);
                blocks.emplace(instruction.dst, blockIndex);
            }
            any = any || IsOverflowCheckedOpcode(instruction.op);
        }
    }
    if (!any) {
        return false;
    }
    std::optional<LirControlFlow> cfg;
    std::optional<LirDominance> dominance;
    const auto analyze = [&] {
        if (!cfg) {
            cfg.emplace(function);
            dominance.emplace(*cfg);
        }
    };
    const auto constant = [&](const LirReg reg) -> std::optional<std::uint64_t> {
        const auto definition = definitions.find(reg);
        if (definition == definitions.end() || definition->second->op != LirOpcode::Const) {
            return std::nullopt;
        }
        const auto value = ParseConstant(definition->second->strArg, definition->second->type);
        if (!value || value->Width() > 64 ||
            (value->GetKind() == TypedConstant::Kind::SignedInteger && value->Bits().IsNegative())) {
            return std::nullopt;
        }
        return value->RawBits();
    };
    // The least value a loop counter takes: its starting constants, when every other way into the phi steps the
    // counter up by a constant without wrapping, because the step traps or a bound the counter is below guards it.
    const auto lowerBound = [&](const LirReg reg) -> std::optional<std::uint64_t> {
        if (const auto value = constant(reg)) {
            return value;
        }
        const auto phi = definitions.find(reg);
        if (phi == definitions.end() || phi->second->op != LirOpcode::Phi) {
            return std::nullopt;
        }
        std::optional<std::uint64_t> least;
        for (const auto &[incoming, predecessor] : phi->second->phiPreds) {
            static_cast<void>(predecessor);
            if (const auto start = constant(incoming)) {
                least = least ? std::min(*least, *start) : *start;
                continue;
            }
            const auto step = definitions.find(incoming);
            if (step == definitions.end() || step->second->srcs.size() != 2 ||
                (step->second->op != LirOpcode::Add && step->second->op != LirOpcode::CheckedAdd)) {
                return std::nullopt;
            }
            const auto &sources = step->second->srcs;
            const bool fromCounter =
                (sources[0] == reg && constant(sources[1])) || (sources[1] == reg && constant(sources[0]));
            if (!fromCounter) {
                return std::nullopt;
            }
            if (step->second->op == LirOpcode::Add) {
                analyze();
                if (!KnownLess(function, *cfg, *dominance, definitions, types, blocks.at(incoming), reg)) {
                    return std::nullopt;
                }
            }
        }
        return least;
    };
    bool changed = false;
    for (std::uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        for (auto &instruction : function.blocks[blockIndex].instrs) {
            if (!IsOverflowCheckedOpcode(instruction.op) || instruction.srcs.size() != 2) {
                continue;
            }
            const auto width = static_cast<std::uint32_t>(instruction.type.SizeInBytes().value_or(0) * 8);
            const std::uint32_t room = instruction.type.IsSigned() ? width - 1 : width;
            const std::uint64_t typeMaximum = room == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << room) - 1;
            // The tighter of what an operand's definition and a dominating comparison with a constant show.
            const auto bounded = [&](const LirReg reg) -> std::optional<std::uint64_t> {
                auto bound = KnownMaximum(reg, definitions, types);
                const auto type = types.find(reg);
                if (type == types.end() || type->second.IsSigned() || (bound && *bound < typeMaximum)) {
                    return bound;
                }
                analyze();
                if (const auto below = BranchBound(function, *cfg, *dominance, definitions, blockIndex, reg, true)) {
                    bound = bound ? std::min(*bound, *below) : *below;
                }
                return bound;
            };
            const auto left = bounded(instruction.srcs[0]);
            const auto right = bounded(instruction.srcs[1]);
            bool safe = false;
            if (left && right) {
                // Two values that are not negative cannot pass the maximum their bounds stay below, and their
                // difference always fits a signed type.
                safe = instruction.op == LirOpcode::CheckedAdd
                         ? static_cast<unsigned __int128>(*left) + *right <= typeMaximum
                     : instruction.op == LirOpcode::CheckedMul
                         ? static_cast<unsigned __int128>(*left) * *right <= typeMaximum
                         : instruction.type.IsSigned();
            }
            if (!safe && instruction.op == LirOpcode::CheckedSub && right) {
                // A value, or a counter, that never falls below the most that is taken from it.
                auto least = lowerBound(instruction.srcs[0]);
                if (!instruction.type.IsSigned() && (!least || *least < *right)) {
                    analyze();
                    least =
                        BranchBound(function, *cfg, *dominance, definitions, blockIndex, instruction.srcs[0], false);
                }
                safe = least && *least >= *right;
                if (!safe && *right <= 1 && !instruction.type.IsSigned()) {
                    // Taking one from a value a dominating test showed is not zero.
                    analyze();
                    safe =
                        KnownNonZero(function, *cfg, *dominance, definitions, types, blockIndex, instruction.srcs[0]);
                }
            }
            if (!safe && instruction.op == LirOpcode::CheckedAdd) {
                const auto one = [&](const LirReg reg) {
                    const auto definition = definitions.find(reg);
                    return definition != definitions.end() && definition->second->op == LirOpcode::Const &&
                           definition->second->strArg == "1";
                };
                const LirReg counter = one(instruction.srcs[1]) ? instruction.srcs[0]
                                     : one(instruction.srcs[0]) ? instruction.srcs[1]
                                                                : LirNoReg;
                if (counter != LirNoReg) {
                    analyze();
                    safe = KnownLess(function, *cfg, *dominance, definitions, types, blockIndex, counter);
                }
            }
            if (!safe && (instruction.op == LirOpcode::CheckedAdd || instruction.op == LirOpcode::CheckedSub)) {
                // A step from a value that just passed a bounds check, which shows it is a valid index: not negative
                // and below a length, which no slice in memory brings near the signed limit.
                const auto amount = constant(instruction.srcs[1]);
                if (amount && *amount <= MaximumIndexStep) {
                    analyze();
                    const auto index =
                        KnownIndex(function, *cfg, *dominance, definitions, types, blockIndex, instruction.srcs[0]);
                    const bool isSigned = instruction.type.IsSigned();
                    safe = instruction.op == LirOpcode::CheckedSub
                             ? isSigned && index != IndexFact::None
                             : index == IndexFact::BelowLength ||
                                   (index == IndexFact::BelowBound && !isSigned && *amount == 1);
                }
            }
            if (safe) {
                instruction.op = UncheckedOpcode(instruction.op);
                instruction.strArg.clear();
                changed = true;
            }
        }
    }
    return changed;
}
} // namespace

std::string_view LirConstantPropagation::Name() const noexcept {
    return "lir-constant-copy-propagation";
}

PassChange LirConstantPropagation::RunFunction(LirFunc &function, const std::size_t, const PassContext &) const {
    if (function.isExtern || function.isAsm)
        return PassChange::None;
    const bool rewritten = RewriteFunction(function, Analyze(function));
    const bool demoted = DemoteProvenChecks(function);
    return rewritten || demoted ? PassChange::Changed : PassChange::None;
}
} // namespace Rux::Optimization
