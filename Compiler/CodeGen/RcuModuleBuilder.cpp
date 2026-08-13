#include "CodeGen/RcuModuleBuilder.h"

#include <algorithm>
#include <format>
#include <limits>
#include <utility>

namespace Rux {
namespace {
[[nodiscard]] bool IsExternalKind(const std::uint8_t kind) {
    return kind == RcuSymKind::ExternFunc || kind == RcuSymKind::ExternData;
}

[[nodiscard]] std::string LiteralKey(const std::string_view family, const std::string_view key) {
    return std::format("{}:{}:{}", family.size(), family, key);
}

[[nodiscard]] RcuSection MakeSection(const RcuModuleSection id, std::vector<std::uint8_t> data,
                                     std::vector<RcuReloc> relocations) {
    RcuSection section;
    section.data = std::move(data);
    section.relocs = std::move(relocations);
    switch (id) {
    case RcuModuleSection::Text:
        section.name = ".text";
        section.type = RcuSecType::Text;
        section.flags = RcuSecFlag::Alloc | RcuSecFlag::Exec | RcuSecFlag::Read;
        section.alignment = 16;
        break;
    case RcuModuleSection::RoData:
        section.name = ".rodata";
        section.type = RcuSecType::RoData;
        section.flags = RcuSecFlag::Alloc | RcuSecFlag::Read;
        section.alignment = 8;
        break;
    case RcuModuleSection::Data:
        section.name = ".data";
        section.type = RcuSecType::Data;
        section.flags = RcuSecFlag::Alloc | RcuSecFlag::Read | RcuSecFlag::Write;
        section.alignment = 8;
        break;
    }
    return section;
}
} // namespace

bool RcuModuleBuildResult::HasErrors() const noexcept {
    return std::ranges::any_of(diagnostics, &Diagnostic::IsError);
}

RcuModuleBuilder::RcuModuleBuilder(RcuModuleDescription inputDescription)
    : description(std::move(inputDescription)) {
}

std::size_t RcuModuleBuilder::SectionIndex(const RcuModuleSection section) {
    return static_cast<std::size_t>(section);
}

std::vector<std::uint8_t> &RcuModuleBuilder::SectionData(const RcuModuleSection section) {
    return sectionData.at(SectionIndex(section));
}

const std::vector<std::uint8_t> &RcuModuleBuilder::SectionData(const RcuModuleSection section) const {
    return sectionData.at(SectionIndex(section));
}

const std::vector<RcuReloc> &RcuModuleBuilder::Relocations(const RcuModuleSection section) const {
    return sectionRelocations.at(SectionIndex(section));
}

std::uint32_t RcuModuleBuilder::AlignSection(const RcuModuleSection section, const std::uint16_t alignment) {
    auto &data = SectionData(section);
    if (alignment == 0) {
        Report("RCU section alignment must be non-zero");
        return static_cast<std::uint32_t>(data.size());
    }
    while (data.size() % alignment != 0) {
        data.push_back(0);
    }
    return static_cast<std::uint32_t>(data.size());
}

std::uint32_t RcuModuleBuilder::Append(const RcuModuleSection section, const std::span<const std::uint8_t> bytes) {
    auto &data = SectionData(section);
    const auto offset = static_cast<std::uint32_t>(data.size());
    data.insert(data.end(), bytes.begin(), bytes.end());
    return offset;
}

bool RcuModuleBuilder::TruncateSection(const RcuModuleSection section, const std::size_t dataSize,
                                       const std::size_t relocationCount) {
    if (finalized) {
        Report("cannot truncate an RCU section after module finalization");
        return false;
    }
    auto &data = SectionData(section);
    auto &relocations = sectionRelocations[SectionIndex(section)];
    if (dataSize > data.size() || relocationCount > relocations.size()) {
        Report("cannot grow an RCU section while truncating it");
        return false;
    }
    data.resize(dataSize);
    relocations.resize(relocationCount);
    return true;
}

std::optional<std::uint32_t> RcuModuleBuilder::FindSymbol(const std::string_view name) const {
    const auto found = std::ranges::find(symbolNames, name, &decltype(symbolNames)::value_type::first);
    if (found == symbolNames.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::optional<std::uint32_t> RcuModuleBuilder::DeclareSymbol(RcuSymbolDeclaration declaration) {
    if (finalized) {
        Report("cannot declare an RCU symbol after module finalization");
        return std::nullopt;
    }
    if (declaration.name.empty()) {
        Report("cannot declare an RCU symbol with an empty name");
        return std::nullopt;
    }
    if (IsExternalKind(declaration.kind)) {
        Report(std::format("RCU symbol '{}' uses an external kind for an internal declaration", declaration.name));
        return std::nullopt;
    }
    if (const auto existing = FindSymbol(declaration.name)) {
        const auto &symbol = symbols[*existing];
        if (!symbolStates[*existing].external && symbol.kind == declaration.kind &&
            symbol.visibility == declaration.visibility && symbol.typeName == declaration.typeName) {
            return existing;
        }
        Report(std::format("conflicting RCU symbol declaration for '{}'", declaration.name));
        return std::nullopt;
    }

    const auto index = static_cast<std::uint32_t>(symbols.size());
    symbols.push_back({.name = std::move(declaration.name),
                       .typeName = std::move(declaration.typeName),
                       .kind = declaration.kind,
                       .visibility = declaration.visibility});
    symbolStates.push_back({});
    symbolNames.emplace_back(symbols.back().name, index);
    return index;
}

std::optional<std::uint32_t> RcuModuleBuilder::DeclareExternal(std::string name, const std::uint8_t kind,
                                                               std::string typeName) {
    if (finalized) {
        Report("cannot declare an RCU symbol after module finalization");
        return std::nullopt;
    }
    if (name.empty()) {
        Report("cannot declare an RCU symbol with an empty name");
        return std::nullopt;
    }
    if (!IsExternalKind(kind)) {
        Report(std::format("RCU external symbol '{}' does not use an external symbol kind", name));
        return std::nullopt;
    }
    if (const auto existing = FindSymbol(name)) {
        const auto &symbol = symbols[*existing];
        if (symbolStates[*existing].external && symbol.kind == kind && symbol.typeName == typeName) {
            return existing;
        }
        Report(std::format("conflicting RCU symbol declaration for '{}'", name));
        return std::nullopt;
    }

    const auto index = static_cast<std::uint32_t>(symbols.size());
    symbols.push_back({.name = std::move(name),
                       .typeName = std::move(typeName),
                       .sectionIdx = RCU_SEC_EXTERNAL,
                       .kind = kind,
                       .visibility = RcuSymVis::Global});
    symbolStates.push_back({.external = true});
    symbolNames.emplace_back(symbols.back().name, index);
    return index;
}

bool RcuModuleBuilder::DefineSymbol(const std::uint32_t symbolIndex, const RcuModuleSection section,
                                    const std::uint32_t offset, const std::uint32_t size) {
    if (finalized) {
        Report("cannot define an RCU symbol after module finalization");
        return false;
    }
    if (symbolIndex >= symbols.size()) {
        Report(std::format("cannot define invalid RCU symbol index {}", symbolIndex));
        return false;
    }
    auto &state = symbolStates[symbolIndex];
    auto &symbol = symbols[symbolIndex];
    if (state.external) {
        Report(std::format("cannot define external RCU symbol '{}'", symbol.name));
        return false;
    }
    if (state.defined) {
        Report(std::format("duplicate RCU symbol definition for '{}'", symbol.name));
        return false;
    }
    const auto sectionSize = SectionData(section).size();
    if (offset > sectionSize || size > sectionSize - offset) {
        Report(std::format("RCU symbol '{}' lies outside its section", symbol.name));
        return false;
    }
    symbol.sectionIdx = static_cast<std::uint16_t>(section);
    symbol.value = offset;
    symbol.size = size;
    state.defined = true;
    return true;
}

std::optional<std::uint32_t> RcuModuleBuilder::AddDefinition(RcuSymbolDeclaration declaration,
                                                             const RcuModuleSection section, const std::uint32_t offset,
                                                             const std::uint32_t size) {
    const auto symbol = DeclareSymbol(std::move(declaration));
    if (!symbol || !DefineSymbol(*symbol, section, offset, size)) {
        return std::nullopt;
    }
    return symbol;
}

bool RcuModuleBuilder::BeginFunction(const std::uint32_t symbolIndex) {
    if (symbolIndex >= symbols.size()) {
        Report(std::format("cannot begin invalid RCU symbol index {}", symbolIndex));
        return false;
    }
    if (symbols[symbolIndex].kind != RcuSymKind::Func) {
        Report(std::format("RCU symbol '{}' is not a function", symbols[symbolIndex].name));
        return false;
    }
    const auto offset = static_cast<std::uint32_t>(SectionData(RcuModuleSection::Text).size());
    if (!DefineSymbol(symbolIndex, RcuModuleSection::Text, offset, 0)) {
        return false;
    }
    auto &state = symbolStates[symbolIndex];
    state.functionOpen = true;
    state.functionStart = offset;
    return true;
}

bool RcuModuleBuilder::EndFunction(const std::uint32_t symbolIndex) {
    if (symbolIndex >= symbols.size()) {
        Report(std::format("cannot finish invalid RCU symbol index {}", symbolIndex));
        return false;
    }
    auto &state = symbolStates[symbolIndex];
    if (!state.functionOpen) {
        Report(std::format("RCU function '{}' was not begun", symbols[symbolIndex].name));
        return false;
    }
    const auto textSize = SectionData(RcuModuleSection::Text).size();
    if (textSize < state.functionStart) {
        Report(std::format("RCU function '{}' starts outside the text section", symbols[symbolIndex].name));
        return false;
    }
    symbols[symbolIndex].size = static_cast<std::uint32_t>(textSize) - state.functionStart;
    state.functionOpen = false;
    return true;
}

std::optional<std::uint32_t> RcuModuleBuilder::InternedLiteral(const std::string_view family,
                                                               const std::string_view key) const {
    const auto composite = LiteralKey(family, key);
    const auto found = std::ranges::find(literals, composite, &decltype(literals)::value_type::first);
    return found == literals.end() ? std::nullopt : std::optional(found->second);
}

bool RcuModuleBuilder::RecordInternedLiteral(std::string family, std::string key, const std::uint32_t symbolIndex) {
    if (symbolIndex >= symbols.size()) {
        Report(std::format("cannot intern a literal with invalid RCU symbol index {}", symbolIndex));
        return false;
    }
    const auto composite = LiteralKey(family, key);
    const auto found = std::ranges::find(literals, composite, &decltype(literals)::value_type::first);
    if (found != literals.end()) {
        if (found->second == symbolIndex) {
            return true;
        }
        Report(std::format("RCU literal '{}:{}' was interned with two symbols", family, key));
        return false;
    }
    literals.emplace_back(composite, symbolIndex);
    return true;
}

bool RcuModuleBuilder::AddRelocation(const RcuModuleSection section, const std::uint32_t sectionOffset,
                                     const std::uint32_t symbolIndex, const std::uint16_t type,
                                     const std::int32_t addend) {
    if (finalized) {
        Report("cannot add an RCU relocation after module finalization");
        return false;
    }
    if (symbolIndex >= symbols.size()) {
        Report(std::format("RCU relocation references invalid symbol index {}", symbolIndex));
        return false;
    }
    if (sectionOffset >= SectionData(section).size()) {
        Report(std::format("RCU relocation offset {} lies outside its section", sectionOffset));
        return false;
    }
    sectionRelocations[SectionIndex(section)].push_back({sectionOffset, symbolIndex, type, addend});
    return true;
}

void RcuModuleBuilder::Report(std::string message) {
    diagnostics.push_back(ErrorDiagnostic(std::move(message)));
}

RcuModuleBuildResult RcuModuleBuilder::Finalize() {
    if (finalized) {
        Report("RCU module builder was finalized more than once");
    }
    finalized = true;
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        if (!symbolStates[i].external && !symbolStates[i].defined) {
            Report(std::format("RCU symbol '{}' was declared but not defined", symbols[i].name));
        }
        if (symbolStates[i].functionOpen) {
            Report(std::format("RCU function '{}' was not finished", symbols[i].name));
        }
    }

    RcuModuleBuildResult result;
    result.diagnostics = diagnostics;
    if (result.HasErrors()) {
        return result;
    }

    RcuFile file;
    file.arch = description.arch;
    file.flags = description.hasMetadata ? RcuFileFlag::HasMetadata : 0;
    file.hasMetadata = description.hasMetadata;
    file.sourcePath = std::move(description.sourcePath);
    file.packageName = std::move(description.packageName);
    file.buildTimestamp = description.buildTimestamp;
    file.ruxVersion = description.ruxVersion;
    file.compilerFlags = description.compilerFlags;
    file.sourceHash = description.sourceHash;
    file.sections.reserve(3);
    file.sections.push_back(
        MakeSection(RcuModuleSection::Text, std::move(sectionData[0]), std::move(sectionRelocations[0])));
    file.sections.push_back(
        MakeSection(RcuModuleSection::RoData, std::move(sectionData[1]), std::move(sectionRelocations[1])));
    file.sections.push_back(
        MakeSection(RcuModuleSection::Data, std::move(sectionData[2]), std::move(sectionRelocations[2])));
    file.symbols = std::move(symbols);
    result.file = std::move(file);
    return result;
}
} // namespace Rux
