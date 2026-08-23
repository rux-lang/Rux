#pragma once

#include "Diagnostics/Diagnostics.h"
#include "Package/Manifest.h"
#include "Syntax/Parser/Parser.h"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace Rux::Documentation {
struct GenerateOptions {
    std::filesystem::path packageRoot;
    std::filesystem::path outputDirectory;
    bool includePrivate = false;
};

/// The outcome of generating a site: whether it was written, and what was wrong with it.
///
/// Content problems and operational failures arrive the same way, because a page that silently dropped a link is
/// as much a failure of the run as one that could not be written at all.
struct GenerateResult {
    /// Whether the site was written.
    bool ok = false;
    /// What was wrong: unwritable output, a route two declarations both claim, a link that would not be emitted, or
    /// a package whose declarations never reached the page.
    std::vector<Diagnostic> diagnostics;

    /// Whether anything reported is an error rather than a warning.
    [[nodiscard]] bool HasErrors() const;
};

/// Generate one deterministic, self-contained package documentation site. `modules` must be the compiler driver's
/// folded, semantically valid frontend result.
[[nodiscard]] GenerateResult Generate(const Manifest &manifest, std::span<const ParseResult> modules,
                                      const GenerateOptions &options);
} // namespace Rux::Documentation
