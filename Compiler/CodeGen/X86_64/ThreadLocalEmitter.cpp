// The address of this thread's copy of a thread-local variable, on x86-64.
// Each system reaches a thread's block its own way. On Linux and FreeBSD the FS segment base is the thread pointer,
// and the program's own block ends there, so a variable is a fixed negative offset from it: two instructions, with the
// offset filled in by the linker. On Windows the GS segment is the thread environment block, whose slot array at 0x58
// is indexed by the image's TLS index to reach this thread's block, and the variable is a fixed offset into that block.
// On macOS each variable has a descriptor whose first word is a function returning this thread's copy; calling it
// clobbers only RAX and RDI, so nothing a Rux function keeps between instructions is disturbed.
// The address is left in RAX and stored as the instruction's result.

#include "CodeGen/X86_64/Encoder.h"
#include "CodeGen/X86_64/FunctionEmitter.h"
#include "Object/Rcu/Rcu.h"

#include <string>

namespace Rux {
bool X86_64FunctionEmitter::EmitThreadLocal(const LirInstr &instruction) {
    if (instruction.op != LirOpcode::ThreadLocalAddr) {
        return false;
    }
    switch (hooks.TargetOs()) {
    case Target::OS::Windows: {
        // mov r11, gs:[0x58]
        for (const std::uint8_t byte : {0x65, 0x4C, 0x8B, 0x1C, 0x25, 0x58, 0x00, 0x00, 0x00}) {
            encoder.Byte(byte);
        }
        // mov r10d, [rip + _tls_index]
        encoder.Byte(0x44);
        encoder.Byte(0x8B);
        encoder.Byte(0x15);
        const std::uint32_t indexOffset = encoder.Size();
        encoder.Dword(0);
        hooks.AddTextRelocation(indexOffset, hooks.ResolveGlobalSymbol(std::string(kTlsIndexSymbol)));
        // mov r11, [r11 + r10 * 8]
        for (const std::uint8_t byte : {0x4F, 0x8B, 0x1C, 0xD3}) {
            encoder.Byte(byte);
        }
        // lea rax, [r11 + offset]
        encoder.Byte(0x49);
        encoder.Byte(0x8D);
        encoder.Byte(0x83);
        const std::uint32_t offset = encoder.Size();
        encoder.Dword(0);
        hooks.AddTypedTextRelocation(offset, hooks.ResolveGlobalSymbol(instruction.strArg), RcuRelType::TlsOffset32);
        break;
    }
    case Target::OS::MacOS: {
        // lea rdi, [rip + descriptor]; call [rdi]
        encoder.Byte(0x48);
        encoder.Byte(0x8D);
        encoder.Byte(0x3D);
        const std::uint32_t descriptor = encoder.Size();
        encoder.Dword(0);
        hooks.AddTypedTextRelocation(descriptor, hooks.ResolveGlobalSymbol(instruction.strArg),
                                     RcuRelType::TlvDescriptorRel32);
        encoder.Byte(0xFF);
        encoder.Byte(0x17);
        break;
    }
    default: {
        // mov rax, fs:[0]; lea rax, [rax + offset]
        for (const std::uint8_t byte : {0x64, 0x48, 0x8B, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x80}) {
            encoder.Byte(byte);
        }
        const std::uint32_t offset = encoder.Size();
        encoder.Dword(0);
        hooks.AddTypedTextRelocation(offset, hooks.ResolveGlobalSymbol(instruction.strArg), RcuRelType::TlsOffset32);
        break;
    }
    }
    hooks.StoreA(instruction.dst, instruction.type);
    return true;
}
} // namespace Rux
