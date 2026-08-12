#pragma once

// AArch64 RCU code generation: lowers a LirModule to an in-memory RcuFile, the
// same object the x86-64 back end produces and the same one the linker
// consumes. The counterpart of CodeGen/X86_64/RcuEmitter.h.
//
// This is the only AArch64 back end: nothing here calls an external assembler,
// compiler or linker. What it does not lower it reports as a diagnostic naming
// the construct rather than emitting something plausible, so an object this
// produces is either right or absent.
//
// The target operating system does not replace AAPCS64, but it selects platform
// variants within it: Windows C variadic calls use only the general-purpose
// argument file, while Apple fixed arguments use naturally packed stack slots,
// do not skip an odd general register for 16-byte alignment, and extend narrow
// integers in the caller. Apple C variadic calls preserve that fixed prefix,
// then pass the promoted anonymous tail in eight-byte stack slots. It also
// decides how a failed assertion prints its message; Unix syscall details and
// Windows API imports are platform properties rather than architecture properties.

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
