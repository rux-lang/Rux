// Synthesized software-float conversion helpers: integer to float, float to integer and float to float.

#include "Lowering/HirToLir/HirToLirSoftFloat.h"

namespace Rux::HirToLirDetail {
void SynthesizeSoftFloatConversion(SoftFloatHelperSynthesis &synthesis) {
    EmitSoftFloatPlaceholder(synthesis);
}

/// None yet: wide integers to float32 and float64, and uint64 to a float on x86-64, still convert in the back ends.
bool ConvertsThroughSoftFloatHelper(const TypeRef::Kind from, const TypeRef::Kind to, const Target::Arch arch) {
    static_cast<void>(from);
    static_cast<void>(to);
    static_cast<void>(arch);
    return false;
}
} // namespace Rux::HirToLirDetail
