#pragma once

#include "Package/Manifest.h"

#include <string>
#include <string_view>
#include <vector>

namespace Rux {
/// The oldest compiler release a published package may declare as its minimum.
inline constexpr std::string_view publicationMinRuxFloor = "0.4.0";

/**
 * @brief Check a locally valid manifest against the publication profile.
 *
 * Publication requires a registry identity, a supported artifact kind and
 * dependencies that can be resolved without the publisher's source tree.
 * Rejections have no source position because missing required publication
 * fields have no token to point at.
 */
[[nodiscard]] std::vector<std::string> ValidateForPublication(const Manifest &manifest);
} // namespace Rux
