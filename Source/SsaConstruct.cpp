/*
    Rux Compiler
    Copyright © 2026 Rux Contributors
    Licensed under the MIT License
*/

// SSA Construction Pass — Cytron et al. 1991
//
// Implements the standard three-phase SSA construction algorithm:
//
//   Phase 1 — Dominator tree via Lengauer-Tarjan (1979) semi-dominator method.
//             O(n α(n)) — effectively linear for all real programs.
//
//   Phase 2 — Dominance frontiers + phi insertion at the IDF of each
//             alloca-defined variable (Cytron et al. 1991 §5).
//
//   Phase 3 — Variable renaming: one DFS over the dominator tree replaces
//             each alloca/load/store triple with direct vreg defs and uses,
//             inserting phi-src assignments on back-edges.
//
// After this pass the function contains no Alloca/Load/Store for scalar
// local variables — every value flows through SSA phi nodes and direct
// register defs.  Pointer-escaping allocas (address taken via FieldPtr /
// IndexPtr / passed to calls) are left alone.

#include "Rux/SsaConstruct.h"

#include <algorithm>
#include <cassert>
#include <functional>
#include <numeric>
#include <optional>
#include <stack>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Rux {
namespace {

// ─────────────────────────────────────────────────────────────────────────────
// §1  CFG helpers
// ─────────────────────────────────────────────────────────────────────────────

struct Cfg {
    uint32_t n = 0;
    std::vector<std::vector<uint32_t>> succ;
    std::vector<std::vector<uint32_t>> pred;

    explicit Cfg(const LirFunc& func) : n(static_cast<uint32_t>(func.blocks.size())),
                                        succ(n), pred(n) {
        for (uint32_t b = 0; b < n; b++) {
            if (!func.blocks[b].term) continue;
            const auto& t = *func.blocks[b].term;
            auto add = [&](uint32_t to) {
                if (to < n) { succ[b].push_back(to); pred[to].push_back(b); }
            };
            switch (t.kind) {
            case LirTermKind::Jump:   add(t.trueTarget); break;
            case LirTermKind::Branch: add(t.trueTarget); add(t.falseTarget); break;
            case LirTermKind::Switch:
                add(t.defaultTarget);
                for (const auto& c : t.cases) add(c.target);
                break;
            case LirTermKind::Return: break;
            }
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// §2  Lengauer-Tarjan dominator tree
//
// Reference: Lengauer & Tarjan, "A fast algorithm for finding dominators in a
// flowgraph", TOPLAS 1979.
//
// Returns idom[b] = immediate dominator of b (idom[0] = 0, the entry).
// ─────────────────────────────────────────────────────────────────────────────

struct DomTree {
    std::vector<uint32_t> idom;     // immediate dominator
    std::vector<std::vector<uint32_t>> children; // domtree children

    explicit DomTree(const Cfg& cfg) : idom(cfg.n, cfg.n), children(cfg.n) {
        const uint32_t n = cfg.n;
        if (n == 0) return;

        // DFS order arrays
        std::vector<uint32_t> order;     // DFS visit order
        std::vector<uint32_t> dfnum(n, n); // DFS number of each block
        std::vector<uint32_t> parent(n, n);

        // Iterative DFS to avoid stack overflow on large functions
        {
            std::stack<std::pair<uint32_t,uint32_t>> stk; // (node, parent)
            stk.push({0, 0});
            while (!stk.empty()) {
                auto [v, p] = stk.top(); stk.pop();
                if (dfnum[v] != n) continue; // already visited
                dfnum[v] = static_cast<uint32_t>(order.size());
                parent[v] = p;
                order.push_back(v);
                // Push successors in reverse so we process them left-to-right
                for (auto it = cfg.succ[v].rbegin(); it != cfg.succ[v].rend(); ++it)
                    if (dfnum[*it] == n) stk.push({*it, v});
            }
        }

        const uint32_t sz = static_cast<uint32_t>(order.size());

        // Semi-dominator arrays
        std::vector<uint32_t> semi(n);
        std::iota(semi.begin(), semi.end(), 0); // semi[v] = v initially

        // Union-Find with path compression for the "link-eval" structure
        std::vector<uint32_t> ancestor(n, n); // forest root
        std::vector<uint32_t> best(n);        // best semi seen on path to root
        std::iota(best.begin(), best.end(), 0);

        std::function<uint32_t(uint32_t)> eval = [&](uint32_t v) -> uint32_t {
            if (ancestor[v] == n) return v; // root of its tree
            // Path-compress
            uint32_t root = eval(ancestor[v]);
            if (dfnum[semi[best[ancestor[v]]]] < dfnum[semi[best[v]]])
                best[v] = best[ancestor[v]];
            ancestor[v] = root;
            return root;
        };

        // bucket[v] = nodes whose semi-dominator is v
        std::vector<std::vector<uint32_t>> bucket(n);

        // idom array (will be finalised below)
        std::vector<uint32_t> dom(n, n);

        // Process in reverse DFS order (skip entry = index 0)
        for (uint32_t i = sz; i-- > 1;) {
            uint32_t w = order[i];

            // Step 1: compute semi-dominator of w
            for (uint32_t v : cfg.pred[w]) {
                if (dfnum[v] == n) continue; // unreachable predecessor
                uint32_t u = (dfnum[v] <= dfnum[w]) ? v : eval(v);
                if (dfnum[semi[u]] < dfnum[semi[w]]) semi[w] = semi[u];
            }
            bucket[semi[w]].push_back(w);
            // Link w to its spanning-tree parent
            ancestor[w] = parent[w];

            // Step 2: for each v in bucket[parent[w]], compute idom candidate
            for (uint32_t v : bucket[parent[w]]) {
                uint32_t u = eval(v);
                dom[v] = (dfnum[semi[u]] < dfnum[semi[v]]) ? u : parent[w];
            }
            bucket[parent[w]].clear();
        }

        // Step 3: finalise idoms
        idom.assign(n, 0);
        idom[0] = 0;
        for (uint32_t i = 1; i < sz; i++) {
            uint32_t w = order[i];
            if (dom[w] != semi[w]) dom[w] = idom[dom[w]];
            idom[w] = dom[w];
        }

        // Build children list
        for (uint32_t v = 1; v < n; v++)
            if (idom[v] < n) children[idom[v]].push_back(v);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// §3  Dominance frontiers
//
// DF(n) = { y | ∃ pred p of y : n dom p, n does not strictly dom y }
// Computed via the "bottom-up" algorithm from Cytron et al. §4.
// ─────────────────────────────────────────────────────────────────────────────

std::vector<std::unordered_set<uint32_t>> ComputeDF(
        const Cfg& cfg, const DomTree& dt) {
    const uint32_t n = cfg.n;
    std::vector<std::unordered_set<uint32_t>> df(n);

    // Post-order traversal of domtree
    std::vector<uint32_t> postorder;
    {
        std::stack<std::pair<uint32_t, bool>> stk;
        stk.push({0, false});
        while (!stk.empty()) {
            auto& [v, visited] = stk.top();
            if (!visited) {
                visited = true;
                for (uint32_t c : dt.children[v]) stk.push({c, false});
            } else {
                postorder.push_back(v);
                stk.pop();
            }
        }
    }

    for (uint32_t b : postorder) {
        // Local contribution: successors not immediately dominated by b
        for (uint32_t s : cfg.succ[b])
            if (dt.idom[s] != b) df[b].insert(s);
        // Upward contribution: DF of domtree children bubbles up
        for (uint32_t c : dt.children[b])
            for (uint32_t w : df[c])
                if (dt.idom[w] != b) df[b].insert(w);
    }
    return df;
}

// ─────────────────────────────────────────────────────────────────────────────
// §4  Promotable alloca detection
//
// An alloca is promotable to SSA if its address never escapes — i.e. the
// pointer vreg is only consumed by Load and Store instructions and never
// passed to calls, stored through another pointer, or used in FieldPtr /
// IndexPtr.
// ─────────────────────────────────────────────────────────────────────────────

bool IsPromotable(LirReg allocaReg, const LirFunc& func) {
    for (const auto& blk : func.blocks) {
        for (const auto& ins : blk.instrs) {
            // Any use of allocaReg that is not a direct Load or Store target
            // is considered an escape.
            if (ins.op == LirOpcode::Load) {
                // OK — Load through this pointer
                if (!ins.srcs.empty() && ins.srcs[0] == allocaReg) continue;
                // If allocaReg appears in any other position → escape
                for (LirReg s : ins.srcs) if (s == allocaReg) return false;
                continue;
            }
            if (ins.op == LirOpcode::Store) {
                // srcs[0] = value, srcs[1] = pointer
                // Storing TO this alloca is fine; storing the POINTER itself is escape
                if (ins.srcs.size() >= 2 && ins.srcs[1] == allocaReg) continue;
                for (LirReg s : ins.srcs) if (s == allocaReg) return false;
                continue;
            }
            // Any other opcode that uses this reg = escape
            for (LirReg s : ins.srcs) if (s == allocaReg) return false;
        }
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// §5  Phi insertion
//
// For each promotable alloca variable, find all blocks that define it (Store)
// and insert phi nodes at their iterated dominance frontiers.
// ─────────────────────────────────────────────────────────────────────────────

// Per-variable info collected during phi insertion
struct VarInfo {
    LirReg allocaReg = LirNoReg; // the original alloca vreg
    TypeRef type;                // value type (not pointer type)
    std::unordered_set<uint32_t> defBlocks; // blocks containing a Store to this alloca
    std::unordered_map<uint32_t, LirReg> phiDst; // block → phi-dst vreg (inserted phi)
};

// Insert phi nodes and return per-variable metadata
std::vector<VarInfo> InsertPhis(
        LirFunc& func,
        const Cfg& cfg,
        const std::vector<std::unordered_set<uint32_t>>& df,
        LirReg& nextReg) {

    const uint32_t n = cfg.n;

    // Collect promotable allocas: map allocaReg → VarInfo
    std::unordered_map<LirReg, VarInfo> vars;
    for (uint32_t bi = 0; bi < n; bi++) {
        for (const auto& ins : func.blocks[bi].instrs) {
            if (ins.op != LirOpcode::Alloca) continue;
            if (!IsPromotable(ins.dst, func)) continue;
            VarInfo vi;
            vi.allocaReg = ins.dst;
            // The alloca type is the element type (alloca yields a pointer)
            vi.type = ins.type; // LirOpcode::Alloca stores element type in .type
            vars[ins.dst] = std::move(vi);
        }
    }

    // Find defining blocks (blocks that Store to each alloca)
    for (uint32_t bi = 0; bi < n; bi++) {
        for (const auto& ins : func.blocks[bi].instrs) {
            if (ins.op != LirOpcode::Store) continue;
            if (ins.srcs.size() < 2) continue;
            LirReg ptr = ins.srcs[1];
            if (auto it = vars.find(ptr); it != vars.end())
                it->second.defBlocks.insert(bi);
        }
    }

    // IDF phi insertion per variable
    for (auto& [_, vi] : vars) {
        // Worklist = initial defining blocks
        std::vector<uint32_t> worklist(vi.defBlocks.begin(), vi.defBlocks.end());
        std::unordered_set<uint32_t> inWorklist(worklist.begin(), worklist.end());
        std::unordered_set<uint32_t> phiInserted;

        while (!worklist.empty()) {
            uint32_t b = worklist.back(); worklist.pop_back();
            for (uint32_t y : df[b]) {
                if (phiInserted.count(y)) continue;
                phiInserted.insert(y);

                // Insert phi at the front of block y
                LirReg phiReg = nextReg++;
                LirInstr phi;
                phi.op  = LirOpcode::Phi;
                phi.dst = phiReg;
                phi.type = vi.type;
                // phiPreds will be filled during the rename pass
                func.blocks[y].instrs.insert(func.blocks[y].instrs.begin(), phi);
                vi.phiDst[y] = phiReg;

                if (!inWorklist.count(y)) {
                    inWorklist.insert(y);
                    worklist.push_back(y);
                }
            }
        }
    }

    // Convert map to vector for stable iteration
    std::vector<VarInfo> result;
    result.reserve(vars.size());
    for (auto& [_, vi] : vars) result.push_back(std::move(vi));
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// §6  Variable renaming
//
// DFS over the dominator tree.  Maintain a stack of "current reaching
// definition" for each variable.  Replace:
//   - Store %val, %ptr  →  push %val as new reaching def, remove Store
//   - Load %dst, %ptr   →  replace with a copy %dst = reaching_def, remove Load
//   - Phi for variable  →  push the phi dst as new reaching def
// Fill phi-src operands when we reach a successor block.
// ─────────────────────────────────────────────────────────────────────────────

void RenameVars(
        LirFunc& func,
        const Cfg& cfg,
        const DomTree& dt,
        std::vector<VarInfo>& vars,
        LirReg& nextReg) {

    (void)nextReg; // reserved for future use

    const uint32_t n = cfg.n;
    const uint32_t numVars = static_cast<uint32_t>(vars.size());

    // Index: allocaReg → variable index
    std::unordered_map<LirReg, uint32_t> allocaIdx;
    for (uint32_t i = 0; i < numVars; i++)
        allocaIdx[vars[i].allocaReg] = i;

    // Reaching definition stacks: reachDef[varIdx] = stack of current vreg
    // Initialise with LirNoReg (undefined / undef value)
    std::vector<std::vector<LirReg>> reachDef(numVars);

    // DFS over domtree (iterative to avoid deep recursion)
    // Each stack frame records: (block, varStackSizes before processing block)
    struct Frame {
        uint32_t block;
        std::vector<uint32_t> savedSizes; // per-var stack depth at entry
    };

    std::stack<Frame> dfsStack;
    dfsStack.push({0, std::vector<uint32_t>(numVars, 0)});

    while (!dfsStack.empty()) {
        auto [b, savedSizes] = std::move(dfsStack.top());
        dfsStack.pop();

        // Restore stack depths to what they were before we entered b
        // (this handles the "pop after domtree subtree" requirement)
        for (uint32_t i = 0; i < numVars; i++)
            reachDef[i].resize(savedSizes[i]);

        auto& blk = func.blocks[b];
        std::vector<LirInstr> newInstrs;
        newInstrs.reserve(blk.instrs.size());

        for (auto& ins : blk.instrs) {
            if (ins.op == LirOpcode::Alloca) {
                if (allocaIdx.count(ins.dst)) {
                    // Drop the alloca — the variable will live purely in registers
                    continue;
                }
                newInstrs.push_back(std::move(ins));
                continue;
            }

            if (ins.op == LirOpcode::Store && ins.srcs.size() >= 2) {
                LirReg ptr = ins.srcs[1];
                if (auto it = allocaIdx.find(ptr); it != allocaIdx.end()) {
                    // This store defines the variable — push reaching def
                    LirReg val = ins.srcs[0];
                    reachDef[it->second].push_back(val);
                    // Drop the Store
                    continue;
                }
                newInstrs.push_back(std::move(ins));
                continue;
            }

            if (ins.op == LirOpcode::Load && ins.srcs.size() >= 1 && ins.strArg.empty()) {
                LirReg ptr = ins.srcs[0];
                if (auto it = allocaIdx.find(ptr); it != allocaIdx.end()) {
                    uint32_t vi = it->second;
                    LirReg reaching = reachDef[vi].empty() ? LirNoReg : reachDef[vi].back();
                    if (reaching != LirNoReg && reaching != ins.dst) {
                        // Replace load with a direct copy instruction (Add dst, reaching, 0
                        // would work but we emit a Const-copy via a dedicated path;
                        // simplest is to add a Cast with same type which becomes a mov)
                        LirInstr copy;
                        copy.op   = LirOpcode::Cast;
                        copy.dst  = ins.dst;
                        copy.type = ins.type;
                        copy.srcs = {reaching};
                        newInstrs.push_back(std::move(copy));
                    } else if (reaching == ins.dst) {
                        // Trivial self-copy, drop it
                    }
                    // If no reaching def: the variable was used before init
                    // (undefined behaviour in Rux) — emit a zero const
                    else {
                        LirInstr zero;
                        zero.op     = LirOpcode::Const;
                        zero.dst    = ins.dst;
                        zero.type   = ins.type;
                        zero.strArg = "0";
                        newInstrs.push_back(std::move(zero));
                    }
                    continue;
                }
                newInstrs.push_back(std::move(ins));
                continue;
            }

            if (ins.op == LirOpcode::Phi) {
                // Check if this phi belongs to one of our variables
                bool found = false;
                for (uint32_t vi = 0; vi < numVars; vi++) {
                    if (vars[vi].phiDst.count(b) && vars[vi].phiDst.at(b) == ins.dst) {
                        // Push this phi's dst as new reaching def
                        reachDef[vi].push_back(ins.dst);
                        found = true;
                        break;
                    }
                }
                newInstrs.push_back(std::move(ins));
                (void)found;
                continue;
            }

            newInstrs.push_back(std::move(ins));
        }

        blk.instrs = std::move(newInstrs);

        // Fill phi-src entries in successor blocks
        for (uint32_t s : cfg.succ[b]) {
            for (uint32_t vi = 0; vi < numVars; vi++) {
                auto phiIt = vars[vi].phiDst.find(s);
                if (phiIt == vars[vi].phiDst.end()) continue;
                LirReg phiDst = phiIt->second;
                // Find the phi instruction in block s and append our pred entry
                for (auto& ins : func.blocks[s].instrs) {
                    if (ins.op == LirOpcode::Phi && ins.dst == phiDst) {
                        LirReg reaching = reachDef[vi].empty() ? LirNoReg : reachDef[vi].back();
                        ins.phiPreds.push_back({reaching, b});
                        break;
                    }
                }
            }
        }

        // Push children onto DFS stack (capture current stack depths first)
        std::vector<uint32_t> curSizes(numVars);
        for (uint32_t i = 0; i < numVars; i++)
            curSizes[i] = static_cast<uint32_t>(reachDef[i].size());
        for (uint32_t c : dt.children[b])
            dfsStack.push({c, curSizes});
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// §7  Cleanup: remove phi nodes with all-undef operands; prune unreachable
//     alloca refs left behind after renaming
// ─────────────────────────────────────────────────────────────────────────────

void Cleanup(LirFunc& func) {
    // Collect vregs that are defined (have at least one non-trivial def)
    std::unordered_set<LirReg> defined;
    for (const auto& blk : func.blocks) {
        for (const auto& ins : blk.instrs) {
            if (ins.dst != LirNoReg) defined.insert(ins.dst);
        }
        for (const auto& p : func.params) defined.insert(p.reg);
    }

    for (auto& blk : func.blocks) {
        std::vector<LirInstr> keep;
        keep.reserve(blk.instrs.size());
        for (auto& ins : blk.instrs) {
            if (ins.op == LirOpcode::Phi) {
                // Remove phi if all preds are LirNoReg (var never defined on any path)
                bool allUndef = std::all_of(ins.phiPreds.begin(), ins.phiPreds.end(),
                    [](const auto& p){ return p.first == LirNoReg; });
                if (allUndef && ins.phiPreds.empty()) { continue; }
                if (allUndef && !ins.phiPreds.empty()) {
                    // Replace with a zero const so downstream code has something valid
                    LirInstr zero;
                    zero.op     = LirOpcode::Const;
                    zero.dst    = ins.dst;
                    zero.type   = ins.type;
                    zero.strArg = "0";
                    keep.push_back(std::move(zero));
                    continue;
                }
            }
            keep.push_back(std::move(ins));
        }
        blk.instrs = std::move(keep);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// §8  Find the highest vreg used in a function (to continue numbering from)
// ─────────────────────────────────────────────────────────────────────────────

LirReg MaxRegInFunc(const LirFunc& func) {
    LirReg maxReg = 0;
    auto bump = [&](LirReg r) { if (r != LirNoReg && r > maxReg) maxReg = r; };
    for (const auto& p : func.params) bump(p.reg);
    for (const auto& blk : func.blocks) {
        for (const auto& ins : blk.instrs) {
            bump(ins.dst);
            for (LirReg s : ins.srcs) bump(s);
            for (const auto& [r, _] : ins.phiPreds) bump(r);
        }
        if (blk.term) {
            bump(blk.term->cond);
            if (blk.term->retVal) bump(*blk.term->retVal);
        }
    }
    return maxReg;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

void SsaConstruct::RunFunc(LirFunc& func, LirReg& nextReg) {
    if (func.isExtern || func.blocks.empty()) return;

    Cfg cfg(func);
    DomTree dt(cfg);
    auto df = ComputeDF(cfg, dt);
    auto vars = InsertPhis(func, cfg, df, nextReg);
    if (vars.empty()) return; // nothing to promote
    RenameVars(func, cfg, dt, vars, nextReg);
    Cleanup(func);
}

void SsaConstruct::Run(LirPackage& pkg) {
    for (auto& mod : pkg.modules) {
        for (auto& func : mod.funcs) {
            LirReg nextReg = MaxRegInFunc(func) + 1;
            RunFunc(func, nextReg);
        }
    }
}

} // namespace Rux
