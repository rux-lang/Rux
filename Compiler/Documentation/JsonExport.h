#pragma once

#include "Documentation/Generator.h"
#include "Documentation/Install.h"

#include <span>
#include <vector>

namespace Rux::Documentation {
/// The version of the API snapshot shape. Any change to the shape a consumer reads bumps it.
inline constexpr int JsonSchemaVersion = 1;

/// One package's API snapshot, rendered but not yet written.
struct JsonSnapshot {
    /// The snapshot, named `<Package>.json`.
    OutputFile file;
    /// Invalid documentation and unreadable sources, which would otherwise leave holes in the snapshot.
    std::vector<Diagnostic> diagnostics;

    /// Whether anything reported is an error rather than a warning.
    [[nodiscard]] bool HasErrors() const;
};

/// Render one package's public API as a deterministic JSON snapshot: its manifest metadata, each module with its file
/// header, and every documented item in source order with the members of its `extend` blocks. `modules` must be the
/// compiler driver's folded, semantically valid frontend result, and their source files must still be readable, since
/// a constant's value is shown as it was written.
[[nodiscard]] JsonSnapshot RenderJson(const Manifest &manifest, std::span<const ParseResult> modules,
                                      const GenerateOptions &options);

/// Render one package's snapshot and install it as `<output>/<Package>.json` in a managed documentation directory.
[[nodiscard]] GenerateResult GenerateJson(const Manifest &manifest, std::span<const ParseResult> modules,
                                          const GenerateOptions &options);
} // namespace Rux::Documentation
