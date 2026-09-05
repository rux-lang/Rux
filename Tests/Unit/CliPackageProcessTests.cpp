#include "CliProcessTestSupport.h"

using namespace Rux;
using namespace Rux::Testing::CliProcessTestSupport;

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
    source << "pub module Widget {\n    pub func Answer() -> int { return 42; }\n}\n";
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
