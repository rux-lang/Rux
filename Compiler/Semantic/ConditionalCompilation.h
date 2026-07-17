#pragma once

// Conditional compilation: folding of `when` chains.
//
// A `when` has the shape of an `if`, but its condition is evaluated by the
// compiler rather than by the running program. This pass runs before semantic
// analysis: it evaluates each condition against the program's compile-time
// constants, splices the taken branch into the enclosing statement or
// declaration list, and discards the branches that were not taken. Code in a
// discarded branch is parsed but never resolved, type-checked or lowered, so it
// may reference symbols that do not exist on the current build.

#include "Diagnostics/Diagnostics.h"
#include "Semantic/CompileTimeContext.h"
#include "Syntax/Ast/Ast.h"

#include <string>
#include <string_view>
#include <vector>

namespace Rux {
// Folds every `when` in `modules` in place using the same context later
// consumed by semantic analysis and lowering.
void ResolveConditionalCompilation(const std::vector<Module *> &modules, const CompileTimeContext &context,
                                   std::vector<Diagnostic> &diags);

// Compatibility overload used by embedders and focused tests which only need
// to select an OS. Empty means the host.
void ResolveConditionalCompilation(const std::vector<Module *> &modules, std::string_view targetSystem,
                                   std::vector<Diagnostic> &diags);
} // namespace Rux
