#include "CodeGen/AArch64/AssemblerContext.h"
#include "CodeGen/AArch64/Registers.h"

#include <cctype>
#include <format>

namespace Rux::AArch64AssemblerPrivate {
namespace {
[[nodiscard]] A64Reg ToA64Reg(const AsmRegInfo &info) noexcept {
    const auto bits = static_cast<unsigned>(info.size) * 8U;
    if (info.file == AsmRegFile::Vector) {
        return A64::Vn(static_cast<unsigned>(info.code), bits);
    }
    A64Reg reg = A64::Gpr(static_cast<unsigned>(info.code), bits);
    reg.stackPointer = info.stackPointer;
    return reg;
}

[[nodiscard]] std::string Uppered(const std::string_view name) {
    std::string uppered;
    uppered.reserve(name.size());
    for (const char c : name) {
        uppered += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return uppered;
}

} // namespace

A64Reg ZeroLike(const A64Reg reg) noexcept {
    return A64::Gpr(31, reg.bits);
}

A64ShiftKind ToA64Shift(const AsmShiftKind kind) noexcept {
    switch (kind) {
    case AsmShiftKind::Lsr:
        return A64ShiftKind::Lsr;
    case AsmShiftKind::Asr:
        return A64ShiftKind::Asr;
    case AsmShiftKind::Ror:
        return A64ShiftKind::Ror;
    case AsmShiftKind::None:
    case AsmShiftKind::Lsl:
        break;
    }
    return A64ShiftKind::Lsl;
}

std::string_view ShiftName(const AsmShiftKind kind) noexcept {
    switch (kind) {
    case AsmShiftKind::Lsr:
        return "LSR";
    case AsmShiftKind::Asr:
        return "ASR";
    case AsmShiftKind::Ror:
        return "ROR";
    case AsmShiftKind::None:
    case AsmShiftKind::Lsl:
        break;
    }
    return "LSL";
}

std::string_view ExtendName(const AsmExtendKind kind) noexcept {
    switch (kind) {
    case AsmExtendKind::Uxtb:
        return "UXTB";
    case AsmExtendKind::Uxth:
        return "UXTH";
    case AsmExtendKind::Uxtw:
        return "UXTW";
    case AsmExtendKind::Sxtb:
        return "SXTB";
    case AsmExtendKind::Sxth:
        return "SXTH";
    case AsmExtendKind::Sxtw:
        return "SXTW";
    case AsmExtendKind::Sxtx:
        return "SXTX";
    case AsmExtendKind::None:
    case AsmExtendKind::Uxtx:
        break;
    }
    return "UXTX";
}

bool ExtendsWholeRegister(const AsmExtendKind kind) noexcept {
    return kind == AsmExtendKind::None || kind == AsmExtendKind::Uxtx || kind == AsmExtendKind::Sxtx;
}

A64ExtendKind ToA64Extend(const AsmExtendKind kind) noexcept {
    switch (kind) {
    case AsmExtendKind::Uxtb:
        return A64ExtendKind::Uxtb;
    case AsmExtendKind::Uxth:
        return A64ExtendKind::Uxth;
    case AsmExtendKind::Uxtw:
        return A64ExtendKind::Uxtw;
    case AsmExtendKind::Sxtb:
        return A64ExtendKind::Sxtb;
    case AsmExtendKind::Sxth:
        return A64ExtendKind::Sxth;
    case AsmExtendKind::Sxtw:
        return A64ExtendKind::Sxtw;
    case AsmExtendKind::Sxtx:
        return A64ExtendKind::Sxtx;
    case AsmExtendKind::None:
    case AsmExtendKind::Uxtx:
        break;
    }
    return A64ExtendKind::Uxtx;
}

A64IndexMode ToA64IndexMode(const AsmIndexMode mode) noexcept {
    switch (mode) {
    case AsmIndexMode::PreIndex:
        return A64IndexMode::PreIndex;
    case AsmIndexMode::PostIndex:
        return A64IndexMode::PostIndex;
    case AsmIndexMode::Offset:
        break;
    }
    return A64IndexMode::Offset;
}

std::string Lowered(const std::string_view name) {
    std::string lowered;
    lowered.reserve(name.size());
    for (const char c : name) {
        lowered += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return lowered;
}

std::string FoundText(const AsmOperand &op) {
    switch (op.kind) {
    case AsmOperand::Kind::Reg:
        return std::format("the register '{}'", op.name);
    case AsmOperand::Kind::Sym:
        return std::format("the symbol '{}'", op.name);
    case AsmOperand::Kind::Imm:
        return std::format("the immediate {}", op.imm);
    case AsmOperand::Kind::Mem:
        return "a memory operand";
    case AsmOperand::Kind::None:
        break;
    }
    return "nothing";
}

AssemblerContext::AssemblerContext(const std::vector<AsmInstr> &instrs, std::string sourceName, Bytes &out)
    : instrs_(instrs)
    , sourceName_(std::move(sourceName))
    , out_(out)
    , enc_(out_) {
}

AsmAssembly AssemblerContext::Run() {
    CollectLabels();
    for (const auto &instr : instrs_) {
        if (!instr.labelDef.empty()) {
            labels_[instr.labelDef] = Here();
            continue;
        }
        EncodeInstr(instr);
    }
    ResolveLocalTargets();
    result_.ok = result_.diagnostics.empty();
    return std::move(result_);
}

void AssemblerContext::Begin(const AsmInstr &in, const Syntax syntax) {
    in_ = &in;
    syntax_ = syntax;
}

void AssemblerContext::Error(const SourceLocation &loc, std::string msg) {
    Diagnostic diagnostic;
    diagnostic.severity = Diagnostic::Severity::Error;
    diagnostic.message = std::move(msg);
    diagnostic.location = loc;
    diagnostic.sourceName = sourceName_;
    result_.diagnostics.push_back(std::move(diagnostic));
}

std::string AssemblerContext::FormText() const {
    std::string mnemonic = Uppered(in_->mnemonic);
    return syntax_.empty() ? mnemonic : std::format("{} {}", mnemonic, syntax_);
}

void AssemblerContext::FormError(const SourceLocation &loc, const std::string &what) {
    Error(loc, std::format("{}; the form is '{}'", what, FormText()));
}

const std::string &AssemblerContext::Mnemonic() const {
    return in_->mnemonic;
}

std::size_t AssemblerContext::IndexOf(const AsmOperand &op) const {
    return static_cast<std::size_t>(&op - in_->operands.data()) + 1;
}

std::uint32_t AssemblerContext::Here() const {
    return static_cast<std::uint32_t>(out_.size());
}

bool AssemblerContext::Operands(const std::size_t count) {
    if (in_->operands.size() == count) {
        return true;
    }
    FormError(in_->location, std::format("'{}' takes {} operand{}, found {}", in_->mnemonic, count,
                                         count == 1 ? "" : "s", in_->operands.size()));
    return false;
}

bool AssemblerContext::Operands(const std::size_t least, const std::size_t most) {
    if (in_->operands.size() >= least && in_->operands.size() <= most) {
        return true;
    }
    FormError(in_->location,
              std::format("'{}' takes {} to {} operands, found {}", in_->mnemonic, least, most, in_->operands.size()));
    return false;
}

std::optional<A64Reg> AssemblerContext::RegNamed(const std::string &name, const SourceLocation &loc) {
    const AsmRegInfo info = LookupRegister(Target::Arch::AArch64, name);
    if (!info.valid) {
        FormError(loc, std::format("'{}' is not an AArch64 register", name));
        return std::nullopt;
    }
    return ToA64Reg(info);
}

std::optional<A64Reg> AssemblerContext::RegOf(const AsmOperand &op) {
    if (op.kind != AsmOperand::Kind::Reg) {
        FormError(op.location, std::format("'{}' takes a register as operand {}, found {}", in_->mnemonic, IndexOf(op),
                                           FoundText(op)));
        return std::nullopt;
    }
    return RegNamed(op.name, op.location);
}

std::optional<std::int64_t> AssemblerContext::ImmOf(const AsmOperand &op) {
    if (op.kind != AsmOperand::Kind::Imm) {
        FormError(op.location, std::format("'{}' takes an immediate as operand {}, found {}", in_->mnemonic,
                                           IndexOf(op), FoundText(op)));
        return std::nullopt;
    }
    return op.imm;
}

std::uint64_t AssemblerContext::ShiftedImm(const AsmOperand &op) const {
    const auto value = static_cast<std::uint64_t>(op.imm);
    if (op.shift == AsmShiftKind::Lsl && op.shiftAmount > 0 && op.shiftAmount < 64) {
        return value << static_cast<unsigned>(op.shiftAmount);
    }
    return value;
}

std::optional<unsigned> AssemblerContext::UnsignedImmOf(const AsmOperand &op, const std::uint64_t limit,
                                                        const std::string_view what) {
    const auto value = ImmOf(op);
    if (!value) {
        return std::nullopt;
    }
    if (*value < 0 || static_cast<std::uint64_t>(*value) > limit) {
        FormError(op.location, std::format("'{}' takes {} of 0 to {}, found {}", in_->mnemonic, what, limit, *value));
        return std::nullopt;
    }
    return static_cast<unsigned>(*value);
}

std::optional<A64Condition> AssemblerContext::CondOf(const AsmOperand &op) {
    if (op.kind != AsmOperand::Kind::Sym) {
        FormError(op.location, std::format("'{}' takes a condition as operand {}, found {}", in_->mnemonic, IndexOf(op),
                                           FoundText(op)));
        return std::nullopt;
    }
    if (const auto cond = ConditionFromName(Lowered(op.name))) {
        return cond;
    }
    FormError(op.location, std::format("unknown condition '{}'", op.name));
    return std::nullopt;
}

AssemblerContext::RegRef AssemblerContext::Ref(const AsmOperand &op, const A64Reg reg) {
    return {&op, reg, op.name};
}

bool AssemblerContext::Uniform(const RegClass regClass, const std::initializer_list<RegRef> regs) {
    if (regClass == RegClass::Mixed) {
        return true;
    }
    const RegRef *first = nullptr;
    for (const auto &ref : regs) {
        if (regClass == RegClass::Float && !ref.reg.IsVector()) {
            FormError(ref.op->location, std::format("'{}' takes a floating-point register as operand {}, found the "
                                                    "general-purpose '{}'",
                                                    in_->mnemonic, IndexOf(*ref.op), ref.name));
            return false;
        }
        if (regClass == RegClass::General && ref.reg.IsVector()) {
            FormError(ref.op->location,
                      std::format("'{}' takes a general-purpose register as operand {}, found the floating-point '{}'",
                                  in_->mnemonic, IndexOf(*ref.op), ref.name));
            return false;
        }
        if (first == nullptr) {
            first = &ref;
            continue;
        }
        if (ref.reg.bits != first->reg.bits) {
            FormError(ref.op->location,
                      std::format("'{}' takes operands of one width, and operand {} '{}' is {}-bit where '{}' is "
                                  "{}-bit",
                                  in_->mnemonic, IndexOf(*ref.op), ref.name, ref.reg.bits, first->name,
                                  first->reg.bits));
            return false;
        }
    }
    return true;
}

bool AssemblerContext::NoStackPointer(const std::initializer_list<RegRef> regs) {
    for (const auto &ref : regs) {
        if (!ref.reg.IsStackPointer()) {
            continue;
        }
        FormError(ref.op->location,
                  std::format("'{}' reads register 31 as the zero register, so operand {} cannot be '{}' but may be "
                              "'{}'",
                              in_->mnemonic, IndexOf(*ref.op), ref.name, ref.reg.Is64() ? "xzr" : "wzr"));
        return false;
    }
    return true;
}

bool AssemblerContext::CheckArithImm(const AsmOperand &op, const std::uint64_t value) {
    if (TryEncodeArithImm12(value)) {
        return true;
    }
    FormError(op.location, std::format("'{}' takes an immediate of 0 to 4095 or a multiple of 4096 up to "
                                       "16773120, found {}",
                                       in_->mnemonic, value));
    return false;
}

bool AssemblerContext::CheckLogicalImm(const AsmOperand &op, const std::uint64_t value, const bool is64) {
    if (TryEncodeBitmaskImm(value, is64)) {
        return true;
    }
    FormError(op.location, std::format("'{}' takes a bitmask immediate (a run of one bits, rotated and repeated to "
                                       "fill the register), and {} is not one",
                                       in_->mnemonic, value));
    return false;
}

std::optional<unsigned> AssemblerContext::BitOf(const AsmOperand &op, const A64Reg reg, const std::string_view what) {
    return UnsignedImmOf(op, reg.bits - 1U, what);
}

void AssemblerContext::Emit(const AsmInstr &in, const A64Status status) {
    if (status != A64Status::Ok) {
        Error(in.location, std::format("cannot encode '{}': {}", in.mnemonic, A64StatusName(status)));
    }
}

void AssemblerContext::EncodeInstr(const AsmInstr &in) {
    const std::uint32_t before = Here();
    const std::size_t reported = result_.diagnostics.size();
    Dispatch(in);
    if (result_.diagnostics.size() != reported && Here() == before) {
        enc_.Word(0);
    }
}

void AssemblerContext::AddFixup(const std::uint32_t at, const std::string &symbol, const std::uint16_t relType) {
    result_.fixups.push_back({at, symbol, relType, 0});
}

} // namespace Rux::AArch64AssemblerPrivate
