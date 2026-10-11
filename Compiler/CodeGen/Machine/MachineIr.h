#pragma once

// Machine IR: the form a function takes between instruction selection and encoding.
//
// Instructions carry target opcodes but a target-independent operand model, so register allocation can run over any
// back end. Registers below `PhysicalRegisterCount` name machine registers; every other register is virtual and has a
// class. A function is not in SSA form: a selector may define a virtual register more than once (two-address forms,
// the copies phi elimination leaves behind), and liveness is recomputed from the instructions themselves.

#include <cstdint>
#include <vector>

namespace Rux::Machine {
using Reg = std::uint32_t;
inline constexpr Reg NoReg = ~Reg{};
inline constexpr Reg PhysicalRegisterCount = 64;
inline constexpr std::uint32_t NoSymbol = ~std::uint32_t{};

[[nodiscard]] constexpr bool IsPhysical(const Reg reg) noexcept {
    return reg < PhysicalRegisterCount;
}

[[nodiscard]] constexpr bool IsVirtual(const Reg reg) noexcept {
    return reg != NoReg && reg >= PhysicalRegisterCount;
}

[[nodiscard]] constexpr std::uint64_t RegisterBit(const Reg reg) noexcept {
    return std::uint64_t{1} << reg;
}

enum class RegClass : std::uint8_t {
    General,
    Vector
};

/// A memory reference `[base + index * scale + displacement]`. A reference to a frame object addresses that object
/// relative to the frame pointer once the frame is laid out; a reference with a symbol is relative to the instruction
/// pointer and has neither base nor index.
struct Address {
    Reg base = NoReg;
    Reg index = NoReg;
    std::uint8_t scale = 1;
    std::int32_t displacement = 0;
    std::int32_t frameObject = -1;
    std::uint32_t symbol = NoSymbol;
};

struct Operand {
    enum class Kind : std::uint8_t {
        None,
        Register,
        Immediate,
        Memory,
        Block,
        Symbol
    };

    Kind kind = Kind::None;
    bool def = false;
    bool use = false;
    Reg reg = NoReg;
    std::int64_t immediate = 0;
    Address address;
    std::uint32_t target = 0;

    [[nodiscard]] static Operand Use(const Reg reg) noexcept {
        Operand operand;
        operand.kind = Kind::Register;
        operand.use = true;
        operand.reg = reg;
        return operand;
    }

    [[nodiscard]] static Operand Def(const Reg reg) noexcept {
        Operand operand;
        operand.kind = Kind::Register;
        operand.def = true;
        operand.reg = reg;
        return operand;
    }

    /// A register an instruction reads and then overwrites, which is what a two-address destination is.
    [[nodiscard]] static Operand UseDef(const Reg reg) noexcept {
        Operand operand;
        operand.kind = Kind::Register;
        operand.use = true;
        operand.def = true;
        operand.reg = reg;
        return operand;
    }

    [[nodiscard]] static Operand Imm(const std::int64_t value) noexcept {
        Operand operand;
        operand.kind = Kind::Immediate;
        operand.immediate = value;
        return operand;
    }

    [[nodiscard]] static Operand Mem(const Address &address) noexcept {
        Operand operand;
        operand.kind = Kind::Memory;
        operand.address = address;
        return operand;
    }

    [[nodiscard]] static Operand BlockTarget(const std::uint32_t block) noexcept {
        Operand operand;
        operand.kind = Kind::Block;
        operand.target = block;
        return operand;
    }

    [[nodiscard]] static Operand SymbolTarget(const std::uint32_t symbol) noexcept {
        Operand operand;
        operand.kind = Kind::Symbol;
        operand.target = symbol;
        return operand;
    }

    [[nodiscard]] bool IsRegister() const noexcept {
        return kind == Kind::Register;
    }
};

struct Instruction {
    std::uint16_t opcode = 0;
    /// Operand width in bytes, as the target reads it.
    std::uint8_t size = 8;
    /// A second width, such as the source of an extension or conversion.
    std::uint8_t sourceSize = 0;
    /// A condition code, for the opcodes that test one.
    std::uint8_t condition = 0;
    /// A plain register-to-register copy of one class, which the allocator may remove by coalescing.
    bool isCopy = false;
    /// The definition can be recomputed at any use instead of being kept in a register or spilled.
    bool rematerializable = false;
    std::vector<Operand> operands;
    /// Physical registers the instruction writes or reads without naming them, such as a call's clobbers.
    std::uint64_t implicitDefs = 0;
    std::uint64_t implicitUses = 0;
};

struct Block {
    std::vector<Instruction> instructions;
    std::vector<std::uint32_t> successors;
    std::uint32_t loopDepth = 0;
    /// Reached only on the way to a failure, so layout places it after every hot block.
    bool cold = false;
};

/// Storage in the frame: a spill slot, an aggregate value's home, or an allocation's data. Offsets are assigned
/// after register allocation, once the number of saved registers is known.
struct FrameObject {
    std::int32_t size = 8;
    std::int32_t alignment = 8;
    std::int32_t offset = 0;
};

struct Function {
    std::vector<Block> blocks;
    std::vector<RegClass> virtualClasses;
    std::vector<FrameObject> frameObjects;

    [[nodiscard]] Reg NewVirtual(const RegClass regClass) {
        virtualClasses.push_back(regClass);
        return PhysicalRegisterCount + static_cast<Reg>(virtualClasses.size() - 1);
    }

    [[nodiscard]] std::size_t VirtualCount() const noexcept {
        return virtualClasses.size();
    }

    [[nodiscard]] RegClass VirtualClass(const Reg reg) const {
        return virtualClasses.at(reg - PhysicalRegisterCount);
    }

    [[nodiscard]] std::int32_t NewFrameObject(const std::int32_t size, const std::int32_t alignment) {
        frameObjects.push_back({size > 0 ? size : 1, alignment > 0 ? alignment : 1, 0});
        return static_cast<std::int32_t>(frameObjects.size() - 1);
    }
};
} // namespace Rux::Machine
