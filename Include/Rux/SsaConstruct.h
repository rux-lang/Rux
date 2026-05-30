/*
    Rux Compiler
    Copyright © 2026 Rux Contributors
    Licensed under the MIT License
*/

#pragma once

#include "Rux/Lir.h"

namespace Rux {
    // SSA Construction Pass
    //
    // Transforms a LirPackage whose functions are in non-SSA "alloca/load/store"
    // form into true SSA form using the classic Cytron et al. algorithm:
    //
    //   1. Compute the dominator tree  (Lengauer-Tarjan semi-dominator algorithm)
    //   2. Compute dominance frontiers (Cytron et al. DF formula over domtree)
    //   3. Insert phi nodes at iterated dominance frontier (IDF) for each
    //      variable that is defined in more than one block
    //   4. Rename variables           (single DFS pass over domtree)
    //
    // After this pass every LirReg has exactly one definition point in the
    // function.  The Wimmer-Franz allocator in Asm.cpp can then exploit the
    // single-def invariant to build exact live intervals without a fixed-point
    // dataflow loop.
    //
    // The pass operates entirely on `LirFunc` and mutates the package in-place.
    // It is idempotent: calling it twice on already-SSA code is safe.
    //
    // Usage (Cli.cpp, between Lir::Generate() and Asm/Rcu):
    //
    //   SsaConstruct::Run(lirPackage);
    //
    class SsaConstruct {
    public:
        // Transform every non-extern function in `pkg` into SSA form.
        static void Run(LirPackage& pkg);

        // Transform a single function in-place.  Exported for testing.
        static void RunFunc(LirFunc& func, LirReg& nextReg);
    };
} // namespace Rux
