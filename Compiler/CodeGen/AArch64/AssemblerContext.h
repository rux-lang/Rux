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
    AssemblerContext(const std::vector<AsmInstr> &instrs, std::string sourceName, Bytes &out);
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
    void Error(const SourceLocation &loc, std::string msg);
    void FormError(const SourceLocation &loc, const std::string &what);
    [[nodiscard]] const std::string &Mnemonic() const;
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
} // namespace Rux::AArch64AssemblerPrivate
