#pragma once

#include "Linker/ArtifactKind.h"
#include "Object/Rcu/Rcu.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Rux {
struct RcuSymbolLocation {
    std::size_t objectIndex = 0;
    std::size_t symbolIndex = 0;

    auto operator<=>(const RcuSymbolLocation &) const = default;
};

struct RcuLinkDefinition {
    std::string name;
    RcuSymbolLocation location;
    std::uint8_t kind = RcuSymKind::Unknown;
    std::uint8_t visibility = RcuSymVis::Local;
};

enum class RcuLinkResolution {
    LocalDefinition,
    CrossObjectDefinition,
    External,
    Unresolved,
};

struct RcuLinkReference {
    std::size_t objectIndex = 0;
    std::size_t sectionIndex = 0;
    std::size_t relocationIndex = 0;
    std::size_t symbolIndex = 0;
    RcuLinkResolution resolution = RcuLinkResolution::Unresolved;
    std::optional<RcuSymbolLocation> definition;
};

struct RcuReferencedExternal {
    std::string name;
    std::uint8_t kind = RcuSymKind::Unknown;
    std::vector<std::size_t> referenceIndices;
};

enum class RcuLinkDiagnosticKind {
    UnsupportedArchitecture,
    ArchitectureMismatch,
    DuplicateDefinition,
    UndefinedSymbol,
    MissingEntryPoint,
};

struct RcuLinkDiagnostic {
    RcuLinkDiagnosticKind kind = RcuLinkDiagnosticKind::UndefinedSymbol;
    std::string symbol;
    std::string objectName;
    std::uint8_t actualArchitecture = RcuArch::Unknown;
    std::uint8_t expectedArchitecture = RcuArch::Unknown;
};

// A target-format-independent index of the symbol relationships in a set of
// RCU objects. Locations are stable object/symbol indexes rather than pointers,
// so the graph is an ordinary value and can be retained by the linker facade.
class RcuLinkGraph {
public:
    [[nodiscard]] static RcuLinkGraph Build(std::span<const RcuFile> objects, std::string_view packageName,
                                            ArtifactKind artifactKind, Target::Arch targetArchitecture);

    [[nodiscard]] const std::vector<RcuLinkDefinition> &Definitions() const {
        return definitions;
    }

    [[nodiscard]] const std::vector<RcuLinkReference> &References() const {
        return references;
    }

    [[nodiscard]] const std::vector<RcuReferencedExternal> &ReferencedExternals() const {
        return referencedExternals;
    }

    [[nodiscard]] const std::vector<RcuSymbolLocation> &ExportRoots() const {
        return exportRoots;
    }

    [[nodiscard]] const std::optional<RcuSymbolLocation> &EntryRoot() const {
        return entryRoot;
    }

    [[nodiscard]] const std::vector<RcuLinkDiagnostic> &Diagnostics() const {
        return diagnostics;
    }

    [[nodiscard]] bool HasErrors() const {
        return !diagnostics.empty();
    }

    [[nodiscard]] std::optional<RcuSymbolLocation> FindDefinition(std::string_view name) const;

private:
    std::vector<RcuLinkDefinition> definitions;
    std::vector<RcuLinkDefinition> namedDefinitions;
    std::vector<RcuLinkReference> references;
    std::vector<RcuReferencedExternal> referencedExternals;
    std::vector<RcuSymbolLocation> exportRoots;
    std::optional<RcuSymbolLocation> entryRoot;
    std::vector<RcuLinkDiagnostic> diagnostics;
};
} // namespace Rux
