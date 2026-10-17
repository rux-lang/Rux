#include "CodeGen/Machine/RegisterAllocator.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <memory>
#include <unordered_map>

namespace Rux::Machine {
namespace {
class BitSet {
public:
    BitSet() = default;

    explicit BitSet(const std::size_t size)
        : words((size + 63) / 64, 0) {
    }

    [[nodiscard]] bool Test(const std::size_t bit) const noexcept {
        return (words[bit / 64] >> (bit % 64)) & 1U;
    }

    void Set(const std::size_t bit) noexcept {
        words[bit / 64] |= std::uint64_t{1} << (bit % 64);
    }

    void Reset(const std::size_t bit) noexcept {
        words[bit / 64] &= ~(std::uint64_t{1} << (bit % 64));
    }

    /// this |= generated | (outgoing & ~killed): one step of backward liveness without a temporary set; returns
    /// whether anything changed.
    bool UnionTransfer(const BitSet &generated, const BitSet &outgoing, const BitSet &killed) noexcept {
        bool changed = false;
        for (std::size_t index = 0; index < words.size(); ++index) {
            const std::uint64_t next =
                words[index] | generated.words[index] | (outgoing.words[index] & ~killed.words[index]);
            changed = changed || next != words[index];
            words[index] = next;
        }
        return changed;
    }

    /// Becomes a copy of `other`, reusing this set's storage.
    void Assign(const BitSet &other) {
        words.assign(other.words.begin(), other.words.end());
    }

    bool Union(const BitSet &other) noexcept {
        bool changed = false;
        for (std::size_t index = 0; index < words.size(); ++index) {
            const std::uint64_t next = words[index] | other.words[index];
            changed = changed || next != words[index];
            words[index] = next;
        }
        return changed;
    }

    template <typename Visit>
    void ForEach(Visit visit) const {
        for (std::size_t index = 0; index < words.size(); ++index) {
            std::uint64_t word = words[index];
            while (word != 0) {
                const auto bit = static_cast<std::size_t>(std::countr_zero(word));
                visit(index * 64 + bit);
                word &= word - 1;
            }
        }
    }

private:
    std::vector<std::uint64_t> words;
};

/// A list of values per node, all kept in one growing run of fixed-size chunks, so a node's list grows without an
/// allocation of its own. Lists are read in the order they were written. A visitor may append to other nodes' lists:
/// every chunk is looked up again after each visit.
template <typename T>
class NodeLists {
public:
    void Reset(const std::size_t nodes) {
        head.assign(nodes, None);
        tail.assign(nodes, None);
        chunks.clear();
    }

    void Push(const std::size_t node, const T value) {
        std::uint32_t last = tail[node];
        if (last == None || chunks[last].count == ChunkSize) {
            const auto fresh = static_cast<std::uint32_t>(chunks.size());
            chunks.emplace_back();
            if (last == None)
                head[node] = fresh;
            else
                chunks[last].next = fresh;
            tail[node] = fresh;
            last = fresh;
        }
        auto &chunk = chunks[last];
        chunk.items[chunk.count++] = value;
    }

    /// Calls `visit` with each value of `node`'s list in order, stopping at the first call that returns true; returns
    /// whether one did.
    template <typename Visit>
    bool Find(const std::size_t node, Visit visit) const {
        for (std::uint32_t chunk = head[node]; chunk != None; chunk = chunks[chunk].next) {
            for (std::uint32_t item = 0; item < chunks[chunk].count; ++item) {
                const T value = chunks[chunk].items[item];
                if (visit(value))
                    return true;
            }
        }
        return false;
    }

    template <typename Visit>
    void ForEach(const std::size_t node, Visit visit) const {
        Find(node, [&](const T value) {
            visit(value);
            return false;
        });
    }

    /// Appends `from`'s list to `to`'s, which must be another node.
    void Append(const std::size_t to, const std::size_t from) {
        ForEach(from, [&](const T value) { Push(to, value); });
    }

private:
    static constexpr std::uint32_t None = std::numeric_limits<std::uint32_t>::max();
    static constexpr std::uint32_t ChunkSize = 6;

    struct Chunk {
        T items[ChunkSize]{};
        std::uint32_t count = 0;
        std::uint32_t next = None;
    };

    std::vector<Chunk> chunks;
    std::vector<std::uint32_t> head;
    std::vector<std::uint32_t> tail;
};

/// Interference between two registers of one class, stored as the lower triangle of a bit matrix over that class's
/// dense node numbering.
class TriangleMatrix {
public:
    explicit TriangleMatrix(const std::size_t nodes)
        : bits(nodes * (nodes + 1) / 2) {
    }

    [[nodiscard]] bool Test(std::size_t a, std::size_t b) const noexcept {
        if (a < b)
            std::swap(a, b);
        return bits.Test(a * (a + 1) / 2 + b);
    }

    void Set(std::size_t a, std::size_t b) noexcept {
        if (a < b)
            std::swap(a, b);
        bits.Set(a * (a + 1) / 2 + b);
    }

private:
    BitSet bits;
};

enum class NodeState : std::uint8_t {
    Precolored,
    Initial,
    Simplify,
    Freeze,
    Spill,
    Spilled,
    Coalesced,
    Colored,
    Stacked,
    Ignored
};

enum class MoveState : std::uint8_t {
    Worklist,
    Active,
    Coalesced,
    Constrained,
    Frozen
};

struct Move {
    Reg destination = NoReg;
    Reg source = NoReg;
    MoveState state = MoveState::Worklist;
};

class Allocator {
public:
    Allocator(Function &inputFunction, const TargetRegisters &inputTarget, const SpillCodeFactory &inputSpills)
        : function(inputFunction)
        , target(inputTarget)
        , spills(inputSpills) {
    }

    AllocationResult Run() {
        AllocationResult result;
        for (std::uint32_t round = 0; round < MaximumAllocatorRounds; ++round) {
            result.rounds = round + 1;
            if (PhysicalRegisterCount + function.VirtualCount() > MaximumAllocatorNodes + PhysicalRegisterCount)
                return result;
            Prepare();
            ComputeLiveness();
            Build();
            MakeWorklist();
            while (true) {
                if (!simplifyWorklist.empty())
                    Simplify();
                else if (!moveWorklist.empty())
                    Coalesce();
                else if (!freezeWorklist.empty())
                    Freeze();
                else if (!spillWorklist.empty())
                    SelectSpill();
                else
                    break;
                CompactWorklists();
            }
            AssignColors();
            if (spilledNodes.empty()) {
                result.usedCalleeSaved = Rewrite();
                result.succeeded = true;
                return result;
            }
            result.spilledRegisters += static_cast<std::uint32_t>(spilledNodes.size());
            RewriteSpills();
        }
        return result;
    }

private:
    Function &function;
    const TargetRegisters &target;
    const SpillCodeFactory &spills;

    std::size_t registerCount = 0;
    std::vector<NodeState> state;
    std::vector<std::uint32_t> degree;
    NodeLists<Reg> adjacency;
    NodeLists<std::uint32_t> moveList;
    std::vector<Reg> alias;
    std::vector<Reg> color;
    std::vector<double> spillCost;
    std::vector<std::size_t> denseIndex;
    std::array<std::size_t, 2> classSize{};
    std::array<std::unique_ptr<TriangleMatrix>, 2> interference;
    std::vector<Move> moves;
    std::vector<BitSet> liveOut;
    /// The live set `Build` walks each block with, kept so its storage is reused.
    BitSet live;
    /// The neighbours `Conservative` has counted, kept so its storage is reused.
    mutable std::vector<Reg> counted;

    std::vector<Reg> simplifyWorklist;
    std::vector<Reg> freezeWorklist;
    std::vector<Reg> spillWorklist;
    std::vector<std::uint32_t> moveWorklist;
    std::vector<Reg> selectStack;
    std::vector<Reg> spilledNodes;
    /// Registers created by spill rewriting. Their ranges are as short as they can be, so spilling them again could
    /// only loop.
    std::vector<bool> unspillable;

    [[nodiscard]] bool IsTracked(const Reg reg) const noexcept {
        if (IsVirtual(reg))
            return true;
        if (!IsPhysical(reg))
            return false;
        return ((target.classMask[0] | target.classMask[1]) & RegisterBit(reg)) != 0;
    }

    [[nodiscard]] RegClass ClassOf(const Reg reg) const {
        if (IsVirtual(reg))
            return function.VirtualClass(reg);
        return (target.classMask[1] & RegisterBit(reg)) != 0 ? RegClass::Vector : RegClass::General;
    }

    [[nodiscard]] std::uint32_t K(const Reg reg) const {
        return static_cast<std::uint32_t>(target.allocationOrder[static_cast<std::size_t>(ClassOf(reg))].size());
    }

    [[nodiscard]] bool Precolored(const Reg reg) const noexcept {
        return IsPhysical(reg);
    }

    void Mentions(const Instruction &instruction, std::vector<Reg> &uses, std::vector<Reg> &defs) const {
        uses.clear();
        defs.clear();
        for (const auto &operand : instruction.operands) {
            if (operand.kind == Operand::Kind::Register) {
                if (operand.use && IsTracked(operand.reg))
                    uses.push_back(operand.reg);
                if (operand.def && IsTracked(operand.reg))
                    defs.push_back(operand.reg);
            }
            else if (operand.kind == Operand::Kind::Memory) {
                if (operand.address.base != NoReg && IsTracked(operand.address.base))
                    uses.push_back(operand.address.base);
                if (operand.address.index != NoReg && IsTracked(operand.address.index))
                    uses.push_back(operand.address.index);
            }
        }
        for (std::uint64_t bits = instruction.implicitUses; bits != 0; bits &= bits - 1) {
            const auto reg = static_cast<Reg>(std::countr_zero(bits));
            if (IsTracked(reg))
                uses.push_back(reg);
        }
        for (std::uint64_t bits = instruction.implicitDefs; bits != 0; bits &= bits - 1) {
            const auto reg = static_cast<Reg>(std::countr_zero(bits));
            if (IsTracked(reg))
                defs.push_back(reg);
        }
    }

    [[nodiscard]] bool IsAllocatableCopy(const Instruction &instruction) const {
        if (!instruction.isCopy || instruction.operands.size() != 2)
            return false;
        const auto &destination = instruction.operands[0];
        const auto &source = instruction.operands[1];
        return destination.IsRegister() && source.IsRegister() && IsTracked(destination.reg) && IsTracked(source.reg) &&
               ClassOf(destination.reg) == ClassOf(source.reg);
    }

    void Prepare() {
        registerCount = PhysicalRegisterCount + function.VirtualCount();
        state.assign(registerCount, NodeState::Initial);
        degree.assign(registerCount, 0);
        adjacency.Reset(registerCount);
        moveList.Reset(registerCount);
        alias.assign(registerCount, NoReg);
        color.assign(registerCount, NoReg);
        spillCost.assign(registerCount, 0.0);
        unspillable.resize(registerCount, false);
        denseIndex.assign(registerCount, 0);
        classSize = {0, 0};
        for (Reg reg = 0; reg < registerCount; ++reg) {
            if (!IsTracked(reg)) {
                state[reg] = NodeState::Ignored;
                continue;
            }
            const auto regClass = static_cast<std::size_t>(ClassOf(reg));
            denseIndex[reg] = classSize[regClass]++;
            if (Precolored(reg)) {
                state[reg] = NodeState::Precolored;
                color[reg] = reg;
                degree[reg] = std::numeric_limits<std::uint32_t>::max() / 2;
            }
        }
        for (std::size_t regClass = 0; regClass < 2; ++regClass)
            interference[regClass] = std::make_unique<TriangleMatrix>(classSize[regClass]);
        moves.clear();
        simplifyWorklist.clear();
        freezeWorklist.clear();
        spillWorklist.clear();
        moveWorklist.clear();
        selectStack.clear();
        spilledNodes.clear();
    }

    void ComputeLiveness() {
        const auto blockCount = function.blocks.size();
        std::vector<BitSet> generated(blockCount, BitSet(registerCount));
        std::vector<BitSet> killed(blockCount, BitSet(registerCount));
        std::vector<Reg> uses;
        std::vector<Reg> defs;
        for (std::size_t block = 0; block < blockCount; ++block) {
            for (const auto &instruction : function.blocks[block].instructions) {
                Mentions(instruction, uses, defs);
                for (const auto reg : uses)
                    if (!killed[block].Test(reg))
                        generated[block].Set(reg);
                for (const auto reg : defs)
                    killed[block].Set(reg);
            }
        }
        std::vector<BitSet> liveIn(blockCount, BitSet(registerCount));
        liveOut.assign(blockCount, BitSet(registerCount));
        bool changed = true;
        while (changed) {
            changed = false;
            for (std::size_t reverse = blockCount; reverse > 0; --reverse) {
                const auto block = reverse - 1;
                for (const auto successor : function.blocks[block].successors)
                    liveOut[block].Union(liveIn[successor]);
                changed = liveIn[block].UnionTransfer(generated[block], liveOut[block], killed[block]) || changed;
            }
        }
    }

    void AddEdge(const Reg u, const Reg v) {
        if (u == v || ClassOf(u) != ClassOf(v))
            return;
        auto &matrix = *interference[static_cast<std::size_t>(ClassOf(u))];
        if (matrix.Test(denseIndex[u], denseIndex[v]))
            return;
        matrix.Set(denseIndex[u], denseIndex[v]);
        if (!Precolored(u)) {
            adjacency.Push(u, v);
            ++degree[u];
        }
        if (!Precolored(v)) {
            adjacency.Push(v, u);
            ++degree[v];
        }
    }

    [[nodiscard]] bool Interferes(const Reg u, const Reg v) const {
        if (ClassOf(u) != ClassOf(v))
            return false;
        return interference[static_cast<std::size_t>(ClassOf(u))]->Test(denseIndex[u], denseIndex[v]);
    }

    void Build() {
        std::vector<Reg> uses;
        std::vector<Reg> defs;
        for (std::size_t block = 0; block < function.blocks.size(); ++block) {
            const double weight = std::pow(10.0, std::min<std::uint32_t>(function.blocks[block].loopDepth, 6));
            live.Assign(liveOut[block]);
            const auto &instructions = function.blocks[block].instructions;
            for (std::size_t reverse = instructions.size(); reverse > 0; --reverse) {
                const auto &instruction = instructions[reverse - 1];
                Mentions(instruction, uses, defs);
                for (const auto reg : uses)
                    spillCost[reg] += weight;
                for (const auto reg : defs)
                    spillCost[reg] += weight;
                if (IsAllocatableCopy(instruction)) {
                    for (const auto reg : uses)
                        live.Reset(reg);
                    const auto index = static_cast<std::uint32_t>(moves.size());
                    moves.push_back({instruction.operands[0].reg, instruction.operands[1].reg, MoveState::Worklist});
                    moveList.Push(instruction.operands[0].reg, index);
                    moveList.Push(instruction.operands[1].reg, index);
                    moveWorklist.push_back(index);
                }
                for (const auto reg : defs)
                    live.Set(reg);
                for (const auto definition : defs)
                    live.ForEach([&](const std::size_t other) { AddEdge(static_cast<Reg>(other), definition); });
                for (const auto reg : defs)
                    live.Reset(reg);
                for (const auto reg : uses)
                    live.Set(reg);
            }
        }
    }

    [[nodiscard]] bool MoveRelated(const Reg reg) const {
        return moveList.Find(reg, [&](const std::uint32_t move) {
            return moves[move].state == MoveState::Worklist || moves[move].state == MoveState::Active;
        });
    }

    void MakeWorklist() {
        for (Reg reg = PhysicalRegisterCount; reg < registerCount; ++reg) {
            if (state[reg] != NodeState::Initial)
                continue;
            if (degree[reg] >= K(reg)) {
                state[reg] = NodeState::Spill;
                spillWorklist.push_back(reg);
            }
            else if (MoveRelated(reg)) {
                state[reg] = NodeState::Freeze;
                freezeWorklist.push_back(reg);
            }
            else {
                state[reg] = NodeState::Simplify;
                simplifyWorklist.push_back(reg);
            }
        }
    }

    template <typename Visit>
    void ForEachAdjacent(const Reg reg, Visit visit) const {
        adjacency.ForEach(reg, [&](const Reg other) {
            if (state[other] != NodeState::Stacked && state[other] != NodeState::Coalesced)
                visit(other);
        });
    }

    void EnableMoves(const Reg reg) {
        moveList.ForEach(reg, [&](const std::uint32_t move) {
            if (moves[move].state == MoveState::Active) {
                moves[move].state = MoveState::Worklist;
                moveWorklist.push_back(move);
            }
        });
    }

    void DecrementDegree(const Reg reg) {
        if (Precolored(reg))
            return;
        const auto previous = degree[reg]--;
        if (previous != K(reg))
            return;
        EnableMoves(reg);
        ForEachAdjacent(reg, [&](const Reg other) { EnableMoves(other); });
        if (state[reg] != NodeState::Spill)
            return;
        if (MoveRelated(reg)) {
            state[reg] = NodeState::Freeze;
            freezeWorklist.push_back(reg);
        }
        else {
            state[reg] = NodeState::Simplify;
            simplifyWorklist.push_back(reg);
        }
    }

    void Simplify() {
        const Reg reg = simplifyWorklist.back();
        simplifyWorklist.pop_back();
        if (state[reg] != NodeState::Simplify)
            return;
        state[reg] = NodeState::Stacked;
        selectStack.push_back(reg);
        ForEachAdjacent(reg, [&](const Reg other) { DecrementDegree(other); });
    }

    [[nodiscard]] Reg Alias(Reg reg) const {
        while (state[reg] == NodeState::Coalesced)
            reg = alias[reg];
        return reg;
    }

    void AddWorklist(const Reg reg) {
        if (!Precolored(reg) && !MoveRelated(reg) && degree[reg] < K(reg) && state[reg] == NodeState::Freeze) {
            state[reg] = NodeState::Simplify;
            simplifyWorklist.push_back(reg);
        }
    }

    [[nodiscard]] bool Ok(const Reg candidate, const Reg precolored) const {
        return degree[candidate] < K(candidate) || Precolored(candidate) || Interferes(candidate, precolored);
    }

    [[nodiscard]] bool Conservative(const Reg u, const Reg v) const {
        std::uint32_t significant = 0;
        counted.clear();
        const auto count = [&](const Reg other) {
            if (std::ranges::find(counted, other) != counted.end())
                return;
            counted.push_back(other);
            if (degree[other] >= K(other))
                ++significant;
        };
        ForEachAdjacent(u, count);
        ForEachAdjacent(v, count);
        return significant < K(u);
    }

    void Combine(const Reg u, const Reg v) {
        state[v] = NodeState::Coalesced;
        alias[v] = u;
        spillCost[u] += spillCost[v];
        unspillable[u] = unspillable[u] && unspillable[v];
        moveList.Append(u, v);
        EnableMoves(v);
        ForEachAdjacent(v, [&](const Reg other) {
            AddEdge(other, u);
            DecrementDegree(other);
        });
        if (!Precolored(u) && degree[u] >= K(u) && state[u] == NodeState::Freeze) {
            state[u] = NodeState::Spill;
            spillWorklist.push_back(u);
        }
    }

    void Coalesce() {
        const auto index = moveWorklist.back();
        moveWorklist.pop_back();
        auto &move = moves[index];
        if (move.state != MoveState::Worklist)
            return;
        Reg u = Alias(move.destination);
        Reg v = Alias(move.source);
        if (Precolored(v))
            std::swap(u, v);
        if (u == v) {
            move.state = MoveState::Coalesced;
            AddWorklist(u);
            return;
        }
        if (Precolored(v) || Interferes(u, v)) {
            move.state = MoveState::Constrained;
            AddWorklist(u);
            AddWorklist(v);
            return;
        }
        bool safe = false;
        if (Precolored(u)) {
            safe = true;
            ForEachAdjacent(v, [&](const Reg other) { safe = safe && Ok(other, u); });
        }
        else {
            safe = Conservative(u, v);
        }
        if (safe) {
            move.state = MoveState::Coalesced;
            Combine(u, v);
            AddWorklist(u);
        }
        else {
            move.state = MoveState::Active;
        }
    }

    void FreezeMoves(const Reg reg) {
        moveList.ForEach(reg, [&](const std::uint32_t index) {
            auto &move = moves[index];
            if (move.state != MoveState::Worklist && move.state != MoveState::Active)
                return;
            const Reg x = Alias(move.destination);
            const Reg y = Alias(move.source);
            const Reg other = y == Alias(reg) ? x : y;
            move.state = MoveState::Frozen;
            if (!Precolored(other) && state[other] == NodeState::Freeze && !MoveRelated(other) &&
                degree[other] < K(other)) {
                state[other] = NodeState::Simplify;
                simplifyWorklist.push_back(other);
            }
        });
    }

    void Freeze() {
        const Reg reg = freezeWorklist.back();
        freezeWorklist.pop_back();
        if (state[reg] != NodeState::Freeze)
            return;
        state[reg] = NodeState::Simplify;
        simplifyWorklist.push_back(reg);
        FreezeMoves(reg);
    }

    void SelectSpill() {
        Reg chosen = NoReg;
        double best = std::numeric_limits<double>::infinity();
        bool chosenUnspillable = true;
        for (const auto reg : spillWorklist) {
            if (state[reg] != NodeState::Spill)
                continue;
            const double metric = spillCost[reg] / std::max<double>(1.0, degree[reg]);
            // A spill temporary is chosen only when nothing else remains, and then only to keep simplifying: its
            // optimistic coloring still succeeds because its range spans a single instruction.
            if (chosen == NoReg || (chosenUnspillable && !unspillable[reg]) ||
                (chosenUnspillable == unspillable[reg] && metric < best)) {
                chosen = reg;
                best = metric;
                chosenUnspillable = unspillable[reg];
            }
        }
        if (chosen == NoReg) {
            spillWorklist.clear();
            return;
        }
        state[chosen] = NodeState::Simplify;
        simplifyWorklist.push_back(chosen);
        FreezeMoves(chosen);
    }

    void CompactWorklists() {
        if (spillWorklist.size() > 64) {
            std::erase_if(spillWorklist, [&](const Reg reg) { return state[reg] != NodeState::Spill; });
        }
    }

    /// Prefers the color of a move partner that already has one, so a copy the coalescer had to leave still
    /// disappears when both ends land in the same register.
    [[nodiscard]] Reg PreferredColor(const Reg reg, const std::vector<bool> &available) const {
        Reg preferred = NoReg;
        if (moveList.Find(reg, [&](const std::uint32_t index) {
                const auto &move = moves[index];
                const Reg partner =
                    Alias(move.destination) == Alias(reg) ? Alias(move.source) : Alias(move.destination);
                preferred = color[partner];
                return preferred != NoReg && available[preferred];
            }))
            return preferred;
        for (const auto candidate : target.allocationOrder[static_cast<std::size_t>(ClassOf(reg))])
            if (available[candidate])
                return candidate;
        return NoReg;
    }

    void AssignColors() {
        std::vector<bool> available(PhysicalRegisterCount, false);
        while (!selectStack.empty()) {
            const Reg reg = selectStack.back();
            selectStack.pop_back();
            std::ranges::fill(available, false);
            for (const auto candidate : target.allocationOrder[static_cast<std::size_t>(ClassOf(reg))])
                available[candidate] = true;
            adjacency.ForEach(reg, [&](const Reg other) {
                const Reg representative = Alias(other);
                if (state[representative] == NodeState::Colored || state[representative] == NodeState::Precolored)
                    available[color[representative]] = false;
            });
            const Reg chosen = PreferredColor(reg, available);
            if (chosen == NoReg) {
                state[reg] = NodeState::Spilled;
                spilledNodes.push_back(reg);
            }
            else {
                state[reg] = NodeState::Colored;
                color[reg] = chosen;
            }
        }
        for (Reg reg = PhysicalRegisterCount; reg < registerCount; ++reg)
            if (state[reg] == NodeState::Coalesced)
                color[reg] = color[Alias(reg)];
    }

    [[nodiscard]] std::uint64_t Rewrite() {
        std::uint64_t used = 0;
        const auto map = [&](Reg &reg) {
            if (!IsVirtual(reg))
                return;
            reg = color[reg];
            used |= RegisterBit(reg);
        };
        for (auto &block : function.blocks) {
            for (auto &instruction : block.instructions) {
                for (auto &operand : instruction.operands) {
                    if (operand.kind == Operand::Kind::Register) {
                        map(operand.reg);
                        if (IsPhysical(operand.reg))
                            used |= RegisterBit(operand.reg);
                    }
                    else if (operand.kind == Operand::Kind::Memory) {
                        map(operand.address.base);
                        map(operand.address.index);
                    }
                }
            }
            std::erase_if(block.instructions, [](const Instruction &instruction) {
                return instruction.isCopy && instruction.operands.size() == 2 && instruction.operands[0].IsRegister() &&
                       instruction.operands[1].IsRegister() &&
                       instruction.operands[0].reg == instruction.operands[1].reg;
            });
        }
        // Only registers an instruction names explicitly are saved: a call's clobbers are the callee's to preserve.
        return used & target.calleeSaved;
    }

    void RewriteSpills() {
        std::unordered_map<Reg, std::int32_t> slots;
        std::unordered_map<Reg, const Instruction *> rematerialized;
        std::vector<std::uint32_t> definitionCount(registerCount, 0);
        std::vector<const Instruction *> definition(registerCount, nullptr);
        std::vector<Reg> uses;
        std::vector<Reg> defs;
        for (const auto &block : function.blocks) {
            for (const auto &instruction : block.instructions) {
                Mentions(instruction, uses, defs);
                for (const auto reg : defs) {
                    ++definitionCount[reg];
                    definition[reg] = &instruction;
                }
            }
        }
        std::vector<Instruction> rematerializedDefinitions;
        rematerializedDefinitions.reserve(spilledNodes.size());
        for (const auto reg : spilledNodes) {
            if (definitionCount[reg] == 1 && definition[reg]->rematerializable) {
                rematerializedDefinitions.push_back(*definition[reg]);
                continue;
            }
            slots.emplace(reg, function.NewFrameObject(spills.SpillSize(ClassOf(reg)), spills.SpillSize(ClassOf(reg))));
        }
        std::size_t rematerializedIndex = 0;
        for (const auto reg : spilledNodes) {
            if (!slots.contains(reg))
                rematerialized.emplace(reg, &rematerializedDefinitions[rematerializedIndex++]);
        }
        const auto spilled = [&](const Reg reg) {
            return IsVirtual(reg) && (slots.contains(reg) || rematerialized.contains(reg));
        };
        const auto newTemporary = [&](const Reg reg) {
            const Reg temporary = function.NewVirtual(ClassOf(reg));
            unspillable.resize(PhysicalRegisterCount + function.VirtualCount(), false);
            unspillable[temporary] = true;
            return temporary;
        };
        for (auto &block : function.blocks) {
            std::vector<Instruction> rewritten;
            rewritten.reserve(block.instructions.size());
            for (auto &instruction : block.instructions) {
                // The rematerializable definition itself disappears; every use recomputes it.
                if (instruction.rematerializable) {
                    bool removed = false;
                    for (const auto &operand : instruction.operands)
                        if (operand.IsRegister() && operand.def && rematerialized.contains(operand.reg))
                            removed = true;
                    if (removed)
                        continue;
                }
                // A copy into or out of a spilled register becomes the store or load itself.
                if (instruction.isCopy && instruction.operands.size() == 2 && instruction.operands[0].IsRegister() &&
                    instruction.operands[1].IsRegister()) {
                    const Reg destination = instruction.operands[0].reg;
                    const Reg source = instruction.operands[1].reg;
                    if (slots.contains(destination) && !spilled(source)) {
                        rewritten.push_back(spills.Store(source, ClassOf(destination), slots.at(destination)));
                        continue;
                    }
                    if (slots.contains(source) && !spilled(destination)) {
                        rewritten.push_back(spills.Load(destination, ClassOf(source), slots.at(source)));
                        continue;
                    }
                }
                std::unordered_map<Reg, Reg> replacements;
                bool reads = false;
                std::vector<Reg> loaded;
                std::vector<Reg> stored;
                const auto replace = [&](Reg &reg, const bool use, const bool def) {
                    if (!spilled(reg))
                        return;
                    auto found = replacements.find(reg);
                    if (found == replacements.end())
                        found = replacements.emplace(reg, newTemporary(reg)).first;
                    if (use && std::ranges::find(loaded, reg) == loaded.end())
                        loaded.push_back(reg);
                    if (def && std::ranges::find(stored, reg) == stored.end())
                        stored.push_back(reg);
                    reads = reads || use;
                    reg = found->second;
                };
                for (auto &operand : instruction.operands) {
                    if (operand.kind == Operand::Kind::Register)
                        replace(operand.reg, operand.use, operand.def);
                    else if (operand.kind == Operand::Kind::Memory) {
                        if (operand.address.base != NoReg)
                            replace(operand.address.base, true, false);
                        if (operand.address.index != NoReg)
                            replace(operand.address.index, true, false);
                    }
                }
                for (const auto reg : loaded) {
                    const Reg temporary = replacements.at(reg);
                    if (const auto found = rematerialized.find(reg); found != rematerialized.end()) {
                        Instruction copy = *found->second;
                        for (auto &operand : copy.operands)
                            if (operand.IsRegister() && operand.def && operand.reg == reg)
                                operand.reg = temporary;
                        rewritten.push_back(std::move(copy));
                    }
                    else {
                        rewritten.push_back(spills.Load(temporary, ClassOf(reg), slots.at(reg)));
                    }
                }
                rewritten.push_back(std::move(instruction));
                for (const auto reg : stored) {
                    if (const auto slot = slots.find(reg); slot != slots.end())
                        rewritten.push_back(spills.Store(replacements.at(reg), ClassOf(reg), slot->second));
                }
                static_cast<void>(reads);
            }
            block.instructions = std::move(rewritten);
        }
    }
};
} // namespace

AllocationResult AllocateRegisters(Function &function, const TargetRegisters &target, const SpillCodeFactory &spills) {
    return Allocator(function, target, spills).Run();
}
} // namespace Rux::Machine
