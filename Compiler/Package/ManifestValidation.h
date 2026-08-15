#pragma once

#include "Package/Manifest.h"
#include "Package/ManifestSyntax.h"

#include <expected>
#include <string>

namespace Rux::ManifestDetail {
struct ValidationError {
    Location location;
    std::string message;
    std::optional<std::string> help;
    std::optional<std::string> documentationUrl;
};

struct ValidationResult {
    std::optional<Manifest> manifest;
    std::vector<ValidationError> diagnostics;

    [[nodiscard]] bool Ok() const noexcept {
        return manifest.has_value();
    }
};

/**
 * @brief Map a parsed manifest document onto the strict Version 1 schema.
 *
 * Syntax parsing and schema validation are deliberately separate. This layer
 * consumes only the private value tree and retains its source locations for
 * schema diagnostics.
 */
[[nodiscard]] std::expected<Manifest, ValidationError> ValidateManifestV1(Document document);

/// Validate while retaining every independent schema failure in source order.
[[nodiscard]] ValidationResult ValidateManifestV1All(Document document);
} // namespace Rux::ManifestDetail
