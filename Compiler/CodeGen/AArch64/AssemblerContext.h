#pragma once

// Private implementation context shared by the AArch64 assembler translation
// units. The public assembler surface remains in Assembler.h; this header owns
// only the state and operand/form decoding needed by instruction families.

#include "CodeGen/AArch64/Assembler.h"
#include "CodeGen/AArch64/Encoder.h"

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace Rux::AArch64AssemblerPrivate {
using Bytes = std::vector<std::uint8_t>;

using RegRegImmFn = A64Status (A64Enc::*)(A64Reg, A64Reg, std::uint64_t) const;
using ShiftedFn = A64Status (A64Enc::*)(A64Reg, A64Reg, A64Reg, A64ShiftKind, unsigned) const;
using ExtendedFn = A64Status (A64Enc::*)(A64Reg, A64Reg, A64Reg, A64ExtendKind, unsigned) const;
using UnscaledMemFn = A64Status (A64Enc::*)(A64Reg, A64Reg, std::int64_t, A64IndexMode) const;
using PairFn = A64Status (A64Enc::*)(A64Reg, A64Reg, A64Reg, std::int64_t, A64IndexMode) const;
using Reg1Fn = A64Status (A64Enc::*)(A64Reg) const;
using Reg2Fn = A64Status (A64Enc::*)(A64Reg, A64Reg) const;
using Reg3Fn = A64Status (A64Enc::*)(A64Reg, A64Reg, A64Reg) const;
using Reg4Fn = A64Status (A64Enc::*)(A64Reg, A64Reg, A64Reg, A64Reg) const;
using Reg2ShiftFn = A64Status (A64Enc::*)(A64Reg, A64Reg, A64ShiftKind, unsigned) const;
using ShiftImmFn = A64Status (A64Enc::*)(A64Reg, A64Reg, unsigned) const;
using BitfieldFn = A64Status (A64Enc::*)(A64Reg, A64Reg, unsigned, unsigned) const;
using MovwFn = A64Status (A64Enc::*)(A64Reg, std::uint16_t, unsigned) const;
using CondSelFn = A64Status (A64Enc::*)(A64Reg, A64Reg, A64Reg, A64Condition) const;
using CondAliasFn = A64Status (A64Enc::*)(A64Reg, A64Reg, A64Condition) const;
using CondSetFn = A64Status (A64Enc::*)(A64Reg, A64Condition) const;
using CompareBranchFn = A64Status (A64Enc::*)(A64Reg, std::int64_t) const;
using TestBranchFn = A64Status (A64Enc::*)(A64Reg, unsigned, std::int64_t) const;
using Imm16Fn = A64Status (A64Enc::*)(std::uint16_t) const;
using BarrierFn = A64Status (A64Enc::*)(A64Barrier) const;

enum class RegClass : std::uint8_t {
    General,
    Float,
    Mixed,
};

using Syntax = std::string_view;

template <typename Fn>
struct Form {
    Fn fn = nullptr;
    Syntax syntax = "";
    RegClass regClass = RegClass::General;
};

struct ArithForms {
    RegRegImmFn imm;
    ShiftedFn shifted;
    ExtendedFn extended;
    bool discardsResult = false;
    bool writesStackPointer = false;
};

struct LogicForms {
    RegRegImmFn imm;
    ShiftedFn shifted;
    bool discardsResult = false;
};

struct MemForms {
    RegRegImmFn scaled;
    UnscaledMemFn unscaled;
    ExtendedFn indexed;
    unsigned accessBytes = 0;
    bool literal = false;
};

struct ShiftForms {
    ShiftImmFn imm;
    Reg3Fn variable;
};

struct BitfieldForms {
    BitfieldFn fn;
    Syntax syntax;
    bool field = false;
};

enum class TargetField : std::uint8_t {
    Imm26,
    Imm19,
    Imm14,
    Adr,
};

[[nodiscard]] A64Reg ZeroLike(A64Reg reg) noexcept;
[[nodiscard]] A64ShiftKind ToA64Shift(AsmShiftKind kind) noexcept;
[[nodiscard]] std::string_view ShiftName(AsmShiftKind kind) noexcept;
[[nodiscard]] std::string_view ExtendName(AsmExtendKind kind) noexcept;
[[nodiscard]] bool ExtendsWholeRegister(AsmExtendKind kind) noexcept;
[[nodiscard]] A64ExtendKind ToA64Extend(AsmExtendKind kind) noexcept;
[[nodiscard]] A64IndexMode ToA64IndexMode(AsmIndexMode mode) noexcept;
[[nodiscard]] std::string Lowered(std::string_view name);
[[nodiscard]] std::string FoundText(const AsmOperand &op);
[[nodiscard]] std::optional<A64Condition> ConditionFromName(std::string_view name);
[[nodiscard]] std::optional<A64Barrier> BarrierFromName(std::string_view name);
[[nodiscard]] std::optional<std::uint16_t> SysRegFromName(std::string_view name);

template <typename Map>
[[nodiscard]] const typename Map::mapped_type *Lookup(const Map &table, const std::string &mnemonic) {
    const auto it = table.find(mnemonic);
    return it == table.end() ? nullptr : &it->second;
}

class AssemblerContext {
public:
    AssemblerContext(const std::vector<AsmInstr> &instrs, std::string sourceName, Bytes &out, Target::OS targetOs);
    virtual ~AssemblerContext() = default;

    AsmAssembly Run();

private:
    struct LocalTarget {
        std::uint32_t instrOffset;
        std::string label;
        SourceLocation loc;
        std::string mnemonic;
        TargetField field;
    };

    const std::vector<AsmInstr> &instrs_;
    std::string sourceName_;
    Target::OS targetOs_;
    Bytes &out_;
    AsmAssembly result_;
    std::unordered_map<std::string, std::uint32_t> labels_;
    std::vector<LocalTarget> targets_;
    const AsmInstr *in_ = nullptr;
    Syntax syntax_;

protected:
    struct RegRef {
        const AsmOperand *op = nullptr;
        A64Reg reg;
        std::string_view name;
    };

    void Begin(const AsmInstr &in, Syntax syntax);
    void Error(const SourceLocation &loc, std::string msg, std::vector<std::string> notes = {},
               std::optional<std::string> help = {});
    void FormError(const SourceLocation &loc, const std::string &what);
    [[nodiscard]] const std::string &Mnemonic() const;
    [[nodiscard]] Target::OS TargetOs() const;
    [[nodiscard]] std::size_t IndexOf(const AsmOperand &op) const;
    [[nodiscard]] std::uint32_t Here() const;
    [[nodiscard]] bool Operands(std::size_t count);
    [[nodiscard]] bool Operands(std::size_t least, std::size_t most);
    [[nodiscard]] std::optional<A64Reg> RegNamed(const std::string &name, const SourceLocation &loc);
    [[nodiscard]] std::optional<A64Reg> RegOf(const AsmOperand &op);
    [[nodiscard]] std::optional<std::int64_t> ImmOf(const AsmOperand &op);
    [[nodiscard]] std::uint64_t ShiftedImm(const AsmOperand &op) const;
    [[nodiscard]] std::optional<unsigned> UnsignedImmOf(const AsmOperand &op, std::uint64_t limit,
                                                        std::string_view what);
    [[nodiscard]] std::optional<A64Condition> CondOf(const AsmOperand &op);
    [[nodiscard]] static RegRef Ref(const AsmOperand &op, A64Reg reg);
    [[nodiscard]] bool Uniform(RegClass regClass, std::initializer_list<RegRef> regs);
    [[nodiscard]] bool NoStackPointer(std::initializer_list<RegRef> regs);
    [[nodiscard]] bool CheckArithImm(const AsmOperand &op, std::uint64_t value);
    [[nodiscard]] bool CheckLogicalImm(const AsmOperand &op, std::uint64_t value, bool is64);
    [[nodiscard]] std::optional<unsigned> BitOf(const AsmOperand &op, A64Reg reg, std::string_view what);
    void Emit(const AsmInstr &in, A64Status status);
    void AddFixup(std::uint32_t at, const std::string &symbol, std::uint16_t relType);
    void RecordTarget(const AsmInstr &in, const AsmOperand &target, std::uint32_t at, TargetField field,
                      std::uint16_t relType);

    A64Enc enc_;

private:
    [[nodiscard]] std::string FormText() const;
    void CollectLabels();
    void EncodeInstr(const AsmInstr &in);
    void ResolveLocalTargets();
    virtual void Dispatch(const AsmInstr &in) = 0;
};

// Integer data-processing forms live in their own implementation while sharing
// the label, diagnostic and encoder state owned by AssemblerContext. The final
// facade derives from this private intermediate context and falls through to
// the remaining instruction families when DispatchInteger returns false.
class IntegerAssemblerContext : public AssemblerContext {
public:
    using AssemblerContext::AssemblerContext;

protected:
    [[nodiscard]] bool DispatchInteger(const AsmInstr &in);
    void EncodeReg2(const AsmInstr &in, const Form<Reg2Fn> &form);
    void EncodeReg3(const AsmInstr &in, const Form<Reg3Fn> &form);
    void EncodeReg4(const AsmInstr &in, const Form<Reg4Fn> &form);
    void EncodeCondSel(const AsmInstr &in, const Form<CondSelFn> &form);

private:
    void EncodeArith(const AsmInstr &in, const ArithForms &forms);
    void EncodeLogic(const AsmInstr &in, const LogicForms &forms);
    void EncodeMov(const AsmInstr &in);
    void EncodeMovw(const AsmInstr &in, MovwFn fn);
    void EncodeBitfield(const AsmInstr &in, const BitfieldForms &form);
    void EncodeShift(const AsmInstr &in, const ShiftForms &forms);
    void EncodeExtr(const AsmInstr &in);
    void EncodeReg2Shift(const AsmInstr &in, Reg2ShiftFn fn);
    void EncodeCondAlias(const AsmInstr &in, CondAliasFn fn);
    void EncodeCondSet(const AsmInstr &in, CondSetFn fn);
};

// Load/store, addressing, floating-point and conversion forms live in their
// own implementation. The final facade derives from this context and falls
// through to branch and system instructions when DispatchMemoryFloating does
// not claim a mnemonic.
class MemoryFloatingAssemblerContext : public IntegerAssemblerContext {
public:
    using IntegerAssemblerContext::IntegerAssemblerContext;

protected:
    [[nodiscard]] bool DispatchMemoryFloating(const AsmInstr &in);

private:
    [[nodiscard]] std::optional<A64Reg> BaseOf(const AsmOperand &op);
    [[nodiscard]] bool CheckBase(const AsmOperand &addr, A64Reg base);
    void EncodeMem(const AsmInstr &in, const MemForms &forms);
    void EncodePair(const AsmInstr &in, PairFn fn);
    void EncodeFmov(const AsmInstr &in);
    void EncodeFcmp(const AsmInstr &in, bool signalling);
    void EncodeFccmp(const AsmInstr &in);
};

// Branch and symbol-address forms, local-label resolution, system forms and
// the final mnemonic dispatch live behind this last private context. The
// public assembler facade only constructs this implementation and runs it.
class BranchSystemAssemblerContext final : public MemoryFloatingAssemblerContext {
public:
    using MemoryFloatingAssemblerContext::MemoryFloatingAssemblerContext;

private:
    void EncodeAdr(const AsmInstr &in, bool page);
    void EncodeBranch(const AsmInstr &in, bool link);
    void EncodeCondBranch(const AsmInstr &in, std::string_view suffix);
    void EncodeCompareBranch(const AsmInstr &in, CompareBranchFn fn);
    void EncodeTestBranch(const AsmInstr &in, TestBranchFn fn);
    void EncodeBranchReg(const AsmInstr &in, Reg1Fn fn, bool optional);
    void EncodeException(const AsmInstr &in, Imm16Fn fn);
    void EncodeBarrier(const AsmInstr &in, BarrierFn fn);
    [[nodiscard]] std::optional<std::uint16_t> SysRegOf(const AsmOperand &op);
    void EncodeSysMove(const AsmInstr &in, bool read);
    void Unsupported(const AsmInstr &in);
    void Dispatch(const AsmInstr &in) override;
};
} // namespace Rux::AArch64AssemblerPrivate
