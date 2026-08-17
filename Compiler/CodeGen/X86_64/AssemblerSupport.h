#pragma once

// Private label/fixup state and operand-form lookup shared by the x86-64
// assembler implementation. The public assembler surface remains in
// Assembler.h; these details exist only to keep its implementation cohesive.

#include "CodeGen/X86_64/Assembler.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace Rux::X86_64AssemblerPrivate {
using Bytes = std::vector<std::uint8_t>;

/// Append a little-endian 32-bit value, the byte order every x86-64 immediate and displacement uses.
void Append32(Bytes &out, std::int32_t value);
void Append64(Bytes &out, std::uint64_t value);
/// Whether the value fits the compact one-byte immediate form, which is what decides between the short and long
/// encodings of many instructions.
[[nodiscard]] bool FitsInt8(std::int64_t value);
[[nodiscard]] bool FitsInt32(std::int64_t value);

/// Resolves branches to labels defined inside the same `asm func` body.
///
/// A label the body defines is patched here once its offset is known; a name the body does not define is left to the
/// linker as a relocation. Collecting the defined labels up front is what makes that distinction possible before the
/// forward branch has been reached.
class LabelFixups {
public:
    LabelFixups(std::string_view inputSourceName, Bytes &inputOut, AsmAssembly &inputResult);

    /// Record every label the body defines, before encoding starts, so a forward branch is recognized as local.
    void Collect(const std::vector<AsmInstr> &instrs);
    /// Fix a label's offset, once encoding reaches it.
    void Define(const std::string &label, std::uint32_t offset);
    /// Note a PC-relative reference to `name`, to be patched locally if the label is ours and emitted as a relocation
    /// otherwise.
    void RecordRel32(const std::string &name, const SourceLocation &loc, std::uint32_t fieldOffset);
    /// Patch every local branch now that all offsets are known. A branch to a label nothing defined is reported here.
    void Resolve();

private:
    struct LocalJump {
        std::uint32_t fieldOffset;
        std::string label;
        SourceLocation location;
    };

    void Error(const SourceLocation &loc, std::string message);

    std::string_view sourceName;
    Bytes &out;
    AsmAssembly &result;
    std::unordered_map<std::string, std::uint32_t> labels;
    std::vector<LocalJump> localJumps;
};

enum class SseFormKind : std::uint8_t {
    RegRm,
    Move,
};

struct SseForm {
    SseFormKind kind;
    std::uint8_t prefix;
    std::uint8_t loadOpcode;
    std::uint8_t storeOpcode;
};

[[nodiscard]] std::optional<std::uint8_t> ConditionCode(std::string_view mnemonic, std::string_view prefix);
[[nodiscard]] const SseForm *LookupSseForm(std::string_view mnemonic);
} // namespace Rux::X86_64AssemblerPrivate
