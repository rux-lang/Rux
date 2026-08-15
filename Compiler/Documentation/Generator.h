#pragma once

#include "Diagnostics/Diagnostics.h"
#include "Package/Manifest.h"
#include "Syntax/Parser/Parser.h"

#include <filesystem>
#include <span>
#include <string>

namespace Rux::Documentation {
struct GenerateOptions {
    std::filesystem::path packageRoot;
    std::filesystem::path outputDirectory;
    bool includePrivate = false;
};

// Generate one deterministic, self-contained package documentation site.
// `modules` must be the compiler driver's folded, semantically valid frontend
// result. On failure, `error` contains an operational diagnostic.
[[nodiscard]] bool Generate(const Manifest &manifest, std::span<const ParseResult> modules,
                            const GenerateOptions &options, Diagnostic &error);
} // namespace Rux::Documentation
