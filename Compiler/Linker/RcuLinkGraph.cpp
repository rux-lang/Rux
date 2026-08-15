#include "Linker/RcuLinkGraph.h"

#include <algorithm>
#include <bit>
#include <format>
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

[[nodiscard]] std::string ObjectName(const RcuFile &object, const std::string_view packageName,
                                     const std::size_t objectIndex) {
    if (!object.sourcePath.empty()) {
        return object.sourcePath;
    }
    return std::format("{}[{}]", packageName, objectIndex);
}

[[nodiscard]] constexpr std::size_t RelocationWidth(const std::uint16_t type) noexcept {
    switch (type) {
    case RcuRelType::Abs64:
    case RcuRelType::AArch64Prel64:
        return 8;
    case RcuRelType::Abs32:
    case RcuRelType::Rel32:
    case RcuRelType::AArch64Call26:
    case RcuRelType::AArch64Jump26:
    case RcuRelType::AArch64CondBr19:
    case RcuRelType::AArch64TstBr14:
    case RcuRelType::AArch64AdrPrelPgHi21:
    case RcuRelType::AArch64AddAbsLo12Nc:
    case RcuRelType::AArch64LdstAbsLo12Nc:
    case RcuRelType::AArch64MovwUabsG0:
    case RcuRelType::AArch64MovwUabsG1:
    case RcuRelType::AArch64MovwUabsG2:
    case RcuRelType::AArch64MovwUabsG3:
    case RcuRelType::AArch64Prel32:
        return 4;
    default:
        return 0;
    }
}

[[nodiscard]] constexpr std::optional<std::uint32_t> SectionTypeForIndex(const std::uint16_t index) noexcept {
    switch (index) {
    case RCU_TEXT_IDX:
        return RcuSecType::Text;
    case RCU_RODATA_IDX:
        return RcuSecType::RoData;
    case RCU_DATA_IDX:
        return RcuSecType::Data;
    case RCU_BSS_IDX:
        return RcuSecType::Bss;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] constexpr bool IsLinkableSectionType(const std::uint32_t type) noexcept {
    return type == RcuSecType::Text || type == RcuSecType::RoData || type == RcuSecType::Data ||
           type == RcuSecType::Bss;
}

[[nodiscard]] constexpr bool IsAArch64Relocation(const std::uint16_t type) noexcept {
    return type >= RcuRelType::AArch64Call26 && type <= RcuRelType::AArch64Prel64;
}

void InvalidObject(std::vector<RcuLinkDiagnostic> &diagnostics, std::string message,
                   std::vector<std::string> notes = {}) {
    RcuLinkDiagnostic diagnostic = Diagnostic(RcuLinkDiagnosticKind::InvalidObject);
    diagnostic.message = std::move(message);
    diagnostic.notes = std::move(notes);
    diagnostics.push_back(std::move(diagnostic));
}

void ValidateObject(const RcuFile &object, const std::string_view packageName, const std::size_t objectIndex,
                    std::vector<RcuLinkDiagnostic> &diagnostics) {
    const std::string objectName = ObjectName(object, packageName, objectIndex);
    std::map<std::uint32_t, const RcuSection *> sectionsByType;
    for (const RcuSection &section : object.sections) {
        if (!IsLinkableSectionType(section.type)) {
            InvalidObject(diagnostics,
                          std::format("RCU object '{}' contains unsupported section '{}'", objectName, section.name),
                          {std::format("section type: {}", section.type)});
            continue;
        }
        if (!sectionsByType.emplace(section.type, &section).second) {
            InvalidObject(
                diagnostics,
                std::format("RCU object '{}' contains more than one section of type {}", objectName, section.type),
                {std::format("duplicate section: '{}'", section.name)});
        }
        if (section.alignment == 0 || !std::has_single_bit(section.alignment)) {
            InvalidObject(
                diagnostics,
                std::format("RCU object '{}' has invalid alignment for section '{}'", objectName, section.name),
                {std::format("alignment: {} bytes", section.alignment),
                 "section alignment must be a non-zero power of two"});
        }
    }

    for (const RcuSymbol &symbol : object.symbols) {
        if (symbol.sectionIdx == RCU_SEC_EXTERNAL || symbol.sectionIdx == RCU_SEC_ABSOLUTE) {
            continue;
        }
        const auto expectedType = SectionTypeForIndex(symbol.sectionIdx);
        const auto section = expectedType ? sectionsByType.find(*expectedType) : sectionsByType.end();
        if (!expectedType || section == sectionsByType.end()) {
            InvalidObject(
                diagnostics,
                std::format("RCU object '{}' places symbol '{}' in a missing section", objectName, symbol.name),
                {std::format("section index: {}", symbol.sectionIdx)});
            continue;
        }
        const std::size_t sectionSize = section->second->data.size();
        if (symbol.value > sectionSize || symbol.size > sectionSize - symbol.value) {
            InvalidObject(diagnostics,
                          std::format("RCU object '{}' places symbol '{}' outside section '{}'", objectName,
                                      symbol.name, section->second->name),
                          {std::format("symbol offset: {}; symbol size: {} bytes", symbol.value, symbol.size),
                           std::format("section size: {} bytes", sectionSize)});
        }
    }

    for (const RcuSection &section : object.sections) {
        for (const RcuReloc &relocation : section.relocs) {
            if (relocation.type == RcuRelType::None) {
                continue;
            }
            const std::size_t width = RelocationWidth(relocation.type);
            const std::string symbol = relocation.symbolIndex < object.symbols.size()
                                         ? object.symbols[relocation.symbolIndex].name
                                         : std::format("<index {}>", relocation.symbolIndex);
            if (relocation.symbolIndex >= object.symbols.size()) {
                InvalidObject(diagnostics,
                              std::format("RCU object '{}' has a relocation in section '{}' with an invalid symbol "
                                          "index",
                                          objectName, section.name),
                              {std::format("source offset: {}; symbol index: {}; symbol count: {}",
                                           relocation.sectionOffset, relocation.symbolIndex, object.symbols.size())});
                continue;
            }
            if (width == 0) {
                InvalidObject(
                    diagnostics,
                    std::format("RCU object '{}' has an unknown relocation to symbol '{}'", objectName, symbol),
                    {std::format("source section: '{}'; offset: {}; relocation type: {}", section.name,
                                 relocation.sectionOffset, relocation.type)});
                continue;
            }
            if (object.arch == RcuArch::X86_64 && IsAArch64Relocation(relocation.type)) {
                InvalidObject(
                    diagnostics,
                    std::format("RCU object '{}' uses AArch64 relocation '{}' for x86-64 symbol '{}'", objectName,
                                RcuRelTypeName(relocation.type), symbol),
                    {std::format("source section: '{}'; offset: {}", section.name, relocation.sectionOffset)});
            }
            if (IsAArch64Relocation(relocation.type) && relocation.type <= RcuRelType::AArch64MovwUabsG3 &&
                section.type != RcuSecType::Text) {
                InvalidObject(diagnostics,
                              std::format("RCU object '{}' uses instruction relocation '{}' outside a text section",
                                          objectName, RcuRelTypeName(relocation.type)),
                              {std::format("source section: '{}'; symbol: '{}'", section.name, symbol)});
            }
            if (relocation.sectionOffset > section.data.size() ||
                width > section.data.size() - relocation.sectionOffset) {
                InvalidObject(
                    diagnostics,
                    std::format("RCU object '{}' has relocation '{}' to symbol '{}' outside section '{}'", objectName,
                                RcuRelTypeName(relocation.type), symbol, section.name),
                    {std::format("source offset: {}; relocation width: {} bytes", relocation.sectionOffset, width),
                     std::format("section size: {} bytes", section.data.size())});
            }
        }
    }
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
    for (std::size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex) {
        ValidateObject(objects[objectIndex], packageName, objectIndex, graph.diagnostics);
    }
    if (graph.HasErrors()) {
        return graph;
    }

    std::unordered_map<std::string, RcuSymbolLocation> definitionsByName;
    std::unordered_map<std::string, RcuSymbolLocation> strongDefinitions;
    for (std::size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex) {
        const RcuFile &object = objects[objectIndex];
        for (std::size_t symbolIndex = 0; symbolIndex < object.symbols.size(); ++symbolIndex) {
            const RcuSymbol &symbol = object.symbols[symbolIndex];
            if (!IsDefinition(symbol)) {
                continue;
            }
            const RcuSymbolLocation location{objectIndex, symbolIndex};
            graph.definitions.push_back({symbol.name, location, symbol.kind, symbol.visibility});
            if (symbol.visibility == RcuSymVis::Global) {
                const auto [existing, inserted] = strongDefinitions.emplace(symbol.name, location);
                if (!inserted) {
                    RcuLinkDiagnostic diagnostic = Diagnostic(RcuLinkDiagnosticKind::DuplicateDefinition, symbol.name);
                    diagnostic.objectName =
                        ObjectName(objects[existing->second.objectIndex], packageName, existing->second.objectIndex);
                    diagnostic.relatedObjectName = ObjectName(object, packageName, objectIndex);
                    graph.diagnostics.push_back(std::move(diagnostic));
                }
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
                        RcuLinkDiagnostic diagnostic = Diagnostic(RcuLinkDiagnosticKind::UndefinedSymbol, symbol.name);
                        diagnostic.objectName = ObjectName(object, packageName, objectIndex);
                        diagnostic.notes.push_back(std::format("referenced by RCU object '{}', section '{}', offset {}",
                                                               diagnostic.objectName, section.name,
                                                               relocation.sectionOffset));
                        graph.diagnostics.push_back(std::move(diagnostic));
                    }
                }
                graph.references.push_back(reference);
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
