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

    BuildInfo(std::string inputCompilerVersion, const std::int64_t inputTimestamp)
        : compilerVersion(std::move(inputCompilerVersion))
        , timestamp(inputTimestamp) {
    }

    [[nodiscard]] const std::string &CompilerVersion() const noexcept {
        return compilerVersion;
    }

    [[nodiscard]] std::int64_t Timestamp() const noexcept {
        return timestamp;
    }

private:
    std::string compilerVersion = "0.0.0";
    std::int64_t timestamp = 0;
};
} // namespace Rux
