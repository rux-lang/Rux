#pragma once

#include "Optimization/Pass.h"

namespace Rux::Optimization {
/// Split private scalar-field ranges, slices, tuples and concrete structs, including closed whole-object copy chains.
/// Escapes, overlapping/unknown projections, volatile accesses and observable object representations retain storage.
class LirScalarReplacement final : public LirPass {
public:
    [[nodiscard]] std::string_view Name() const noexcept override;
    PassChange Run(LirPackage &package, const PassContext &context) override;
};
} // namespace Rux::Optimization
