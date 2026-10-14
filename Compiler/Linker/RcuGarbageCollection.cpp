// Function-level garbage collection over RCU objects, before their sections are merged.

#include "Linker/RcuGarbageCollection.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>

namespace Rux {
namespace {
struct Atom {
    std::uint32_t start = 0;
    std::uint32_t end = 0;
};

struct ObjectAtoms {
    std::optional<std::size_t> textSection;
    std::vector<Atom> atoms;
    /// The atom every symbol of the text section lies in, by symbol index.
    std::vector<std::optional<std::size_t>> symbolAtoms;
    std::vector<bool> live;
};

/// The atoms of `object`: the ranges of its text symbols, merged where they overlap, and the atom each text symbol is
/// in. A symbol of no size names the atom it points into.
ObjectAtoms FindAtoms(const RcuFile &object) {
    ObjectAtoms result;
    result.symbolAtoms.resize(object.symbols.size());
    for (std::size_t index = 0; index < object.sections.size(); ++index)
        if (object.sections[index].type == RcuSecType::Text)
            result.textSection = index;
    if (!result.textSection)
        return result;
    const auto text = static_cast<std::uint16_t>(*result.textSection);
    std::vector<Atom> ranges;
    for (const RcuSymbol &symbol : object.symbols)
        if (symbol.sectionIdx == text && symbol.size > 0)
            ranges.push_back({symbol.value, symbol.value + symbol.size});
    std::ranges::sort(ranges, {}, &Atom::start);
    for (const Atom &range : ranges) {
        if (!result.atoms.empty() && range.start < result.atoms.back().end)
            result.atoms.back().end = std::max(result.atoms.back().end, range.end);
        else
            result.atoms.push_back(range);
    }
    for (std::size_t index = 0; index < object.symbols.size(); ++index) {
        const RcuSymbol &symbol = object.symbols[index];
        if (symbol.sectionIdx != text)
            continue;
        const auto after = std::ranges::upper_bound(result.atoms, symbol.value, {}, &Atom::start);
        if (after != result.atoms.begin())
            result.symbolAtoms[index] = static_cast<std::size_t>(after - result.atoms.begin()) - 1;
    }
    result.live.assign(result.atoms.size(), false);
    return result;
}

/// Where an object's stack-trace function tables lie: what relocations there refer to is weak.
struct FunctionTable {
    std::size_t section = 0;
    std::uint32_t start = 0;
    std::uint32_t end = 0;
};

std::vector<FunctionTable> FunctionTables(const RcuFile &object) {
    std::vector<FunctionTable> tables;
    for (const RcuSymbol &symbol : object.symbols)
        if (symbol.sectionIdx < object.sections.size() &&
            std::string_view(symbol.name).starts_with(kFunctionTablePrefix))
            tables.push_back({symbol.sectionIdx, symbol.value, symbol.value + symbol.size});
    return tables;
}
} // namespace

bool CollectUnreachableFunctions(std::vector<RcuFile> &objects, const RcuLinkGraph &graph, const std::uint8_t padding) {
    std::vector<ObjectAtoms> atoms;
    atoms.reserve(objects.size());
    for (const RcuFile &object : objects)
        atoms.push_back(FindAtoms(object));

    // Which references each object's text section makes, in relocation order, so a live atom finds its own.
    std::vector<std::vector<std::size_t>> textReferences(objects.size());
    std::vector<std::size_t> worklist;
    std::vector<std::vector<std::size_t>> pending(objects.size());
    const auto mark = [&](const RcuSymbolLocation location) {
        const auto &objectAtoms = atoms[location.objectIndex];
        const auto atom = objectAtoms.symbolAtoms[location.symbolIndex];
        if (!atom || objectAtoms.live[*atom])
            return;
        atoms[location.objectIndex].live[*atom] = true;
        pending[location.objectIndex].push_back(*atom);
        worklist.push_back(location.objectIndex);
    };
    std::vector<std::vector<FunctionTable>> tables;
    tables.reserve(objects.size());
    for (const RcuFile &object : objects)
        tables.push_back(FunctionTables(object));
    const auto inFunctionTable = [&](const std::size_t objectIndex, const std::size_t section,
                                     const std::uint32_t offset) {
        return std::ranges::any_of(tables[objectIndex], [&](const FunctionTable &table) {
            return table.section == section && offset >= table.start && offset < table.end;
        });
    };
    const auto &references = graph.References();
    for (std::size_t index = 0; index < references.size(); ++index) {
        const RcuLinkReference &reference = references[index];
        if (atoms[reference.objectIndex].textSection == reference.sectionIndex) {
            textReferences[reference.objectIndex].push_back(index);
            continue;
        }
        const RcuFile &object = objects[reference.objectIndex];
        const auto offset = object.sections[reference.sectionIndex].relocs[reference.relocationIndex].sectionOffset;
        if (reference.definition && !inFunctionTable(reference.objectIndex, reference.sectionIndex, offset))
            mark(*reference.definition);
    }
    if (graph.EntryRoot())
        mark(*graph.EntryRoot());
    for (const RcuSymbolLocation root : graph.ExportRoots())
        mark(root);
    if (const auto entry = graph.FindDefinition("DllMain"))
        mark(*entry);

    while (!worklist.empty()) {
        const std::size_t objectIndex = worklist.back();
        worklist.pop_back();
        while (!pending[objectIndex].empty()) {
            const std::size_t atomIndex = pending[objectIndex].back();
            pending[objectIndex].pop_back();
            const Atom atom = atoms[objectIndex].atoms[atomIndex];
            const RcuSection &text = objects[objectIndex].sections[*atoms[objectIndex].textSection];
            for (const std::size_t index : textReferences[objectIndex]) {
                const RcuLinkReference &reference = references[index];
                const auto offset = text.relocs[reference.relocationIndex].sectionOffset;
                if (offset >= atom.start && offset < atom.end && reference.definition)
                    mark(*reference.definition);
            }
        }
    }

    bool removed = false;
    for (std::size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex) {
        const ObjectAtoms &objectAtoms = atoms[objectIndex];
        if (!objectAtoms.textSection || std::ranges::all_of(objectAtoms.live, [](const bool live) { return live; }))
            continue;
        removed = true;
        RcuFile &object = objects[objectIndex];
        RcuSection &text = object.sections[*objectAtoms.textSection];
        const auto textIndex = static_cast<std::uint16_t>(*objectAtoms.textSection);

        // Live atoms move down, each keeping its offset modulo sixteen.
        std::vector<std::optional<std::uint32_t>> moved(objectAtoms.atoms.size());
        std::vector<std::uint8_t> data;
        data.reserve(text.data.size());
        for (std::size_t atomIndex = 0; atomIndex < objectAtoms.atoms.size(); ++atomIndex) {
            if (!objectAtoms.live[atomIndex])
                continue;
            const Atom atom = objectAtoms.atoms[atomIndex];
            const auto residue = static_cast<std::uint32_t>((atom.start - data.size()) % 16);
            data.resize(data.size() + residue, padding);
            moved[atomIndex] = static_cast<std::uint32_t>(data.size());
            data.insert(data.end(), text.data.begin() + atom.start, text.data.begin() + atom.end);
        }
        const auto relocate = [&](const std::uint32_t offset) -> std::optional<std::uint32_t> {
            const auto after = std::ranges::upper_bound(objectAtoms.atoms, offset, {}, &Atom::start);
            if (after == objectAtoms.atoms.begin())
                return std::nullopt;
            const auto atomIndex = static_cast<std::size_t>(after - objectAtoms.atoms.begin()) - 1;
            const Atom atom = objectAtoms.atoms[atomIndex];
            if (!moved[atomIndex] || offset > atom.end)
                return std::nullopt;
            return *moved[atomIndex] + (offset - atom.start);
        };

        // Symbols of dropped functions go; the rest are renumbered and moved.
        std::vector<std::optional<std::uint32_t>> renumbered(object.symbols.size());
        std::vector<RcuSymbol> symbols;
        symbols.reserve(object.symbols.size());
        for (std::size_t index = 0; index < object.symbols.size(); ++index) {
            RcuSymbol symbol = std::move(object.symbols[index]);
            if (symbol.sectionIdx == textIndex) {
                const auto value = relocate(symbol.value);
                if (!value)
                    continue;
                symbol.value = *value;
            }
            renumbered[index] = static_cast<std::uint32_t>(symbols.size());
            symbols.push_back(std::move(symbol));
        }
        object.symbols = std::move(symbols);
        text.data = std::move(data);

        for (std::size_t sectionIndex = 0; sectionIndex < object.sections.size(); ++sectionIndex) {
            RcuSection &section = object.sections[sectionIndex];
            std::vector<RcuReloc> relocs;
            relocs.reserve(section.relocs.size());
            for (RcuReloc reloc : section.relocs) {
                if (sectionIndex == textIndex) {
                    const auto offset = relocate(reloc.sectionOffset);
                    if (!offset)
                        continue;
                    reloc.sectionOffset = *offset;
                }
                // Only a function table refers to a dropped function from what is kept; its slot stays zero.
                const auto symbol =
                    reloc.symbolIndex < renumbered.size() ? renumbered[reloc.symbolIndex] : std::nullopt;
                if (!symbol)
                    continue;
                reloc.symbolIndex = *symbol;
                relocs.push_back(reloc);
            }
            section.relocs = std::move(relocs);
        }
    }
    return removed;
}
} // namespace Rux
