#pragma once

#include "Optimization/Pass.h"

namespace Rux::Optimization {
/// Split private range/slice fields whose addresses are used only by matching scalar loads and stores.
class LirScalarReplacement final : public LirPass {
public:
    [[nodiscard]] std::string_view Name() const noexcept override;
    PassChange Run(LirPackage &package, const PassContext &context) override;
};
} // namespace Rux::Optimization
