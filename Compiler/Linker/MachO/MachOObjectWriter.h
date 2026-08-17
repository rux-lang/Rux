#pragma once

#include "Object/Rcu/Rcu.h"
#include "Target/Target.h"

#include <string>

namespace Rux {
struct NativeObject;

/// Translate one RCU object into a relocatable Mach-O object, so a Rux static library can be linked by a foreign
/// toolchain that cannot read RCU.
///
/// @param error Set to an operational reason when the translation is rejected
/// @return false when the object could not be written
[[nodiscard]] bool WriteMachOObject(const RcuFile &file, Target::Arch targetArch, NativeObject &output,
                                    std::string &error);
} // namespace Rux
