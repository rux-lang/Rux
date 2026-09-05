#include "CliProcessTestSupport.h"

using namespace Rux;
using namespace Rux::Testing::CliProcessTestSupport;

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
