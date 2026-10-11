#pragma once

// Machine IR to AArch64 instruction words. Registers are physical by now; addresses still name frame objects and
// symbols, which the encoder resolves: a frame object is an offset from SP, a symbol an ADRP and ADD pair with a
// relocation on each. Whatever an operand needs that its instruction cannot encode — a displacement out of reach, an
// index with a displacement, an immediate with no encoding — is computed in X16 or X17 first, which nothing else
// allocates.

#include "CodeGen/AArch64/Encoder.h"
#include "CodeGen/AArch64/MachineInstructions.h"
#include "Target/Target.h"

#include <cstdint>
#include <vector>

namespace Rux::AArch64Machine {
/// A relocation the function needs: `type` applied at `offset` from the start of the function to symbol `symbol` of
/// the function's symbol table, plus `addend`.
struct SymbolUse {
    std::uint32_t offset = 0;
    std::uint32_t symbol = 0;
    std::uint16_t type = 0;
    std::int32_t addend = 0;
};

class MachineEncoder {
public:
    MachineEncoder(std::vector<std::uint8_t> &bytes, std::vector<SymbolUse> &symbolUses,
                   const std::vector<std::int32_t> &objectOffsets, const SelectedFunction &selected,
                   Target::OS targetOs);

    /// Encodes one instruction other than a branch or a return, which the emitter places itself.
    void Encode(const Machine::Instruction &instruction);

    /// The words themselves, for the prologue, the epilogue and branches.
    [[nodiscard]] const A64Enc &Words() const {
        return enc;
    }

    /// Records a refusal of the underlying encoder; a function with any is left to the established emitter.
    void Must(A64Status status);

    [[nodiscard]] bool Failed() const {
        return failed;
    }

private:
    struct Memory {
        A64Reg base;
        A64Reg index;
        bool indexed = false;
        unsigned shift = 0;
        std::int64_t offset = 0;
        bool unscaled = false;
    };

    std::vector<SymbolUse> &symbolUses;
    const std::vector<std::int32_t> &objectOffsets;
    const SelectedFunction &selected;
    Target::OS targetOs;
    A64Enc enc;
    bool failed = false;

    /// Brings an address within reach of an access of `width` bytes, using `scratch` when it must.
    [[nodiscard]] Memory Resolve(const Machine::Address &address, unsigned width, A64Reg scratch);
    void ComputeAddress(A64Reg destination, const Machine::Address &address);
    void SymbolAddress(A64Reg destination, std::uint32_t symbol, std::int32_t addend);
    void LoadStore(const Machine::Instruction &instruction);
    void Arithmetic(const Machine::Instruction &instruction);
    void Panic(const Machine::Instruction &instruction);
    void WriteStatic(std::uint32_t symbol, std::uint32_t length);
};
} // namespace Rux::AArch64Machine
