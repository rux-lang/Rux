#include "Optimization/LirFillWidening.h"

#include "Ir/Lir/NaturalLoops.h"
#include "Optimization/ConstantEvaluator.h"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace Rux::Optimization {
namespace {
bool Word(const TypeRef &type) {
    return type.kind == TypeRef::Kind::UInt || type.kind == TypeRef::Kind::UInt64;
}

/// The size of a value whose bytes a wider store can repeat: an integer, character or Boolean of one, two or four
/// bytes. Floating-point values convert by value, not by bits, so they keep their loops.
std::uint64_t ElementSize(const TypeRef &type) {
    switch (type.kind) {
    case TypeRef::Kind::UInt8:
    case TypeRef::Kind::Int8:
    case TypeRef::Kind::Char8:
    case TypeRef::Kind::Bool8:
        return 1;
    case TypeRef::Kind::UInt16:
    case TypeRef::Kind::Int16:
    case TypeRef::Kind::Char16:
    case TypeRef::Kind::Bool16:
        return 2;
    case TypeRef::Kind::UInt32:
    case TypeRef::Kind::Int32:
    case TypeRef::Kind::Char32:
    case TypeRef::Kind::Bool32:
        return 4;
    default:
        return 0;
    }
}

std::string Constant(const std::uint64_t value) {
    return std::to_string(value);
}

bool Widen(LirFunc &function) {
    if (function.isExtern || function.isAsm || function.blocks.size() < 3 || function.blocks.size() > 4096)
        return false;
    const LirControlFlow cfg(function);
    const LirDominance dominance(cfg);
    const LirNaturalLoops loops(cfg, dominance);
    if (!loops.complete)
        return false;

    struct Definition {
        const LirInstr *instruction;
        std::uint32_t block;
        TypeRef type;
    };

    std::unordered_map<LirReg, Definition> definitions;
    LirReg next = 0;
    for (const auto &parameter : function.params) {
        definitions.emplace(parameter.reg, Definition{nullptr, 0, parameter.type});
        next = std::max(next, parameter.reg + 1);
    }
    for (std::uint32_t block = 0; block < function.blocks.size(); ++block) {
        for (const auto &instruction : function.blocks[block].instrs) {
            if (instruction.dst == LirNoReg)
                continue;
            if (!definitions
                     .emplace(instruction.dst,
                              Definition{&instruction, block,
                                         instruction.op == LirOpcode::Alloca ? TypeRef::MakePointer(instruction.type)
                                                                             : instruction.type})
                     .second)
                return false;
            next = std::max(next, instruction.dst + 1);
        }
    }
    const auto get = [&](const LirReg reg) -> const LirInstr * {
        const auto found = definitions.find(reg);
        return found == definitions.end() ? nullptr : found->second.instruction;
    };
    const auto constantOf = [&](const LirReg reg) -> std::optional<std::uint64_t> {
        const auto *instruction = get(reg);
        if (!instruction || instruction->op != LirOpcode::Const || instruction->isVolatile ||
            !instruction->srcs.empty())
            return std::nullopt;
        const auto value = ParseConstant(instruction->strArg, instruction->type);
        return value ? std::optional<std::uint64_t>(value->RawBits()) : std::nullopt;
    };
    for (const auto &[header, members] : loops.members) {
        if (header == 0 || members.size() != 2 || cfg.predecessors[header].size() != 2)
            continue;
        std::uint32_t body = header;
        for (const auto member : members)
            if (member != header)
                body = member;
        const auto &term = *function.blocks[header].term;
        const auto &back = *function.blocks[body].term;
        if (term.kind != LirTermKind::Branch || term.trueTarget != body || members.contains(term.falseTarget) ||
            back.kind != LirTermKind::Jump || back.trueTarget != header ||
            cfg.predecessors[body] != std::vector<std::uint32_t>{header})
            continue;
        auto preheader =
            cfg.predecessors[header][0] == body ? cfg.predecessors[header][1] : cfg.predecessors[header][0];
        if (cfg.successors[preheader].size() != 1 || function.blocks[preheader].term->kind != LirTermKind::Jump)
            continue;
        // The header holds the count, the test and constants; anything else would need copying.
        const LirInstr *phi = nullptr;
        const LirInstr *test = nullptr;
        bool simple = true;
        for (const auto &instruction : function.blocks[header].instrs) {
            if (instruction.op == LirOpcode::Phi && !phi)
                phi = &instruction;
            else if (instruction.op == LirOpcode::CmpLt && instruction.dst == term.cond && !instruction.isVolatile)
                test = &instruction;
            else if (instruction.op != LirOpcode::Const)
                simple = false;
        }
        if (!simple || !phi || !test || test->srcs.size() != 2 || test->srcs[0] != phi->dst || !Word(phi->type) ||
            phi->phiPreds.size() != 2)
            continue;
        const auto limit = test->srcs[1];
        const auto outside = [&](const LirReg reg) {
            const auto found = definitions.find(reg);
            return found != definitions.end() && !members.contains(found->second.block) &&
                   (found->second.instruction == nullptr || dominance.Dominates(found->second.block, preheader));
        };
        if (!outside(limit) || definitions.at(limit).type != phi->type)
            continue;
        auto start = LirNoReg;
        auto increment = LirNoReg;
        for (const auto &[value, predecessor] : phi->phiPreds)
            (predecessor == body ? increment : start) = value;
        if (start == LirNoReg || increment == LirNoReg || constantOf(start) != 0)
            continue;
        // The body computes the address of element i, stores the value there and steps i by one.
        const LirInstr *address = nullptr;
        const LirInstr *store = nullptr;
        const LirInstr *step = nullptr;
        const LirInstr *index = nullptr;
        for (const auto &instruction : function.blocks[body].instrs) {
            if (instruction.isVolatile) {
                simple = false;
            }
            else if (instruction.op == LirOpcode::Const) {
                continue;
            }
            else if (instruction.op == LirOpcode::Cast && !index && instruction.srcs.size() == 1 &&
                     instruction.srcs[0] == phi->dst && Word(instruction.type)) {
                index = &instruction;
            }
            else if (instruction.op == LirOpcode::IndexPtr && !address && instruction.srcs.size() == 2 &&
                     (instruction.srcs[1] == phi->dst || (index && instruction.srcs[1] == index->dst))) {
                address = &instruction;
            }
            else if (instruction.op == LirOpcode::Store && !store && address && instruction.srcs.size() == 2 &&
                     instruction.srcs[1] == address->dst) {
                store = &instruction;
            }
            else if (instruction.op == LirOpcode::Add && !step && instruction.dst == increment &&
                     instruction.srcs.size() == 2 && instruction.srcs[0] == phi->dst && instruction.type == phi->type &&
                     constantOf(instruction.srcs[1]) == 1) {
                step = &instruction;
            }
            else {
                simple = false;
            }
        }
        if (!simple || !address || !store || !step || !outside(address->srcs[0]) || !outside(store->srcs[0]) ||
            address->type.kind != TypeRef::Kind::Pointer || address->type.inner.size() != 1 ||
            address->type.inner[0] != store->type)
            continue;
        const auto size = ElementSize(store->type);
        if (size == 0)
            continue;
        // Nothing the loop computes is read after it.
        bool escapes = false;
        const auto local = [&](const LirReg reg) {
            const auto found = definitions.find(reg);
            return found != definitions.end() && found->second.instruction && members.contains(found->second.block);
        };
        for (std::uint32_t block = 0; block < function.blocks.size() && !escapes; ++block) {
            if (members.contains(block))
                continue;
            for (const auto &instruction : function.blocks[block].instrs) {
                for (const auto source : instruction.srcs)
                    escapes = escapes || local(source);
                for (const auto &[value, predecessor] : instruction.phiPreds)
                    escapes = escapes || local(value);
            }
            const auto &exit = *function.blocks[block].term;
            escapes = escapes || local(exit.cond) || (exit.retVal && local(*exit.retVal));
        }
        if (escapes)
            continue;

        const auto perWord = 8 / size;
        // A constant limit below one word leaves the word loop nothing to do; cleanup would fold it away, and the next
        // iteration would widen the loop again.
        if (const auto bound = constantOf(limit); bound && *bound < perWord)
            continue;
        static constexpr std::array<std::uint64_t, 5> splats = {0, 0x0101010101010101u, 0x0001000100010001u, 0,
                                                                0x0000000100000001u};
        const auto indexType = phi->type;
        const auto storeType = store->type;
        const auto addressType = address->type;
        const auto base = address->srcs[0];
        const auto value = store->srcs[0];
        const bool castIndex = index != nullptr;
        const auto castType = castIndex ? index->type : TypeRef{};
        auto wordPointee = TypeRef::MakeUInt64();
        wordPointee.isMut = addressType.inner[0].isMut;
        const auto wordPointer = TypeRef::MakePointer(wordPointee);

        auto &setup = function.blocks[preheader].instrs;
        const auto emit = [&](std::vector<LirInstr> &into, const LirOpcode op, const TypeRef &type,
                              std::vector<LirReg> sources, std::string argument = {}) {
            LirInstr instruction;
            instruction.op = op;
            instruction.dst = next++;
            instruction.type = type;
            instruction.srcs = std::move(sources);
            instruction.strArg = std::move(argument);
            into.push_back(std::move(instruction));
            return into.back().dst;
        };
        const auto mask = emit(setup, LirOpcode::Const, indexType, {}, Constant(~(perWord - 1)));
        const auto wordLimit = emit(setup, LirOpcode::And, indexType, {limit, mask});
        const auto widened = emit(setup, LirOpcode::Cast, TypeRef::MakeUInt64(), {value}, storeType.ToString());
        const auto low = emit(setup, LirOpcode::Const, TypeRef::MakeUInt64(), {},
                              Constant(size == 4 ? 0xFFFFFFFFu : (std::uint64_t{1} << (8 * size)) - 1));
        const auto bits = emit(setup, LirOpcode::And, TypeRef::MakeUInt64(), {widened, low});
        const auto copies = emit(setup, LirOpcode::Const, TypeRef::MakeUInt64(), {}, Constant(splats[size]));
        const auto word = emit(setup, LirOpcode::Mul, TypeRef::MakeUInt64(), {bits, copies});
        const auto zero = emit(setup, LirOpcode::Const, indexType, {}, "0");

        const auto wordHeader = static_cast<std::uint32_t>(function.blocks.size());
        const auto wordBody = wordHeader + 1;
        LirBlock loop;
        loop.label = function.blocks[header].label + ".fill.words";
        LirInstr counter;
        counter.op = LirOpcode::Phi;
        counter.dst = next++;
        counter.type = indexType;
        const auto position = counter.dst;
        const auto advanced = next++;
        counter.phiPreds = {{zero, preheader}, {advanced, wordBody}};
        loop.instrs.push_back(std::move(counter));
        const auto more = emit(loop.instrs, LirOpcode::CmpLt, TypeRef::MakeBool(), {position, wordLimit});
        loop.term.emplace();
        loop.term->kind = LirTermKind::Branch;
        loop.term->cond = more;
        loop.term->trueTarget = wordBody;
        loop.term->falseTarget = header;

        LirBlock fill;
        fill.label = function.blocks[header].label + ".fill.store";
        const auto element =
            castIndex ? emit(fill.instrs, LirOpcode::Cast, castType, {position}, indexType.ToString()) : position;
        const auto at = emit(fill.instrs, LirOpcode::IndexPtr, addressType, {base, element});
        const auto target = emit(fill.instrs, LirOpcode::Cast, wordPointer, {at}, addressType.ToString());
        LirInstr write;
        write.op = LirOpcode::Store;
        write.type = TypeRef::MakeUInt64();
        write.srcs = {word, target};
        fill.instrs.push_back(std::move(write));
        const auto stride = emit(fill.instrs, LirOpcode::Const, indexType, {}, Constant(perWord));
        LirInstr add;
        add.op = LirOpcode::Add;
        add.dst = advanced;
        add.type = indexType;
        add.srcs = {position, stride};
        fill.instrs.push_back(std::move(add));
        fill.term.emplace();
        fill.term->kind = LirTermKind::Jump;
        fill.term->trueTarget = wordHeader;

        // The element loop now starts where the word loop stopped and is entered only from it.
        function.blocks[preheader].term->trueTarget = wordHeader;
        for (auto &instruction : function.blocks[header].instrs)
            if (instruction.op == LirOpcode::Phi)
                for (auto &[incoming, predecessor] : instruction.phiPreds)
                    if (predecessor == preheader) {
                        incoming = instruction.dst == phi->dst ? position : incoming;
                        predecessor = wordHeader;
                    }
        function.blocks.push_back(std::move(loop));
        function.blocks.push_back(std::move(fill));
        return true;
    }
    return false;
}
} // namespace

std::string_view LirFillWidening::Name() const noexcept {
    return "lir-fill-widening";
}

PassChange LirFillWidening::RunFunction(LirFunc &function, const std::size_t, const PassContext &) const {
    bool changed = false;
    // A widened loop starts its element loop at the word loop's index, never at zero, so no round revisits it.
    for (int round = 0; round < 8 && Widen(function); ++round)
        changed = true;
    return changed ? PassChange::Changed : PassChange::None;
}
} // namespace Rux::Optimization
