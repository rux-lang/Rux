#include "CliProcessTestSupport.h"
#include "Driver/CompilerDriver.h"
#include "Driver/DependencyGraph.h"

using namespace Rux;
using namespace Rux::Driver;
using namespace Rux::Testing::CliProcessTestSupport;

namespace {
struct GraphFixture {
    ScopedCliPackageCache cache;
    std::filesystem::path root = RegistryPackagesDir().parent_path() / "Sources";

    Manifest Make(const std::string &directory, const std::string &name, const std::string &ns = "") const {
        Manifest manifest;
        manifest.package.name = *IdentitySegment::Parse(name);
        if (!ns.empty())
            manifest.package.ns = *IdentitySegment::Parse(ns);
        manifest.package.version = *SemanticVersion::Parse("0.4.0");
        manifest.package.type = ManifestPackageType::SourceLibrary;
        Save(directory, manifest);
        return manifest;
    }

    void Save(const std::string &directory, const Manifest &manifest) const {
        std::filesystem::create_directories(root / directory / "Src");
        REQUIRE(manifest.Save(root / directory / "Rux.toml"));
    }

    void Path(Manifest &owner, const std::string &alias, const std::string &directory) const {
        REQUIRE(owner.AddPathDependency(*IdentitySegment::Parse(alias), "../" + directory));
    }

    void Registry(Manifest &owner, const std::string &alias, const std::string &ns = "rux",
                  const std::string &name = "text", const std::string &version = "^0.4.0") const {
        REQUIRE(owner.AddRegistryDependency(*IdentitySegment::Parse(alias), *IdentitySegment::Parse(ns),
                                            *VersionRange::Parse(version)));
        owner.dependencies.back().package = *IdentitySegment::Parse(name);
    }

    DependencyGraph Graph(Manifest manifest, const std::vector<std::filesystem::path> &locals = {}) const {
        return {std::move(manifest), root / "App" / "Rux.toml", Target::TargetTriple::Host(), locals};
    }

    CompileOptions StageOptions() const {
        const auto source = std::filesystem::path(RUX_TESTS_DIR) / "Fixtures" / "PackageIdentity";
        CompileOptions options;
        for (const std::string directory : {"App", "FirstText", "SecondText", "FirstBridge", "SecondBridge"}) {
            const auto loaded = Manifest::Load(source / directory / (directory == "App" ? "Fixture.toml" : "Rux.toml"));
            REQUIRE(loaded.Ok());
            auto manifest = *loaded.manifest;
            manifest.build.output = "Bin";
            Save(directory, manifest);
            WriteTextFile(root / directory / "Src" / "Main.rux", ReadTextFile(source / directory / "Src" / "Main.rux"));
            if (directory == "App")
                options.manifest = std::move(manifest);
        }
        options.manifestPath = root / "App" / "Rux.toml";
        return options;
    }
};
} // namespace

TEST_CASE("package graph selects a root local source before transitive registry imports in either order") {
    GraphFixture fixture;
    const auto selected = fixture.Make("Selected", "Text", "Rux");
    auto bridge = fixture.Make("Bridge", "Bridge");
    fixture.Registry(bridge, "Words");
    fixture.Save("Bridge", bridge);
    auto app = fixture.Make("App", "Text");
    fixture.Path(app, "LocalWords", "Selected");
    fixture.Path(app, "Bridge", "Bridge");
    for (const bool staleCache : {false, true}) {
        if (staleCache) {
            const auto cached =
                RegistryPackageDir(*selected.package.ns, selected.package.name, selected.package.version);
            std::filesystem::create_directories(cached);
            REQUIRE(selected.Save(cached / "Rux.toml"));
            WriteTextFile(cached / "Src" / "Main.rux", "pub func StaleOnly() -> int { return 99; }");
        }
        for (const bool directFirst : {false, true}) {
            auto graph = fixture.Graph(app);
            if (directFirst)
                REQUIRE(graph.Resolve(graph.Root(), "LocalWords"));
            const auto *owner = graph.Resolve(graph.Root(), "Bridge");
            REQUIRE(owner);
            const auto *dependency = graph.Resolve(*owner, "Words");
            REQUIRE(dependency);
            CHECK(dependency->id == "rux/text");
            CHECK(dependency->Root() == std::filesystem::weakly_canonical(fixture.root / "Selected"));
            CHECK(graph.Resolve(graph.Root(), "LocalWords") == dependency);
            CHECK(graph.Root().id != dependency->id);
            CHECK(graph.Bindings().at(owner->id).at("Words") == dependency->id);
            CHECK(graph.Diagnostics().empty());
        }
    }
}

TEST_CASE("package graph keeps aliases and unnamed package identities local to their owners") {
    GraphFixture fixture;
    fixture.Make("LeftHelper", "Helper");
    fixture.Make("RightHelper", "Helper");
    auto left = fixture.Make("Left", "Owner");
    auto right = fixture.Make("Right", "Owner");
    fixture.Path(left, "Common", "LeftHelper");
    fixture.Path(right, "Common", "RightHelper");
    fixture.Save("Left", left);
    fixture.Save("Right", right);
    auto app = fixture.Make("App", "App");
    fixture.Path(app, "Left", "Left");
    fixture.Path(app, "Right", "Right");
    fixture.Path(app, "SameLeft", "Left/../Left");
    auto graph = fixture.Graph(app);
    const auto *leftOwner = graph.Resolve(graph.Root(), "Left");
    const auto *rightOwner = graph.Resolve(graph.Root(), "Right");
    REQUIRE(leftOwner);
    REQUIRE(rightOwner);
    CHECK(leftOwner != rightOwner);
    CHECK(graph.Resolve(graph.Root(), "SameLeft") == leftOwner);
    const auto *leftHelper = graph.Resolve(*leftOwner, "Common");
    const auto *rightHelper = graph.Resolve(*rightOwner, "Common");
    REQUIRE(leftHelper);
    REQUIRE(rightHelper);
    CHECK(leftHelper->id != rightHelper->id);
    CHECK(graph.Bindings().at(leftOwner->id).at("Common") == leftHelper->id);
    CHECK(graph.Bindings().at(rightOwner->id).at("Common") == rightHelper->id);
    CHECK(graph.Resolve(*leftOwner, "Owner") == leftOwner);
    CHECK_FALSE(graph.Resolve(*rightOwner, "Left"));
    REQUIRE(graph.Diagnostics().size() == 1);
    CHECK(graph.Diagnostics().front().sourceName == rightOwner->manifestPath.string());
    CHECK(graph.Diagnostics().front().message.contains("not listed in [Dependencies]"));
}

TEST_CASE("package graph matches both normalized namespace and name for workspace sources") {
    GraphFixture fixture;
    fixture.Make("First", "My_Text", "Rux_Labs");
    fixture.Make("Second", "My_Text", "Other");
    auto app = fixture.Make("App", "App");
    fixture.Registry(app, "First", "rux-labs", "my-text");
    fixture.Registry(app, "Second", "other", "my-text");
    auto graph = fixture.Graph(app, {fixture.root / "First", fixture.root / "Second"});
    const auto *first = graph.Resolve(graph.Root(), "First");
    const auto *second = graph.Resolve(graph.Root(), "Second");
    REQUIRE(first);
    REQUIRE(second);
    CHECK(first->id == "rux-labs/my-text");
    CHECK(second->id == "other/my-text");
    CHECK(graph.Diagnostics().empty());
}

TEST_CASE("package graph never substitutes a bare name or a different namespace for a registry identity") {
    GraphFixture fixture;
    fixture.Make("Unnamed", "Text");
    fixture.Make("Other", "Text", "Other");
    auto app = fixture.Make("App", "App");
    fixture.Path(app, "Local", "Unnamed");
    fixture.Registry(app, "Words");
    auto graph = fixture.Graph(app, {fixture.root / "Unnamed", fixture.root / "Other"});
    CHECK_FALSE(graph.Resolve(graph.Root(), "Words"));
    REQUIRE(graph.Diagnostics().size() == 1);
    CHECK(graph.Diagnostics().front().message.contains("no installed version of 'rux/text'"));
    CHECK(graph.Diagnostics().front().notes.front() == "no versions are installed");
}

TEST_CASE("package graph reports incompatible local identities and versions at their declaring manifest") {
    GraphFixture fixture;
    fixture.Make("First", "Text", "Rux");
    fixture.Make("Second", "Text", "Rux");
    auto app = fixture.Make("App", "App");
    fixture.Path(app, "First", "First");
    fixture.Path(app, "Second", "Second");
    SUBCASE("two explicit sources for one identity") {
        auto graph = fixture.Graph(app);
        REQUIRE(graph.Resolve(graph.Root(), "First"));
        CHECK_FALSE(graph.Resolve(graph.Root(), "Second"));
        REQUIRE(graph.Diagnostics().size() == 1);
        const auto &error = graph.Diagnostics().front();
        CHECK(error.sourceName == graph.Root().manifestPath.string());
        CHECK(error.message.contains("incompatible sources"));
        REQUIRE(error.notes.size() == 2);
        CHECK(error.notes[0].contains("rux/text@0.4.0"));
        CHECK(error.notes[0].contains("First"));
        CHECK(error.notes[1].contains("Second"));
    }
    SUBCASE("ambiguous workspace sources") {
        fixture.Registry(app, "Words");
        auto graph = fixture.Graph(app);
        CHECK_FALSE(graph.Resolve(graph.Root(), "Words"));
        REQUIRE(graph.Diagnostics().size() == 1);
        CHECK(graph.Diagnostics().front().message.contains("conflicting local sources"));
        CHECK(graph.Diagnostics().front().notes.size() == 2);
    }
    SUBCASE("a selected local source cannot silently violate a version requirement") {
        app.dependencies.pop_back();
        fixture.Registry(app, "Words", "Rux", "Text", "^0.5.0");
        auto graph = fixture.Graph(app);
        CHECK_FALSE(graph.Resolve(graph.Root(), "Words"));
        REQUIRE(graph.Diagnostics().size() == 1);
        CHECK(graph.Diagnostics().front().message.contains("requires Rux/Text@^0.5.0 but selected rux/text@0.4.0"));
    }
}

TEST_CASE("compiler driver executes distinct owner-local aliases in both profiles and import orders") {
    GraphFixture fixture;
    for (const std::string side : {"Left", "Right"}) {
        fixture.Make(side + "Helper", "Helper");
        auto owner = fixture.Make(side, "Owner");
        fixture.Path(owner, "Common", side + "Helper");
        fixture.Save(side, owner);
        WriteTextFile(fixture.root / (side + "Helper") / "Src" / "Main.rux",
                      "pub type " + side + "Native = int; pub func " + side + "Value() -> int { return " +
                          (side == "Left" ? "10" : "20") + "; }");
        WriteTextFile(fixture.root / side / "Src" / "Main.rux",
                      "import Common::{ " + side + "Value, " + side + "Native }; pub type " + side + "Word = " + side +
                          "Native; pub func Get" + side + "() -> int { return " + side + "Value(); }");
    }
    auto app = fixture.Make("App", "App");
    app.package.type = ManifestPackageType::Executable;
    fixture.Path(app, "Left", "Left");
    fixture.Path(app, "Right", "Right");
    for (const auto profile : {BuildProfile::Debug, BuildProfile::Release}) {
        for (const bool reverse : {false, true}) {
            WriteTextFile(
                fixture.root / "App" / "Src" / "Main.rux",
                std::string(reverse ? "import Right::{ GetRight, RightWord }; import Left::{ GetLeft, LeftWord };"
                                    : "import Left::{ GetLeft, LeftWord }; import Right::{ GetRight, RightWord };") +
                    "func Main() -> int { return GetLeft() + GetRight() - 30; }");
            CompileOptions options;
            options.manifestPath = fixture.root / "App" / "Rux.toml";
            options.manifest = app;
            options.profile = profile;
            const auto compiled = CompilerDriver(std::move(options)).Compile();
            for (const auto &diagnostic : compiled.diagnostics)
                INFO(diagnostic.message);
            REQUIRE(compiled.ok);
            CHECK(compiled.stats.dependencyFiles == 4);
            const auto executed = System::RunCaptured(compiled.primaryArtifactPath, std::vector<std::string_view>{});
            REQUIRE(executed);
            CHECK(executed->exitCode == 0);
        }
    }
}

TEST_CASE("same-named packages preserve generic declarations and the executable entry in either import order") {
    GraphFixture fixture;
    auto options = fixture.StageOptions();
    const auto path = fixture.root / "App" / "Src" / "Main.rux";
    const auto original = ReadTextFile(path);
    for (const bool reverse : {false, true}) {
        auto source = original;
        if (reverse) {
            const auto first = source.find('\n');
            const auto second = source.find('\n', first + 1);
            source = source.substr(first + 1, second - first) + source.substr(0, first + 1) + source.substr(second + 1);
        }
        WriteTextFile(path, source);
        for (const auto profile : {BuildProfile::Debug, BuildProfile::Release}) {
            options.profile = profile;
            const auto compiled = CompilerDriver(options).Compile();
            for (const auto &diagnostic : compiled.diagnostics)
                INFO(diagnostic.message);
            REQUIRE(compiled.ok);
            CHECK(compiled.stats.dependencyFiles == 4);
            const auto executed = System::RunCaptured(compiled.primaryArtifactPath, std::vector<std::string_view>{});
            REQUIRE(executed);
            CHECK(executed->exitCode == 0);
        }
    }
}

TEST_CASE("imported constants keep declaration types across generic callers and import orders") {
    GraphFixture fixture;
    const auto source = std::filesystem::path(RUX_TESTS_DIR) / "Fixtures" / "ImportedConstants";
    CompileOptions options;
    for (const std::string directory : {"App", "Generic", "Provider"}) {
        const auto loaded = Manifest::Load(source / directory / (directory == "App" ? "Fixture.toml" : "Rux.toml"));
        REQUIRE(loaded.Ok());
        auto manifest = *loaded.manifest;
        manifest.build.output = "Bin";
        fixture.Save(directory, manifest);
        WriteTextFile(fixture.root / directory / "Src" / "Main.rux",
                      ReadTextFile(source / directory / "Src" / "Main.rux"));
        if (directory == "App")
            options.manifest = std::move(manifest);
    }
    options.manifestPath = fixture.root / "App" / "Rux.toml";
    const auto genericPath = fixture.root / "Generic" / "Src" / "Main.rux";
    const auto original = ReadTextFile(genericPath);
    for (const bool importFirst : {false, true}) {
        const auto importOffset = original.find("import Provider");
        WriteTextFile(genericPath,
                      importFirst ? original.substr(importOffset) + original.substr(0, importOffset) : original);
        for (const bool reverse : {false, true}) {
            auto app = ReadTextFile(source / "App" / "Src" / "Main.rux");
            if (reverse) {
                const auto main = app.find("func Main() -> int {");
                app.insert(main + std::string_view("func Main() -> int {").size(),
                           "\n    if (Nested<int64>() != 25.75 || Read<int32>() != 18.75) { return 6; }\n");
            }
            WriteTextFile(fixture.root / "App" / "Src" / "Main.rux", app);
            for (const auto profile : {BuildProfile::Debug, BuildProfile::Release}) {
                options.profile = profile;
                const auto compiled = CompilerDriver(options).Compile();
                for (const auto &diagnostic : compiled.diagnostics)
                    INFO(diagnostic.message);
                REQUIRE(compiled.ok);
                CHECK(compiled.stats.dependencyFiles == 2);
                const auto executed =
                    System::RunCaptured(compiled.primaryArtifactPath, std::vector<std::string_view>{});
                REQUIRE(executed);
                CHECK(executed->exitCode == 0);
            }
        }
    }
}

TEST_CASE("a dependency or nested module Main cannot supply a missing executable entry") {
    GraphFixture fixture;
    auto options = fixture.StageOptions();
    SUBCASE("dependency entry") {
        WriteTextFile(fixture.root / "App" / "Src" / "Main.rux",
                      "import First::First; pub func Answer() -> int { return First<int32>(7); }");
    }
    SUBCASE("nested entry") {
        WriteTextFile(fixture.root / "App" / "Src" / "Main.rux", "module Nested { func Main() -> int { return 0; } }");
    }
    const auto compiled = CompilerDriver(options).Compile();
    CHECK_FALSE(compiled.ok);
    REQUIRE_FALSE(compiled.diagnostics.empty());
    CHECK(compiled.diagnostics.back().message.contains("entry point symbol 'Main' is undefined"));
}

TEST_CASE("same-named package imports retain private declaration and module boundaries") {
    GraphFixture fixture;
    auto options = fixture.StageOptions();
    options.checkOnly = true;
    fixture.Path(options.manifest, "Words", "FirstText");
    SUBCASE("private function") {
        WriteTextFile(fixture.root / "App" / "Src" / "Main.rux",
                      "import Words::Inner; func Main() -> int { return Inner<int32>(7); }");
    }
    SUBCASE("private module") {
        WriteTextFile(fixture.root / "App" / "Src" / "Main.rux",
                      "import Words::Hidden::Secret; func Main() -> int { return Secret(); }");
    }
    const auto compiled = CompilerDriver(options).Compile();
    CHECK_FALSE(compiled.ok);
    CHECK(std::ranges::any_of(compiled.diagnostics,
                              [](const Diagnostic &diagnostic) { return diagnostic.message.contains("private"); }));
}

TEST_CASE("a caller cannot import an alias declared only by a transitive package") {
    GraphFixture fixture;
    auto options = fixture.StageOptions();
    WriteTextFile(fixture.root / "App" / "Src" / "Main.rux",
                  "import First::First; import Words::Compute; func Main() -> int { return Compute<int32>(7); }");
    const auto compiled = CompilerDriver(options).Compile();
    CHECK_FALSE(compiled.ok);
    const auto error = std::ranges::find_if(compiled.diagnostics, [](const Diagnostic &diagnostic) {
        return diagnostic.message.contains("package 'Words' is not listed in [Dependencies]");
    });
    REQUIRE(error != compiled.diagnostics.end());
    CHECK(error->sourceName == std::filesystem::weakly_canonical(options.manifestPath).string());
}

TEST_CASE("package graph defers unused paths and respects target restrictions") {
    GraphFixture fixture;
    auto app = fixture.Make("App", "App");
    fixture.Path(app, "Absent", "Absent");
    fixture.Path(app, "Excluded", "Excluded");
    app.dependencies.back().targetOS = {Target::OS::Linux};
    DependencyGraph graph(app, fixture.root / "App" / "Rux.toml", *Target::TargetTriple::Parse("windows-x86_64"));
    CHECK(graph.Diagnostics().empty());
    CHECK_FALSE(graph.Resolve(graph.Root(), "Excluded"));
    REQUIRE(graph.Diagnostics().size() == 1);
    CHECK(graph.Diagnostics().front().message.contains("not available for target"));
    CHECK_FALSE(graph.Resolve(graph.Root(), "Absent"));
    CHECK(graph.Diagnostics().back().message.contains("cannot load dependency package"));
}
