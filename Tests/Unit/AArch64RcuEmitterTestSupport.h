#pragma once

#include "Diagnostics/Diagnostics.h"
#include "Ir/Lir/Lir.h"
#include "Object/Rcu/Rcu.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Rux::Testing {
[[nodiscard]] LirPackage CompileToAArch64Lir(const std::string &source,
                                             std::string_view targetTriple = "linux-aarch64");

[[nodiscard]] std::string HexWord(std::uint32_t word);
[[nodiscard]] std::vector<std::uint32_t> FunctionWords(const RcuFile &object, std::string_view name);
[[nodiscard]] std::optional<std::int64_t> StackPointerAdjustment(std::uint32_t word, bool subtract);

inline constexpr std::uint32_t kStackProbeTouch = 0xF90003FFU; // str xzr, [sp]

[[nodiscard]] const RcuSymbol *FindSymbol(const RcuFile &object, std::string_view name);
[[nodiscard]] std::vector<std::uint8_t> RodataOf(const RcuFile &object, std::string_view name);
[[nodiscard]] std::vector<RcuReloc> RelocsFor(const RcuFile &object, std::uint16_t sectionIdx, std::string_view symbol);
[[nodiscard]] std::uint32_t TextWordAt(const RcuFile &object, std::uint32_t offset);
[[nodiscard]] std::optional<std::uint32_t> FramePointerAddImm(std::uint32_t word, unsigned reg = 9);
[[nodiscard]] bool HasFloatForm(const std::vector<std::uint32_t> &words, std::uint32_t opcode, unsigned sources = 2);
[[nodiscard]] bool HasCset(const std::vector<std::uint32_t> &words, std::uint32_t cset);
[[nodiscard]] bool HasRegisterForm(const std::vector<std::uint32_t> &words, std::uint32_t opcode);
[[nodiscard]] std::optional<std::uint32_t> FieldAddImm(std::uint32_t word);
[[nodiscard]] std::optional<std::uint32_t> StackPointerAddImm(std::uint32_t word, unsigned reg);
[[nodiscard]] bool IsPairAccess(std::uint32_t word);
[[nodiscard]] std::int32_t BranchDisplacement(std::uint32_t word);
[[nodiscard]] std::optional<std::uint32_t> SlotDisplacement(std::uint32_t word, bool store);
[[nodiscard]] std::optional<std::size_t> BranchAndLinkIndex(const std::vector<std::uint32_t> &words);
[[nodiscard]] std::vector<std::size_t> BranchAndLinkIndices(const std::vector<std::uint32_t> &words);
[[nodiscard]] std::optional<unsigned> ArgumentFilled(std::uint32_t word);
[[nodiscard]] std::optional<unsigned> ArgumentDrained(std::uint32_t word);
[[nodiscard]] std::uint32_t SlotAccessDisplacement(std::uint32_t word);

struct VectorSlotAccess {
    unsigned reg = 0;
    std::uint32_t displacement = 0;
};

[[nodiscard]] std::optional<VectorSlotAccess> VectorSlotAccessOf(std::uint32_t word, unsigned bits, bool store);
[[nodiscard]] std::optional<unsigned> VectorArgumentFilled(std::uint32_t word, unsigned bits);
[[nodiscard]] std::optional<unsigned> FloatBitsArgumentFilled(std::uint32_t word, unsigned bits);
[[nodiscard]] std::optional<std::uint32_t> StackArgumentStored(std::uint32_t word, unsigned width = 8);
[[nodiscard]] std::optional<std::uint32_t> StackArgumentStoredBy(std::uint32_t word, unsigned width, unsigned reg);
[[nodiscard]] std::optional<std::uint32_t> VectorStackArgumentStored(std::uint32_t word, unsigned bits);
[[nodiscard]] std::optional<std::uint32_t> StackPairArgumentStored(std::uint32_t word);
[[nodiscard]] std::optional<unsigned> VectorArgumentDrained(std::uint32_t word, unsigned bits);
[[nodiscard]] std::size_t DrainStart(const std::vector<std::uint32_t> &words);
[[nodiscard]] std::size_t VectorDrainStart(const std::vector<std::uint32_t> &words, unsigned bits);
[[nodiscard]] std::optional<std::int32_t> PreIndexedFrameSize(std::uint32_t word);
[[nodiscard]] std::optional<std::int32_t> IncomingDisplacement(std::uint32_t word, unsigned width, bool sign = false);

struct PreservedRegisters {
    std::vector<unsigned> general;
    std::vector<unsigned> vector;
};

[[nodiscard]] PreservedRegisters SavedRegisters(const std::vector<std::uint32_t> &words);
[[nodiscard]] bool RestoresRegister(const std::vector<std::uint32_t> &words, unsigned reg, bool isVector);
[[nodiscard]] LirPackage PackageOf(LirFunc func);
[[nodiscard]] LirInstr ConstInstr(LirReg dst, std::string_view value, TypeRef type);
[[nodiscard]] LirInstr PhiInstr(LirReg dst, const std::vector<std::pair<LirReg, std::uint32_t>> &preds);
[[nodiscard]] LirTerminator ReturnTerm(LirReg value);
void CheckFunctionImage(const RcuFile &object, std::string_view name, const std::vector<std::uint32_t> &expected);
[[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint16_t>>
FunctionRelocs(const RcuFile &object, std::string_view function, std::string_view symbol);
[[nodiscard]] std::string JoinMessages(const std::vector<Diagnostic> &diagnostics);
} // namespace Rux::Testing
