#pragma once

namespace Rux {
/// What one package builds. The manifest's `SourceLibrary` type has no entry here because it produces no artifact of
/// its own: it is compiled into its dependents. The linker reads this to choose the image form, and declaration pruning
/// reads it to decide whether `Main` alone or every public symbol is a reachability root.
enum class ArtifactKind {
    Executable,
    SharedLibrary,
    StaticLibrary,
};
} // namespace Rux
