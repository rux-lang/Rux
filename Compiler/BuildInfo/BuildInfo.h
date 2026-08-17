#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace Rux {
/// Immutable compiler identity and timestamp shared by every stage of one compilation. The driver composes the real
/// values; focused embedders and tests may provide their own deterministic values.
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

    /// Seconds since the Unix epoch. Read by the compile-time `Build` values a source file can query and stamped into
    /// RCU object metadata, so it is a build input rather than a log line: the driver honours `SOURCE_DATE_EPOCH` to
    /// keep a rebuild byte-identical.
    [[nodiscard]] std::int64_t Timestamp() const noexcept {
        return timestamp;
    }

private:
    std::string compilerVersion = "0.0.0";
    std::int64_t timestamp = 0;
};
} // namespace Rux
