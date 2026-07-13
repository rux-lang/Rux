#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

#include "Target/Target.h"

namespace Rux {

// Build properties which are known before semantic analysis and may therefore
// participate in `#if` expressions or be embedded as ordinary literals.
enum class OptimizationMode : std::uint8_t {
    None,
    Size,
    Speed,
};

enum class OutputKind : std::uint8_t {
    Executable,
    StaticLibrary,
    SharedLibrary,
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
};

} // namespace Rux
