#pragma once

#include "Object/Rcu/Rcu.h"
#include "Target/Target.h"

#include <string>

namespace Rux {
struct NativeObject;

/// Translate one RCU object into a relocatable ELF object.
///
/// Emitting the platform's own object format is what lets a Rux static library be consumed by a foreign toolchain,
/// which cannot read RCU. The target OS is a parameter rather than a host check because the writer must produce the
/// same bytes whichever machine it runs on.
///
/// @param error Set to an operational reason when the translation is rejected
/// @return false when the object could not be written
[[nodiscard]] bool WriteElfObject(const RcuFile &file, Target::OS targetOs, Target::Arch targetArch,
                                  NativeObject &output, std::string &error);
} // namespace Rux
