#pragma once

#include "Semantic/SemanticModel.h"

#include <filesystem>

namespace Rux {
class SemanticPrinter {
public:
    static bool Dump(const SemanticModel &model, const std::filesystem::path &path);
};
} // namespace Rux
