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
// No target operating system travels with the emitter the way one travels with
// the x86-64 emitter, because AAPCS64 is the single calling convention on every
// AArch64 target and nothing here has yet had cause to ask which system it is
// building for.

#include "Diagnostics/Diagnostics.h"
#include "Ir/Lir/Lir.h"
#include "Object/Rcu/Rcu.h"

#include <string>
#include <vector>

namespace Rux {
class AArch64RcuEmitter {
public:
    explicit AArch64RcuEmitter(const LirPackage &package, std::string inputPackageName = {});
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
    mutable std::vector<Diagnostic> diagnostics;
};
} // namespace Rux
