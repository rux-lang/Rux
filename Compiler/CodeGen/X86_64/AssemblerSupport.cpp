// Private label/fixup handling and SSE operand-form dispatch for the x86-64
// `asm func` assembler.

#include "CodeGen/X86_64/AssemblerSupport.h"

#include "Object/Rcu/Rcu.h"

#include <format>
#include <utility>

namespace Rux::X86_64AssemblerPrivate {
void Append32(Bytes &out, const std::int32_t value) {
    const auto bits = static_cast<std::uint32_t>(value);
    out.push_back(bits & 0xFF);
    out.push_back((bits >> 8) & 0xFF);
    out.push_back((bits >> 16) & 0xFF);
    out.push_back((bits >> 24) & 0xFF);
}

void Append64(Bytes &out, std::uint64_t value) {
    for (int byte = 0; byte < 8; ++byte) {
        out.push_back(value & 0xFF);
        value >>= 8;
    }
}

bool FitsInt8(const std::int64_t value) {
    return value >= -128 && value <= 127;
}

bool FitsInt32(const std::int64_t value) {
    return value >= INT32_MIN && value <= INT32_MAX;
}

LabelFixups::LabelFixups(const std::string_view sourceName, Bytes &out, AsmAssembly &result)
    : sourceName_(sourceName)
    , out_(out)
    , result_(result) {
}

void LabelFixups::Collect(const std::vector<AsmInstr> &instrs) {
    // Offsets are filled during encoding; reserve names first so a forward
    // branch can be distinguished from an external symbol reference.
    for (const auto &instr : instrs) {
        if (!instr.labelDef.empty()) {
            labels_.emplace(instr.labelDef, 0);
        }
    }
}

void LabelFixups::Define(const std::string &label, const std::uint32_t offset) {
    labels_[label] = offset;
}

void LabelFixups::RecordRel32(const std::string &name, const SourceLocation &loc, const std::uint32_t fieldOffset) {
    if (labels_.contains(name)) {
        localJumps_.push_back({fieldOffset, name, loc});
        return;
    }

    // The linker resolves rel32 as targetVA + addend - (site + 4).
    result_.fixups.push_back({fieldOffset, name, RcuRelType::Rel32, 0});
}

void LabelFixups::Resolve() {
    for (const auto &jump : localJumps_) {
        const auto it = labels_.find(jump.label);
        if (it == labels_.end()) {
            Error(jump.location, std::format("undefined label '{}'", jump.label));
            continue;
        }
        const auto target = static_cast<std::int32_t>(it->second);
        const std::int32_t rel = target - static_cast<std::int32_t>(jump.fieldOffset + 4);
        out_[jump.fieldOffset] = rel & 0xFF;
        out_[jump.fieldOffset + 1] = (rel >> 8) & 0xFF;
        out_[jump.fieldOffset + 2] = (rel >> 16) & 0xFF;
        out_[jump.fieldOffset + 3] = (rel >> 24) & 0xFF;
    }
}

void LabelFixups::Error(const SourceLocation &loc, std::string message) {
    Diagnostic diagnostic;
    diagnostic.severity = Diagnostic::Severity::Error;
    diagnostic.message = std::move(message);
    diagnostic.location = loc;
    diagnostic.sourceName = sourceName_;
    result_.diagnostics.push_back(std::move(diagnostic));
}

std::optional<std::uint8_t> ConditionCode(const std::string_view mnemonic, const std::string_view prefix) {
    if (mnemonic.size() <= prefix.size() || mnemonic.substr(0, prefix.size()) != prefix) {
        return std::nullopt;
    }
    const std::string_view condition = mnemonic.substr(prefix.size());
    static const std::unordered_map<std::string_view, std::uint8_t> conditions = {
        {"o", 0x0},  {"no", 0x1}, {"b", 0x2},  {"c", 0x2},  {"nae", 0x2}, {"ae", 0x3},  {"nb", 0x3}, {"nc", 0x3},
        {"e", 0x4},  {"z", 0x4},  {"ne", 0x5}, {"nz", 0x5}, {"be", 0x6},  {"na", 0x6},  {"a", 0x7},  {"nbe", 0x7},
        {"s", 0x8},  {"ns", 0x9}, {"p", 0xA},  {"pe", 0xA}, {"np", 0xB},  {"po", 0xB},  {"l", 0xC},  {"nge", 0xC},
        {"ge", 0xD}, {"nl", 0xD}, {"le", 0xE}, {"ng", 0xE}, {"g", 0xF},   {"nle", 0xF},
    };
    if (const auto it = conditions.find(condition); it != conditions.end()) {
        return it->second;
    }
    return std::nullopt;
}

const SseForm *LookupSseForm(const std::string_view mnemonic) {
    // RegRm forms use loadOpcode as their sole opcode. Move forms use both the
    // load and store opcodes, keeping the whole SSE r/m dispatch in one table.
    static const std::unordered_map<std::string_view, SseForm> forms = {
        {"sqrtsd", {SseFormKind::RegRm, 0xF2, 0x51, 0}},   {"sqrtss", {SseFormKind::RegRm, 0xF3, 0x51, 0}},
        {"addsd", {SseFormKind::RegRm, 0xF2, 0x58, 0}},    {"addss", {SseFormKind::RegRm, 0xF3, 0x58, 0}},
        {"subsd", {SseFormKind::RegRm, 0xF2, 0x5C, 0}},    {"subss", {SseFormKind::RegRm, 0xF3, 0x5C, 0}},
        {"mulsd", {SseFormKind::RegRm, 0xF2, 0x59, 0}},    {"mulss", {SseFormKind::RegRm, 0xF3, 0x59, 0}},
        {"divsd", {SseFormKind::RegRm, 0xF2, 0x5E, 0}},    {"divss", {SseFormKind::RegRm, 0xF3, 0x5E, 0}},
        {"minsd", {SseFormKind::RegRm, 0xF2, 0x5D, 0}},    {"minss", {SseFormKind::RegRm, 0xF3, 0x5D, 0}},
        {"maxsd", {SseFormKind::RegRm, 0xF2, 0x5F, 0}},    {"maxss", {SseFormKind::RegRm, 0xF3, 0x5F, 0}},
        {"cvtsd2ss", {SseFormKind::RegRm, 0xF2, 0x5A, 0}}, {"cvtss2sd", {SseFormKind::RegRm, 0xF3, 0x5A, 0}},
        {"ucomisd", {SseFormKind::RegRm, 0x66, 0x2E, 0}},  {"ucomiss", {SseFormKind::RegRm, 0x00, 0x2E, 0}},
        {"comisd", {SseFormKind::RegRm, 0x66, 0x2F, 0}},   {"comiss", {SseFormKind::RegRm, 0x00, 0x2F, 0}},
        {"xorps", {SseFormKind::RegRm, 0x00, 0x57, 0}},    {"xorpd", {SseFormKind::RegRm, 0x66, 0x57, 0}},
        {"andps", {SseFormKind::RegRm, 0x00, 0x54, 0}},    {"andpd", {SseFormKind::RegRm, 0x66, 0x54, 0}},
        {"orps", {SseFormKind::RegRm, 0x00, 0x56, 0}},     {"orpd", {SseFormKind::RegRm, 0x66, 0x56, 0}},
        {"andnps", {SseFormKind::RegRm, 0x00, 0x55, 0}},   {"andnpd", {SseFormKind::RegRm, 0x66, 0x55, 0}},
        {"pxor", {SseFormKind::RegRm, 0x66, 0xEF, 0}},     {"pand", {SseFormKind::RegRm, 0x66, 0xDB, 0}},
        {"por", {SseFormKind::RegRm, 0x66, 0xEB, 0}},      {"movsd", {SseFormKind::Move, 0xF2, 0x10, 0x11}},
        {"movss", {SseFormKind::Move, 0xF3, 0x10, 0x11}},  {"movups", {SseFormKind::Move, 0x00, 0x10, 0x11}},
        {"movupd", {SseFormKind::Move, 0x66, 0x10, 0x11}}, {"movaps", {SseFormKind::Move, 0x00, 0x28, 0x29}},
        {"movapd", {SseFormKind::Move, 0x66, 0x28, 0x29}},
    };
    if (const auto it = forms.find(mnemonic); it != forms.end()) {
        return &it->second;
    }
    return nullptr;
}
} // namespace Rux::X86_64AssemblerPrivate
