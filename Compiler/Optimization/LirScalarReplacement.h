#pragma once

#include "Optimization/Pass.h"

#include <map>
#include <string>
#include <unordered_map>

namespace Rux::Optimization {
/// Split private scalar-field ranges, slices, tuples and concrete structs, including closed whole-object copy chains.
/// Escapes, overlapping/unknown projections, volatile accesses and observable object representations retain storage.
class LirScalarReplacement final : public LirFunctionPass {
public:
    [[nodiscard]] std::string_view Name() const noexcept override;
    [[nodiscard]] PassChange RunFunction(LirFunc &function, std::size_t moduleIndex,
                                         const PassContext &context) const override;
    void Prepare(const LirPackage &package) override;

private:
    std::unordered_map<std::string, std::map<std::string, TypeRef>> structs;
};
} // namespace Rux::Optimization
