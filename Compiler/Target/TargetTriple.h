#pragma once

#include "Target/Target.h"

#include <compare>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Rux::Target {
// A supported OS/architecture pair. Construction is restricted to validated
// parsing, supported enum pairs, or the compiler host, so Unknown can never
// enter target selection through this type.
class TargetTriple {
public:
    [[nodiscard]] static std::optional<TargetTriple> Parse(std::string_view text) noexcept;
    [[nodiscard]] static std::optional<TargetTriple> From(OS os, Arch arch) noexcept;
    [[nodiscard]] static TargetTriple Host() noexcept;

    // The stable order used by diagnostics today and by build matrices later.
    [[nodiscard]] static std::span<const TargetTriple> Supported() noexcept;

    [[nodiscard]] constexpr OS Os() const noexcept {
        return os;
    }

    [[nodiscard]] constexpr Arch Architecture() const noexcept {
        return arch;
    }

    // Machine-facing spelling, for paths, metadata, and command output.
    [[nodiscard]] std::string_view CanonicalName() const noexcept;

    // Human-facing spelling, such as "Windows x86-64".
    [[nodiscard]] std::string DisplayName() const;

    auto operator<=>(const TargetTriple &) const = default;

private:
    constexpr TargetTriple(const OS inputOs, const Arch inputArch) noexcept
        : os(inputOs)
        , arch(inputArch) {
    }

    OS os;
    Arch arch;
};

// Comma-separated canonical catalog used in CLI diagnostics.
[[nodiscard]] const std::string &SupportedTargetTripleNames();
} // namespace Rux::Target
