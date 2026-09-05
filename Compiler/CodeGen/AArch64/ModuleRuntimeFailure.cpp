#include "CodeGen/AArch64/ModuleEmitter.h"

namespace Rux::AArch64Detail {

// One of the four reinterpretations, which is one FMOV between the register
// files: the two registers hold the same number of bits — a word pairs with
// an S register and a doubleword with a D one — and nothing is converted on
// the way. Which direction it is decides which file the slot is read at.
// Assertions
//
// A failed assertion says what failed and where, then stops the process
// where it failed. Saying it is a write to standard error, reached directly
// through a Unix system call or through KERNEL32 on Windows so neither path
// needs a C runtime. Stopping is BRK, which is what UD2 is on x86-64 — an
// instruction no operand makes valid, so a debugger attached to the process
// lands on the assertion rather than on whatever unwinding it to a handler
// reached.
//
// Three writes rather than one, for the reason the x86-64 back end makes
// three: the prefix and the location are constants this object interns, the
// message is a slice the program built at runtime, and joining them would
// mean allocating somewhere to join them in.

// fd, buffer and length are already in X0, X1 and X2.
void AArch64ModuleEmitter::EmitWriteSyscall(const WriteSyscall &call) {
    Must(enc.LoadImm64(A64::Xn(call.numberReg), call.number), "a system call number");
    Must(enc.Svc(call.trap), "a system call");
}

void AArch64ModuleEmitter::GenAssert(AArch64TerminatorEmitter &terminatorEmitter, const LirInstr &instr) {
    const bool isAssertion = instr.op == LirOpcode::Assert;
    if (instr.srcs.size() < (isAssertion ? 2U : 1U)) {
        Report(std::format("AArch64 code generation reached a '{}' with too few operands in '{}'",
                           LirOpcodeName(instr.op), currentFunc));
        return;
    }
    const bool isWindows = targetOs == Target::OS::Windows;
    const auto syscall = WriteSyscallFor(targetOs);
    if (!isWindows && !syscall) {
        NotImplemented("an assertion on this operating system");
        return;
    }

    // A condition that held skips the whole failure path, which is the one
    // thing about this opcode that costs a running program anything. A
    // panic has no condition and no branch.
    std::uint32_t heldBranch = 0;
    if (isAssertion) {
        LoadFromSlot(A64::Xn(kTemp), instr.srcs[0], TypeRef::MakeBool());
        heldBranch = terminatorEmitter.EmitBranchOverNonZero(A64::Xn(kTemp));
    }

    const RuntimeFailureLayout layout =
        BuildRuntimeFailureLayout(isAssertion ? RuntimeFailureKind::Assertion : RuntimeFailureKind::Panic,
                                  instr.sourceFunction, instr.sourceFile, instr.sourceLine, instr.sourceColumn);

    if (isWindows) {
        const std::uint32_t getStdHandle =
            GetOrAddExtern("GetStdHandle", RcuSymKind::ExternFunc, std::string(kKernel32));
        const std::uint32_t writeFile = GetOrAddExtern("WriteFile", RcuSymKind::ExternFunc, std::string(kKernel32));

        // One aligned doubleword pair gives WriteFile a writable DWORD for
        // lpNumberOfBytesWritten. The failure path ends at BRK, so it never
        // has to close this area; a held assertion branches over it.
        Must(enc.FrameAdjust(-16), "the assertion write area");
        EmitWindowsWriteStatic(getStdHandle, writeFile, layout.prefix);

        const LirReg messageReg = instr.srcs[isAssertion ? 1 : 0];
        PrepareWindowsWrite(getStdHandle);
        LoadPointer(A64::Xn(kAddr), messageReg);
        Must(enc.Ldp(A64::Xn(1), A64::Xn(2), A64::Xn(kAddr), 0), "an assertion message");
        EmitWindowsCall(writeFile, "a call to WriteFile");

        EmitWindowsWriteStatic(getStdHandle, writeFile, layout.location);
    }
    else {
        EmitWriteStatic(*syscall, layout.prefix);

        // The message is a `string` the caller built, so what the
        // operand holds is its address and the two doublewords behind it
        // are what the system call takes.
        const LirReg messageReg = instr.srcs[isAssertion ? 1 : 0];
        LoadPointer(A64::Xn(kAddr), messageReg);
        Must(enc.Ldp(A64::Xn(1), A64::Xn(2), A64::Xn(kAddr), 0), "an assertion message");
        Must(enc.LoadImm64(A64::Xn(0), kStandardError), "the standard error descriptor");
        EmitWriteSyscall(*syscall);

        EmitWriteStatic(*syscall, layout.location);
    }

    Must(enc.Brk(1), "an assertion trap");
    if (isAssertion) {
        terminatorEmitter.PatchBranchOver(heldBranch);
    }
}
} // namespace Rux::AArch64Detail
