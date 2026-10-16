// Garbage collection over RCU objects, before their sections are merged: functions and data no root reaches are
// dropped.

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

/// Whether the atoms of a section of this type can be dropped. Zero-fill, thread-local and metadata sections are
/// kept whole.
bool Collectable(const RcuSection &section) {
    return section.type == RcuSecType::Text || section.type == RcuSecType::RoData || section.type == RcuSecType::Data;
}

struct SectionAtoms {
    bool collectable = false;
    std::vector<Atom> atoms;
    std::vector<bool> live;
    /// The references made from this section, by the offset of their relocation, so an atom finds its own.
    std::vector<std::pair<std::uint32_t, std::size_t>> references;
};

struct ObjectAtoms {
    std::vector<SectionAtoms> sections;
    /// The section and atom every symbol defined in a collectable section names, by symbol index.
    std::vector<std::optional<std::pair<std::size_t, std::size_t>>> symbolAtoms;
};

/// The atom of `atoms` that holds `offset`, or the one before it when `inside` is false.
std::optional<std::size_t> AtomAt(const std::vector<Atom> &atoms, const std::uint32_t offset, const bool inside) {
    const auto after = std::ranges::upper_bound(atoms, offset, {}, &Atom::start);
    if (after == atoms.begin())
        return std::nullopt;
    const auto index = static_cast<std::size_t>(after - atoms.begin()) - 1;
    if (inside && offset >= atoms[index].end)
        return std::nullopt;
    return index;
}

/// The atoms of `object`. In each collectable section they are the ranges of its sized symbols, merged where they
/// overlap. Code between functions is padding, but bytes of a data section no sized symbol covers may be a literal
/// reached through a label of no size: a run of them that holds anything but zeros, a relocation or a label is an atom
/// of its own that is always live, and so is every atom of a section something refers to by the section's own symbol.
ObjectAtoms FindAtoms(const RcuFile &object) {
    ObjectAtoms result;
    result.sections.resize(object.sections.size());
    result.symbolAtoms.resize(object.symbols.size());
    for (std::size_t sectionIndex = 0; sectionIndex < object.sections.size(); ++sectionIndex) {
        const RcuSection &section = object.sections[sectionIndex];
        SectionAtoms &atoms = result.sections[sectionIndex];
        atoms.collectable = Collectable(section);
        if (!atoms.collectable)
            continue;
        std::vector<Atom> ranges;
        for (const RcuSymbol &symbol : object.symbols)
            if (symbol.sectionIdx == sectionIndex && symbol.size > 0)
                ranges.push_back({symbol.value, symbol.value + symbol.size});
        std::ranges::sort(ranges, {}, &Atom::start);
        std::vector<Atom> merged;
        for (const Atom &range : ranges) {
            if (!merged.empty() && range.start < merged.back().end)
                merged.back().end = std::max(merged.back().end, range.end);
            else
                merged.push_back(range);
        }
        if (section.type == RcuSecType::Text) {
            atoms.atoms = std::move(merged);
            atoms.live.assign(atoms.atoms.size(), false);
            continue;
        }
        // A data section's uncovered runs, kept when they could hold something.
        std::vector<bool> pinned;
        const auto size = static_cast<std::uint32_t>(section.data.size());
        std::uint32_t at = 0;
        const auto addGap = [&](const std::uint32_t start, const std::uint32_t end) {
            if (start >= end)
                return;
            bool holds = std::any_of(section.data.begin() + start, section.data.begin() + end,
                                     [](const std::uint8_t byte) { return byte != 0; });
            holds = holds || std::ranges::any_of(section.relocs, [&](const RcuReloc &reloc) {
                        return reloc.sectionOffset >= start && reloc.sectionOffset < end;
                    });
            holds = holds || std::ranges::any_of(object.symbols, [&](const RcuSymbol &symbol) {
                        return symbol.sectionIdx == sectionIndex && symbol.value >= start && symbol.value < end;
                    });
            if (!holds)
                return;
            atoms.atoms.push_back({start, end});
            pinned.push_back(true);
        };
        for (const Atom &atom : merged) {
            addGap(at, atom.start);
            atoms.atoms.push_back(atom);
            pinned.push_back(false);
            at = atom.end;
        }
        addGap(at, size);
        atoms.live = std::move(pinned);
    }
    for (std::size_t index = 0; index < object.symbols.size(); ++index) {
        const RcuSymbol &symbol = object.symbols[index];
        if (symbol.sectionIdx >= result.sections.size() || !result.sections[symbol.sectionIdx].collectable)
            continue;
        const SectionAtoms &atoms = result.sections[symbol.sectionIdx];
        const bool text = object.sections[symbol.sectionIdx].type == RcuSecType::Text;
        if (const auto atom = AtomAt(atoms.atoms, symbol.value, !text))
            result.symbolAtoms[index] = std::pair{static_cast<std::size_t>(symbol.sectionIdx), *atom};
    }
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

bool CollectUnreachable(std::vector<RcuFile> &objects, const RcuLinkGraph &graph, const std::uint8_t padding) {
    std::vector<ObjectAtoms> atoms;
    atoms.reserve(objects.size());
    for (const RcuFile &object : objects)
        atoms.push_back(FindAtoms(object));

    // Atoms waiting to have their references followed, as (object, section, atom).
    struct Pending {
        std::size_t object;
        std::size_t section;
        std::size_t atom;
    };

    std::vector<Pending> worklist;
    const auto markAtom = [&](const std::size_t objectIndex, const std::size_t sectionIndex,
                              const std::size_t atomIndex) {
        auto &live = atoms[objectIndex].sections[sectionIndex].live;
        if (live[atomIndex])
            return;
        live[atomIndex] = true;
        worklist.push_back({objectIndex, sectionIndex, atomIndex});
    };
    const auto mark = [&](const RcuSymbolLocation location) {
        const RcuSymbol &symbol = objects[location.objectIndex].symbols[location.symbolIndex];
        // A reference through a section's own symbol may reach anywhere in it.
        if (symbol.kind == RcuSymKind::Section && symbol.sectionIdx < atoms[location.objectIndex].sections.size() &&
            atoms[location.objectIndex].sections[symbol.sectionIdx].collectable) {
            const auto &section = atoms[location.objectIndex].sections[symbol.sectionIdx];
            for (std::size_t atom = 0; atom < section.atoms.size(); ++atom)
                markAtom(location.objectIndex, symbol.sectionIdx, atom);
            return;
        }
        if (const auto atom = atoms[location.objectIndex].symbolAtoms[location.symbolIndex])
            markAtom(location.objectIndex, atom->first, atom->second);
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
        const RcuFile &object = objects[reference.objectIndex];
        const auto offset = object.sections[reference.sectionIndex].relocs[reference.relocationIndex].sectionOffset;
        if (!reference.definition)
            continue;
        // A function a function table names stays only if something else keeps it. The name beside it is data only
        // the table reads, so it is kept with the table: dropping it left every name of a trace a null pointer.
        if (inFunctionTable(reference.objectIndex, reference.sectionIndex, offset)) {
            const RcuFile &owner = objects[reference.definition->objectIndex];
            const RcuSymbol &target = owner.symbols[reference.definition->symbolIndex];
            if (target.sectionIdx >= owner.sections.size() ||
                owner.sections[target.sectionIdx].type == RcuSecType::Text)
                continue;
        }
        auto &section = atoms[reference.objectIndex].sections[reference.sectionIndex];
        if (section.collectable)
            section.references.emplace_back(offset, index);
        else
            mark(*reference.definition);
    }
    for (auto &object : atoms)
        for (auto &section : object.sections)
            std::ranges::sort(section.references);
    // The runs of data no symbol covers start live.
    for (std::size_t objectIndex = 0; objectIndex < atoms.size(); ++objectIndex)
        for (std::size_t sectionIndex = 0; sectionIndex < atoms[objectIndex].sections.size(); ++sectionIndex)
            for (std::size_t atom = 0; atom < atoms[objectIndex].sections[sectionIndex].live.size(); ++atom)
                if (atoms[objectIndex].sections[sectionIndex].live[atom])
                    worklist.push_back({objectIndex, sectionIndex, atom});
    if (graph.EntryRoot())
        mark(*graph.EntryRoot());
    for (const RcuSymbolLocation root : graph.ExportRoots())
        mark(root);
    if (const auto entry = graph.FindDefinition("DllMain"))
        mark(*entry);
    // An image writer looks the process start frame up by name.
    if (const auto frame = graph.FindDefinition(kProcessStartFrameSymbol))
        mark(*frame);

    while (!worklist.empty()) {
        const Pending pending = worklist.back();
        worklist.pop_back();
        const auto &section = atoms[pending.object].sections[pending.section];
        const Atom atom = section.atoms[pending.atom];
        auto reference = std::ranges::lower_bound(section.references, std::pair{atom.start, std::size_t{0}});
        for (; reference != section.references.end() && reference->first < atom.end; ++reference)
            mark(*references[reference->second].definition);
    }

    bool removed = false;
    for (std::size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex) {
        const ObjectAtoms &objectAtoms = atoms[objectIndex];
        const bool keepsAll = std::ranges::all_of(objectAtoms.sections, [](const SectionAtoms &section) {
            return std::ranges::all_of(section.live, [](const bool live) { return live; });
        });
        if (keepsAll)
            continue;
        removed = true;
        RcuFile &object = objects[objectIndex];

        // Live atoms move down, each keeping its offset modulo the section's alignment, and at least sixteen for code.
        std::vector<std::vector<std::optional<std::uint32_t>>> moved(object.sections.size());
        for (std::size_t sectionIndex = 0; sectionIndex < object.sections.size(); ++sectionIndex) {
            const SectionAtoms &sectionAtoms = objectAtoms.sections[sectionIndex];
            if (!sectionAtoms.collectable ||
                std::ranges::all_of(sectionAtoms.live, [](const bool live) { return live; }))
                continue;
            RcuSection &section = object.sections[sectionIndex];
            const bool text = section.type == RcuSecType::Text;
            const std::uint32_t modulus = std::max<std::uint32_t>(16, section.alignment);
            auto &offsets = moved[sectionIndex];
            offsets.resize(sectionAtoms.atoms.size());
            std::vector<std::uint8_t> data;
            data.reserve(section.data.size());
            for (std::size_t atomIndex = 0; atomIndex < sectionAtoms.atoms.size(); ++atomIndex) {
                if (!sectionAtoms.live[atomIndex])
                    continue;
                const Atom atom = sectionAtoms.atoms[atomIndex];
                const auto residue = static_cast<std::uint32_t>((atom.start - data.size()) % modulus);
                data.resize(data.size() + residue, text ? padding : std::uint8_t{0});
                offsets[atomIndex] = static_cast<std::uint32_t>(data.size());
                data.insert(data.end(), section.data.begin() + atom.start, section.data.begin() + atom.end);
            }
            section.data = std::move(data);
        }
        const auto relocate = [&](const std::size_t sectionIndex,
                                  const std::uint32_t offset) -> std::optional<std::uint32_t> {
            const SectionAtoms &sectionAtoms = objectAtoms.sections[sectionIndex];
            if (moved[sectionIndex].empty())
                return offset;
            const auto atomIndex = AtomAt(sectionAtoms.atoms, offset, false);
            if (!atomIndex || !moved[sectionIndex][*atomIndex] || offset > sectionAtoms.atoms[*atomIndex].end)
                return std::nullopt;
            return *moved[sectionIndex][*atomIndex] + (offset - sectionAtoms.atoms[*atomIndex].start);
        };

        // Symbols of dropped atoms go; the rest are renumbered and moved.
        std::vector<std::optional<std::uint32_t>> renumbered(object.symbols.size());
        std::vector<RcuSymbol> symbols;
        symbols.reserve(object.symbols.size());
        for (std::size_t index = 0; index < object.symbols.size(); ++index) {
            RcuSymbol symbol = std::move(object.symbols[index]);
            if (symbol.sectionIdx < object.sections.size() && symbol.kind != RcuSymKind::Section) {
                const auto value = relocate(symbol.sectionIdx, symbol.value);
                if (!value)
                    continue;
                symbol.value = *value;
            }
            renumbered[index] = static_cast<std::uint32_t>(symbols.size());
            symbols.push_back(std::move(symbol));
        }
        object.symbols = std::move(symbols);

        for (std::size_t sectionIndex = 0; sectionIndex < object.sections.size(); ++sectionIndex) {
            RcuSection &section = object.sections[sectionIndex];
            std::vector<RcuReloc> relocs;
            relocs.reserve(section.relocs.size());
            for (RcuReloc reloc : section.relocs) {
                const auto offset = relocate(sectionIndex, reloc.sectionOffset);
                if (!offset)
                    continue;
                reloc.sectionOffset = *offset;
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
