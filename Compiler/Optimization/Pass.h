#pragma once

#include "BuildInfo/BuildProfile.h"
#include "Ir/Hir/Hir.h"
#include "Ir/Lir/Lir.h"

#include <cstddef>
#include <string_view>

namespace Rux::Optimization {
enum class PassChange {
    None,
    Changed,
};

struct PassContext {
    BuildProfile profile = BuildProfile::Debug;
    std::size_t iteration = 0;
    std::size_t fixedPointLimit = 1;
};

struct PassRunReport {
    PassChange change = PassChange::None;
    std::size_t iterations = 0;
    bool reachedFixedPoint = true;
};

template <typename Ir>
class Pass {
public:
    virtual ~Pass() = default;

    [[nodiscard]] virtual std::string_view Name() const noexcept = 0;
    virtual PassChange Run(Ir &ir, const PassContext &context) = 0;
};

using HirPass = Pass<HirPackage>;
using LirPass = Pass<LirPackage>;
} // namespace Rux::Optimization
