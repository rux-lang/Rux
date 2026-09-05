#include "Types/TypeProperties.h"

namespace Rux {
std::string_view ValueConsumptionKindName(const ValueConsumptionKind kind) noexcept {
    switch (kind) {
    case ValueConsumptionKind::Initialization:
        return "initialization";
    case ValueConsumptionKind::Argument:
        return "argument";
    case ValueConsumptionKind::Receiver:
        return "receiver";
    case ValueConsumptionKind::Return:
        return "return";
    case ValueConsumptionKind::Assignment:
        return "assignment";
    case ValueConsumptionKind::Aggregate:
        return "aggregate";
    case ValueConsumptionKind::ArrayRepeat:
        return "array repeat";
    case ValueConsumptionKind::ConditionalArm:
        return "conditional-arm";
    case ValueConsumptionKind::CoalescingOperand:
        return "coalescing operand";
    case ValueConsumptionKind::CoalescingFallback:
        return "coalescing fallback";
    case ValueConsumptionKind::ExplicitMove:
        return "explicit";
    }
    return "unknown";
}
} // namespace Rux
