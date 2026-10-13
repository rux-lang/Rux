#pragma once

#include "Optimization/Pass.h"

namespace Rux::Optimization {
/// Removes loops that do nothing observable and always finish.
///
/// A loop qualifies when it writes no memory, calls nothing, cannot trap, produces no value read after it, and leaves
/// only for one block without phis, and when an unsigned count stepping by one from below an invariant limit is tested
/// on every iteration, so it ends. Running it or not leaves the program in the same state; its entry edges go
/// straight to its exit instead. Such loops remain where inlining drops elements that need no cleanup: the loop that
/// visits every element of an array of plain values reads each one and discards it.
class LirDeadLoops final : public LirFunctionPass {
public:
    [[nodiscard]] std::string_view Name() const noexcept override;
    [[nodiscard]] PassChange RunFunction(LirFunc &function, std::size_t moduleIndex,
                                         const PassContext &context) const override;
};
} // namespace Rux::Optimization
