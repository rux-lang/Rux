#include "CliProcessTestSupport.h"

#include <chrono>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>

using namespace Rux;
using namespace Rux::Testing::CliProcessTestSupport;

namespace {
/// A user workspace in a temporary directory whose members declare namespace `Acme`. Members and test packages are
/// written on request; the directory is removed with the fixture.
class WorkspaceFixture {
public:
    explicit WorkspaceFixture(const std::string_view label) {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        root = System::TempDirectory() / std::format("rux-workspace-{}-{}", label, nonce);
    }

    ~WorkspaceFixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    WorkspaceFixture(const WorkspaceFixture &) = delete;
    WorkspaceFixture &operator=(const WorkspaceFixture &) = delete;

    void Workspace(const std::string_view members) const {
        WriteTextFile(root / "Rux.toml",
                      std::format("[Manifest]\nVersion = 1\n\n[Workspace]\nPackages = [{}]\n", members));
    }

    /// A package below the workspace root. `dependencies` is the body of its [Dependencies] table.
    void Package(const std::string_view directory, const std::string_view name, const std::string_view type,
                 const std::string_view dependencies, const std::string_view file, const std::string_view source,
                 const std::string_view ns = "Acme") const {
        std::string manifest = "[Manifest]\nVersion = 1\n\n[Package]\n";
        if (!ns.empty())
            manifest += std::format("Namespace = \"{}\"\n", ns);
        manifest += std::format("Name = \"{}\"\nVersion = \"0.1.0\"\nType = \"{}\"\n", name, type);
        if (!dependencies.empty())
            manifest += std::format("\n[Dependencies]\n{}", dependencies);
        WriteTextFile(root / directory / "Rux.toml", manifest);
        WriteTextFile(root / directory / "Src" / file, source);
    }

    [[nodiscard]] std::string Manifest() const {
        return (root / "Rux.toml").string();
    }

    std::filesystem::path root;
};

/// `Rux/Io` as `rux install` would leave it in the package cache: one module with one function.
void InstallIo() {
    const auto io = WriteCachedPackage("Rux", "Io", "0.1.0", "cached standard I/O");
    WriteTextFile(io / "Src" / "Api.rux",
                  "pub module Api {\n    pub func Status() -> int {\n        return 0;\n    }\n}\n");
}

/// A user workspace: App and Greeter are members, and both use `Rux/Io`, which no member supplies.
void WriteGreeterWorkspace(const WorkspaceFixture &workspace) {
    workspace.Workspace("\"App\", \"Greeter\"");
    workspace.Package("Greeter", "Greeter", "SourceLibrary", "Io = { Namespace = \"Rux\", Version = \"*\" }\n",
                      "Api.rux",
                      "import Io::Api::Status;\n\npub module Greet {\n    pub func Answer() -> int {\n"
                      "        return Status();\n    }\n}\n");
    workspace.Package("App", "App", "Executable",
                      "Greeter = { Namespace = \"Acme\", Version = \"*\" }\nIo = { Namespace = \"Rux\", Version = "
                      "\"*\" }\n",
                      "Main.rux",
                      "import Greeter::Greet::Answer;\nimport Io::Api::Status;\n\nfunc Main() -> int {\n"
                      "    return Answer() + Status();\n}\n");
}
} // namespace

TEST_CASE("workspace check and test take registry packages outside the workspace's namespaces from the cache") {
    const ScopedCliPackageCache cache;
    InstallIo();
    const WorkspaceFixture workspace("registry");
    WriteGreeterWorkspace(workspace);
    // A central test package uses a member by path and the standard library by registry identity.
    workspace.Package("Tests/Smoke", "Smoke", "Executable",
                      "Greeter = { Path = \"../../Greeter\" }\nIo = { Namespace = \"Rux\", Version = \"*\" }\n",
                      "Main.rux",
                      "import Greeter::Greet::Answer;\nimport Io::Api::Status;\n\nfunc Main() -> int {\n"
                      "    return Answer() + Status();\n}\n",
                      "");

    const auto manifest = workspace.Manifest();
    const auto checked = Run(std::array<std::string_view, 4>{"--manifest", manifest, "--color=never", "check"});
    CAPTURE(checked.output);
    CHECK(checked.exitCode == 0);
    CHECK(checked.output.contains("Checked 2 packages in "));

    const auto tested = Run(std::array<std::string_view, 4>{"--manifest", manifest, "--color=never", "test"});
    CAPTURE(tested.output);
    CHECK(tested.exitCode == 0);
    CHECK(tested.output.contains("Passed Smoke in "));
    CHECK_FALSE(tested.output.contains("registry dependencies"));
}

TEST_CASE("workspace check refuses a package of its own namespace that no member supplies") {
    const ScopedCliPackageCache cache;
    // Even an installed copy does not stand in for a member of a namespace the workspace owns.
    WriteCachedPackage("Acme", "Missing", "0.1.0", "stale cached copy");
    const WorkspaceFixture workspace("owned");
    workspace.Workspace("\"App\"");
    workspace.Package("App", "App", "Executable", "Missing = { Namespace = \"Acme\", Version = \"*\" }\n", "Main.rux",
                      "import Missing::Api::Value;\n\nfunc Main() -> int {\n    return Value();\n}\n");

    const auto checked =
        Run(std::array<std::string_view, 4>{"--manifest", workspace.Manifest(), "--color=never", "check"});
    CAPTURE(checked.output);
    CHECK(checked.exitCode == 1);
    CHECK(checked.output.contains(
        "error: package 'Acme/Missing' is not a workspace member, but the workspace owns namespace 'Acme'"));
    CHECK(checked.output.contains("help: add the package to [Workspace].Packages or use a local Path dependency"));
}

TEST_CASE("workspace build compiles every member that produces an artifact") {
    const ScopedCliPackageCache cache;
    InstallIo();
    const WorkspaceFixture workspace("build");
    WriteGreeterWorkspace(workspace);

    const auto built =
        Run(std::array<std::string_view, 4>{"--manifest", workspace.Manifest(), "--color=never", "build"});
    CAPTURE(built.output);
    CHECK(built.exitCode == 0);
    CHECK(built.output.contains("Compiling workspace (Debug, " +
                                Driver::TargetDisplayName(Rux::Target::TargetTriple::Host()) + ")"));
    CHECK(built.output.contains("Compiling App v0.1.0 (Debug, "));
    CHECK_FALSE(built.output.contains("Compiling  v"));
    CHECK_FALSE(built.output.contains("Compiling Greeter"));
    CHECK(built.output.contains("Output: " + (std::filesystem::path("App") / "Bin" / "Debug").string()));
    CHECK(built.output.contains("Built 1 package in "));
    CHECK(built.output.contains("(1 succeeded, 0 failed)"));

    // A member that fails to compile fails the build without hiding the members that succeed.
    workspace.Workspace("\"App\", \"Greeter\", \"Broken\"");
    workspace.Package("Broken", "Broken", "Executable", "", "Main.rux", "func Main() -> int {\n    return true;\n}\n");
    const auto failed =
        Run(std::array<std::string_view, 4>{"--manifest", workspace.Manifest(), "--color=never", "build"});
    CAPTURE(failed.output);
    CHECK(failed.exitCode == 1);
    CHECK(failed.output.contains("Built App (Debug, "));
    CHECK(failed.output.contains("Failed 2 packages in "));
    CHECK(failed.output.contains("(1 succeeded, 1 failed)"));
}

TEST_CASE("workspace build reports a workspace of source libraries as having nothing to build") {
    const WorkspaceFixture workspace("libraries");
    workspace.Workspace("\"Greeter\"");
    workspace.Package("Greeter", "Greeter", "SourceLibrary", "", "Api.rux", "pub module Greet {}\n");

    const auto built =
        Run(std::array<std::string_view, 4>{"--manifest", workspace.Manifest(), "--color=never", "build"});
    CAPTURE(built.output);
    CHECK(built.exitCode == 1);
    CHECK(built.output.contains("has no member to build"));
    CHECK(built.output.contains(
        "note: a source library is compiled into the packages that depend on it and builds nothing alone"));
    CHECK(built.output.contains("help: check the members with 'rux check'"));
    CHECK_FALSE(built.output.contains("does not exist"));
}

TEST_CASE("workspace run has nothing to run and names its executable members") {
    const ScopedCliPackageCache cache;
    InstallIo();
    const WorkspaceFixture workspace("run");
    WriteGreeterWorkspace(workspace);

    const auto ran = Run(std::array<std::string_view, 4>{"--manifest", workspace.Manifest(), "--color=never", "run"});
    CAPTURE(ran.output);
    CHECK(ran.exitCode == 1);
    CHECK(ran.output.contains("error: manifest '" + workspace.Manifest() + "' is a workspace and has nothing to run"));
    CHECK(ran.output.contains("note: a workspace declares member packages and has no entry point of its own"));
    CHECK(ran.output.contains("help: run its executable member with 'rux --manifest "));
    CHECK(ran.output.contains("App/Rux.toml run'"));
    CHECK_FALSE(ran.output.contains("Greeter/Rux.toml"));
    CHECK_FALSE(ran.output.contains("Compiling"));
}

TEST_CASE("workspace fmt formats the root manifest and every member's manifest and sources") {
    const WorkspaceFixture workspace("fmt");
    workspace.Workspace("\"App\", \"Greeter\"");
    workspace.Package("Greeter", "Greeter", "SourceLibrary", "", "Api.rux", "pub module Greet {}  \r\n");
    workspace.Package("App", "App", "Executable", "", "Main.rux", "func Main() -> int {\n    return 0;\n}\n");
    const auto source = workspace.root / "Greeter" / "Src" / "Api.rux";

    const auto unformatted =
        Run(std::array<std::string_view, 5>{"--manifest", workspace.Manifest(), "--color=never", "fmt", "--check"});
    CAPTURE(unformatted.output);
    CHECK(unformatted.exitCode == 1);
    CHECK(unformatted.output.contains("source file '" + source.string() + "' is not formatted"));
    CHECK_FALSE(unformatted.output.contains("no source files were examined"));

    const auto formatted =
        Run(std::array<std::string_view, 4>{"--manifest", workspace.Manifest(), "--color=never", "fmt"});
    CAPTURE(formatted.output);
    CHECK(formatted.exitCode == 0);
    CHECK(ReadTextFile(source) == "pub module Greet {}\n");

    const auto checked =
        Run(std::array<std::string_view, 5>{"--manifest", workspace.Manifest(), "--color=never", "fmt", "--check"});
    CAPTURE(checked.output);
    CHECK(checked.exitCode == 0);
    CHECK(checked.output.contains("Checked 5 files in "));
}
