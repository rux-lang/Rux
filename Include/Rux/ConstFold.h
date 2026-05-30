/*
    Rux Compiler
    Copyright © 2026 Rux Contributors
    Licensed under the MIT License
*/

#pragma once

#include "Rux/Lir.h"

namespace Rux {

// Constant Folding and Dead Branch Elimination pass.
//
// Must run after SSA construction and CopyProp.
//
// The pass performs:
//
//   1. Constant evaluation
//      Binary and unary instructions whose every operand is a compile-time
//      constant are evaluated at compile time and replaced with a single
//      Const instruction.  Supports all integer and boolean types; float
//      folding is intentionally omitted to avoid rounding-mode differences.
//      Includes integer exponentiation (Pow).
//
//   2. Constant branch elimination
//      A Branch whose condition is a constant bool is replaced with an
//      unconditional Jump to the always-taken successor.
//
//   3. Dead block elimination
//      Blocks that cannot be reached from the function entry (after step 2)
//      have their instruction lists cleared.  Block indices are preserved so
//      no phi-predecessor reindexing is needed.
//
//   4. Phi predecessor cleanup
//      Phi nodes in reachable blocks that list unreachable predecessors have
//      those entries removed.  A phi that is left with a single predecessor is
//      converted to a trivial Cast copy so that the subsequent CopyProp pass
//      can eliminate it.
//
// After this pass, clients should re-run CopyProp to propagate the new copies
// and remove the resulting dead instructions.
class ConstFold {
public:
    static void Run(LirPackage& pkg);
    static void RunFunc(LirFunc& func, LirReg& nextReg);
};

} // namespace Rux
