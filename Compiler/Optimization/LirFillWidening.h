#pragma once

#include "Optimization/Pass.h"

namespace Rux::Optimization {
/// Fills memory eight bytes at a time where a loop stores one invariant narrow value at every index.
///
/// A loop `for i in 0..limit { p[i] = v }` over one-, two- or four-byte integers, characters or Booleans stores the
/// same bytes it would store as eight-byte words holding copies of `v`. A new loop writes those words up to the last
/// whole word below the limit and hands its index to the original loop, which finishes the remainder element by
/// element. Every byte receives the value it received before, in the same order within each word, and nothing is read.
class LirFillWidening final : public LirFunctionPass {
public:
    [[nodiscard]] std::string_view Name() const noexcept override;
    [[nodiscard]] PassChange RunFunction(LirFunc &function, std::size_t moduleIndex,
                                         const PassContext &context) const override;
};
} // namespace Rux::Optimization
