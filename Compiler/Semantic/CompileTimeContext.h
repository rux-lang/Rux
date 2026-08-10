#pragma once

#include "Target/Target.h"

#include <chrono>
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
    TargetContext target = TargetContext::CreateNative();
    std::string targetTriple;

    std::string profileName = "Debug";
    Target::BuildMode buildMode = Target::BuildMode::Debug;
    OptimizationMode optimization = OptimizationMode::None;
    bool debugAssertions = true;
    bool debugInfo = true;
    bool isTest = false;
    OutputKind outputKind = OutputKind::Executable;

    // One instant for the whole compilation. The driver initializes this from
    // SOURCE_DATE_EPOCH when present, otherwise from the system clock.
    std::int64_t buildTimestamp =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

    std::string compilerVersion;

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
