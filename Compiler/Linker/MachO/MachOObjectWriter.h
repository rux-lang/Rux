#pragma once

#include "Object/Rcu/Rcu.h"
#include "Target/Target.h"

#include <string>

namespace Rux {
struct NativeObject;

[[nodiscard]] bool WriteMachOObject(const RcuFile &file, Target::Arch targetArch, NativeObject &output,
                                    std::string &error);
} // namespace Rux
