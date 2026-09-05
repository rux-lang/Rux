#pragma once

#include "BuildInfo/BuildInfo.h"
#include "BuildInfo/BuildProfile.h"
#include "Target/Target.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>

namespace Rux {

/// Build properties which are known before semantic analysis and may therefore participate in `#if` expressions or be
/// embedded as ordinary literals.
enum class OptimizationMode : std::uint8_t {
    None = 0,
    Size = 1,
    Speed = 2,
};

enum class OutputKind : std::uint8_t {
    Executable = 0,
    SharedLibrary = 1,
    StaticLibrary = 2,
    SourceLibrary = 3,
};

struct CompileTimeContext {
    BuildInfo buildInfo;
    TargetContext target = TargetContext::CreateNative();
    std::string targetTriple;

    BuildProfile profile = BuildProfile::Debug;
    bool isTest = false;
    OutputKind outputKind = OutputKind::Executable;

    [[nodiscard]] constexpr std::string_view ProfileName() const noexcept {
        return ToString(profile);
    }

    [[nodiscard]] constexpr Target::BuildMode BuildMode() const noexcept {
        return profile == BuildProfile::Release ? Target::BuildMode::Release : Target::BuildMode::Debug;
    }

    [[nodiscard]] constexpr OptimizationMode Optimization() const noexcept {
        return profile == BuildProfile::Release ? OptimizationMode::Speed : OptimizationMode::None;
    }

    [[nodiscard]] constexpr bool DebugAssertions() const noexcept {
        return profile == BuildProfile::Debug;
    }

    [[nodiscard]] constexpr bool DebugInfo() const noexcept {
        return profile == BuildProfile::Debug;
    }

    /// Used to turn physical input paths into stable package-relative paths.
    std::filesystem::path sourceRoot;

    /// Manifest [Build.Defines] values, overridden by command-line --define.
    std::map<std::string, std::string> config;
};
} // namespace Rux
