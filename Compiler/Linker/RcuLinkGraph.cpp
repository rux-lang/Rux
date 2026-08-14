#include "Linker/RcuLinkGraph.h"

#include <algorithm>
#include <map>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace Rux {
namespace {
[[nodiscard]] bool IsExtern(const RcuSymbol &symbol) {
    return symbol.kind == RcuSymKind::ExternFunc || symbol.kind == RcuSymKind::ExternData;
}

[[nodiscard]] bool IsDefinition(const RcuSymbol &symbol) {
    return !IsExtern(symbol) && symbol.sectionIdx != RCU_SEC_EXTERNAL && !symbol.name.empty();
}

[[nodiscard]] bool IsCrossObjectVisible(const RcuSymbol &symbol) {
    // Private functions have final package-unique names and are referenced by
    // extern-shaped symbol entries in other source-file objects. Generated
    // local data and constant labels remain object-relative.
    return symbol.visibility != RcuSymVis::Local || symbol.kind == RcuSymKind::Func;
}

[[nodiscard]] RcuLinkDiagnostic Diagnostic(const RcuLinkDiagnosticKind kind, std::string symbol = {}) {
    RcuLinkDiagnostic diagnostic;
    diagnostic.kind = kind;
    diagnostic.symbol = std::move(symbol);
    return diagnostic;
}
} // namespace

RcuLinkGraph RcuLinkGraph::Build(const std::span<const RcuFile> objects, const std::string_view packageName,
                                 const ArtifactKind artifactKind, const Target::Arch targetArchitecture) {
    RcuLinkGraph graph;
    const std::uint8_t expectedArchitecture = RcuArchFor(targetArchitecture);
    if (expectedArchitecture == RcuArch::Unknown) {
        graph.diagnostics.push_back(Diagnostic(RcuLinkDiagnosticKind::UnsupportedArchitecture));
        return graph;
    }
    for (std::size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex) {
        const RcuFile &object = objects[objectIndex];
        if (object.arch != expectedArchitecture) {
            RcuLinkDiagnostic diagnostic = Diagnostic(RcuLinkDiagnosticKind::ArchitectureMismatch);
            diagnostic.objectName = object.sourcePath.empty() ? std::string(packageName) : object.sourcePath;
            diagnostic.actualArchitecture = object.arch;
            diagnostic.expectedArchitecture = expectedArchitecture;
            graph.diagnostics.push_back(std::move(diagnostic));
            return graph;
        }
    }

    std::unordered_map<std::string, RcuSymbolLocation> definitionsByName;
    std::unordered_set<std::string> strongDefinitions;
    for (std::size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex) {
        const RcuFile &object = objects[objectIndex];
        for (std::size_t symbolIndex = 0; symbolIndex < object.symbols.size(); ++symbolIndex) {
            const RcuSymbol &symbol = object.symbols[symbolIndex];
            if (!IsDefinition(symbol)) {
                continue;
            }
            const RcuSymbolLocation location{objectIndex, symbolIndex};
            graph.definitions.push_back({symbol.name, location, symbol.kind, symbol.visibility});
            if (symbol.visibility == RcuSymVis::Global && !strongDefinitions.insert(symbol.name).second) {
                graph.diagnostics.push_back(Diagnostic(RcuLinkDiagnosticKind::DuplicateDefinition, symbol.name));
            }
            if (!IsCrossObjectVisible(symbol)) {
                continue;
            }
            const auto existing = definitionsByName.find(symbol.name);
            if (existing == definitionsByName.end() ||
                (objects[existing->second.objectIndex].symbols[existing->second.symbolIndex].visibility !=
                     RcuSymVis::Global &&
                 symbol.visibility == RcuSymVis::Global)) {
                definitionsByName[symbol.name] = location;
            }
        }
    }

    std::ranges::sort(graph.definitions, {}, [](const RcuLinkDefinition &definition) {
        return std::tuple(definition.name, definition.location.objectIndex, definition.location.symbolIndex);
    });
    for (const auto &[name, location] : definitionsByName) {
        const RcuSymbol &symbol = objects[location.objectIndex].symbols[location.symbolIndex];
        graph.namedDefinitions.push_back({name, location, symbol.kind, symbol.visibility});
    }
    std::ranges::sort(graph.namedDefinitions, {}, &RcuLinkDefinition::name);

    if (artifactKind != ArtifactKind::Executable) {
        for (const RcuLinkDefinition &definition : graph.namedDefinitions) {
            if (definition.visibility != RcuSymVis::Local) {
                graph.exportRoots.push_back(definition.location);
            }
        }
    }
    else if (const auto main = definitionsByName.find("Main"); main != definitionsByName.end()) {
        graph.entryRoot = main->second;
    }
    else {
        graph.diagnostics.push_back(Diagnostic(RcuLinkDiagnosticKind::MissingEntryPoint, "Main"));
    }

    std::map<std::string, RcuReferencedExternal> externalsByName;
    std::unordered_set<std::string> undefinedSymbols;
    for (std::size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex) {
        const RcuFile &object = objects[objectIndex];
        for (std::size_t sectionIndex = 0; sectionIndex < object.sections.size(); ++sectionIndex) {
            const RcuSection &section = object.sections[sectionIndex];
            for (std::size_t relocationIndex = 0; relocationIndex < section.relocs.size(); ++relocationIndex) {
                const RcuReloc &relocation = section.relocs[relocationIndex];
                if (relocation.type == RcuRelType::None || relocation.symbolIndex >= object.symbols.size()) {
                    continue;
                }
                const RcuSymbol &symbol = object.symbols[relocation.symbolIndex];
                RcuLinkReference reference;
                reference.objectIndex = objectIndex;
                reference.sectionIndex = sectionIndex;
                reference.relocationIndex = relocationIndex;
                reference.symbolIndex = relocation.symbolIndex;
                if (IsDefinition(symbol)) {
                    reference.resolution = RcuLinkResolution::LocalDefinition;
                    reference.definition = RcuSymbolLocation{objectIndex, relocation.symbolIndex};
                }
                else if (const auto definition = definitionsByName.find(symbol.name);
                         definition != definitionsByName.end()) {
                    reference.definition = definition->second;
                    reference.resolution = definition->second.objectIndex == objectIndex
                                             ? RcuLinkResolution::LocalDefinition
                                             : RcuLinkResolution::CrossObjectDefinition;
                }
                else if (IsExtern(symbol)) {
                    reference.resolution = RcuLinkResolution::External;
                    auto &[name, kind, referenceIndices] = externalsByName[symbol.name];
                    name = symbol.name;
                    kind = symbol.kind;
                    referenceIndices.push_back(graph.references.size());
                }
                else {
                    reference.resolution = RcuLinkResolution::Unresolved;
                    if (!symbol.name.empty() && undefinedSymbols.insert(symbol.name).second) {
                        graph.diagnostics.push_back(Diagnostic(RcuLinkDiagnosticKind::UndefinedSymbol, symbol.name));
                    }
                }
                graph.references.push_back(std::move(reference));
            }
        }
    }
    for (auto &[name, external] : externalsByName) {
        (void)name;
        graph.referencedExternals.push_back(std::move(external));
    }
    return graph;
}

std::optional<RcuSymbolLocation> RcuLinkGraph::FindDefinition(const std::string_view name) const {
    const auto definition = std::ranges::lower_bound(namedDefinitions, name, {}, &RcuLinkDefinition::name);
    return definition != namedDefinitions.end() && definition->name == name ? std::optional(definition->location)
                                                                            : std::nullopt;
}
} // namespace Rux
