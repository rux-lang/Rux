#pragma once

#include <chrono>
#include <cstddef>
#include <string>

namespace Rux::Optimization {
/// Optional measurements. Instruction counts cover LIR only; timing excludes the counting work itself.
struct PassStatistics {
    std::string ir;
    std::string name;
    std::size_t runs = 0;
    std::size_t changes = 0;
    std::chrono::microseconds elapsed{0};
    std::size_t instructionsAdded = 0;
    std::size_t instructionsRemoved = 0;
    bool disabled = false;
};
} // namespace Rux::Optimization
