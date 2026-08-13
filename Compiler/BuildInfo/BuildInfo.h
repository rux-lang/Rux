#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace Rux {
// Immutable compiler identity and timestamp shared by every stage of one
// compilation. The driver composes the real values; focused embedders and
// tests may provide their own deterministic values.
class BuildInfo {
public:
    BuildInfo() = default;

    BuildInfo(std::string compilerVersion, const std::int64_t timestamp)
        : compilerVersion_(std::move(compilerVersion))
        , timestamp_(timestamp) {
    }

    [[nodiscard]] const std::string &CompilerVersion() const noexcept {
        return compilerVersion_;
    }

    [[nodiscard]] std::int64_t Timestamp() const noexcept {
        return timestamp_;
    }

private:
    std::string compilerVersion_ = "0.0.0";
    std::int64_t timestamp_ = 0;
};
} // namespace Rux
