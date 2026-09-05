#pragma once

#include "BuildInfo/CompilerMetadata.h"
#include "Driver/BuildTarget.h"
#include "ElfReader.h"
#include "MachOReader.h"
#include "Package/Cache.h"
#include "Package/Credentials.h"
#include "System/Os.h"
#include "System/Process.h"

#include <array>
#include <chrono>
#include <doctest.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Rux::Packages;

namespace Rux::Testing::CliProcessTestSupport {
using namespace Rux;

inline std::filesystem::path RuxExecutable() {
    return std::filesystem::path(RUX_ROOT_DIR) / "Bin" / System::ExecutableFileName("rux");
}

// A checked-in package to point option-handling checks at, so they do not
// depend on the directory the test binary happens to run from.
inline std::string ArithmeticManifest() {
    return (std::filesystem::path(RUX_ROOT_DIR) / "Tests" / "Language" / "Arithmetic" / "Rux.toml").string();
}

template <std::size_t N>
System::RunResult Run(const std::array<std::string_view, N> &arguments) {
    const auto result = System::RunCaptured(RuxExecutable(), arguments);
    REQUIRE(result.has_value());
    return *result;
}

inline std::vector<unsigned char> ReadBinaryFile(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

inline std::string ReadTextFile(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

inline void WriteTextFile(const std::filesystem::path &path, const std::string_view contents) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    REQUIRE(!error);
    std::ofstream output(path, std::ios::binary);
    output << contents;
    output.close();
    REQUIRE(output);
}

constexpr const char *packageCacheHomeVariable = Target::HostOS == Target::OS::Windows ? "LOCALAPPDATA" : "HOME";

class ScopedCliPackageCache {
public:
    ScopedCliPackageCache() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        savedHome = System::GetEnvPath(packageCacheHomeVariable);
        root = System::TempDirectory() / ("rux-cli-package-cache-test-" + std::to_string(nonce));

        std::error_code error;
        std::filesystem::create_directories(root, error);
        REQUIRE(!error);
        REQUIRE(System::SetEnvPath(packageCacheHomeVariable, root));
        REQUIRE(Packages::RegistryPackagesDir().string().starts_with(root.string()));
    }

    ScopedCliPackageCache(const ScopedCliPackageCache &) = delete;
    ScopedCliPackageCache &operator=(const ScopedCliPackageCache &) = delete;

    ~ScopedCliPackageCache() {
        if (savedHome) {
            static_cast<void>(System::SetEnvPath(packageCacheHomeVariable, *savedHome));
        }
        else {
            static_cast<void>(System::UnsetEnv(packageCacheHomeVariable));
        }
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

private:
    std::optional<std::filesystem::path> savedHome;
    std::filesystem::path root;
};

class ScopedEnvironmentValue {
public:
    explicit ScopedEnvironmentValue(const std::string_view inputName)
        : name(inputName)
        , saved(System::GetEnv(name.c_str())) {
    }

    ~ScopedEnvironmentValue() {
        if (saved) {
            static_cast<void>(System::SetEnv(name.c_str(), *saved));
        }
        else {
            static_cast<void>(System::UnsetEnv(name.c_str()));
        }
    }

private:
    std::string name;
    std::optional<std::string> saved;
};

inline std::filesystem::path WriteCachedPackage(const std::string_view packageNamespace,
                                                const std::string_view packageName, const std::string_view version,
                                                const std::string_view description) {
    const auto ns = IdentitySegment::Parse(packageNamespace);
    const auto name = IdentitySegment::Parse(packageName);
    const auto semanticVersion = SemanticVersion::Parse(version);
    REQUIRE(ns.has_value());
    REQUIRE(name.has_value());
    REQUIRE(semanticVersion.has_value());

    const auto packageDir = Packages::RegistryPackageDir(*ns, *name, *semanticVersion);
    std::error_code error;
    std::filesystem::create_directories(packageDir, error);
    REQUIRE(!error);
    std::ofstream manifest(packageDir / "Rux.toml", std::ios::binary);
    manifest << "[Manifest]\nVersion = 1\n\n[Package]\nNamespace = \"" << packageNamespace << "\"\nName = \""
             << packageName << "\"\nVersion = \"" << version << "\"\nType = \"SourceLibrary\"\nDescription = \""
             << description << "\"\n";
    manifest.close();
    REQUIRE(manifest);
    return packageDir;
}

inline uint16_t Read16(const std::vector<unsigned char> &bytes, const std::size_t offset) {
    return static_cast<uint16_t>(bytes[offset]) | static_cast<uint16_t>(bytes[offset + 1] << 8U);
}

inline uint32_t Read32(const std::vector<unsigned char> &bytes, const std::size_t offset) {
    uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= static_cast<uint32_t>(bytes[offset + i]) << (i * 8U);
    }
    return value;
}

inline std::size_t CountOccurrences(const std::string_view text, const std::string_view needle) {
    std::size_t count = 0;
    std::size_t offset = 0;
    while ((offset = text.find(needle, offset)) != std::string_view::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

inline std::string NormalizeNewlines(std::string text) {
    std::erase(text, '\r');
    return text;
}

} // namespace Rux::Testing::CliProcessTestSupport
