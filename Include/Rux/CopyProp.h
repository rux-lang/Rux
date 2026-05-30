/*
    Rux Compiler
    Copyright © 2026 Rux Contributors
    Licensed under the MIT License
*/

#pragma once

#include "Rux/Lir.h"

namespace Rux {

// Copy Propagation and Dead Code Elimination pass.
//
// Runs after SSA construction.  Two sub-passes in one traversal:
//
//   1. Copy propagation
//      - Constant deduplication: if two Const instructions produce the same
//        (type, value) pair, replace all uses of the duplicate with the first.
//      - Trivial-copy elimination: Cast instructions with an empty strArg
//        (produced by SsaConstruct's renaming phase) are identity copies;
//        all uses of their dst are replaced with the original src register.
//
//   2. Dead code elimination (DCE)
//      - Any instruction whose dst has zero uses and whose opcode has no
//        observable side effects is removed.  Iterated to a fixed point so
//        that cascading dead instructions (e.g. dead const feeding a dead
//        cast) are fully pruned in a single call.
//
// The pass is strictly local to each function and preserves SSA form.
class CopyProp {
public:
    static void Run(LirPackage& pkg);
    static void RunFunc(LirFunc& func);
};

} // namespace Rux
