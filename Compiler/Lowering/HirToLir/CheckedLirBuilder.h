#pragma once

#include "Ir/Lir/Lir.h"
#include "Lexer/Token.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>

namespace Rux {
/**
 * @brief Builds one LIR function, refusing to construct invalid IR.
 *
 * Every mutation is checked against the invariants the code generators rely on: a register is defined exactly once and
 * only read after that definition, a block gains no instruction once terminated, and a terminator only names blocks
 * that exist. Catching a violation here rather than in a backend turns a miscompilation into a diagnostic that still
 * names the function being lowered.
 *
 * A rejected operation returns false and leaves the function untouched, with `FailureReason` describing what was wrong.
 * Lowering turns that into an internal-error diagnostic; nothing here throws.
 */
class CheckedLirBuilder {
public:
    using BlockIndex = std::uint32_t;

    explicit CheckedLirBuilder(LirFunc &inputFunction);

    /// Reserve a register without defining it. It stays unusable as a source until an inserted instruction names it as
    /// a destination.
    [[nodiscard]] LirReg AllocateRegister();

    /// Reserve a register that is already defined, for a value the calling convention put in place before the body
    /// runs.
    [[nodiscard]] LirReg DefineParameter();

    /// Append a block and return its index. An empty label is filled in as `bb<n>`, so every block is nameable in a
    /// dump.
    [[nodiscard]] BlockIndex CreateBlock(std::string label = {});

    /// Make `index` the block that `Insert` and `Terminate` append to.
    [[nodiscard]] bool SelectBlock(BlockIndex index);

    /// Whether the selected block already ends in a terminator, and so cannot take further instructions.
    [[nodiscard]] bool IsTerminated() const noexcept;

    /// The selected block. Selecting one first is a precondition, not a check: calling this with no block selected is
    /// undefined.
    [[nodiscard]] BlockIndex CurrentBlock() const noexcept;

    [[nodiscard]] LirFunc &Function() const noexcept;

    /// Why the last operation was rejected. Empty after a successful one.
    [[nodiscard]] std::string_view FailureReason() const noexcept;

    /// Append an instruction to the selected block. Rejects a source register that is not yet defined, and a
    /// destination that is unallocated or was already assigned.
    [[nodiscard]] bool Insert(LirInstr instruction);

    /// Close the selected block. Rejects an undefined condition or return register, and any edge to a block that does
    /// not exist.
    [[nodiscard]] bool Terminate(LirTerminator terminator);

    /// The opcode a source binary operator lowers to, or nullopt when it has no direct LIR form and the caller must
    /// expand it.
    [[nodiscard]] static std::optional<LirOpcode> BinaryOpcode(TokenKind op) noexcept;

    /// The opcode behind a compound assignment such as `+=`, which lowers to the same operation as its bare operator.
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
