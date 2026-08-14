#pragma once

#include "Diagnostics/Diagnostics.h"
#include "Semantic/ConditionalCompilation.h"

#include <string>
#include <vector>

namespace Rux::ConditionalFolding {
[[nodiscard]] std::string FilePathToModulePath(const std::string &filePath);

void FoldDeclarations(const std::vector<Module *> &modules, ConditionalEvaluator &evaluator,
                      std::vector<Diagnostic> &diags);
void FoldStatements(const std::vector<Module *> &modules, ConditionalEvaluator &evaluator,
                    std::vector<Diagnostic> &diags);
} // namespace Rux::ConditionalFolding
