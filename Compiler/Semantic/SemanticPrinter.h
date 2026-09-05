#pragma once

#include "Semantic/Model/SemanticModel.h"

#include <filesystem>

namespace Rux {
/// Writes the resolved semantic model in a readable form, showing the types and resolutions analysis attached to the
/// AST. A debugging aid only.
class SemanticPrinter {
public:
    /// @return false when the file could not be written
    static bool Dump(const SemanticModel &model, const std::filesystem::path &path);
};
} // namespace Rux
