#pragma once

#include "BuildInfo/BuildInfo.h"
#include "BuildInfo/BuildProfile.h"
#include "Target/Target.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>

namespace Rux {
// The import name a package conventionally binds the intrinsics package to.
// Only a default: the name is a manifest's choice, not the language's.
inline constexpr std::string_view defaultIntrinsicsImportName = "Core";

// Build properties which are known before semantic analysis and may therefore
// participate in `#if` expressions or be embedded as ordinary literals.
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

    // Used to turn physical input paths into stable package-relative paths.
    std::filesystem::path sourceRoot;

    // Manifest [Build.Defines] values, overridden by command-line --define.
    std::map<std::string, std::string> config;

    // Import names under which the file being folded has the intrinsics package
    // in scope. A `when` condition may only name a build intrinsic or built-in
    // enum the file imported from that package, and a manifest is free to bind
    // it to any import name, so the spelling cannot be assumed: the driver fills
    // this in from the resolved package identity of each dependency. The default
    // covers embedders and focused tests that declare no manifest at all.
    std::set<std::string> intrinsicsAliases{std::string(defaultIntrinsicsImportName)};
};
} // namespace Rux
