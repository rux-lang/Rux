/*
    Rux Compiler
    Copyright © 2026 Rux Contributors
    Licensed under the MIT License
*/

// Copy Propagation and Dead Code Elimination Pass
//
// Sub-pass 1 — Copy propagation
//   a) Constant deduplication
//      Scan every block for Const instructions.  If an identical (type, value)
//      pair was already seen, record the current dst as a trivial copy of the
//      earlier one.
//
//   b) Cast-copy elimination
//      SsaConstruct emits "cast %src:  to T" (empty strArg) when it replaces
//      a promoted Load with the reaching register.  Because no real type
//      conversion is happening these are pure register-to-register copies.
//      We also handle the case where strArg equals type.ToString() — a
//      degenerate no-op cast that may appear from explicit cast expressions
//      whose source and destination types are identical.
//
//   Both cases are recorded in a `copyOf` map and resolved transitively via
//   path compression.  Every use of a copied register (in srcs, phi preds,
//   branch condition, and return value) is rewritten to the root of its chain.
//
// Sub-pass 2 — Dead code elimination
//   After rewriting all uses, any instruction whose dst has zero uses and
//   whose opcode carries no observable side effects is removed.  The loop
//   repeats until no instructions are removed, which handles chains where
//   deleting one instruction exposes the next as dead.

#include "Rux/CopyProp.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Rux {
namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

// Returns true if removing the instruction would have an observable effect
// beyond its dst register.
static bool HasSideEffect(LirOpcode op) {
    switch (op) {
    case LirOpcode::Store:
    case LirOpcode::Call:
    case LirOpcode::CallIndirect:
    case LirOpcode::Load:       // conservative: may be memory-mapped / volatile
        return true;
    default:
        return false;
    }
}

// Build a flat use-count map over the whole function.
static std::unordered_map<LirReg, int> BuildUseCounts(const LirFunc& func) {
    std::unordered_map<LirReg, int> cnt;
    auto use = [&](LirReg r) { if (r != LirNoReg) cnt[r]++; };
    for (const auto& blk : func.blocks) {
        for (const auto& ins : blk.instrs) {
            for (LirReg s : ins.srcs)          use(s);
            for (const auto& [r, _] : ins.phiPreds) use(r);
        }
        if (blk.term) {
            use(blk.term->cond);
            if (blk.term->retVal) use(*blk.term->retVal);
        }
    }
    return cnt;
}

// Rewrite all uses of every register in the function through the copyOf map.
// copyOf entries are path-compressed during resolution.
static void RewriteUses(LirFunc& func,
                        std::unordered_map<LirReg, LirReg>& copyOf) {

    std::function<LirReg(LirReg)> resolve = [&](LirReg r) -> LirReg {
        if (r == LirNoReg) return r;
        auto it = copyOf.find(r);
        if (it == copyOf.end()) return r;
        LirReg root = resolve(it->second);
        it->second = root; // path compression
        return root;
    };

    for (auto& blk : func.blocks) {
        for (auto& ins : blk.instrs) {
            for (auto& s : ins.srcs)               s = resolve(s);
            for (auto& [r, _] : ins.phiPreds)      r = resolve(r);
        }
        if (blk.term) {
            blk.term->cond = resolve(blk.term->cond);
            if (blk.term->retVal)
                *blk.term->retVal = resolve(*blk.term->retVal);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Main pass
// ─────────────────────────────────────────────────────────────────────────────

void RunFuncImpl(LirFunc& func) {
    if (func.isExtern || func.blocks.empty()) return;

    // ── Sub-pass 1a: constant deduplication ──────────────────────────────────
    // Only deduplicate within the same basic block.  A constant defined in
    // block A is not guaranteed to be on every execution path that reaches
    // blocks not dominated by A, so cross-block deduplication would introduce
    // uses of uninitialised virtual registers on those paths.
    std::unordered_map<LirReg, LirReg> copyOf;

    for (const auto& blk : func.blocks) {
        // Fresh canonical map per block — safe because the first def always
        // precedes any duplicate within the same instruction sequence.
        std::unordered_map<std::string, LirReg> constCanon;
        for (const auto& ins : blk.instrs) {
            if (ins.op != LirOpcode::Const) continue;
            if (ins.dst == LirNoReg) continue;
            std::string key = ins.type.ToString() + "|" + ins.strArg;
            auto [it, inserted] = constCanon.emplace(key, ins.dst);
            if (!inserted && it->second != ins.dst)
                copyOf[ins.dst] = it->second; // duplicate → copy of canonical
        }
    }

    // ── Sub-pass 1b: cast-copy elimination ───────────────────────────────────
    // A Cast is a trivial copy when:
    //   (a) strArg is empty (produced by SsaConstruct renaming), or
    //   (b) strArg == type.ToString() (explicit cast between identical types).
    for (const auto& blk : func.blocks) {
        for (const auto& ins : blk.instrs) {
            if (ins.op != LirOpcode::Cast) continue;
            if (ins.dst == LirNoReg) continue;
            if (ins.srcs.size() != 1 || ins.srcs[0] == LirNoReg) continue;
            if (ins.srcs[0] == ins.dst) continue; // degenerate self-copy
            const bool trivial = ins.strArg.empty() ||
                                 ins.strArg == ins.type.ToString();
            if (trivial)
                copyOf[ins.dst] = ins.srcs[0];
        }
    }

    if (!copyOf.empty())
        RewriteUses(func, copyOf);

    // ── Sub-pass 2: iterated DCE ─────────────────────────────────────────────
    bool changed = true;
    while (changed) {
        changed = false;
        auto useCnt = BuildUseCounts(func);

        for (auto& blk : func.blocks) {
            std::vector<LirInstr> keep;
            keep.reserve(blk.instrs.size());
            for (auto& ins : blk.instrs) {
                // An instruction is dead when:
                //   • it produces a result (dst != LirNoReg)
                //   • that result is never used (not in useCnt, or count == 0)
                //   • its opcode has no side effects
                if (ins.dst != LirNoReg &&
                    !HasSideEffect(ins.op) &&
                    useCnt.count(ins.dst) == 0) {
                    changed = true;
                    continue; // drop
                }
                keep.push_back(std::move(ins));
            }
            blk.instrs = std::move(keep);
        }
    }
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

void CopyProp::RunFunc(LirFunc& func) {
    RunFuncImpl(func);
}

void CopyProp::Run(LirPackage& pkg) {
    for (auto& mod : pkg.modules)
        for (auto& func : mod.funcs)
            RunFuncImpl(func);
}

} // namespace Rux
