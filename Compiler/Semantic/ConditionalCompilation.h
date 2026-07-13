#pragma once

// Conditional compilation: folding of `#if` chains.
//
// A `#if` is an ordinary `if` whose condition is evaluated by the compiler.
// This pass runs before semantic analysis: it evaluates each condition against
// the program's compile-time constants, splices the taken branch into the
// enclosing statement or declaration list, and discards the branches that were
// not taken. Code in a discarded branch is parsed but never type-checked or
// lowered, so it may reference symbols that do not exist on the current build.

#include <string>
#include <string_view>
#include <vector>

#include "Diagnostics/Diagnostics.h"
#include "Semantic/CompileTimeContext.h"
#include "Syntax/Ast/Ast.h"

namespace Rux {

// Folds every `#if` in `modules` in place using the same context later consumed
// by semantic analysis and lowering.
void ResolveConditionalCompilation(const std::vector<Module *> &modules, const CompileTimeContext &context,
                                   std::vector<Diagnostic> &diags);

// Compatibility overload used by embedders and focused tests which only need
// to select an OS. Empty means the host.
void ResolveConditionalCompilation(const std::vector<Module *> &modules, std::string_view targetSystem,
                                   std::vector<Diagnostic> &diags);

} // namespace Rux
