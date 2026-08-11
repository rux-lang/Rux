#pragma once

// AArch64 RCU code generation: lowers a LirModule to an in-memory RcuFile, the
// same object the x86-64 back end produces and the same one the linker
// consumes. The counterpart of CodeGen/X86_64/RcuEmitter.h.
//
// This is the back end that replaces CodeGen/AArch64/NativeEmitter.cpp, which
// writes a C translation unit and shells out to Clang. It is being written one
// group of opcodes at a time (BACKLOG.md Phases 3-5), so what it does not lower
// yet it reports as a diagnostic naming the construct rather than emitting
// something plausible: an object this produces is either right or absent.
//
// The target operating system is not what chooses a calling convention here —
// AAPCS64 is the single one on every AArch64 target — but it is what a failed
// assertion asks to print its message: the write it makes is a system call, and
// the number, the register that number travels in and the immediate SVC carries
// are all a property of the kernel rather than of the architecture.

#include "Diagnostics/Diagnostics.h"
#include "Ir/Lir/Lir.h"
#include "Object/Rcu/Rcu.h"
#include "Target/Target.h"

#include <string>
#include <vector>

namespace Rux {
class AArch64RcuEmitter {
public:
    // `linux-aarch64` is the only target Driver::UnsupportedBackendReason lets
    // this back end reach, so that is what an unspecified system means — unlike
    // the x86-64 emitter, whose default is the host it happens to run on.
    explicit AArch64RcuEmitter(const LirPackage &package, std::string inputPackageName = {},
                               Target::OS inputTargetOs = Target::OS::Linux);
    [[nodiscard]] std::vector<RcuFile> Generate() const;

    // Diagnostics accumulated during generation, which for this back end also
    // covers everything it does not implement yet. Populated by Generate();
    // check after calling it.
    [[nodiscard]] const std::vector<Diagnostic> &Diagnostics() const {
        return diagnostics;
    }

private:
    const LirPackage &lir;
    std::string packageName;
    Target::OS targetOs;
    mutable std::vector<Diagnostic> diagnostics;
};
} // namespace Rux
