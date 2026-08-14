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

class LabelFixups {
public:
    LabelFixups(std::string_view sourceName, Bytes &out, AsmAssembly &result);

    void Collect(const std::vector<AsmInstr> &instrs);
    void Define(const std::string &label, std::uint32_t offset);
    void RecordRel32(const std::string &name, const SourceLocation &loc, std::uint32_t fieldOffset);
    void Resolve();

private:
    struct LocalJump {
        std::uint32_t fieldOffset;
        std::string label;
        SourceLocation location;
    };

    void Error(const SourceLocation &loc, std::string message);

    std::string_view sourceName_;
    Bytes &out_;
    AsmAssembly &result_;
    std::unordered_map<std::string, std::uint32_t> labels_;
    std::vector<LocalJump> localJumps_;
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
