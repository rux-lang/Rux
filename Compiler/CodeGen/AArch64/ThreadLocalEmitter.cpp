// The address of this thread's copy of a thread-local variable, on AArch64.
// On Linux and FreeBSD TPIDR_EL0 is the thread pointer, and the program's own block follows a sixteen-byte control
// block after it, so a variable is a fixed positive offset from it, added in two twelve-bit halves the linker fills in.
// On Windows x18 holds the thread environment block, whose slot array at 0x58 is indexed by the image's TLS index to
// reach this thread's block, and the variable is a fixed offset into it. On macOS each variable has a descriptor whose
// first word is a function returning this thread's copy in x0; it clobbers only x0, x16, x17 and the link register,
// which every Rux prologue has saved.
// The address is built in x9 and stored as the instruction's result.

#include "CodeGen/AArch64/Encoder.h"
#include "CodeGen/AArch64/FunctionEmitter.h"
#include "Object/Rcu/Rcu.h"

#include <string>

namespace Rux {
namespace {
constexpr unsigned kResult = 9;
constexpr unsigned kIndex = 10;
// add x9, x9, #0, lsl #12 and add x9, x9, #0: the halves of an offset the linker fills in.
constexpr std::uint32_t kAddHigh = 0x91400129;
constexpr std::uint32_t kAddLow = 0x91000129;
} // namespace

bool AArch64FunctionEmitter::EmitThreadLocal(const LirInstr &instruction) {
    if (instruction.op != LirOpcode::ThreadLocalAddr) {
        return false;
    }
    // An instruction whose immediate the linker fills in. The relocation follows the word, since the module only
    // accepts one over bytes it already holds.
    const auto relocated = [&](const std::uint32_t word, const std::uint32_t symbol, const std::uint16_t type) {
        const std::uint32_t site = encoder.Size();
        encoder.Word(word);
        hooks.AddTypedTextRelocation(site, symbol, type);
    };
    const auto addOffset = [&] {
        const std::uint32_t variable = hooks.ResolveGlobalSymbol(instruction.strArg);
        relocated(kAddHigh, variable, RcuRelType::AArch64TlsHi12);
        relocated(kAddLow, variable, RcuRelType::AArch64TlsLo12Nc);
    };
    switch (hooks.TargetOs()) {
    case Target::OS::Windows: {
        // adrp x10, _tls_index; ldr w10, [x10, :lo12:_tls_index]
        const std::uint32_t index = hooks.ResolveGlobalSymbol(std::string(kTlsIndexSymbol));
        relocated(0x90000000U | kIndex, index, RcuRelType::AArch64AdrPrelPgHi21);
        relocated(0xB9400000U | kIndex << 5U | kIndex, index, RcuRelType::AArch64LdstAbsLo12Nc);
        encoder.Word(0xF9402E49U); // ldr x9, [x18, #0x58]
        encoder.Word(0xF86A7929U); // ldr x9, [x9, x10, lsl #3]
        addOffset();
        break;
    }
    case Target::OS::MacOS: {
        // adrp x0, descriptor; add x0, x0, :lo12:descriptor
        const std::uint32_t variable = hooks.ResolveGlobalSymbol(instruction.strArg);
        relocated(0x90000000U, variable, RcuRelType::AArch64TlvDescriptorPage21);
        relocated(0x91000000U, variable, RcuRelType::AArch64TlvDescriptorPageOff12);
        encoder.Word(0xF9400010U); // ldr x16, [x0]
        encoder.Word(0xD63F0200U); // blr x16
        Must(encoder.Mov(A64::Xn(kResult), A64::Xn(0)), "a thread-local address");
        break;
    }
    default:
        Must(encoder.Mrs(A64::Xn(kResult), A64::TpidrEl0), "the thread pointer");
        addOffset();
        break;
    }
    hooks.StoreToSlot(A64::Xn(kResult), instruction.dst, instruction.type);
    return true;
}
} // namespace Rux
