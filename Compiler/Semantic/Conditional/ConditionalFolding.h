#pragma once

#include "Diagnostics/Diagnostics.h"
#include "Semantic/Conditional/ConditionalCompilation.h"

#include <string>
#include <vector>

namespace Rux::ConditionalFolding {
/// Resolve `#if` chains between declarations, rewriting each module in place to keep only the taken branch.
///
/// Folding happens before name resolution so the analyzer only ever sees one version of the program, rather than every
/// stage having to reason about which declarations are live.
void FoldDeclarations(const std::vector<Module *> &modules, ConditionalEvaluator &evaluator,
                      std::vector<Diagnostic> &diags);
/// Resolve `#if` chains inside function bodies, the statement-level counterpart to `FoldDeclarations`.
void FoldStatements(const std::vector<Module *> &modules, ConditionalEvaluator &evaluator,
                    std::vector<Diagnostic> &diags);
} // namespace Rux::ConditionalFolding
