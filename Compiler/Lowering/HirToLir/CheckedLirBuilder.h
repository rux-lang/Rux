#pragma once

#include "Ir/Lir/Lir.h"
#include "Lexer/Token.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>

namespace Rux {
class CheckedLirBuilder {
public:
    using BlockIndex = std::uint32_t;

    explicit CheckedLirBuilder(LirFunc &inputFunction);

    [[nodiscard]] LirReg AllocateRegister();
    [[nodiscard]] LirReg DefineParameter();
    [[nodiscard]] BlockIndex CreateBlock(std::string label = {});
    [[nodiscard]] bool SelectBlock(BlockIndex index);
    [[nodiscard]] bool IsTerminated() const noexcept;
    [[nodiscard]] BlockIndex CurrentBlock() const noexcept;
    [[nodiscard]] LirFunc &Function() const noexcept;
    [[nodiscard]] std::string_view FailureReason() const noexcept;

    [[nodiscard]] bool Insert(LirInstr instruction);
    [[nodiscard]] bool Terminate(LirTerminator terminator);

    [[nodiscard]] static std::optional<LirOpcode> BinaryOpcode(TokenKind op) noexcept;
    [[nodiscard]] static std::optional<LirOpcode> CompoundOpcode(TokenKind op) noexcept;

private:
    [[nodiscard]] bool IsDefined(LirReg reg) const noexcept;
    [[nodiscard]] bool HasValidSources(const LirInstr &instruction) const noexcept;
    [[nodiscard]] bool HasValidSources(const LirTerminator &terminator) const noexcept;
    [[nodiscard]] bool HasValidTargets(const LirTerminator &terminator) const noexcept;

    LirFunc *function;
    std::optional<BlockIndex> currentBlock;
    LirReg nextRegister = 0;
    std::unordered_set<LirReg> definedRegisters;
    std::string failureReason;
};
} // namespace Rux
