#include "System/Os.h"
#include "System/Process.h"

#include <array>
#include <chrono>
#include <doctest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

using namespace Rux;

namespace {
std::filesystem::path RuxExecutable() {
    return std::filesystem::path(RUX_ROOT_DIR) / "Bin" / System::ExecutableFileName("rux");
}

template <std::size_t N>
System::RunResult Run(const std::array<std::string_view, N> &arguments) {
    const auto result = System::RunCaptured(RuxExecutable(), arguments);
    REQUIRE(result.has_value());
    return *result;
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

class ScopedCurrentDirectory {
public:
    explicit ScopedCurrentDirectory(const std::filesystem::path &path)
        : previous(std::filesystem::current_path()) {
        std::filesystem::current_path(path);
    }

    ScopedCurrentDirectory(const ScopedCurrentDirectory &) = delete;
    ScopedCurrentDirectory &operator=(const ScopedCurrentDirectory &) = delete;

    ~ScopedCurrentDirectory() {
        std::error_code error;
        std::filesystem::current_path(previous, error);
    }

private:
    std::filesystem::path previous;
};

bool ContainsDuration(const std::string_view text) {
    return text.contains(" in ") && (text.contains(" ms") || text.contains(" s") || text.contains(" min "));
}
} // namespace

TEST_CASE("new reports every package kind destination changes and elapsed time") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = System::TempDirectory() / ("rux-new-command-test-" + std::to_string(nonce));
    std::filesystem::create_directories(root);
    const auto rootText = root.string();

    struct PackageKindCase {
        std::string_view name;
        std::string_view flag;
        std::string_view label;
        std::string_view manifestType;
        std::string_view sourceFile;
        bool hasBin;
    };

    constexpr std::array cases = {
        PackageKindCase{"App", "--executable", "executable", "Executable", "Main.rux", true},
        PackageKindCase{"Io", "--shared", "shared-library", "SharedLibrary", "Lib.rux", true},
        PackageKindCase{"Math", "--static", "static-library", "StaticLibrary", "Lib.rux", true},
        PackageKindCase{"Json", "--source", "source-library", "SourceLibrary", "Lib.rux", false},
    };

    for (const auto &test : cases) {
        const auto result = Run(std::array<std::string_view, 5>{"new", test.name, test.flag, "--path", rootText});
        REQUIRE_MESSAGE(result.exitCode == 0, result.output);
        CHECK(result.output.contains("Created " + std::string(test.label) + " package '" + std::string(test.name) +
                                     "' in "));
        CHECK(result.output.contains("Path: " + (root / test.name).string()));
        CHECK(result.output.contains("Files: 3 written, 0 preserved"));
        CHECK(ContainsDuration(result.output));
        CHECK(
            ReadTextFile(root / test.name / "Rux.toml").contains("Type = \"" + std::string(test.manifestType) + "\""));
        CHECK(std::filesystem::is_regular_file(root / test.name / "Src" / test.sourceFile));
        CHECK(std::filesystem::exists(root / test.name / "Bin") == test.hasBin);
    }

    const auto quiet = Run(std::array<std::string_view, 6>{"--quiet", "new", "Silent", "--source", "--path", rootText});
    REQUIRE(quiet.exitCode == 0);
    CHECK(quiet.output.empty());
    CHECK(std::filesystem::is_regular_file(root / "Silent" / "Rux.toml"));

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

TEST_CASE("new rejects invalid names existing destinations and package kind conflicts with usable corrections") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = System::TempDirectory() / ("rux-new-diagnostic-test-" + std::to_string(nonce));
    std::filesystem::create_directories(root / "Existing");
    const auto rootText = root.string();

    const auto invalid = Run(std::array<std::string_view, 4>{"new", "Bad.Name", "--path", rootText});
    CHECK(invalid.exitCode == 1);
    CHECK(invalid.output.contains("error: package name 'Bad.Name' is invalid"));
    CHECK(invalid.output.contains("note: identity segment allows only ASCII"));
    CHECK(invalid.output.contains("help: use 1 to 64 ASCII letters or digits"));
    CHECK_FALSE(ContainsDuration(invalid.output));

    const auto invalidNamespace =
        Run(std::array<std::string_view, 6>{"new", "Namespaced", "--namespace", "Bad.Name", "--path", rootText});
    CHECK(invalidNamespace.exitCode == 1);
    CHECK(invalidNamespace.output.contains("error: namespace 'Bad.Name' is invalid"));
    CHECK(invalidNamespace.output.contains("note: identity segment allows only ASCII"));
    CHECK(invalidNamespace.output.contains("help: use 1 to 64 ASCII letters or digits"));
    CHECK_FALSE(ContainsDuration(invalidNamespace.output));
    CHECK_FALSE(std::filesystem::exists(root / "Namespaced"));

    const auto existing = Run(std::array<std::string_view, 4>{"new", "Existing", "--path", rootText});
    CHECK(existing.exitCode == 1);
    CHECK(existing.output.contains("error: destination '" + (root / "Existing").string() + "' already exists"));
    CHECK(existing.output.contains("help: choose another package name or run 'rux init'"));
    CHECK(existing.output.contains("Failed to create package 'Existing' in "));
    CHECK(ContainsDuration(existing.output));

    const auto conflict =
        Run(std::array<std::string_view, 6>{"new", "Conflict", "--shared", "--static", "--path", rootText});
    CHECK(conflict.exitCode == 2);
    CHECK(conflict.output.contains("options '--shared' and '--static' cannot be used together"));
    CHECK(conflict.output.contains("help: use either '--shared' or '--static', but not both"));
    CHECK_FALSE(ContainsDuration(conflict.output));

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

TEST_CASE("init preserves package files and reports partial write failures") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = System::TempDirectory() / ("rux-init-command-test-" + std::to_string(nonce)) / "InitApp";
    const auto manifestPath = root / "Rux.toml";
    const auto sourcePath = root / "Src" / "Lib.rux";
    WriteTextFile(manifestPath, R"([Manifest]
Version = 1

[Package]
Name = "InitApp"
Version = "7.8.9"
Type = "SourceLibrary"
Description = "preserve me"
)");
    WriteTextFile(sourcePath, "module InitApp {\n    // preserve me\n}\n");
    const auto manifestBefore = ReadTextFile(manifestPath);
    const auto sourceBefore = ReadTextFile(sourcePath);

    {
        const ScopedCurrentDirectory current(root);
        const auto initialized = Run(std::array<std::string_view, 2>{"init", "--shared"});
        REQUIRE_MESSAGE(initialized.exitCode == 0, initialized.output);
        CHECK(initialized.output.contains("Initialized package 'InitApp' in "));
        CHECK(initialized.output.contains("Files: 1 written, 2 preserved"));
        CHECK(ContainsDuration(initialized.output));
    }
    CHECK(ReadTextFile(manifestPath) == manifestBefore);
    CHECK(ReadTextFile(sourcePath) == sourceBefore);

    std::filesystem::remove(manifestPath);
    std::filesystem::remove(sourcePath);
    std::filesystem::remove(root / ".gitignore");
    std::filesystem::create_directories(root / ".gitignore");
    {
        const ScopedCurrentDirectory current(root);
        // init derives its root from the current directory, which the OS may
        // report with symlinks resolved (macOS: /var -> /private/var), so the
        // expected path must come from the resolved directory, not `root`.
        const auto reportedRoot = std::filesystem::current_path();
        const auto failed = Run(std::array<std::string_view, 1>{"init"});
        CHECK(failed.exitCode == 1);
        CHECK(failed.output.contains("error: could not write package file '" + (reportedRoot / ".gitignore").string() +
                                     "'"));
        CHECK(failed.output.contains("note: the path exists but is not a regular file"));
        CHECK(failed.output.contains("note: the destination contains"));
        CHECK(failed.output.contains("help: fix the reported path and run 'rux init' again"));
        CHECK(failed.output.contains("Failed to initialize package 'InitApp' in "));
    }

    std::error_code error;
    std::filesystem::remove_all(root.parent_path(), error);
}
