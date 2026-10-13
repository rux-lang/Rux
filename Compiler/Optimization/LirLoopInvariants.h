#pragma once

#include "Optimization/Pass.h"

#include <string>
#include <unordered_set>

namespace Rux::Optimization {
class LirLoopInvariants final : public LirFunctionPass {
public:
    [[nodiscard]] std::string_view Name() const noexcept override;
    [[nodiscard]] PassChange RunFunction(LirFunc &function, std::size_t moduleIndex,
                                         const PassContext &context) const override;

    [[nodiscard]] bool Enabled(const PassContext &context) const override {
        return context.iteration == 0;
    }

    void Prepare(const LirPackage &package) override;

private:
    std::unordered_set<std::string> constantNames;
};
} // namespace Rux::Optimization
