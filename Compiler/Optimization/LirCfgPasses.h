#pragma once

#include "Optimization/Pass.h"

namespace Rux::Optimization {
/**
 * @brief Checks the LIR control-flow graph and never modifies it.
 *
 * A malformed graph — an unterminated block, an edge to a block that does not exist — means an earlier stage produced
 * bad LIR, so this reports an internal error rather than repairing anything. It runs in every profile, including Debug,
 * because the configuration used to debug the compiler is exactly where that bug most needs to surface.
 */
class LirCfgVerifier final : public LirPass {
public:
    [[nodiscard]] std::string_view Name() const noexcept override;
    PassChange Run(LirPackage &package, const PassContext &context) override;
};

/// Simplifies the control-flow graph itself: drops blocks nothing branches to, and folds a branch whose condition is a
/// known constant into a jump. Runs after the passes that produce those constants, and repeats with them, since
/// removing one edge can leave the next block unreachable.
class LirCfgCleanup final : public LirPass {
public:
    [[nodiscard]] std::string_view Name() const noexcept override;
    PassChange Run(LirPackage &package, const PassContext &context) override;
};
} // namespace Rux::Optimization
