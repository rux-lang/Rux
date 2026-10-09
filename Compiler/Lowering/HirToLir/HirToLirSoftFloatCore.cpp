// Synthesized software-float unpack, round-and-pack and comparison helpers shared by every operation.

#include "Lowering/HirToLir/HirToLirSoftFloat.h"

namespace Rux::HirToLirDetail {
void SynthesizeSoftFloatComparison(SoftFloatHelperSynthesis &synthesis) {
    EmitSoftFloatPlaceholder(synthesis);
}
} // namespace Rux::HirToLirDetail
