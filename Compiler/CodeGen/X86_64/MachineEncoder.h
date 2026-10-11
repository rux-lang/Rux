#pragma once

// Binary encoding of allocated x86-64 machine instructions with general ModRM/SIB/REX operand forms.

#include "CodeGen/X86_64/MachineInstructions.h"
#include "Target/Target.h"

#include <cstdint>
#include <vector>

namespace Rux::X86_64Machine {
/// A RIP-relative or call operand: the 32-bit field at `offset` holds `symbol + addend - (offset + 4)` once linked.
struct SymbolUse {
    std::uint32_t offset = 0;
    std::uint32_t symbol = 0;
    std::int32_t addend = 0;
};

class MachineEncoder {
public:
    MachineEncoder(std::vector<std::uint8_t> &bytes, std::vector<SymbolUse> &symbolUses,
                   const std::vector<std::int32_t> &frameOffsets, const SelectedFunction &function,
                   Target::OS targetOs);

    /// Encodes any instruction other than a branch or return, which the function emitter lays out itself.
    void Encode(const Machine::Instruction &instruction);

    void Push(Reg reg);
    void Pop(Reg reg);
    void MovRbpRsp();
    void SubRsp(std::int32_t bytes);
    /// `or qword [rsp], 0`: touches the page the stack pointer just entered.
    void ProbeRsp();
    void LeaRspFromRbp(std::int32_t displacement);
    void Ret();
    void SaveVector(Reg reg, std::int32_t rbpDisplacement);
    void RestoreVector(Reg reg, std::int32_t rbpDisplacement);
    void Jmp(std::int32_t displacement, bool shortForm);
    void Jcc(Cond condition, std::int32_t displacement, bool shortForm);

    [[nodiscard]] static int JmpSize(const bool shortForm) noexcept {
        return shortForm ? 2 : 5;
    }

    [[nodiscard]] static int JccSize(const bool shortForm) noexcept {
        return shortForm ? 2 : 6;
    }

private:
    struct Rm {
        bool memory = false;
        std::uint8_t reg = 0;
        bool byteRegister = false;
        Machine::Address address;
    };

    std::vector<std::uint8_t> &out;
    std::vector<SymbolUse> &symbolUses;
    const std::vector<std::int32_t> &frameOffsets;
    const SelectedFunction &function;
    Target::OS targetOs;

    void Byte(std::uint8_t value);
    void Immediate(std::int64_t value, int bytes);
    void Emit(std::uint8_t prefix, bool rexW, std::initializer_list<std::uint8_t> opcode, std::uint8_t regField,
              bool regIsByte, const Rm &rm, int immediateBytes = 0, std::int64_t immediate = 0);
    void Memory(std::uint8_t regField, const Machine::Address &address, int immediateBytes);

    [[nodiscard]] static Rm RegisterRm(Reg reg, bool byteRegister = false);
    [[nodiscard]] static Rm MemoryRm(const Machine::Address &address);

    void EncodeMove(const Machine::Instruction &instruction);
    void EncodeAlu(const Machine::Instruction &instruction, std::uint8_t digit, std::uint8_t registerOpcode);
    void EncodeShift(const Machine::Instruction &instruction, std::uint8_t digit);
    void EncodeFloat(const Machine::Instruction &instruction, std::uint8_t opcode);
    void EncodeFpToUInt64(const Machine::Instruction &instruction);
    void EncodeUInt64ToFp(const Machine::Instruction &instruction);
    void EncodePanic(const Machine::Instruction &instruction);
    void CallSymbol(std::uint32_t symbol);
    void LeaRip(Reg destination, std::uint32_t symbol);
};
} // namespace Rux::X86_64Machine
