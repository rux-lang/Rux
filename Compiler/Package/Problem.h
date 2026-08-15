#pragma once

#include <optional>
#include <string>
#include <vector>

namespace Rux {
/**
 * @brief A package validation failure with separately renderable context.
 *
 * Package code owns the cause and supporting context but performs no terminal
 * output. CLI callers decide which stream and style to use.
 */
struct PackageProblem {
    std::string message;
    std::vector<std::string> notes;
    std::optional<std::string> help;
};
} // namespace Rux
