#include "CodeGen/X86_64/ModuleEmitter.h"

namespace Rux::X86_64Detail {

// Instruction code generation
void X86_64ModuleEmitter::GenInstr(X86_64FunctionEmitter &functionEmitter, X86_64CallEmitter &callEmitter,
                                   const LirInstr &instr) {
    if (functionEmitter.EmitArithmetic(instr) || functionEmitter.EmitMemory(instr) || callEmitter.Emit(instr)) {
        return;
    }
    switch (instr.op) {
    case LirOpcode::Assert:
    case LirOpcode::Panic: {
        const bool isAssertion = instr.op == LirOpcode::Assert;
        if (instr.srcs.size() < (isAssertion ? 2 : 1)) {
            Report(UnsupportedLirDiagnostic(
                instr.op, targetOs, Target::Arch::X86_64, currentFunc,
                std::format("expected at least {} operands, found {}", isAssertion ? 2 : 1, instr.srcs.size())));
            break;
        }
        uint32_t okPatch = 0;
        if (isAssertion) {
            LoadA(instr.srcs[0], TypeRef::MakeBool());
            enc.TestRaxRax();
            enc.Jnz(okPatch);
        }

        const LirReg messageReg = instr.srcs[isAssertion ? 1 : 0];
        const RuntimeFailureLayout layout =
            BuildRuntimeFailureLayout(isAssertion ? RuntimeFailureKind::Assertion : RuntimeFailureKind::Panic,
                                      instr.sourceFunction, instr.sourceFile, instr.sourceLine, instr.sourceColumn);

        if (targetOs == Target::OS::Windows) {
            const uint32_t getStdHandle = GetOrAddExtern("GetStdHandle", RcuSymKind::ExternFunc, "KERNEL32.DLL");
            const uint32_t writeFile = GetOrAddExtern("WriteFile", RcuSymKind::ExternFunc, "KERNEL32.DLL");

            // Shadow space, the fifth WriteFile argument, and a DWORD for
            // lpNumberOfBytesWritten. The failure path never returns.
            enc.SubRspImm32(48);
            enc.MovQwordRspImm32(32, 0);

            const auto prepareWrite = [&]() {
                enc.MovEaxImm32(-12); // STD_ERROR_HANDLE
                enc.MovArgWin64Rax(0);
                uint32_t getHandleReloc;
                enc.Call(getHandleReloc);
                AddTextReloc(getHandleReloc, getStdHandle);
                enc.MovArgWin64Rax(0);
                enc.LeaR9Rsp(40);
            };
            const auto writeStatic = [&](const std::string &text) {
                prepareWrite();
                const uint32_t textSymbol = InternStr(text);
                uint32_t textReloc;
                enc.LeaRaxRip(textReloc);
                AddTextReloc(textReloc, textSymbol);
                enc.MovArgWin64Rax(1);
                enc.MovEaxImm32(static_cast<int32_t>(text.size()));
                enc.MovArgWin64Rax(2);
                uint32_t writeReloc;
                enc.Call(writeReloc);
                AddTextReloc(writeReloc, writeFile);
            };

            writeStatic(layout.prefix);

            prepareWrite();
            LoadA(messageReg, TypeRef::MakePointer(TypeRef::MakeString8()));
            enc.MovR10Rax();
            enc.MovRdxR10Load();
            enc.MovR8R10Load(8);
            uint32_t messageWriteReloc;
            enc.Call(messageWriteReloc);
            AddTextReloc(messageWriteReloc, writeFile);

            writeStatic(layout.location);
        }
        else {
            const int syscallNumber = targetOs == Target::OS::Linux ? 1
                                    : targetOs == Target::OS::MacOS ? 0x0200'0004
                                                                    : 4;
            const auto writeStatic = [&](const std::string &text) {
                const uint32_t textSymbol = InternStr(text);
                uint32_t textReloc;
                enc.LeaRaxRip(textReloc);
                AddTextReloc(textReloc, textSymbol);
                enc.MovRsiRax();
                enc.MovEdxImm32(static_cast<int32_t>(text.size()));
                enc.MovEdiImm32(2);
                enc.MovEaxImm32(syscallNumber);
                enc.Syscall();
            };

            writeStatic(layout.prefix);
            LoadA(messageReg, TypeRef::MakePointer(TypeRef::MakeString8()));
            enc.MovR10Rax();
            enc.MovRsiR10Load();
            enc.MovRdxR10Load(8);
            enc.MovEdiImm32(2);
            enc.MovEaxImm32(syscallNumber);
            enc.Syscall();
            writeStatic(layout.location);
        }

        enc.Ud2();
        if (isAssertion) {
            const auto here = static_cast<int32_t>(enc.Size());
            enc.Patch32(okPatch, here - static_cast<int32_t>(okPatch + 4));
        }
        break;
    }
    case LirOpcode::Phi:
        // The predecessor's terminator has already copied the selected
        // incoming value into this register's home.
        break;
    default:
        Report(UnsupportedLirDiagnostic(instr.op, targetOs, Target::Arch::X86_64, currentFunc));
        break;
    }
}
} // namespace Rux::X86_64Detail
