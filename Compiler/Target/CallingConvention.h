#pragma once

namespace Rux {

// Language-level calling convention carried from syntax through codegen.
// Concrete register and stack rules are resolved by TargetInfo.
enum class CallingConvention {
    Default,
    Win64,
};

} // namespace Rux
