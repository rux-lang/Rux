#include "BuildInfo/CompilerMetadata.h"
#include "Driver/BuildTarget.h"
#include "Driver/Credentials.h"
#include "ElfReader.h"
#include "MachOReader.h"
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

using namespace Rux;

namespace {
std::filesystem::path RuxExecutable() {
    return std::filesystem::path(RUX_ROOT_DIR) / "Bin" / System::ExecutableFileName("rux");
}

// A checked-in package to point option-handling checks at, so they do not
// depend on the directory the test binary happens to run from.
std::string ArithmeticManifest() {
    return (std::filesystem::path(RUX_ROOT_DIR) / "Tests" / "Language" / "Arithmetic" / "Rux.toml").string();
}

template <std::size_t N>
System::RunResult Run(const std::array<std::string_view, N> &arguments) {
    const auto result = System::RunCaptured(RuxExecutable(), arguments);
    REQUIRE(result.has_value());
    return *result;
}

std::vector<unsigned char> ReadBinaryFile(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string ReadTextFile(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void WriteTextFile(const std::filesystem::path &path, const std::string_view contents) {
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
        REQUIRE(Driver::RegistryPackagesDir().string().starts_with(root.string()));
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

std::filesystem::path WriteCachedPackage(const std::string_view packageNamespace, const std::string_view packageName,
                                         const std::string_view version, const std::string_view description) {
    const auto ns = IdentitySegment::Parse(packageNamespace);
    const auto name = IdentitySegment::Parse(packageName);
    const auto semanticVersion = SemanticVersion::Parse(version);
    REQUIRE(ns.has_value());
    REQUIRE(name.has_value());
    REQUIRE(semanticVersion.has_value());

    const auto packageDir = Driver::RegistryPackageDir(*ns, *name, *semanticVersion);
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

uint16_t Read16(const std::vector<unsigned char> &bytes, const std::size_t offset) {
    return static_cast<uint16_t>(bytes[offset]) | static_cast<uint16_t>(bytes[offset + 1] << 8U);
}

uint32_t Read32(const std::vector<unsigned char> &bytes, const std::size_t offset) {
    uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= static_cast<uint32_t>(bytes[offset + i]) << (i * 8U);
    }
    return value;
}

std::size_t CountOccurrences(const std::string_view text, const std::string_view needle) {
    std::size_t count = 0;
    std::size_t offset = 0;
    while ((offset = text.find(needle, offset)) != std::string_view::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

std::string NormalizeNewlines(std::string text) {
    std::erase(text, '\r');
    return text;
}

} // namespace

TEST_CASE("help JSON publishes the stable 0.4 command contract") {
    const auto result = Run(std::array<std::string_view, 2>{"help", "--json"});
    CHECK(result.exitCode == 0);
    CHECK(result.output.contains("\"schemaVersion\":1"));
    CHECK(result.output.contains("\"version\":\"" + std::string(CompilerBuild::compilerVersion) + "\""));
    constexpr std::array commands = {"add",     "build",   "check", "clean", "doc",       "fmt",    "help",   "info",
                                     "init",    "install", "lint",  "list",  "login",     "logout", "new",    "pack",
                                     "publish", "remove",  "run",   "test",  "uninstall", "update", "version"};
    for (const std::string_view command : commands) {
        CHECK(result.output.contains("\"name\":\"" + std::string(command) + "\""));
    }
    CHECK_FALSE(result.output.contains("buildDate"));
    CHECK_FALSE(result.output.contains("buildTime"));
}

TEST_CASE("login rejects empty stdin and an empty update is timed and quiet-aware") {
    const auto login = Run(std::array<std::string_view, 3>{"login", "--registry", "http://127.0.0.1:1"});
    CHECK(login.exitCode == 1);
    CHECK(login.output.contains("error: no token was supplied on stdin"));
    CHECK_FALSE(login.output.contains("Token for"));

    const auto updated = Run(std::array<std::string_view, 3>{"--manifest", ArithmeticManifest(), "update"});
    CHECK(updated.exitCode == 0);
    CHECK(updated.output.contains("Up-to-date project dependencies in "));
    CHECK(updated.output.contains("(0 packages)"));
    CHECK(Run(std::array<std::string_view, 4>{"--quiet", "--manifest", ArithmeticManifest(), "update"}).output.empty());
}

TEST_CASE("CLI usage failures return 2 and suggest close matches") {
    const auto unknown = Run(std::array<std::string_view, 1>{"bild"});
    CHECK(unknown.exitCode == 2);
    CHECK(NormalizeNewlines(unknown.output) == "error: unknown command 'bild'\n"
                                               "  help: try 'rux build'\n"
                                               "  help: run 'rux help' for usage information\n");

    CHECK(Run(std::array<std::string_view, 2>{"build", "--target"}).exitCode == 2);
    CHECK(Run(std::array<std::string_view, 2>{"build", "--color=sometimes"}).exitCode == 2);
    CHECK(Run(std::array<std::string_view, 3>{"build", "--quiet", "--verbose"}).exitCode == 2);
    const auto profileConflict = Run(std::array<std::string_view, 3>{"build", "--debug", "--release"});
    CHECK(profileConflict.exitCode == 2);
    CHECK(profileConflict.output.contains("options '--debug' and '--release' cannot be used together"));
    CHECK(Run(std::array<std::string_view, 2>{"fmt", "extra"}).exitCode == 2);
}

TEST_CASE("CLI usage failures identify rejected input and give precise help") {
    const auto unknownCommand = Run(std::array<std::string_view, 1>{"frobnicate"});
    CHECK(unknownCommand.exitCode == 2);
    CHECK(NormalizeNewlines(unknownCommand.output) == "error: unknown command 'frobnicate'\n"
                                                      "  help: run 'rux help' for usage information\n");

    const auto unknownOption = Run(std::array<std::string_view, 2>{"build", "--targt"});
    CHECK(unknownOption.exitCode == 2);
    CHECK(NormalizeNewlines(unknownOption.output) == "error: unknown option '--targt' for command 'build'\n"
                                                     "  help: try 'rux build --target linux-x86_64'\n"
                                                     "  help: run 'rux help build' for usage information\n");

    const auto unrelatedOption = Run(std::array<std::string_view, 2>{"build", "--banana"});
    CHECK(unrelatedOption.exitCode == 2);
    CHECK(NormalizeNewlines(unrelatedOption.output) == "error: unknown option '--banana' for command 'build'\n"
                                                       "  help: run 'rux help build' for usage information\n");

    const auto globalOption = Run(std::array<std::string_view, 1>{"--colo"});
    CHECK(globalOption.exitCode == 2);
    CHECK(NormalizeNewlines(globalOption.output) == "error: unknown option '--colo'\n"
                                                    "  help: try 'rux --color auto'\n"
                                                    "  help: run 'rux help' for usage information\n");
}

TEST_CASE("CLI usage failures explain values operands conflicts and repetition") {
    const auto missingValue = Run(std::array<std::string_view, 2>{"build", "--target"});
    CHECK(missingValue.exitCode == 2);
    CHECK(NormalizeNewlines(missingValue.output) == "error: option '--target' requires a target triple\n"
                                                    "  help: try 'rux build --target linux-x86_64'\n"
                                                    "  help: run 'rux help build' for usage information\n");

    const auto invalidValue = Run(std::array<std::string_view, 1>{"--color=sometimes"});
    CHECK(invalidValue.exitCode == 2);
    CHECK(NormalizeNewlines(invalidValue.output) == "error: value 'sometimes' is not valid for option '--color'\n"
                                                    "  note: accepted color modes are 'auto', 'always', and 'never'\n"
                                                    "  help: try 'rux --color auto'\n"
                                                    "  help: run 'rux help' for usage information\n");

    const auto unexpectedValue = Run(std::array<std::string_view, 1>{"--quiet=true"});
    CHECK(unexpectedValue.exitCode == 2);
    CHECK(NormalizeNewlines(unexpectedValue.output) == "error: option '--quiet' does not accept a value\n"
                                                       "  help: try 'rux --quiet'\n"
                                                       "  help: run 'rux help' for usage information\n");

    const auto missingOperand = Run(std::array<std::string_view, 1>{"new"});
    CHECK(missingOperand.exitCode == 2);
    CHECK(NormalizeNewlines(missingOperand.output) == "error: command 'new' requires a name argument\n"
                                                      "  help: try 'rux new App'\n"
                                                      "  help: run 'rux help new' for usage information\n");

    const auto extraOperand = Run(std::array<std::string_view, 2>{"fmt", "extra"});
    CHECK(extraOperand.exitCode == 2);
    CHECK(NormalizeNewlines(extraOperand.output) == "error: argument 'extra' is not accepted by command 'fmt'\n"
                                                    "  help: run 'rux help fmt' for usage information\n");

    const auto conflict = Run(std::array<std::string_view, 3>{"build", "--debug", "--release"});
    CHECK(conflict.exitCode == 2);
    CHECK(NormalizeNewlines(conflict.output) == "error: options '--debug' and '--release' cannot be used together\n"
                                                "  help: use either '--debug' or '--release', but not both\n"
                                                "  help: run 'rux help build' for usage information\n");

    const auto repeated = Run(std::array<std::string_view, 3>{"build", "--release", "--release"});
    CHECK(repeated.exitCode == 2);
    CHECK(NormalizeNewlines(repeated.output) == "error: option '--release' was specified more than once\n"
                                                "  help: remove the repeated '--release' option\n"
                                                "  help: run 'rux help build' for usage information\n");
}

TEST_CASE("build --all conflicts are usage errors before manifest loading") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto missing =
        (System::TempDirectory() / ("rux-build-matrix-missing-" + std::to_string(nonce)) / "Rux.toml").string();

    const auto target =
        Run(std::array<std::string_view, 6>{"--manifest", missing, "build", "--all", "--target", "linux-aarch64"});
    CHECK(target.exitCode == 2);
    CHECK(target.output.contains("options '--all' and '--target' cannot be used together"));
    CHECK_FALSE(target.output.contains("specified manifest"));

    const auto debug = Run(std::array<std::string_view, 5>{"--manifest", missing, "build", "--all", "--debug"});
    CHECK(debug.exitCode == 2);
    CHECK(debug.output.contains("options '--all' and '--debug' cannot be used together"));
    CHECK_FALSE(debug.output.contains("specified manifest"));

    const auto release = Run(std::array<std::string_view, 5>{"--manifest", missing, "build", "--all", "--release"});
    CHECK(release.exitCode == 2);
    CHECK(release.output.contains("options '--all' and '--release' cannot be used together"));
    CHECK_FALSE(release.output.contains("specified manifest"));

    const auto emit = Run(std::array<std::string_view, 6>{"--manifest", missing, "build", "--all", "--emit", "lir"});
    CHECK(emit.exitCode == 2);
    CHECK(emit.output.contains("options '--all' and '--emit' cannot be used together"));
    CHECK_FALSE(emit.output.contains("specified manifest"));
}

TEST_CASE("add and remove preserve dependency mutation contracts") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = System::TempDirectory() / ("rux-dependency-command-test-" + std::to_string(nonce));
    const auto manifestPath = root / "Rux.toml";
    std::filesystem::create_directories(root);
    std::ofstream manifestFile(manifestPath, std::ios::binary);
    manifestFile << R"([Manifest]
Version = 1

[Package]
Name = "DependencyCommandTest"
Version = "0.1.0"
Type = "SourceLibrary"

[Dependencies]
Alias = { Namespace = "Rux", Package = "Io", Version = "*" }

[Build]
Output = "Artifacts"
)";
    manifestFile.close();
    REQUIRE(manifestFile);

    const auto manifest = manifestPath.string();
    const auto added = Run(std::array<std::string_view, 6>{"--manifest", manifest, "add", "Json", "--path", "../Json"});
    REQUIRE(added.exitCode == 0);
    CHECK(added.output.contains("Added path dependency 'Json' in "));
    CHECK(added.output.contains("Path: ../Json"));
    CHECK(added.output.contains("Manifest: "));

    const std::string afterAdd = ReadTextFile(manifestPath);
    CHECK(afterAdd == R"([Manifest]
Version = 1

[Package]
Name = "DependencyCommandTest"
Version = "0.1.0"
Type = "SourceLibrary"

[Dependencies]
Alias = { Namespace = "Rux", Package = "Io", Version = "*" }
Json = { Path = "../Json" }

[Build]
Output = "Artifacts"
)");

    const auto duplicate =
        Run(std::array<std::string_view, 6>{"--manifest", manifest, "add", "Json", "--path", "../Json"});
    REQUIRE(duplicate.exitCode == 0);
    CHECK(duplicate.output.contains("Unchanged path dependency 'Json' in "));
    CHECK(ReadTextFile(manifestPath) == afterAdd);

    const auto quiet =
        Run(std::array<std::string_view, 7>{"--quiet", "--manifest", manifest, "add", "Json", "--path", "../Json"});
    REQUIRE(quiet.exitCode == 0);
    CHECK(quiet.output.empty());
    CHECK(ReadTextFile(manifestPath) == afterAdd);

    // Import-name matching is normalized, including when the manifest entry
    // aliases a differently named registry package.
    const auto removed = Run(std::array<std::string_view, 4>{"--manifest", manifest, "remove", "alias"});
    REQUIRE(removed.exitCode == 0);
    CHECK(removed.output.contains("Removed dependency 'alias' in "));
    const std::string afterRemove = ReadTextFile(manifestPath);
    CHECK_FALSE(afterRemove.contains("Alias ="));
    CHECK(afterRemove.contains("Json = { Path = \"../Json\" }"));

    const auto missing = Run(std::array<std::string_view, 4>{"--manifest", manifest, "remove", "Alias"});
    CHECK(missing.exitCode == 1);
    CHECK(missing.output.contains("dependency 'Alias' was not found"));
    CHECK(missing.output.contains("help: run 'rux list'"));
    CHECK(ReadTextFile(manifestPath) == afterRemove);

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

TEST_CASE("add and remove diagnostics do not mutate the manifest") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = System::TempDirectory() / ("rux-dependency-diagnostic-test-" + std::to_string(nonce));
    const auto manifestPath = root / "Rux.toml";
    std::filesystem::create_directories(root);
    std::ofstream manifestFile(manifestPath, std::ios::binary);
    manifestFile << R"([Manifest]
Version = 1

[Package]
Name = "DependencyDiagnosticTest"
Version = "0.1.0"
Type = "SourceLibrary"
)";
    manifestFile.close();
    REQUIRE(manifestFile);

    const auto manifest = manifestPath.string();
    const std::string original = ReadTextFile(manifestPath);

    const auto unqualified = Run(std::array<std::string_view, 4>{"--manifest", manifest, "add", "Json"});
    CHECK(unqualified.exitCode == 1);
    CHECK(unqualified.output.contains("registry dependency 'Json' must include a namespace"));
    CHECK(unqualified.output.contains("help: use 'rux add Namespace/Json'"));
    CHECK(ReadTextFile(manifestPath) == original);

    const auto qualifiedPath =
        Run(std::array<std::string_view, 6>{"--manifest", manifest, "add", "Rux/Json", "--path", "../Json"});
    CHECK(qualifiedPath.exitCode == 1);
    CHECK(qualifiedPath.output.contains("path dependency 'Rux/Json' cannot include a registry namespace"));
    CHECK(qualifiedPath.output.contains("help: use 'rux add Json --path ../Json'"));
    CHECK(ReadTextFile(manifestPath) == original);

    const auto versionedPath =
        Run(std::array<std::string_view, 6>{"--manifest", manifest, "add", "Json@1.0.0", "--path", "../Json"});
    CHECK(versionedPath.exitCode == 1);
    CHECK(versionedPath.output.contains("path dependency 'Json@1.0.0' cannot include a version requirement"));
    CHECK(versionedPath.output.contains("help: use 'rux add Json --path ../Json'"));
    CHECK(ReadTextFile(manifestPath) == original);

    const auto sourceConflict = Run(std::array<std::string_view, 8>{"--manifest", manifest, "add", "Json", "--path",
                                                                    "../Json", "--registry", "http://localhost"});
    CHECK(sourceConflict.exitCode == 2);
    CHECK(sourceConflict.output.contains("options '--path' and '--registry' cannot be used together"));
    CHECK_FALSE(sourceConflict.output.contains(" ms"));
    CHECK(ReadTextFile(manifestPath) == original);

    const auto invalidName = Run(std::array<std::string_view, 4>{"--manifest", manifest, "remove", "Rux/Json"});
    CHECK(invalidName.exitCode == 1);
    CHECK(invalidName.output.contains("dependency name 'Rux/Json' is invalid"));
    CHECK(invalidName.output.contains("help: pass the dependency's unqualified import name"));
    CHECK(ReadTextFile(manifestPath) == original);

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

TEST_CASE("list and info label the global cache and selected installed version") {
    const ScopedCliPackageCache cache;
    WriteCachedPackage("rux", "my-pkg", "1.0.0", "first release");
    WriteCachedPackage("rux", "my-pkg", "2.0.0", "second release");

    const auto listed = Run(std::array<std::string_view, 2>{"list", "--global"});
    REQUIRE(listed.exitCode == 0);
    CHECK(listed.output.contains("Global package cache (2 versions):"));
    CHECK(listed.output.contains("Cache: '"));
    CHECK(listed.output.contains("Installed rux/my-pkg 1.0.0"));
    CHECK(listed.output.contains("Installed rux/my-pkg 2.0.0"));

    // The requested spelling normalizes to the spelling on disk, while the
    // explicit version still selects that exact cached manifest.
    const auto json = Run(std::array<std::string_view, 3>{"info", "Rux/My_Pkg@1.0.0", "--json"});
    REQUIRE(json.exitCode == 0);
    CHECK(json.output.contains("\"success\": true"));
    CHECK(json.output.contains("\"namespace\": \"rux\""));
    CHECK(json.output.contains("\"name\": \"my-pkg\""));
    CHECK(json.output.contains("\"version\": \"1.0.0\""));
    CHECK(json.output.contains("\"description\": \"first release\""));
    CHECK_FALSE(json.output.contains("second release"));
    CHECK_FALSE(json.output.contains("Installed package"));
    CHECK_FALSE(json.output.contains("Cache:"));

    const auto text = Run(std::array<std::string_view, 2>{"info", "RUX/MY-PKG@2.0.0"});
    REQUIRE(text.exitCode == 0);
    CHECK(text.output.contains("Installed package 'RUX/MY-PKG' selected by requirement '2.0.0':"));
    CHECK(text.output.contains("Cache: '"));
    CHECK(text.output.contains("Namespace:   rux"));
    CHECK(text.output.contains("Name:        my-pkg"));
    CHECK(text.output.contains("Version:     2.0.0"));
    CHECK(text.output.contains("Description: second release"));
    CHECK(text.output.contains("Package dependencies (0 dependencies):"));
    CHECK(text.output.contains("None"));
}

TEST_CASE("uninstall reports selected, project, and global cache removal results") {
    const ScopedCliPackageCache cache;
    const auto first = WriteCachedPackage("rux", "my-pkg", "1.0.0", "first release");
    const auto second = WriteCachedPackage("rux", "my-pkg", "2.0.0", "second release");
    const auto unrelated = WriteCachedPackage("rux", "other", "1.0.0", "other package");
    const auto stray = first.parent_path() / "not-an-installed-version";
    std::filesystem::create_directories(stray);

    const auto removed = Run(std::array<std::string_view, 2>{"uninstall", "Rux/My_Pkg@1.0.0"});
    REQUIRE(removed.exitCode == 0);
    CHECK(removed.output.contains("Removed Rux/My_Pkg 1.0.0"));
    CHECK(removed.output.contains("Removed 1 package version in "));
    CHECK(removed.output.contains("Cache: '"));
    CHECK_FALSE(std::filesystem::exists(first));
    CHECK(std::filesystem::exists(second));
    CHECK(std::filesystem::exists(unrelated));
    CHECK(std::filesystem::exists(stray));

    const auto missing = Run(std::array<std::string_view, 2>{"uninstall", "rux/my-pkg@9.0.0"});
    CHECK(missing.exitCode == 1);
    CHECK(missing.output.contains("no installed version of 'rux/my-pkg' matches '9.0.0'"));
    CHECK(missing.output.contains("global package cache: '"));
    CHECK(missing.output.contains("run 'rux install rux/my-pkg@9.0.0'"));
    CHECK(std::filesystem::exists(second));
    CHECK(std::filesystem::exists(unrelated));
    CHECK(std::filesystem::exists(stray));

    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = System::TempDirectory() / ("rux-cli-project-uninstall-" + std::to_string(nonce));
    const auto manifestPath = root / "Rux.toml";
    WriteTextFile(manifestPath, R"([Manifest]
Version = 1

[Package]
Name = "UninstallFixture"
Version = "0.1.0"
Type = "SourceLibrary"

[Dependencies]
My_Pkg = { Namespace = "Rux", Version = "^2.0.0" }
Missing = { Namespace = "Acme", Version = "1.0.0" }
)");
    const auto manifest = manifestPath.string();
    const auto project = Run(std::array<std::string_view, 3>{"--manifest", manifest, "uninstall"});
    REQUIRE(project.exitCode == 0);
    CHECK(project.output.contains("Removed Rux/My_Pkg 2.0.0"));
    CHECK(project.output.contains("Missing Acme/Missing"));
    CHECK(project.output.contains("help: install it with 'rux install Acme/Missing@1.0.0'"));
    CHECK(project.output.contains("Finished project uninstall in "));
    CHECK(project.output.contains("(1 removed, 1 missing)"));
    CHECK(project.output.contains("Manifest: '" + manifest + "'"));
    CHECK_FALSE(std::filesystem::exists(second));

    const auto global = Run(std::array<std::string_view, 2>{"uninstall", "--global"});
    REQUIRE(global.exitCode == 0);
    CHECK(global.output.contains("Removed 1 package version from the global package cache in "));
    CHECK(global.output.contains("Cache: '"));
    CHECK_FALSE(std::filesystem::exists(unrelated));
    CHECK(std::filesystem::exists(stray));

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

TEST_CASE("build help publishes the all-target matrix option") {
    const auto textHelp = Run(std::array<std::string_view, 2>{"help", "build"});
    const auto jsonHelp = Run(std::array<std::string_view, 3>{"help", "build", "--json"});

    CHECK(textHelp.exitCode == 0);
    CHECK(textHelp.output.contains("--all"));
    CHECK(textHelp.output.contains("all 16 target/profile cells"));
    CHECK(textHelp.output.contains("--release --target linux-x86_64"));
    CHECK(jsonHelp.exitCode == 0);
    CHECK(jsonHelp.output.contains("\"flags\":\"--all\""));
    CHECK(jsonHelp.output.contains("all 16 target/profile cells"));
    CHECK(jsonHelp.output.contains("--release --target linux-x86_64"));
}

TEST_CASE("build --all reports every cell, continues after failures, and honors quiet") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = System::TempDirectory() / ("rux-build-matrix-" + std::to_string(nonce));
    const auto manifestPath = root / "Rux.toml";
    const auto sourcePath = root / "Src" / "Main.rux";
    std::filesystem::create_directories(sourcePath.parent_path());
    std::ofstream manifestFile(manifestPath, std::ios::binary);
    manifestFile << R"([Manifest]
Version = 1

[Package]
Name = "MatrixFixture"
Version = "0.1.0"
Type = "Executable"

[Build]
Output = "Artifacts"
)";
    manifestFile.close();
    std::ofstream source(sourcePath, std::ios::binary);
    source << R"(asm func Triple(x: int64) -> int64 {
    mov rax, rdi
    imul rax, 3
    ret
}

func Main() -> int {
    return 0;
}
)";
    source.close();
    REQUIRE(manifestFile);
    REQUIRE(source);

    const auto manifest = manifestPath.string();
    const auto reported = Run(std::array<std::string_view, 8>{"--manifest", manifest, "--color=never", "build", "--all",
                                                              "--stats", "--define", "Matrix=enabled"});

    CHECK(reported.exitCode == 1);
    CHECK(CountOccurrences(reported.output, "'imul' is an x86-64 instruction") == 8);
    CHECK(reported.output.contains("Build matrix"));
    CHECK(reported.output.contains("Status  Profile  Target              Time"));
    CHECK(reported.output.contains("Built   Debug    FreeBSD x86-64"));
    CHECK(reported.output.contains("Failed  Debug    FreeBSD AArch64"));
    CHECK(reported.output.contains("Built   Release  Windows x86-64"));
    CHECK(reported.output.contains("Failed  Release  Windows AArch64"));
    CHECK(reported.output.contains("Failed 16 cells in "));
    CHECK(reported.output.contains("(8 succeeded, 8 failed)"));
    CHECK(reported.output.contains("Aggregate statistics:"));
    CHECK_FALSE(reported.output.contains("\033["));
    // Artifact paths are reported relative to the package root.
    CHECK(reported.output.contains(
        (std::filesystem::path("Artifacts") / "Debug" / "FreeBSD" / "x86-64" / "MatrixFixture").string()));

    for (const std::string_view profile : {"Debug", "Release"}) {
        for (const std::string_view os : {"FreeBSD", "Linux", "macOS", "Windows"}) {
            const auto fileName = os == "Windows" ? "MatrixFixture.exe" : "MatrixFixture";
            CHECK(std::filesystem::is_regular_file(root / "Artifacts" / profile / os / "x86-64" / fileName));
        }
    }

    std::ofstream invalidSource(sourcePath, std::ios::binary | std::ios::trunc);
    invalidSource << "func Main() -> int { return Missing; }\n";
    invalidSource.close();
    REQUIRE(invalidSource);
    const auto quiet = Run(std::array<std::string_view, 5>{"--manifest", manifest, "build", "--all", "--quiet"});
    CHECK(quiet.exitCode == 1);
    CHECK(CountOccurrences(quiet.output, "name 'Missing' is not defined in this scope") == 16);
    CHECK_FALSE(quiet.output.contains("Build matrix"));
    CHECK_FALSE(quiet.output.contains("Aggregate statistics:"));
    CHECK_FALSE(quiet.output.contains("Compiling"));

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

TEST_CASE("build --stats reports each section once and stays silent when quiet") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = System::TempDirectory() / ("rux-build-stats-" + std::to_string(nonce));
    const auto manifestPath = root / "Rux.toml";
    const auto sourcePath = root / "Src" / "Main.rux";
    std::filesystem::create_directories(sourcePath.parent_path());
    std::ofstream manifestFile(manifestPath, std::ios::binary);
    manifestFile << R"([Manifest]
Version = 1

[Package]
Name = "StatsFixture"
Version = "0.2.0"
Type = "Executable"
)";
    manifestFile.close();
    std::ofstream source(sourcePath, std::ios::binary);
    source << "func Main() -> int { return 0; }\n";
    source.close();
    REQUIRE(manifestFile);
    REQUIRE(source);

    const auto manifest = manifestPath.string();
    const auto reported =
        Run(std::array<std::string_view, 5>{"--manifest", manifest, "--color=never", "build", "--stats"});

    CAPTURE(reported.output);
    CHECK(reported.exitCode == 0);
    // The report opens with the same progress and status lines a plain build
    // prints: --stats adds sections, it does not replace the summary.
    const auto host = Driver::TargetDisplayName(Rux::Target::TargetTriple::Host());
    CHECK(reported.output.contains("Compiling StatsFixture v0.2.0 (Debug, " + host + ")"));
    CHECK(reported.output.contains("Built StatsFixture (Debug, " + host + ") in "));
    CHECK(reported.output.contains("  Package:  StatsFixture v0.2.0"));
    CHECK(reported.output.contains("Time:"));
    CHECK(reported.output.contains("Source:"));
    CHECK(reported.output.contains("Optimization:"));
    CHECK(reported.output.contains("Performance:"));
    // Machine-facing IDs stay out of report prose.
    CHECK_FALSE(reported.output.contains(Driver::HostTargetTriple()));

    const auto quiet =
        Run(std::array<std::string_view, 6>{"--manifest", manifest, "--color=never", "build", "--stats", "--quiet"});
    CHECK(quiet.exitCode == 0);
    CHECK(quiet.output.empty());

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

TEST_CASE("clean removes only the configured output root and Temp tree") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = System::TempDirectory() / ("rux-clean-test-" + std::to_string(nonce));
    const auto manifestPath = root / "Rux.toml";
    std::filesystem::create_directories(root / "Artifacts" / "Release" /
                                        Driver::TargetOutputPath(Target::TargetTriple::Host()));
    std::filesystem::create_directories(root / "Temp" / "Obj");
    std::filesystem::create_directories(root / "Keep");
    std::ofstream manifestFile(manifestPath, std::ios::binary);
    manifestFile << R"([Manifest]
Version = 1

[Package]
Name = "CleanTest"
Version = "0.1.0"
Type = "Executable"

[Build]
Output = "Artifacts"
)";
    manifestFile.close();
    std::ofstream sentinel(root / "Keep" / "sentinel", std::ios::binary);
    sentinel << "keep";
    sentinel.close();
    REQUIRE(manifestFile);
    REQUIRE(sentinel);

    const auto manifest = manifestPath.string();
    const auto tempOnly =
        Run(std::array<std::string_view, 5>{"--manifest", manifest, "--color=never", "clean", "--temp"});
    CHECK(tempOnly.exitCode == 0);
    CHECK(tempOnly.output.contains("Removed 1 directory in "));
    CHECK(tempOnly.output.contains("Path: Temp"));
    CHECK(std::filesystem::exists(root / "Artifacts"));
    CHECK_FALSE(std::filesystem::exists(root / "Temp"));

    const auto result = Run(std::array<std::string_view, 4>{"--manifest", manifest, "clean", "--quiet"});

    CHECK(result.exitCode == 0);
    CHECK(result.output.empty());
    CHECK_FALSE(std::filesystem::exists(root / "Artifacts"));
    CHECK_FALSE(std::filesystem::exists(root / "Temp"));
    CHECK(std::filesystem::is_regular_file(root / "Keep" / "sentinel"));
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

TEST_CASE("documentation and source archives use the configured raw output root") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = System::TempDirectory() / ("rux-raw-output-test-" + std::to_string(nonce));
    const auto manifestPath = root / "Rux.toml";
    std::filesystem::create_directories(root / "Src");
    std::ofstream manifestFile(manifestPath, std::ios::binary);
    manifestFile << R"([Manifest]
Version = 1
MinRux = "0.4.0"

[Package]
Namespace = "Acme"
Name = "Widget"
Version = "1.2.3"
Type = "SourceLibrary"

[Build]
Output = "Artifacts"
)";
    manifestFile.close();
    std::ofstream source(root / "Src" / "Widget.rux", std::ios::binary);
    source << "module Widget {\n    pub func Answer() -> int { return 42; }\n}\n";
    source.close();
    REQUIRE(manifestFile);
    REQUIRE(source);

    const auto manifest = manifestPath.string();
    const auto pack = Run(std::array<std::string_view, 4>{"--manifest", manifest, "--quiet", "pack"});
    const auto doc = Run(std::array<std::string_view, 4>{"--manifest", manifest, "--quiet", "doc"});

    CHECK(pack.exitCode == 0);
    CHECK(doc.exitCode == 0);
    CHECK(std::filesystem::is_regular_file(root / "Artifacts" / "Widget-1.2.3.ruxpkg"));
    CHECK(std::filesystem::is_regular_file(root / "Artifacts" / "Docs" / "index.html"));
    CHECK_FALSE(std::filesystem::exists(root / "Artifacts" / "Debug"));
    CHECK_FALSE(std::filesystem::exists(root / "Artifacts" / "Release"));
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

TEST_CASE("pack and publish tell one publication story and keep failures actionable") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = System::TempDirectory() / ("rux-publication-report-test-" + std::to_string(nonce));
    const auto manifestPath = root / "Package" / "Rux.toml";
    WriteTextFile(manifestPath, R"([Manifest]
Version = 1
MinRux = "0.4.0"

[Package]
Namespace = "Acme"
Name = "Widget"
Version = "1.2.3"
Type = "SourceLibrary"
)");
    WriteTextFile(root / "Package" / "Src" / "Widget.rux", "module Widget {}\n");
    const std::string manifest = manifestPath.string();

    const auto archive = root / "packed" / "Widget.ruxpkg";
    const std::string archiveText = archive.string();
    const auto packed = Run(std::array<std::string_view, 7>{"--manifest", manifest, "--color=never", "pack", "--output",
                                                            archiveText, "--verbose"});
    CAPTURE(packed.output);
    CHECK(packed.exitCode == 0);
    CHECK(packed.output.contains("Packing Acme/Widget 1.2.3"));
    CHECK(packed.output.contains("Packed Acme/Widget 1.2.3 in "));
    CHECK(packed.output.contains("Files: 2 ("));
    CHECK(packed.output.contains("Output: '" + archiveText + "'"));
    CHECK(std::filesystem::is_regular_file(archive));

    const auto dryRun = Run(
        std::array<std::string_view, 6>{"--manifest", manifest, "--color=never", "publish", "--dry-run", "--verbose"});
    CAPTURE(dryRun.output);
    CHECK(dryRun.exitCode == 0);
    CHECK(dryRun.output.contains("Packed Acme/Widget 1.2.3 (2 files, "));
    CHECK(dryRun.output.contains("Validated Acme/Widget 1.2.3 for publication in "));
    CHECK(dryRun.output.contains("Dry run: the package was not uploaded"));
    CHECK_FALSE(dryRun.output.contains("Uploading"));

    const auto quietArchive = root / "quiet.ruxpkg";
    const std::string quietArchiveText = quietArchive.string();
    const auto quiet = Run(std::array<std::string_view, 7>{"--quiet", "--manifest", manifest, "pack", "--output",
                                                           quietArchiveText, "--color=never"});
    CHECK(quiet.exitCode == 0);
    CHECK(quiet.output.empty());
    CHECK(std::filesystem::is_regular_file(quietArchive));

    // The archive a registry receives has to be a function of the sources and nothing else: two packs of the same
    // tree must agree byte for byte, or a rebuilt release cannot be checked against a published one.
    CHECK(ReadTextFile(archive) == ReadTextFile(quietArchive));

    const std::string rootText = root.string();
    const auto writeFailure =
        Run(std::array<std::string_view, 6>{"--manifest", manifest, "pack", "--output", rootText, "--color=never"});
    CHECK(writeFailure.exitCode == 1);
    CHECK(writeFailure.output.contains("error: could not write package archive"));
    CHECK(writeFailure.output.contains("help: choose a writable archive path"));

    WriteTextFile(root / "Rux.toml", "[Manifest]\nVersion = 1\n\n[Workspace]\nPackages = [\"Package\"]\n");
    const std::string workspaceManifest = (root / "Rux.toml").string();
    const auto workspace =
        Run(std::array<std::string_view, 4>{"--manifest", workspaceManifest, "pack", "--color=never"});
    CHECK(workspace.exitCode == 1);
    CHECK(workspace.output.contains("workspace manifest '"));
    CHECK(workspace.output.contains("publication requires the manifest of one workspace member package"));
    CHECK(workspace.output.contains("--manifest"));

    WriteTextFile(root / "Invalid.toml", R"([Manifest]
Version = 1

[Package]
Name = "Invalid"
Version = "1.0.0"
Type = "Executable"

[Dependencies]
Local = { Path = "Local" }
)");
    const std::string invalidManifest = (root / "Invalid.toml").string();
    const auto invalid =
        Run(std::array<std::string_view, 4>{"--manifest", invalidManifest, "publish", "--color=never"});
    CHECK(invalid.exitCode == 1);
    CHECK(CountOccurrences(invalid.output, "error:") == 1);
    CHECK(invalid.output.contains("does not meet publication requirements"));
    CHECK(CountOccurrences(invalid.output, "note:") == 8);
    CHECK(invalid.output.contains("help: update '"));

    const ScopedCliPackageCache cache;
    const ScopedEnvironmentValue token(Driver::kCredentialVariable);
    REQUIRE(System::UnsetEnv(Driver::kCredentialVariable));
    const auto missingCredential = Run(std::array<std::string_view, 6>{"--manifest", manifest, "publish", "--registry",
                                                                       "http://127.0.0.1:1", "--color=never"});
    CHECK(missingCredential.exitCode == 1);
    CHECK(missingCredential.output.contains("no publication credential was found"));
    CHECK(missingCredential.output.contains("rux login --registry http://127.0.0.1:1"));

    REQUIRE(System::SetEnv(Driver::kCredentialVariable, "bad token"));
    const auto invalidCredential = Run(std::array<std::string_view, 6>{"--manifest", manifest, "publish", "--registry",
                                                                       "http://127.0.0.1:1", "--color=never"});
    CHECK(invalidCredential.exitCode == 1);
    CHECK(invalidCredential.output.contains("publication credential from 'RUX_TOKEN' is invalid"));
    CHECK(invalidCredential.output.contains("tokens cannot contain whitespace"));

    std::error_code error;
    std::filesystem::remove_all(root, error);
    CHECK(!error);
}

TEST_CASE("lint and format report examined files, outcomes, and elapsed time") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = System::TempDirectory() / ("rux-lint-format-report-test-" + std::to_string(nonce));
    const auto manifestPath = root / "Rux.toml";
    const auto sourcePath = root / "Src" / "Main.rux";
    WriteTextFile(manifestPath, R"([Manifest]
Version = 1

[Package]
Name = "ReportTest"
Version = "0.1.0"
Type = "Executable"
)");
    WriteTextFile(sourcePath, "func Main() -> int { return 0; }\n");
    const auto manifest = manifestPath.string();

    const auto passed = Run(std::array<std::string_view, 4>{"--manifest", manifest, "--color=never", "lint"});
    CHECK(passed.exitCode == 0);
    CHECK(passed.output.contains("Linting ReportTest v0.1.0"));
    CHECK(passed.output.contains("Linted ReportTest in "));
    CHECK(passed.output.contains("1 file, 0 warnings, 0 errors"));

    WriteTextFile(sourcePath, "func bad_name() {}\n");
    const auto warned = Run(std::array<std::string_view, 4>{"--manifest", manifest, "--color=never", "lint"});
    CHECK(warned.exitCode == 0);
    CHECK(warned.output.contains("warning: function name 'bad_name' should be PascalCase"));
    CHECK(warned.output.contains("help: rename it to 'BadName'"));
    CHECK(warned.output.contains("1 file, 1 warning, 0 errors"));

    WriteTextFile(sourcePath, "func Main(\n");
    const auto failed =
        Run(std::array<std::string_view, 5>{"--manifest", manifest, "--color=never", "lint", "--quiet"});
    CHECK(failed.exitCode == 1);
    CHECK(failed.output.contains("error:"));
    CHECK(failed.output.contains("Failed ReportTest in "));

    WriteTextFile(sourcePath, "func Main() -> int { return 0; }  \r\n");
    const auto check = Run(std::array<std::string_view, 5>{"--manifest", manifest, "--color=never", "fmt", "--check"});
    CHECK(check.exitCode == 1);
    CHECK(check.output.contains("source file '" + sourcePath.string() + "' is not formatted"));
    CHECK(check.output.contains("2 files"));
    CHECK(check.output.contains("1 formatted, 1 need formatting"));

    const auto changed = Run(std::array<std::string_view, 4>{"--manifest", manifest, "--color=never", "fmt"});
    CHECK(changed.exitCode == 0);
    CHECK(changed.output.contains("Formatted 2 files in "));
    CHECK(changed.output.contains("1 changed, 1 unchanged"));
    const auto unchanged = Run(std::array<std::string_view, 4>{"--manifest", manifest, "--color=never", "fmt"});
    CHECK(unchanged.exitCode == 0);
    CHECK(unchanged.output.contains("0 changed, 2 unchanged"));

    std::error_code error;
    std::filesystem::remove_all(root / "Src", error);
    REQUIRE(!error);
    const auto missing =
        Run(std::array<std::string_view, 5>{"--manifest", manifest, "--color=never", "fmt", "--source-only"});
    CHECK(missing.exitCode == 0);
    CHECK(missing.output.contains("no source files were examined"));
    CHECK(missing.output.contains("Formatted 0 files in "));
    std::filesystem::remove_all(root, error);
}

TEST_CASE("documentation reports package and workspace output without replacing unmanaged directories") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = System::TempDirectory() / ("rux-doc-report-test-" + std::to_string(nonce));
    const auto manifestPath = root / "Rux.toml";
    WriteTextFile(manifestPath, "[Manifest]\nVersion = 1\n\n[Workspace]\nPackages = [\"Alpha\", \"Beta\"]\n");
    for (const std::string_view package : {"Alpha", "Beta"}) {
        const auto packageRoot = root / package;
        WriteTextFile(packageRoot / "Rux.toml",
                      std::format("[Manifest]\nVersion = 1\n\n[Package]\nName = \"{}\"\nVersion = \"0.1.0\"\n"
                                  "Type = \"SourceLibrary\"\n",
                                  package));
        WriteTextFile(packageRoot / "Src" / (std::string(package) + ".rux"),
                      std::format("module {} {{\n    pub func Visible() {{}}\n    func Hidden() {{}}\n}}\n", package));
    }
    const auto manifest = manifestPath.string();
    const auto output = root / "GeneratedDocs";
    const auto outputText = output.string();
    const auto generated = Run(std::array<std::string_view, 7>{"--manifest", manifest, "--color=never", "doc",
                                                               "--document-private-items", "--output", outputText});
    CHECK(generated.exitCode == 0);
    CHECK(generated.output.contains("Generating documentation for workspace"));
    CHECK(generated.output.contains("Generated documentation for 2 packages in "));
    CHECK(generated.output.contains("Output: " + (output / "index.html").string()));
    CHECK(std::filesystem::is_regular_file(output / "Alpha" / "index.html"));
    CHECK(ReadTextFile(output / "Alpha" / "index.html").contains("Hidden"));

    const auto unmanaged = root / "Unmanaged";
    WriteTextFile(unmanaged / "sentinel.txt", "keep\n");
    const auto unmanagedText = unmanaged.string();
    const auto protectedOutput =
        Run(std::array<std::string_view, 6>{"--manifest", manifest, "doc", "--output", unmanagedText, "--color=never"});
    CHECK(protectedOutput.exitCode == 1);
    CHECK(protectedOutput.output.contains("refusing to replace non-empty unmarked directory"));
    CHECK(std::filesystem::is_regular_file(unmanaged / "sentinel.txt"));

    const auto quiet = Run(std::array<std::string_view, 7>{"--manifest", manifest, "--quiet", "doc", "--output",
                                                           outputText, "--document-private-items"});
    CHECK(quiet.exitCode == 0);
    CHECK(quiet.output.empty());
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

TEST_CASE("command help accepts equals options and reports one command") {
    const auto result = Run(std::array<std::string_view, 3>{"help", "build", "--json"});
    CHECK(result.exitCode == 0);
    CHECK(result.output.contains("\"name\":\"build\""));
    CHECK_FALSE(result.output.contains("\"name\":\"check\""));
    CHECK(result.output.contains("--emit <kind[,kind...]>"));
}

TEST_CASE("run is host-only while build and test accept a target") {
    const auto runHelp = Run(std::array<std::string_view, 3>{"help", "run", "--json"});
    CHECK(runHelp.exitCode == 0);
    CHECK_FALSE(runHelp.output.contains("--target <triple>"));
    const auto testHelp = Run(std::array<std::string_view, 3>{"help", "test", "--json"});
    CHECK(testHelp.exitCode == 0);
    CHECK(testHelp.output.contains("--target <triple>"));

    CHECK(Run(std::array<std::string_view, 2>{"run", "--target"}).exitCode == 2);
    const auto rejected = Run(std::array<std::string_view, 3>{"run", "--target", "linux-aarch64"});
    CHECK(rejected.exitCode == 2);
    CHECK(rejected.output.contains("command 'run' cannot execute target 'linux-aarch64'"));
    CHECK(rejected.output.contains("rux build --target linux-aarch64"));

    // An unknown triple is rejected by commands that still select targets.
    const auto manifest = ArithmeticManifest();
    for (const std::string_view command : {"build", "test"}) {
        const auto unknown =
            Run(std::array<std::string_view, 5>{"--manifest", manifest, command, "--target", "plan9-x86_64"});
        CHECK(unknown.exitCode == 1);
        CHECK(unknown.output.contains("target 'plan9-x86_64'"));
    }
}

TEST_CASE("invalid target input is rejected before manifest discovery") {
    const auto missing = (std::filesystem::path(RUX_ROOT_DIR) / "Tests" / "missing-Rux.toml").string();
    const auto result =
        Run(std::array<std::string_view, 5>{"--manifest", missing, "build", "--target", "plan9-x86_64"});

    CHECK(result.exitCode == 1);
    CHECK(result.output.contains("target 'plan9-x86_64' is not supported"));
    CHECK_FALSE(result.output.contains("manifest"));
}

TEST_CASE("check renders unsupported targets as structured diagnostics without changing JSON schema") {
    const auto missing = (std::filesystem::path(RUX_ROOT_DIR) / "Tests" / "missing-Rux.toml").string();
    const auto human = Run(std::array<std::string_view, 5>{"--manifest", missing, "check", "--target", "plan9-x86_64"});

    CHECK(human.exitCode == 1);
    CHECK(human.output.contains("error: target 'plan9-x86_64' is not supported"));
    CHECK(human.output.contains("  note: supported targets are "));
    CHECK(human.output.contains("  help: try 'rux check --target linux-x86_64'"));
    CHECK(human.output.contains("  docs: https://rux-lang.dev/docs/cli"));
    CHECK_FALSE(human.output.contains("manifest"));

    const auto json =
        Run(std::array<std::string_view, 6>{"--manifest", missing, "check", "--json", "--target", "plan9-x86_64"});
    CHECK(json.exitCode == 1);
    CHECK(json.output.contains("\"message\":\"target 'plan9-x86_64' is not supported\""));
    CHECK_FALSE(json.output.contains("supported targets are"));
    CHECK_FALSE(json.output.contains("documentationUrl"));
    CHECK_FALSE(json.output.contains("rux-lang.dev"));
}

TEST_CASE("check and lint render source frames while check JSON remains frame-free") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = System::TempDirectory() / ("rux-source-frame-test-" + std::to_string(nonce));
    const auto sourceDir = root / "Src";
    const auto manifestPath = root / "Rux.toml";
    const auto sourcePath = sourceDir / "Main.rux";
    std::error_code error;
    std::filesystem::create_directories(sourceDir, error);
    REQUIRE(!error);
    {
        std::ofstream manifest(manifestPath, std::ios::binary);
        manifest << "[Manifest]\nVersion = 1\n\n[Package]\nName = \"FrameTest\"\nVersion = \"0.1.0\"\n"
                    "Type = \"Executable\"\n";
        REQUIRE(manifest.good());
    }
    {
        std::ofstream source(sourcePath, std::ios::binary);
        source << "func Main() -> int {\n    return Missing;\n}\n";
        REQUIRE(source.good());
    }

    const auto manifest = manifestPath.string();
    const auto check =
        Run(std::array<std::string_view, 5>{"--manifest", manifest, "check", "--quiet", "--color=never"});
    CAPTURE(check.output);
    CHECK(check.exitCode == 1);
    CHECK(check.output.contains("  2 |     return Missing;"));
    CHECK(check.output.contains("    |            ^"));

    const auto json = Run(std::array<std::string_view, 5>{"--manifest", manifest, "check", "--quiet", "--json"});
    CHECK(json.exitCode == 1);
    CHECK(json.output.contains("\"line\":2"));
    CHECK_FALSE(json.output.contains("return Missing"));
    CHECK_FALSE(json.output.contains('|'));
    CHECK_FALSE(json.output.contains('^'));

    {
        std::ofstream source(sourcePath, std::ios::binary | std::ios::trunc);
        source << "func bad_name() {}\n";
        REQUIRE(source.good());
    }
    const auto lint = Run(std::array<std::string_view, 5>{"--manifest", manifest, "lint", "--quiet", "--color=never"});
    CAPTURE(lint.output);
    CHECK(lint.exitCode == 0);
    CHECK(lint.output.contains("  1 | func bad_name() {}"));
    CHECK(lint.output.contains("    | ^"));

    std::filesystem::remove_all(root, error);
    CHECK(!error);
}

TEST_CASE("check reports package and workspace outcomes with shared timing grammar") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = System::TempDirectory() / ("rux-check-report-test-" + std::to_string(nonce));
    const auto manifestPath = root / "Rux.toml";
    WriteTextFile(manifestPath, R"([Manifest]
Version = 1

[Workspace]
Packages = ["Alpha", "Beta"]
)");
    for (const std::string_view package : {"Alpha", "Beta"}) {
        const auto packageRoot = root / package;
        WriteTextFile(packageRoot / "Rux.toml",
                      std::format("[Manifest]\nVersion = 1\n\n[Package]\nName = \"{}\"\nVersion = \"0.1.0\"\n"
                                  "Type = \"SourceLibrary\"\n",
                                  package));
        WriteTextFile(packageRoot / "Src" / (std::string(package) + ".rux"), std::format("module {} {{}}\n", package));
    }

    const auto manifest = manifestPath.string();
    const auto human = Run(std::array<std::string_view, 4>{"--manifest", manifest, "--color=never", "check"});
    CAPTURE(human.output);
    CHECK(human.exitCode == 0);
    CHECK(human.output.contains("Checking workspace (" + Driver::TargetDisplayName(Rux::Target::TargetTriple::Host()) +
                                ")"));
    CHECK(human.output.contains("  Packages: 2"));
    CHECK(human.output.contains("Checked Alpha in "));
    CHECK(human.output.contains("Checked Beta in "));
    CHECK(human.output.contains("Checked 2 packages in "));
    CHECK(human.output.contains("(2 passed, 0 failed)"));
    CHECK_FALSE(human.output.contains("[PASSED]"));

    const auto json = Run(std::array<std::string_view, 4>{"--manifest", manifest, "check", "--json"});
    CHECK(json.exitCode == 0);
    const auto normalizedJson = NormalizeNewlines(json.output);
    CHECK(normalizedJson.starts_with("{\n"));
    CHECK(json.output.contains("\"success\": true"));
    CHECK_FALSE(json.output.contains("Checking"));
    CHECK_FALSE(json.output.contains("Checked"));

    std::error_code error;
    std::filesystem::remove_all(root, error);
    CHECK(!error);
}

TEST_CASE("test reports successful empty trees as timed no-work outcomes") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = System::TempDirectory() / ("rux-empty-test-report-" + std::to_string(nonce));
    const auto manifestPath = root / "Rux.toml";
    WriteTextFile(manifestPath, R"([Manifest]
Version = 1

[Package]
Name = "EmptyTests"
Version = "0.1.0"
Type = "Executable"
)");
    WriteTextFile(root / "Src" / "Main.rux", "func Main() -> int { return 0; }\n");

    const auto manifest = manifestPath.string();
    const auto result = Run(std::array<std::string_view, 4>{"--manifest", manifest, "--color=never", "test"});
    CAPTURE(result.output);
    CHECK(result.exitCode == 0);
    CHECK(result.output.contains("Testing EmptyTests v0.1.0 (Debug, " +
                                 Driver::TargetDisplayName(Rux::Target::TargetTriple::Host()) + ")"));
    CHECK(result.output.contains("Passed 0 tests in "));
    CHECK(result.output.contains("No test directory found at 'Tests/'"));

    const auto quiet = Run(std::array<std::string_view, 4>{"--manifest", manifest, "test", "--quiet"});
    CHECK(quiet.exitCode == 0);
    CHECK(quiet.output.empty());

    std::error_code error;
    std::filesystem::remove_all(root, error);
    CHECK(!error);
}

TEST_CASE("test keeps failed rows, reasons, diagnostics, and captured output together") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = System::TempDirectory() / ("rux-test-report-" + std::to_string(nonce));
    const auto manifestPath = root / "Rux.toml";
    WriteTextFile(manifestPath, R"([Manifest]
Version = 1

[Package]
Name = "TestReport"
Version = "0.1.0"
Type = "Executable"
)");
    WriteTextFile(root / "Src" / "Main.rux", "func Main() -> int { return 0; }\n");

    auto WriteTest = [&](const std::string_view name, const std::string_view source,
                         const std::string_view dependencies = {}) {
        const auto testRoot = root / "Tests" / name;
        WriteTextFile(testRoot / "Rux.toml",
                      std::format("[Manifest]\nVersion = 1\n\n[Package]\nName = \"{}\"\nVersion = \"0.1.0\"\n"
                                  "Type = \"Executable\"\n{}",
                                  name, dependencies));
        WriteTextFile(testRoot / "Src" / "Main.rux", source);
    };
    WriteTest("Pass", "func Main() -> int { return 0; }\n");
    WriteTest("BuildFail", "func Main() -> int { return Missing; }\n");
    const auto runtimeRoot = root / "Tests" / "RuntimeFail";
    std::error_code copyError;
    std::filesystem::create_directories(root / "Packages", copyError);
    REQUIRE(!copyError);
    std::filesystem::copy(std::filesystem::path(RUX_ROOT_DIR) / "Packages" / "Core", root / "Packages" / "Core",
                          std::filesystem::copy_options::recursive, copyError);
    CAPTURE(copyError.message());
    REQUIRE(!copyError);
    WriteTest("RuntimeFail", "import Core::Panic;\nfunc Main() -> int { Panic(\"first line\\nsecond line\"); }\n",
              "\n[Dependencies]\nCore = { Path = \"../../Packages/Core\" }\n");
    const auto runtimeManifest = Manifest::Load(runtimeRoot / "Rux.toml");
    std::string runtimeManifestErrors;
    for (const auto &diagnostic : runtimeManifest.diagnostics) {
        runtimeManifestErrors += diagnostic.Format() + '\n';
    }
    CAPTURE(runtimeManifestErrors);
    REQUIRE(runtimeManifest.Ok());

    const auto manifest = manifestPath.string();
    const auto result = Run(std::array<std::string_view, 4>{"--manifest", manifest, "--color=never", "test"});
    const auto normalized = NormalizeNewlines(result.output);
    CAPTURE(normalized);
    CHECK(result.exitCode == 1);
    CHECK(normalized.contains("Passed Pass in "));
    CHECK(normalized.contains("Failed BuildFail in "));
    CHECK(normalized.contains("note: the test package did not compile"));
    CHECK(normalized.contains("name 'Missing' is not defined in this scope"));
    CHECK(normalized.contains("Failed RuntimeFail in "));
    CHECK(normalized.contains("note: test 'RuntimeFail' exited with code"));
    CHECK(normalized.contains("  Output:\n"));
    CHECK(normalized.contains("    Panic: first line\n    second line"));
    CHECK(normalized.contains("Failed 3 tests in "));
    CHECK(normalized.contains("(1 passed, 2 failed)"));
    CHECK_FALSE(normalized.contains("[FAILED]"));

    const auto quiet = Run(std::array<std::string_view, 5>{"--manifest", manifest, "--color=never", "test", "--quiet"});
    const auto normalizedQuiet = NormalizeNewlines(quiet.output);
    CAPTURE(normalizedQuiet);
    CHECK(quiet.exitCode == 1);
    CHECK(normalizedQuiet.contains("Failed BuildFail in "));
    CHECK(normalizedQuiet.contains("Failed RuntimeFail in "));
    CHECK(normalizedQuiet.contains("note: the test package did not compile"));
    CHECK(normalizedQuiet.contains("  Output:\n"));
    CHECK_FALSE(normalizedQuiet.contains("Testing TestReport"));
    CHECK_FALSE(normalizedQuiet.contains("Running 3 tests"));
    CHECK_FALSE(normalizedQuiet.contains("Passed Pass"));
    CHECK_FALSE(normalizedQuiet.contains("Failed 3 tests"));

    std::error_code error;
    std::filesystem::remove_all(root, error);
    CHECK(!error);
}

TEST_CASE("CLI checks and builds the canonical macOS AArch64 target through its ARM64 alias") {
    const auto manifest = ArithmeticManifest();
    const auto check =
        Run(std::array<std::string_view, 6>{"--manifest", manifest, "check", "--target", "macos-arm64", "--quiet"});
    REQUIRE(check.exitCode == 0);

    const auto result = Run(std::array<std::string_view, 7>{"--manifest", manifest, "build", "--release", "--target",
                                                            "macos-arm64", "--quiet"});

    REQUIRE(result.exitCode == 0);
    CHECK_FALSE(result.output.contains("cannot run"));

    const auto output = std::filesystem::path(RUX_ROOT_DIR) / "Bin" / "Tests" / "Language" / "Release" / "macOS" /
                        "AArch64" / "Arithmetic";
    REQUIRE(std::filesystem::is_regular_file(output));

    std::ifstream input(output, std::ios::binary);
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    Testing::MachOImage image;
    std::string error;
    const bool parsed = Testing::ReadMachO64(bytes, image, error);
    CAPTURE(error);
    REQUIRE(parsed);
    CHECK(image.Architecture() == Testing::MachOArchitecture::AArch64);
    CHECK(image.fileType == 2); // MH_EXECUTE
    REQUIRE(image.codeSignature);
    CHECK(image.codeSignature->offset + image.codeSignature->size == bytes.size());

    const auto rejected = Run(std::array<std::string_view, 3>{"run", "--target", "macos-aarch64"});
    CHECK(rejected.exitCode == 2);
    CHECK(rejected.output.contains("command 'run' cannot execute target 'macos-aarch64'"));
}

TEST_CASE("CLI checks and builds the canonical FreeBSD AArch64 target through its ARM64 alias") {
    const auto manifest = ArithmeticManifest();
    const auto check =
        Run(std::array<std::string_view, 6>{"--manifest", manifest, "check", "--target", "freebsd-arm64", "--quiet"});
    REQUIRE(check.exitCode == 0);

    const auto result = Run(std::array<std::string_view, 7>{"--manifest", manifest, "build", "--release", "--target",
                                                            "freebsd-arm64", "--quiet"});

    REQUIRE(result.exitCode == 0);
    CHECK_FALSE(result.output.contains("not implemented"));
    CHECK_FALSE(result.output.contains("cannot run"));

    const auto output = std::filesystem::path(RUX_ROOT_DIR) / "Bin" / "Tests" / "Language" / "Release" / "FreeBSD" /
                        "AArch64" / "Arithmetic";
    REQUIRE(std::filesystem::is_regular_file(output));

    Testing::ElfImage image{ReadBinaryFile(output)};
    REQUIRE(image.bytes.size() >= 64);
    CHECK(image.OsAbi() == 9);     // ELFOSABI_FREEBSD
    CHECK(image.Type() == 2);      // ET_EXEC
    CHECK(image.Machine() == 183); // EM_AARCH64
    CHECK(image.Entry() != 0);
    CHECK(image.Interpreter().empty());
}

TEST_CASE("target tests reject a foreign operating system before discovering packages") {
    const std::string foreignTarget =
        Driver::HostTargetTriple().starts_with("windows-") ? "linux-x86_64" : "windows-x86_64";
    const auto result = Run(std::array<std::string_view, 3>{"test", "--target", foreignTarget});

    CHECK(result.exitCode == 1);
    CHECK(result.output.contains("target '" + foreignTarget + "' cannot be executed on host"));
    CHECK(result.output.contains("can build and check target '" + foreignTarget + "'"));
    CHECK(result.output.contains("rux build --target " + foreignTarget));
    CHECK(result.output.contains("rux check --target " + foreignTarget));
    CHECK(result.output.contains("rux test --target " + foreignTarget));
    CHECK(result.output.contains("on a native '" + foreignTarget + "' host"));
    CHECK_FALSE(result.output.contains("Running "));
}

TEST_CASE("CLI cross-builds a Windows AArch64 executable without trying to run it") {
    const auto manifest = ArithmeticManifest();
    const auto result = Run(std::array<std::string_view, 7>{"--manifest", manifest, "build", "--release", "--target",
                                                            "windows-aarch64", "--quiet"});

    REQUIRE(result.exitCode == 0);
    CHECK_FALSE(result.output.contains("cannot run"));

    const auto output = std::filesystem::path(RUX_ROOT_DIR) / "Bin" / "Tests" / "Language" / "Release" / "Windows" /
                        "AArch64" / "Arithmetic.exe";
    REQUIRE(std::filesystem::is_regular_file(output));

    std::ifstream input(output, std::ios::binary);
    const std::vector<unsigned char> image((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    REQUIRE(image.size() >= 0x40);
    REQUIRE(image[0] == 'M');
    REQUIRE(image[1] == 'Z');
    const std::size_t peOffset = Read32(image, 0x3C);
    REQUIRE(peOffset + 6 <= image.size());
    CHECK(Read32(image, peOffset) == 0x00004550); // PE\0\0
    CHECK(Read16(image, peOffset + 4) == 0xAA64); // IMAGE_FILE_MACHINE_ARM64
}

TEST_CASE("structured commands emit a complete JSON document on operational failure") {
    const auto missing = (std::filesystem::path(RUX_ROOT_DIR) / "Tests" / "missing-Rux.toml").string();
    const auto check = Run(std::array<std::string_view, 4>{"check", "--json", "--manifest", missing});
    CHECK(check.exitCode == 1);
    CHECK(check.output.contains("\"success\": false"));
    CHECK(check.output.contains("\"diagnostics\": ["));

    const auto info = Run(std::array<std::string_view, 4>{"info", "--json", "--manifest", missing});
    CHECK(info.exitCode == 1);
    CHECK(info.output.contains("{\"success\":false,\"error\":"));
}

TEST_CASE("human help honors color mode while JSON stays unstyled") {
    const auto colored = Run(std::array<std::string_view, 2>{"help", "--color=always"});
    CHECK(colored.exitCode == 0);
    CHECK(colored.output.contains("\033[1m\033[36mRux\033[0m"));
    CHECK(colored.output.contains("\033[36mbuild"));

    const auto plain = Run(std::array<std::string_view, 2>{"help", "--color=never"});
    CHECK(plain.exitCode == 0);
    CHECK_FALSE(plain.output.contains("\033["));

    const auto command = Run(std::array<std::string_view, 3>{"help", "build", "--color=always"});
    CHECK(command.exitCode == 0);
    CHECK(command.output.contains("\033[1mUsage:\033[0m"));
    CHECK(command.output.contains("\033[36m--emit <kind[,kind...]>"));

    const auto json = Run(std::array<std::string_view, 3>{"help", "--json", "--color=always"});
    CHECK(json.exitCode == 0);
    CHECK_FALSE(json.output.contains("\033["));
    CHECK(json.output.starts_with("{\"schemaVersion\":1"));
}
