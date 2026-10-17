// Propagation and coalescing dispatch: `expr?` and `option ?? fallback` lower only native outcomes, whose matches are
// built in AstToHirNative.cpp. A semantic fact is required for every accepted expression, because lowering never
// decides what an operand is from its type alone.

#include "Lowering/AstToHir/Detail/AstToHirContext.h"

#include <cassert>
#include <cstdlib>

namespace Rux::AstToHirDetail {
HirExprPtr AstToHirContext::LowerTryExpr(const TryExpr &expression) {
    const ResolvedPropagation *fact = model.TryGetPropagation(expression);
    assert(fact != nullptr && "accepted propagation is missing its semantic fact");
    if (!fact) {
        std::abort();
    }
    return LowerNativeTry(expression);
}

HirExprPtr AstToHirContext::LowerCoalesceExpr(const BinaryExpr &expression) {
    const ResolvedCoalescing *fact = model.TryGetCoalescing(expression);
    assert(fact != nullptr && "accepted coalescing expression is missing its semantic fact");
    if (!fact) {
        std::abort();
    }
    return LowerNativeCoalesce(expression);
}
} // namespace Rux::AstToHirDetail
