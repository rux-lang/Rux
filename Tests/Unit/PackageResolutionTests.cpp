#include "Package/PackageResolution.h"
#include "ThirdParty/doctest.h"

#include <map>
#include <string>
#include <utility>

using namespace Rux::Packages;

using namespace Rux;

namespace {
IdentitySegment Segment(const std::string_view text) {
    auto segment = IdentitySegment::Parse(text);
    REQUIRE(segment.has_value());
    return std::move(*segment);
}

SemanticVersion Version(const std::string_view text) {
    auto version = SemanticVersion::Parse(text);
    REQUIRE(version.has_value());
    return std::move(*version);
}

VersionRange Range(const std::string_view text) {
    auto range = VersionRange::Parse(text);
    REQUIRE(range.has_value());
    return std::move(*range);
}

Target::TargetTriple Triple(const std::string_view text) {
    auto target = Target::TargetTriple::Parse(text);
    REQUIRE(target.has_value());
    return *target;
}

RegistryDependencyEdge Edge(const std::string_view name, const std::string_view range,
                            std::vector<Target::OS> targets = {}) {
    return RegistryDependencyEdge{.alias = Segment(name),
                                  .ns = Segment("Acme"),
                                  .package = Segment(name),
                                  .range = Range(range),
                                  .targetOS = std::move(targets)};
}

RegistryIndexEntry Index(const std::string_view name, std::vector<RegistryVersion> versions) {
    return RegistryIndexEntry{.ns = Segment("Acme"), .package = Segment(name), .versions = std::move(versions)};
}

RegistryVersion Published(const std::string_view version, std::vector<RegistryDependencyEdge> dependencies = {}) {
    return RegistryVersion{
        .version = Version(version), .minRux = std::nullopt, .yanked = false, .dependencies = std::move(dependencies)};
}

PackageRequirement RequirePackage(const std::string_view name, const std::string_view range = "*") {
    return PackageRequirement{.ns = Segment("Acme"), .package = Segment(name), .range = Range(range)};
}

std::string Key(const IdentitySegment &ns, const IdentitySegment &package) {
    return ns.Normalized() + "/" + package.Normalized();
}
} // namespace

TEST_CASE("CollectPackageRequirements filters registry dependencies through typed targets") {
    Manifest manifest;
    manifest.dependencies = {
        ManifestDependency{.importName = Segment("Common"),
                           .package = Segment("Common"),
                           .source = RegistryDependencySource{.ns = Segment("Acme"), .version = Range("^1.0.0")},
                           .targetOS = {}},
        ManifestDependency{.importName = Segment("Platform"),
                           .package = Segment("Win"),
                           .source = RegistryDependencySource{.ns = Segment("Acme"), .version = Range("=2.0.0")},
                           .targetOS = {Target::OS::Windows}},
        ManifestDependency{.importName = Segment("Local"),
                           .package = Segment("Local"),
                           .source = PathDependencySource{.path = "../Local"},
                           .targetOS = {}}};

    const auto windows = CollectPackageRequirements(manifest, Triple("windows-aarch64"));
    REQUIRE(windows.size() == 2);
    CHECK(windows[0].package.Text() == "Common");
    CHECK(windows[1].package.Text() == "Win");

    const auto linux = CollectPackageRequirements(manifest, Triple("linux-x86_64"));
    REQUIRE(linux.size() == 1);
    CHECK(linux[0].package.Text() == "Common");

    CHECK(CollectPackageRequirements(manifest).size() == 2);
}

TEST_CASE("PackageResolver caches indexes and preserves breadth-first discovery order") {
    std::map<std::string, RegistryIndexEntry> indexes;
    indexes.emplace("acme/a", Index("A", {Published("1.0.0", {Edge("C", ">=1.0.0")})}));
    indexes.emplace("acme/b", Index("B", {Published("1.0.0", {Edge("C", "<2.0.0")})}));
    indexes.emplace("acme/c", Index("C", {Published("1.0.0"), Published("2.0.0")}));
    std::map<std::string, int> fetches;
    PackageResolver resolver("https://example.test",
                             [&](std::string_view, const IdentitySegment &ns,
                                 const IdentitySegment &package) -> std::expected<RegistryIndexEntry, RegistryError> {
                                 const std::string key = Key(ns, package);
                                 ++fetches[key];
                                 return indexes.at(key);
                             });
    const std::vector seeds = {RequirePackage("A"), RequirePackage("B")};

    const auto resolved = resolver.Resolve(seeds, Triple("linux-x86_64"), Version("1.0.0"));

    REQUIRE(resolved.has_value());
    REQUIRE(resolved->size() == 3);
    CHECK((*resolved)[0].package.Text() == "A");
    CHECK((*resolved)[1].package.Text() == "B");
    CHECK((*resolved)[2].package.Text() == "C");
    CHECK((*resolved)[2].version.Text() == "1.0.0");
    CHECK(fetches["acme/c"] == 1);
}

TEST_CASE("PackageResolver filters transitive edges with the complete target triple") {
    std::map<std::string, RegistryIndexEntry> indexes;
    indexes.emplace("acme/root", Index("Root", {Published("1.0.0", {Edge("Unix", "*", {Target::OS::Linux}),
                                                                    Edge("Win", "*", {Target::OS::Windows})})}));
    indexes.emplace("acme/unix", Index("Unix", {Published("1.0.0")}));
    indexes.emplace("acme/win", Index("Win", {Published("1.0.0")}));
    PackageResolver resolver("https://example.test",
                             [&](std::string_view, const IdentitySegment &ns,
                                 const IdentitySegment &package) -> std::expected<RegistryIndexEntry, RegistryError> {
                                 return indexes.at(Key(ns, package));
                             });
    const std::vector seeds = {RequirePackage("Root")};

    const auto resolved = resolver.Resolve(seeds, Triple("windows-aarch64"), Version("1.0.0"));

    REQUIRE(resolved.has_value());
    REQUIRE(resolved->size() == 2);
    CHECK((*resolved)[0].package.Text() == "Root");
    CHECK((*resolved)[1].package.Text() == "Win");
}

TEST_CASE("PackageResolver returns registry and selection failures as values") {
    PackageResolver missing("https://example.test",
                            [](std::string_view, const IdentitySegment &,
                               const IdentitySegment &) -> std::expected<RegistryIndexEntry, RegistryError> {
                                return std::unexpected(RegistryError{
                                    .kind = RegistryErrorKind::NotFound, .status = 404, .code = {}, .detail = {}});
                            });
    const std::vector missingSeeds = {RequirePackage("Missing")};
    const auto missingResult = missing.Resolve(missingSeeds, Triple("linux-x86_64"), Version("1.0.0"));
    REQUIRE_FALSE(missingResult.has_value());
    CHECK(missingResult.error().message ==
          "package 'Acme/Missing' is not available from registry 'https://example.test'");
    CHECK(missingResult.error().notes == std::vector<std::string>{"registry response status: 404"});

    PackageResolver incompatible("https://example.test",
                                 [](std::string_view, const IdentitySegment &,
                                    const IdentitySegment &) -> std::expected<RegistryIndexEntry, RegistryError> {
                                     return Index("Library", {Published("1.0.0")});
                                 });
    const std::vector incompatibleSeeds = {RequirePackage("Library", "^2.0.0")};
    const auto incompatibleResult = incompatible.Resolve(incompatibleSeeds, Triple("linux-x86_64"), Version("1.0.0"));
    REQUIRE_FALSE(incompatibleResult.has_value());
    CHECK(incompatibleResult.error().message == "no version of 'Acme/Library' satisfies '^2.0.0'");
    REQUIRE(incompatibleResult.error().notes.size() == 1);
    CHECK(incompatibleResult.error().notes[0] == "registry 'https://example.test' publishes 1.0.0");
}
