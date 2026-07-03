#pragma once

#include <filesystem>

#include "Semantic/SemanticModel.h"

namespace Rux {

class SemanticPrinter {
public:
    static bool Dump(const SemanticModel &model, const std::filesystem::path &path);
};

} // namespace Rux
