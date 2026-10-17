#pragma once

#include "Optimization/Pass.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace Rux::Optimization {
/// Folds exact typed loads from immutable constant slice headers, without assuming that other memory is immutable.
class LirConstantMemory final : public LirFunctionPass {
public:
    [[nodiscard]] std::string_view Name() const noexcept override;
    [[nodiscard]] PassChange RunFunction(LirFunc &function, std::size_t moduleIndex,
                                         const PassContext &context) const override;
    void Prepare(const LirPackage &package) override;

private:
    std::vector<std::unordered_map<std::string, const LirConstDecl *>> moduleConstants;
};
} // namespace Rux::Optimization
