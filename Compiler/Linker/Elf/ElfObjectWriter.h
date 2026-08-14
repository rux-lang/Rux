#pragma once

#include "Object/Rcu/Rcu.h"
#include "Target/Target.h"

#include <string>

namespace Rux {
struct NativeObject;

[[nodiscard]] bool WriteElfObject(const RcuFile &file, Target::OS targetOs, Target::Arch targetArch,
                                  NativeObject &output, std::string &error);
} // namespace Rux
