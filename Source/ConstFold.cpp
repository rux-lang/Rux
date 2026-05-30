/*
    Rux Compiler
    Copyright © 2026 Rux Contributors
    Licensed under the MIT License
*/

// Constant Folding and Dead Branch Elimination Pass
//
// Phase 1 — Constant propagation lattice
//   Build a map from LirReg to the compile-time constant value it holds.
//   Every Const instruction seeds the map.
//
// Phase 2 — Arithmetic / comparison folding
//   Walk every block linearly.  When an instruction's operands are all in the
//   constant map the instruction is evaluated at compile time and replaced with
//   a Const.  The new Const is immediately added to the map so subsequent
//   instructions in the same block (and later blocks, via the same scan) can
//   continue folding.
//
//   Supported ops: Add Sub Mul Div Mod Pow And Or Xor Shl Shr Neg BitNot Not
//                  CmpEq CmpNe CmpLt CmpLe CmpGt CmpGe Cast
//   Floating-point constant folding is not performed (rounding-mode safety).
//
// Phase 3 — Branch elimination
//   Any Branch whose condition register is in the constant map is replaced with
//   an unconditional Jump.  The not-taken edge is recorded as removed.
//
// Phase 4 — Reachability
//   BFS from block 0 following the edges that survive phase 3.
//
// Phase 5 — Dead block cleanup
//   Unreachable blocks have their instruction lists and terminators cleared
//   in place.  Block indices are not changed so no phi reindexing is needed.
//
// Phase 6 — Phi predecessor repair
//   In each reachable block, phi instructions listing unreachable predecessor
//   blocks have those entries removed.  A phi reduced to one predecessor is
//   converted to a trivial Cast copy (empty strArg) so CopyProp can eliminate
//   it.  A phi reduced to zero predecessors (dead phi in unreachable block)
//   is left for DCE.

#include "Rux/ConstFold.h"

#include <cassert>
#include <cstdint>
#include <functional>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Rux {
namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Constant value representation
// ─────────────────────────────────────────────────────────────────────────────

// All constants are stored as a 64-bit integer.  Boolean true → 1, false → 0.
// Unsigned types use the bit pattern directly; signed types sign-extend.
struct ConstVal {
    std::int64_t bits = 0; // raw bit pattern (sign extended for signed types)
    bool isSigned = false;
    bool isBool   = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// Type classification helpers
// ─────────────────────────────────────────────────────────────────────────────

static bool IsBoolType(const TypeRef& t) {
    return t.kind == TypeRef::Kind::Bool8  ||
           t.kind == TypeRef::Kind::Bool16 ||
           t.kind == TypeRef::Kind::Bool32;
}

static bool IsIntType(const TypeRef& t) {
    switch (t.kind) {
    case TypeRef::Kind::Int8:  case TypeRef::Kind::Int16:
    case TypeRef::Kind::Int32: case TypeRef::Kind::Int64:
    case TypeRef::Kind::Int:
        return true;
    default: return false;
    }
}

static bool IsUIntType(const TypeRef& t) {
    switch (t.kind) {
    case TypeRef::Kind::UInt8:  case TypeRef::Kind::UInt16:
    case TypeRef::Kind::UInt32: case TypeRef::Kind::UInt64:
    case TypeRef::Kind::UInt:
        return true;
    default: return false;
    }
}

// Returns true if we know how to constant-fold values of this type.
static bool IsFoldableType(const TypeRef& t) {
    return IsBoolType(t) || IsIntType(t) || IsUIntType(t);
}

// Bit width of the type (for masking after overflow).
static int TypeBits(const TypeRef& t) {
    switch (t.kind) {
    case TypeRef::Kind::Bool8:
    case TypeRef::Kind::Int8:   case TypeRef::Kind::UInt8:   return 8;
    case TypeRef::Kind::Bool16:
    case TypeRef::Kind::Int16:  case TypeRef::Kind::UInt16:  return 16;
    case TypeRef::Kind::Bool32:
    case TypeRef::Kind::Int32:  case TypeRef::Kind::UInt32:  return 32;
    case TypeRef::Kind::Int64:  case TypeRef::Kind::UInt64:
    case TypeRef::Kind::Int:    case TypeRef::Kind::UInt:    return 64;
    default:                                                  return 0;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Parse / serialise constants
// ─────────────────────────────────────────────────────────────────────────────

static std::optional<ConstVal> ParseConst(const std::string& s, const TypeRef& t) {
    if (!IsFoldableType(t)) return std::nullopt;
    ConstVal v;
    v.isSigned = IsIntType(t);
    v.isBool   = IsBoolType(t);
    try {
        if (v.isBool) {
            if (s == "true")  { v.bits = 1; return v; }
            if (s == "false") { v.bits = 0; return v; }
            v.bits = std::stoll(s) != 0 ? 1 : 0;
            return v;
        }
        if (v.isSigned)
            v.bits = std::stoll(s);
        else
            v.bits = static_cast<std::int64_t>(std::stoull(s));
        return v;
    } catch (...) {
        return std::nullopt;
    }
}

// Mask the raw bit pattern to the target type width and sign-extend if needed.
static ConstVal MaskToType(ConstVal v, const TypeRef& t) {
    int bits = TypeBits(t);
    if (bits <= 0 || bits >= 64) return v;
    std::uint64_t mask = (std::uint64_t(1) << bits) - 1ULL;
    std::uint64_t u = static_cast<std::uint64_t>(v.bits) & mask;
    if (v.isSigned) {
        // sign-extend
        std::uint64_t signBit = std::uint64_t(1) << (bits - 1);
        if (u & signBit)
            v.bits = static_cast<std::int64_t>(u | ~mask);
        else
            v.bits = static_cast<std::int64_t>(u);
    } else {
        v.bits = static_cast<std::int64_t>(u);
    }
    return v;
}

static std::string SerialiseConst(ConstVal v, const TypeRef& t) {
    if (IsBoolType(t))
        return v.bits ? "true" : "false";
    if (IsIntType(t))
        return std::to_string(v.bits);
    // unsigned
    return std::to_string(static_cast<std::uint64_t>(v.bits));
}

// ─────────────────────────────────────────────────────────────────────────────
// Integer exponentiation (repeated squaring)
// ─────────────────────────────────────────────────────────────────────────────

static std::int64_t IPow(std::int64_t base, std::int64_t exp) {
    if (exp < 0)  return 0;   // integer exponentiation with negative exponent → 0
    if (exp == 0) return 1;
    std::int64_t result = 1;
    while (exp > 0) {
        if (exp & 1) result *= base;
        base *= base;
        exp >>= 1;
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Evaluate a binary instruction on two constant operands
// ─────────────────────────────────────────────────────────────────────────────

static std::optional<ConstVal> EvalBinary(LirOpcode op,
                                          ConstVal a, ConstVal b,
                                          const TypeRef& resultType) {
    ConstVal out;
    out.isSigned = IsIntType(resultType);
    out.isBool   = IsBoolType(resultType);

    std::int64_t  ia = a.bits, ib = b.bits;
    std::uint64_t ua = static_cast<std::uint64_t>(a.bits),
                  ub = static_cast<std::uint64_t>(b.bits);
    // For comparisons the result is always bool.  We want to compare in the
    // operand's original signedness domain.
    bool opIsSigned = a.isSigned;

    switch (op) {
    case LirOpcode::Add:    out.bits = ia + ib; break;
    case LirOpcode::Sub:    out.bits = ia - ib; break;
    case LirOpcode::Mul:    out.bits = ia * ib; break;
    case LirOpcode::Div:
        if (ib == 0) return std::nullopt; // don't fold div-by-zero
        out.bits = opIsSigned ? ia / ib
                              : static_cast<std::int64_t>(ua / ub);
        break;
    case LirOpcode::Mod:
        if (ib == 0) return std::nullopt;
        out.bits = opIsSigned ? ia % ib
                              : static_cast<std::int64_t>(ua % ub);
        break;
    case LirOpcode::Pow:
        out.bits = IPow(ia, ib);
        break;
    case LirOpcode::And:    out.bits = ia & ib; break;
    case LirOpcode::Or:     out.bits = ia | ib; break;
    case LirOpcode::Xor:    out.bits = ia ^ ib; break;
    case LirOpcode::Shl:
        if (ub >= 64) return std::nullopt;
        out.bits = opIsSigned ? static_cast<std::int64_t>(
                                    static_cast<std::uint64_t>(ia) << ub)
                              : static_cast<std::int64_t>(ua << ub);
        break;
    case LirOpcode::Shr:
        if (ub >= 64) return std::nullopt;
        out.bits = opIsSigned ? ia >> ib            // arithmetic shift
                              : static_cast<std::int64_t>(ua >> ub); // logical
        break;
    // Comparisons — always produce a bool result
    case LirOpcode::CmpEq:  out.bits = (ia == ib) ? 1 : 0; break;
    case LirOpcode::CmpNe:  out.bits = (ia != ib) ? 1 : 0; break;
    case LirOpcode::CmpLt:
        out.bits = opIsSigned ? (ia  < ib  ? 1 : 0)
                              : (ua  < ub  ? 1 : 0); break;
    case LirOpcode::CmpLe:
        out.bits = opIsSigned ? (ia  <= ib ? 1 : 0)
                              : (ua  <= ub ? 1 : 0); break;
    case LirOpcode::CmpGt:
        out.bits = opIsSigned ? (ia  > ib  ? 1 : 0)
                              : (ua  > ub  ? 1 : 0); break;
    case LirOpcode::CmpGe:
        out.bits = opIsSigned ? (ia  >= ib ? 1 : 0)
                              : (ua  >= ub ? 1 : 0); break;
    default:
        return std::nullopt;
    }

    return MaskToType(out, resultType);
}

// ─────────────────────────────────────────────────────────────────────────────
// Evaluate a unary instruction on one constant operand
// ─────────────────────────────────────────────────────────────────────────────

static std::optional<ConstVal> EvalUnary(LirOpcode op, ConstVal a,
                                         const TypeRef& resultType) {
    ConstVal out;
    out.isSigned = IsIntType(resultType);
    out.isBool   = IsBoolType(resultType);

    switch (op) {
    case LirOpcode::Neg:    out.bits = -a.bits; break;
    case LirOpcode::BitNot: out.bits = ~a.bits; break;
    case LirOpcode::Not:    out.bits = (a.bits == 0) ? 1 : 0; break;
    default:
        return std::nullopt;
    }

    return MaskToType(out, resultType);
}

// ─────────────────────────────────────────────────────────────────────────────
// Evaluate a Cast instruction (type conversion)
// ─────────────────────────────────────────────────────────────────────────────

static std::optional<ConstVal> EvalCast(ConstVal src, const TypeRef& toType) {
    if (!IsFoldableType(toType)) return std::nullopt;
    ConstVal out;
    out.isSigned = IsIntType(toType);
    out.isBool   = IsBoolType(toType);
    if (out.isBool) {
        out.bits = src.bits != 0 ? 1 : 0;
    } else {
        out.bits = src.bits;
    }
    return MaskToType(out, toType);
}

// ─────────────────────────────────────────────────────────────────────────────
// Main pass implementation
// ─────────────────────────────────────────────────────────────────────────────

static void RunFuncImpl(LirFunc& func, LirReg& nextReg) {
    if (func.isExtern || func.blocks.empty()) return;

    // ── Phase 1+2: seed constant map and fold instructions ───────────────────
    std::unordered_map<LirReg, ConstVal> constMap;

    // Seed from all Const instructions first (single-block and cross-block).
    // Cross-block forwarding of constant values is safe in SSA form because
    // the Const instruction dominates all uses.
    for (const auto& blk : func.blocks)
        for (const auto& ins : blk.instrs)
            if (ins.op == LirOpcode::Const && ins.dst != LirNoReg)
                if (auto v = ParseConst(ins.strArg, ins.type))
                    constMap[ins.dst] = *v;

    // Fold instructions in linear order.  Results of folded instructions are
    // immediately visible to subsequent instructions (covers straight-line
    // chains even across blocks since we walk in layout order).
    bool anyFolded = true;
    while (anyFolded) {
        anyFolded = false;
        for (auto& blk : func.blocks) {
            for (auto& ins : blk.instrs) {
                if (ins.dst == LirNoReg) continue;
                if (ins.op == LirOpcode::Const) continue;
                if (!IsFoldableType(ins.type)) continue;

                std::optional<ConstVal> result;

                // Binary ops: exactly 2 srcs, both known constant
                if (ins.srcs.size() == 2) {
                    auto itA = constMap.find(ins.srcs[0]);
                    auto itB = constMap.find(ins.srcs[1]);
                    if (itA != constMap.end() && itB != constMap.end())
                        result = EvalBinary(ins.op, itA->second, itB->second,
                                            ins.type);
                }
                // Unary ops: exactly 1 src, known constant
                else if (ins.srcs.size() == 1) {
                    auto itA = constMap.find(ins.srcs[0]);
                    if (itA != constMap.end()) {
                        if (ins.op == LirOpcode::Cast)
                            result = EvalCast(itA->second, ins.type);
                        else
                            result = EvalUnary(ins.op, itA->second, ins.type);
                    }
                }

                if (!result) continue;

                // Replace instruction with Const
                std::string val = SerialiseConst(*result, ins.type);
                ins.op     = LirOpcode::Const;
                ins.srcs.clear();
                ins.strArg = val;
                ins.phiPreds.clear();
                constMap[ins.dst] = *result;
                anyFolded = true;
            }
        }
    }

    // ── Phase 3: fold constant branches ─────────────────────────────────────
    // Track which directed edges (src_block_idx → dst_block_idx) were removed.
    std::unordered_set<std::uint64_t> removedEdges; // key = (src<<32)|dst

    auto EncodeEdge = [](std::uint32_t src, std::uint32_t dst) -> std::uint64_t {
        return (std::uint64_t(src) << 32) | dst;
    };

    for (std::uint32_t bi = 0; bi < (std::uint32_t)func.blocks.size(); ++bi) {
        auto& blk = func.blocks[bi];
        if (!blk.term) continue;
        if (blk.term->kind != LirTermKind::Branch) continue;
        if (blk.term->cond == LirNoReg) continue;

        auto it = constMap.find(blk.term->cond);
        if (it == constMap.end()) continue;

        bool condTrue = (it->second.bits != 0);
        std::uint32_t taken    = condTrue ? blk.term->trueTarget
                                          : blk.term->falseTarget;
        std::uint32_t notTaken = condTrue ? blk.term->falseTarget
                                          : blk.term->trueTarget;

        blk.term->kind      = LirTermKind::Jump;
        blk.term->trueTarget = taken;
        blk.term->cond      = LirNoReg;
        removedEdges.insert(EncodeEdge(bi, notTaken));
    }

    if (removedEdges.empty()) return; // nothing to clean up

    // ── Phase 4: compute reachable blocks ────────────────────────────────────
    std::vector<bool> reachable(func.blocks.size(), false);
    std::queue<std::uint32_t> worklist;
    worklist.push(0);
    reachable[0] = true;
    while (!worklist.empty()) {
        std::uint32_t bi = worklist.front(); worklist.pop();
        const auto& blk = func.blocks[bi];
        if (!blk.term) continue;
        auto enqueue = [&](std::uint32_t succ) {
            if (succ < func.blocks.size() && !reachable[succ]) {
                reachable[succ] = true;
                worklist.push(succ);
            }
        };
        switch (blk.term->kind) {
        case LirTermKind::Jump:
            enqueue(blk.term->trueTarget);
            break;
        case LirTermKind::Branch:
            enqueue(blk.term->trueTarget);
            enqueue(blk.term->falseTarget);
            break;
        case LirTermKind::Switch:
            enqueue(blk.term->defaultTarget);
            for (const auto& [_, tgt] : blk.term->cases)
                enqueue(tgt);
            break;
        case LirTermKind::Return:
            break;
        }
    }

    // ── Phase 5: clear unreachable block instructions ────────────────────────
    for (std::uint32_t bi = 0; bi < (std::uint32_t)func.blocks.size(); ++bi) {
        if (!reachable[bi]) {
            func.blocks[bi].instrs.clear();
            func.blocks[bi].term = std::nullopt;
        }
    }

    // ── Phase 6: fix phi predecessors in reachable blocks ───────────────────
    for (std::uint32_t bi = 0; bi < (std::uint32_t)func.blocks.size(); ++bi) {
        if (!reachable[bi]) continue;
        auto& blk = func.blocks[bi];
        for (auto& ins : blk.instrs) {
            if (ins.op != LirOpcode::Phi) continue;

            // Remove entries for unreachable predecessors or removed edges.
            std::vector<std::pair<LirReg, std::uint32_t>> kept;
            kept.reserve(ins.phiPreds.size());
            for (const auto& [reg, predIdx] : ins.phiPreds) {
                bool predReachable = (predIdx < func.blocks.size())
                                     && reachable[predIdx];
                bool edgeRemoved   = removedEdges.count(
                                         EncodeEdge(predIdx, bi)) != 0;
                if (predReachable && !edgeRemoved)
                    kept.push_back({reg, predIdx});
            }
            ins.phiPreds = std::move(kept);

            // Trivial single-predecessor phi → convert to trivial Cast copy.
            // CopyProp (run after this pass) will eliminate these.
            if (ins.phiPreds.size() == 1) {
                LirReg sole = ins.phiPreds[0].first;
                ins.op = LirOpcode::Cast;
                ins.srcs = {sole};
                ins.phiPreds.clear();
                ins.strArg = ""; // empty strArg marks it as a trivial copy
            }
        }
    }
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

void ConstFold::RunFunc(LirFunc& func, LirReg& nextReg) {
    RunFuncImpl(func, nextReg);
}

void ConstFold::Run(LirPackage& pkg) {
    for (auto& mod : pkg.modules)
        for (auto& func : mod.funcs) {
            LirReg nextReg = LirNoReg; // ConstFold does not allocate new regs
            RunFuncImpl(func, nextReg);
        }
}

} // namespace Rux
