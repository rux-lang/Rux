#pragma once

#include "BuildInfo/BuildProfile.h"
#include "Diagnostics/Diagnostics.h"
#include "Ir/Hir/Hir.h"
#include "Ir/Lir/Lir.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Rux::Optimization {
enum class PassChange {
    None,
    Changed,
};

struct PassContext {
    BuildProfile profile = BuildProfile::Debug;
    std::size_t iteration = 0;
    std::size_t fixedPointLimit = 1;
    std::vector<Diagnostic> *diagnostics = nullptr;

    void ReportInternalError(std::string message) const {
        if (diagnostics != nullptr) {
            diagnostics->push_back(ErrorDiagnostic(std::move(message)));
        }
    }
};

struct PassRunReport {
    PassChange change = PassChange::None;
    std::size_t iterations = 0;
    bool reachedFixedPoint = true;
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool HasErrors() const noexcept {
        for (const auto &diagnostic : diagnostics) {
            if (diagnostic.IsError()) {
                return true;
            }
        }
        return false;
    }
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
