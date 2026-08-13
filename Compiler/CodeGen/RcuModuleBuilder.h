#pragma once

// Target-neutral construction of the three sections emitted by both native
// back ends. Instruction encoding remains owned by each back end; this class
// owns the RCU bookkeeping around those bytes.

#include "Diagnostics/Diagnostics.h"
#include "Object/Rcu/Rcu.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Rux {
enum class RcuModuleSection : std::uint16_t {
    Text = RCU_TEXT_IDX,
    RoData = RCU_RODATA_IDX,
    Data = RCU_DATA_IDX,
};

struct RcuModuleDescription {
    std::uint8_t arch = RcuArch::X86_64;
    std::string sourcePath;
    std::string packageName;
    std::uint64_t buildTimestamp = 0;
    std::uint32_t ruxVersion = 0;
    std::uint32_t compilerFlags = 0;
    std::array<std::uint8_t, 32> sourceHash = {};
    bool hasMetadata = true;
};

struct RcuSymbolDeclaration {
    std::string name;
    std::string typeName;
    std::uint8_t kind = RcuSymKind::Unknown;
    std::uint8_t visibility = RcuSymVis::Local;
};

struct RcuModuleBuildResult {
    std::optional<RcuFile> file;
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool HasErrors() const noexcept;
};

class RcuModuleBuilder {
public:
    explicit RcuModuleBuilder(RcuModuleDescription description);

    [[nodiscard]] std::vector<std::uint8_t> &SectionData(RcuModuleSection section);
    [[nodiscard]] const std::vector<std::uint8_t> &SectionData(RcuModuleSection section) const;
    [[nodiscard]] const std::vector<RcuReloc> &Relocations(RcuModuleSection section) const;

    // Zero-pad a section and return the offset at which the next value begins.
    [[nodiscard]] std::uint32_t AlignSection(RcuModuleSection section, std::uint16_t alignment);
    [[nodiscard]] std::uint32_t Append(RcuModuleSection section, std::span<const std::uint8_t> bytes);
    // Discard a speculative suffix of a section and its relocations. AArch64
    // uses this when a branch-layout pass must be emitted again with widened
    // conditional branches.
    [[nodiscard]] bool TruncateSection(RcuModuleSection section, std::size_t dataSize, std::size_t relocationCount);

    // Repeated compatible declarations name the same symbol. Definitions are
    // separate so functions can be declared before their forward references
    // and assigned an offset only when emission reaches them.
    [[nodiscard]] std::optional<std::uint32_t> DeclareSymbol(RcuSymbolDeclaration declaration);
    [[nodiscard]] std::optional<std::uint32_t> DeclareExternal(std::string name, std::uint8_t kind,
                                                               std::string typeName = {});
    [[nodiscard]] bool DefineSymbol(std::uint32_t symbolIndex, RcuModuleSection section, std::uint32_t offset,
                                    std::uint32_t size);
    [[nodiscard]] std::optional<std::uint32_t> AddDefinition(RcuSymbolDeclaration declaration, RcuModuleSection section,
                                                             std::uint32_t offset, std::uint32_t size);

    // BeginFunction captures the current .text offset; EndFunction records the
    // final byte count. This keeps helper and user-function sizes consistent.
    [[nodiscard]] bool BeginFunction(std::uint32_t symbolIndex);
    [[nodiscard]] bool EndFunction(std::uint32_t symbolIndex);

    // Literal encoding stays target-owned. These hooks only deduplicate a
    // caller-defined family/key pair and associate it with an RCU symbol.
    [[nodiscard]] std::optional<std::uint32_t> InternedLiteral(std::string_view family, std::string_view key) const;
    [[nodiscard]] bool RecordInternedLiteral(std::string family, std::string key, std::uint32_t symbolIndex);

    [[nodiscard]] bool AddRelocation(RcuModuleSection section, std::uint32_t sectionOffset, std::uint32_t symbolIndex,
                                     std::uint16_t type, std::int32_t addend = 0);

    [[nodiscard]] const std::vector<Diagnostic> &Diagnostics() const noexcept {
        return diagnostics;
    }

    [[nodiscard]] RcuModuleBuildResult Finalize();

private:
    struct SymbolState {
        bool external = false;
        bool defined = false;
        bool functionOpen = false;
        std::uint32_t functionStart = 0;
    };

    RcuModuleDescription description;
    std::array<std::vector<std::uint8_t>, 3> sectionData;
    std::array<std::vector<RcuReloc>, 3> sectionRelocations;
    std::vector<RcuSymbol> symbols;
    std::vector<SymbolState> symbolStates;
    std::vector<Diagnostic> diagnostics;
    std::vector<std::pair<std::string, std::uint32_t>> symbolNames;
    std::vector<std::pair<std::string, std::uint32_t>> literals;
    bool finalized = false;

    [[nodiscard]] static std::size_t SectionIndex(RcuModuleSection section);
    [[nodiscard]] std::optional<std::uint32_t> FindSymbol(std::string_view name) const;
    void Report(std::string message);
};
} // namespace Rux
