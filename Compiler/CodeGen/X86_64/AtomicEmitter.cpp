// Atomic memory operations and fences on x86-64.
// x86-64 orders memory strongly: an ordinary load already acquires and an ordinary store already releases, so loads and
// stores below sequential consistency are plain `mov`s. What it does reorder is a store followed by a load, which a
// sequentially consistent store prevents by being an `xchg`, whose implicit lock drains the store buffer. Every
// read-modify-write is one `lock`-prefixed instruction where one exists (`xchg`, `xadd`, `cmpxchg`), and a
// `lock cmpxchg` loop where none does (and, or and xor that must return the old value).
// The address is in R11, the value in RAX and a second value in R10, and the old value comes back in RAX, narrowed
// by the instruction to the operation's width and extended again to the type before it is stored.

#include "CodeGen/X86_64/Encoder.h"
#include "CodeGen/X86_64/FunctionEmitter.h"

namespace Rux {
namespace {
constexpr std::uint8_t kSequentiallyConsistent = 4;

// The prefix and REX bytes of an instruction whose memory operand is [r11] and whose register operand is RAX, RCX or
// R10, at `size` bytes. 16-bit operations take the operand-size prefix; 64-bit ones take REX.W; [r11] always takes
// REX.B, and R10 also takes REX.R.
void Prefixes(const X64Enc &encoder, const int size, const bool lock, const bool r10) {
    if (lock) {
        encoder.Byte(0xF0);
    }
    if (size == 2) {
        encoder.Byte(0x66);
    }
    std::uint8_t rex = 0x41;
    if (size == 8) {
        rex |= 0x08;
    }
    if (r10) {
        rex |= 0x04;
    }
    encoder.Byte(rex);
}
} // namespace

void X86_64FunctionEmitter::LoadAddressIntoR11(const LirReg pointer) const {
    const auto &physicalRegisters = framePlan.PhysicalRegisters();
    if (const auto physical = physicalRegisters.find(pointer); physical != physicalRegisters.end()) {
        encoder.MovR11PhysReg(physical->second);
    }
    else {
        encoder.MovR11Load(Disp(pointer));
    }
}

// Extends the low `size` bytes of RAX to the whole register as the type reads them, after an instruction that wrote
// only those bytes.
void X86_64FunctionEmitter::ExtendRax(const TypeRef &type, const int size) const {
    const bool sign = type.IsSigned();
    switch (size) {
    case 1:
        encoder.Byte(0x48);
        encoder.Byte(0x0F);
        encoder.Byte(sign ? 0xBE : 0xB6);
        encoder.Byte(0xC0); // movsx/movzx rax, al
        break;
    case 2:
        encoder.Byte(0x48);
        encoder.Byte(0x0F);
        encoder.Byte(sign ? 0xBF : 0xB7);
        encoder.Byte(0xC0); // movsx/movzx rax, ax
        break;
    case 4:
        if (sign) {
            encoder.Byte(0x48);
            encoder.Byte(0x63);
            encoder.Byte(0xC0); // movsxd rax, eax
        }
        else {
            encoder.Byte(0x89);
            encoder.Byte(0xC0); // mov eax, eax
        }
        break;
    default:
        break;
    }
}

bool X86_64FunctionEmitter::EmitAtomic(const LirInstr &instruction) {
    const TypeRef &type = instruction.type;
    switch (instruction.op) {
    case LirOpcode::Fence:
        // Only a sequentially consistent fence orders anything x86-64 would otherwise reorder.
        if (instruction.memoryOrder == kSequentiallyConsistent) {
            encoder.Byte(0x0F);
            encoder.Byte(0xAE);
            encoder.Byte(0xF0); // mfence
        }
        return true;
    case LirOpcode::SpinHint:
        encoder.Byte(0xF3);
        encoder.Byte(0x90); // pause
        return true;
    case LirOpcode::AtomicLoad: {
        const int size = SizeOfRuntime(type);
        LoadAddressIntoR11(instruction.srcs[0]);
        // A naturally aligned load of up to eight bytes is atomic on x86-64, and acquires by itself.
        if (size == 8) {
            encoder.Byte(0x49);
            encoder.Byte(0x8B);
            encoder.Byte(0x03); // mov rax, [r11]
        }
        else if (size == 4) {
            encoder.Byte(0x41);
            encoder.Byte(0x8B);
            encoder.Byte(0x03); // mov eax, [r11]
        }
        else if (size == 2) {
            encoder.Byte(0x49);
            encoder.Byte(0x0F);
            encoder.Byte(0xB7);
            encoder.Byte(0x03); // movzx rax, word [r11]
        }
        else {
            encoder.Byte(0x49);
            encoder.Byte(0x0F);
            encoder.Byte(0xB6);
            encoder.Byte(0x03); // movzx rax, byte [r11]
        }
        ExtendRax(type, size);
        hooks.StoreA(instruction.dst, type);
        return true;
    }
    case LirOpcode::AtomicStore: {
        const int size = SizeOfRuntime(type);
        hooks.LoadA(instruction.srcs[0], type);
        LoadAddressIntoR11(instruction.srcs[1]);
        // A sequentially consistent store must not be reordered with a later load, which only a locked instruction
        // prevents; `xchg` with memory is locked without a prefix.
        const bool exchange = instruction.memoryOrder == kSequentiallyConsistent;
        Prefixes(encoder, size, false, false);
        encoder.Byte(size == 1 ? (exchange ? 0x86 : 0x88) : (exchange ? 0x87 : 0x89));
        encoder.Byte(0x03); // xchg/mov [r11], rax|eax|ax|al
        return true;
    }
    case LirOpcode::AtomicExchange: {
        const int size = SizeOfRuntime(type);
        hooks.LoadA(instruction.srcs[1], type);
        LoadAddressIntoR11(instruction.srcs[0]);
        Prefixes(encoder, size, false, false);
        encoder.Byte(size == 1 ? 0x86 : 0x87);
        encoder.Byte(0x03); // xchg [r11], rax|eax|ax|al
        ExtendRax(type, size);
        hooks.StoreA(instruction.dst, type);
        return true;
    }
    case LirOpcode::AtomicFetchAdd:
    case LirOpcode::AtomicFetchSub: {
        const int size = SizeOfRuntime(type);
        hooks.LoadA(instruction.srcs[1], type);
        if (instruction.op == LirOpcode::AtomicFetchSub) {
            encoder.Byte(0x48);
            encoder.Byte(0xF7);
            encoder.Byte(0xD8); // neg rax
        }
        LoadAddressIntoR11(instruction.srcs[0]);
        Prefixes(encoder, size, true, false);
        encoder.Byte(0x0F);
        encoder.Byte(size == 1 ? 0xC0 : 0xC1);
        encoder.Byte(0x03); // lock xadd [r11], rax|eax|ax|al
        ExtendRax(type, size);
        hooks.StoreA(instruction.dst, type);
        return true;
    }
    case LirOpcode::AtomicCmpXchg: {
        const int size = SizeOfRuntime(type);
        // `cmpxchg` compares RAX with memory and, when equal, writes R10; RAX ends as the old value either way.
        hooks.LoadA(instruction.srcs[1], type);
        hooks.LoadB(instruction.srcs[2], type);
        LoadAddressIntoR11(instruction.srcs[0]);
        Prefixes(encoder, size, true, true);
        encoder.Byte(0x0F);
        encoder.Byte(size == 1 ? 0xB0 : 0xB1);
        encoder.Byte(0x13); // lock cmpxchg [r11], r10
        ExtendRax(type, size);
        hooks.StoreA(instruction.dst, type);
        return true;
    }
    case LirOpcode::AtomicFetchAnd:
    case LirOpcode::AtomicFetchOr:
    case LirOpcode::AtomicFetchXor: {
        const int size = SizeOfRuntime(type);
        hooks.LoadB(instruction.srcs[1], type);
        LoadAddressIntoR11(instruction.srcs[0]);
        // No instruction both combines and returns the old value, so the combination is retried until no other
        // thread wrote between the read and the `cmpxchg`.
        // The value only matters in its low `size` bytes, which `cmpxchg` compares and writes, so a zero-extending
        // 32-bit load serves the narrow widths.
        encoder.Byte(size == 8 ? 0x49 : 0x41);
        if (size == 1) {
            encoder.Byte(0x0F);
            encoder.Byte(0xB6);
        }
        else if (size == 2) {
            encoder.Byte(0x0F);
            encoder.Byte(0xB7);
        }
        else {
            encoder.Byte(0x8B);
        }
        encoder.Byte(0x03); // mov rax|eax, [r11], or movzx eax, byte|word [r11]
        const std::uint32_t loop = encoder.Size();
        encoder.Byte(0x48);
        encoder.Byte(0x89);
        encoder.Byte(0xC1); // mov rcx, rax
        encoder.Byte(0x4C);
        encoder.Byte(instruction.op == LirOpcode::AtomicFetchAnd  ? 0x21
                     : instruction.op == LirOpcode::AtomicFetchOr ? 0x09
                                                                  : 0x31);
        encoder.Byte(0xD1); // and|or|xor rcx, r10
        Prefixes(encoder, size, true, false);
        encoder.Byte(0x0F);
        encoder.Byte(size == 1 ? 0xB0 : 0xB1);
        encoder.Byte(0x0B); // lock cmpxchg [r11], rcx
        encoder.Byte(0x0F);
        encoder.Byte(0x85); // jne loop
        encoder.Dword(static_cast<std::uint32_t>(static_cast<std::int32_t>(loop) -
                                                 static_cast<std::int32_t>(encoder.Size() + 4)));
        ExtendRax(type, size);
        hooks.StoreA(instruction.dst, type);
        return true;
    }
    default:
        return false;
    }
}
} // namespace Rux
