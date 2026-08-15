#include "Package/Manifest.h"
#include "Package/PublicationValidation.h"
#include "System/Os.h"

#include <algorithm>
#include <chrono>
#include <doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using namespace Rux;
using namespace Rux::System;

namespace {
constexpr std::string_view canonicalPackage = R"([Manifest]
Version = 1
MinRux = "0.4.0"

[Package]
Namespace = "Rux"
Name = "App"
Version = "1.2.3-alpha.1+linux"
Type = "Executable"
Description = "An example package"
Authors = ["Rux Contributors <info@rux-lang.dev>"]
Keywords = ["Example", "Demo"]
License = "MIT"
LicenseFile = "LICENSE.md"
Repository = "https://github.com/rux-lang/Rux"
Homepage = "https://rux-lang.dev"
ReadmeFile = "README.md"

[Dependencies]
Io = { Namespace = "Rux", Version = "^1.0.0" }
Json = { Namespace = "Acme", Package = "FastJson", Version = ">=2.0.0, <3.0.0" }
Util = { Path = "../Util" }

[Build]
Output = "Dist"

[Build.Defines]
Channel = "Nightly"
Retries = 3
Tracing = true
)";

constexpr std::string_view canonicalWorkspace = R"([Manifest]
Version = 1

[Workspace]
Packages = [
    "Packages/Math",
    "Packages/Memory",
]
)";

Manifest Accepted(const std::string_view text) {
    auto result = Manifest::Parse(text, "Rux.toml");
    if (!result.Ok()) {
        REQUIRE_MESSAGE(result.Ok(), "expected the manifest to parse: ", result.diagnostics.front().message);
    }
    CHECK(result.diagnostics.empty());
    return std::move(*result.manifest);
}

ManifestDiagnostic Rejected(const std::string_view text) {
    auto result = Manifest::Parse(text, "Rux.toml");
    REQUIRE_MESSAGE(!result.Ok(), "expected the manifest to be rejected");
    REQUIRE(result.diagnostics.size() == 1);
    return result.diagnostics.front();
}

// A minimal valid package, so a case can vary one thing at a time.
std::string WithPackage(const std::string_view body, const std::string_view type = "Executable") {
    return std::string("[Manifest]\nVersion = 1\n\n[Package]\nName = \"App\"\nVersion = \"0.1.0\"\nType = \"") +
           std::string(type) + "\"\n" + std::string(body);
}
} // namespace

TEST_CASE("A canonical package manifest round trips byte for byte") {
    const Manifest manifest = Accepted(canonicalPackage);

    CHECK(manifest.header.schemaVersion == 1);
    REQUIRE(manifest.header.minRux.has_value());
    CHECK(manifest.header.minRux->Text() == "0.4.0");
    REQUIRE(manifest.package.ns.has_value());
    CHECK(manifest.package.ns->Text() == "Rux");
    CHECK(manifest.package.name.Text() == "App");
    CHECK(manifest.package.version.Text() == "1.2.3-alpha.1+linux");
    CHECK(manifest.package.type == ManifestPackageType::Executable);
    CHECK(manifest.package.keywords.size() == 2);
    CHECK(manifest.package.readmeFile == "README.md");
    CHECK(manifest.package.licenseFile == "LICENSE.md");
    CHECK_FALSE(manifest.IsWorkspace());

    REQUIRE(manifest.dependencies.size() == 3);
    const auto *io = manifest.FindDependency(*IdentitySegment::Parse("Io"));
    REQUIRE(io != nullptr);
    REQUIRE(io->Registry() != nullptr);
    CHECK(io->Registry()->ns.Text() == "Rux");
    CHECK(io->Registry()->version.Text() == "^1.0.0");

    const auto *json = manifest.FindDependency(*IdentitySegment::Parse("Json"));
    REQUIRE(json != nullptr);
    CHECK(json->package.Text() == "FastJson");

    const auto *util = manifest.FindDependency(*IdentitySegment::Parse("Util"));
    REQUIRE(util != nullptr);
    CHECK(util->IsPath());
    CHECK(util->Path() == "../Util");

    CHECK(manifest.build.output == "Dist");
    CHECK(manifest.build.defines.at("Retries").kind == DefineValue::Kind::Integer);
    CHECK(manifest.build.defines.at("Tracing").kind == DefineValue::Kind::Boolean);
    CHECK(manifest.build.ConfigValues().at("Tracing") == "true");

    CHECK(manifest.Serialize() == canonicalPackage);
}

TEST_CASE("A canonical workspace manifest round trips byte for byte") {
    const Manifest manifest = Accepted(canonicalWorkspace);

    CHECK(manifest.IsWorkspace());
    CHECK(manifest.workspace.packages == std::vector<std::string>{"Packages/Math", "Packages/Memory"});
    CHECK(manifest.Serialize() == canonicalWorkspace);
}

TEST_CASE("Serialization uses canonical order regardless of input order") {
    const Manifest manifest = Accepted(R"([Manifest]
Version = 1

[Build.Defines]
Zeta = "last"
Alpha = "first"

[Build]
Output = "Out"

[Dependencies]
zulu = { Path = "../Zulu" }
Alpha = { Namespace = "Rux", Version = "*" }

[Package]
Name = "App"
Version = "0.1.0"
Type = "SharedLibrary"
)");

    CHECK(manifest.Serialize() == R"([Manifest]
Version = 1

[Package]
Name = "App"
Version = "0.1.0"
Type = "SharedLibrary"

[Dependencies]
Alpha = { Namespace = "Rux", Version = "*" }
zulu = { Path = "../Zulu" }

[Build]
Output = "Out"

[Build.Defines]
Alpha = "first"
Zeta = "last"
)");
}

TEST_CASE("A default build section is left out of canonical form") {
    const Manifest manifest = Accepted(WithPackage("\n[Build]\nOutput = \"Bin\"\n"));
    CHECK(manifest.build.output == "Bin");
    CHECK(manifest.Serialize().find("[Build]") == std::string::npos);
}

TEST_CASE("The parser accepts the documented TOML surface") {
    const Manifest manifest = Accepted(R"(# A leading comment.
[Manifest]
Version = 1   # trailing comment

[Package]
Name = "App"
Version = "0.1.0"
Type = "SourceLibrary"
Description = "quotes \" backslash \\ tab \t newline \n unicode \u00E9"
Authors = [
    "First Author",
    "Second Author",   # a trailing comma and comment are both fine
]

[Dependencies]
"Quoted" = { Path = "../Quoted" }
)");

    CHECK(manifest.package.description == "quotes \" backslash \\ tab \t newline \n unicode \xc3\xa9");
    CHECK(manifest.package.authors.size() == 2);
    REQUIRE(manifest.dependencies.size() == 1);
    CHECK(manifest.dependencies.front().importName.Text() == "Quoted");
}

TEST_CASE("Manifest diagnostics carry the path, line and column") {
    const ManifestDiagnostic diagnostic = Rejected(R"([Manifest]
Version = 1

[Package]
Name = "App"
Version = "not a version"
Type = "Executable"
)");

    CHECK(diagnostic.path == std::filesystem::path("Rux.toml"));
    CHECK(diagnostic.line == 6);
    CHECK(diagnostic.column == 11);
    CHECK(diagnostic.message.find("'[Package].Version' is not a valid version") != std::string::npos);
    CHECK(diagnostic.Format().starts_with("Rux.toml:6:11: "));
    CHECK(diagnostic.Render().starts_with("Rux.toml:6:11: error: "));
    CHECK(diagnostic.Render().contains("6 | Version = \"not a version\""));
    CHECK(diagnostic.Render().contains("docs: https://rux-lang.dev/docs/manifest"));
}

TEST_CASE("Manifest validation accumulates independent failures in source order") {
    const auto result = Manifest::Parse(R"([Manifest]
Version = "1"

[Package]
name = "App"
Version = false
Type = "executable"
Description = 7

[Build]
output = "Bin"
)",
                                        "Rux.toml");

    REQUIRE_FALSE(result.Ok());
    REQUIRE(result.diagnostics.size() == 7);
    CHECK(result.diagnostics[0].message.contains("[Manifest].Version"));
    CHECK(result.diagnostics[1].message == "[Package] must declare 'Name'");
    CHECK(result.diagnostics[2].message == "unknown field 'name' in [Package]");
    CHECK(result.diagnostics[2].help == "field names are case-sensitive; use 'Name'");
    CHECK(result.diagnostics[3].message.contains("[Package].Version"));
    CHECK(result.diagnostics[4].help == "enum values are case-sensitive; use 'Executable'");
    CHECK(result.diagnostics[5].message.contains("[Package].Description"));
    CHECK(result.diagnostics[6].help == "field names are case-sensitive; use 'Output'");
}

TEST_CASE("Manifest input must be valid UTF-8") {
    std::string source = WithPackage("Description = \"");
    source.push_back(static_cast<char>(0xFF));
    source += "\"\n";

    auto result = Manifest::Parse(source, "Rux.toml");
    REQUIRE_FALSE(result.Ok());
    REQUIRE(result.diagnostics.size() == 1);
    const auto &diagnostic = result.diagnostics.front();
    CHECK(diagnostic.line == 8);
    CHECK(diagnostic.column == 16);
    CHECK(diagnostic.message == "manifest contains invalid UTF-8");
    CHECK(diagnostic.help == "save the manifest as valid UTF-8");
}

TEST_CASE("The schema version is required and pinned") {
    CHECK(Rejected("[Package]\nName = \"App\"\nVersion = \"0.1.0\"\nType = \"Executable\"\n")
              .message.find("'[Manifest]'") != std::string::npos);

    const auto unsupported = Rejected("[Manifest]\nVersion = 2\n\n[Package]\nName = \"App\"\n"
                                      "Version = \"0.1.0\"\nType = \"Executable\"\n");
    CHECK(unsupported.line == 2);
    CHECK(unsupported.message.find("unsupported manifest version 2") != std::string::npos);

    CHECK(Rejected("[Manifest]\nVersion = \"1\"\n\n[Package]\nName = \"App\"\nVersion = \"0.1.0\"\n"
                   "Type = \"Executable\"\n")
              .message.find("must be an integer") != std::string::npos);

    CHECK(Rejected("[Manifest]\n\n[Package]\nName = \"App\"\nVersion = \"0.1.0\"\nType = \"Executable\"\n")
              .message.find("must declare 'Version'") != std::string::npos);
}

TEST_CASE("Package and workspace tables are mutually exclusive") {
    CHECK(Rejected(R"([Manifest]
Version = 1

[Package]
Name = "App"
Version = "0.1.0"
Type = "Executable"

[Workspace]
Packages = ["Packages/Math"]
)")
              .message.find("mutually exclusive") != std::string::npos);

    CHECK(Rejected("[Manifest]\nVersion = 1\n").message.find("either '[Package]' or '[Workspace]'") !=
          std::string::npos);
}

TEST_CASE("Unknown, duplicate and mistyped input is rejected") {
    SUBCASE("unknown section") {
        const auto diagnostic = Rejected(WithPackage("\n[Extras]\nKey = \"value\"\n"));
        CHECK(diagnostic.line == 9);
        CHECK(diagnostic.message.find("unknown section '[Extras]'") != std::string::npos);
    }
    SUBCASE("duplicate section") {
        CHECK(Rejected(WithPackage("\n[Build]\nOutput = \"A\"\n\n[Build]\nOutput = \"B\"\n"))
                  .message.find("duplicate section '[Build]'") != std::string::npos);
    }
    SUBCASE("duplicate key") {
        CHECK(Rejected(WithPackage("Description = \"one\"\nDescription = \"two\"\n"))
                  .message.find("duplicate key 'Description'") != std::string::npos);
    }
    SUBCASE("unknown field") {
        CHECK(Rejected(WithPackage("Maintainer = \"someone\"\n")).message.find("unknown field 'Maintainer'") !=
              std::string::npos);
    }
    SUBCASE("wrong value type") {
        CHECK(Rejected(WithPackage("Description = 7\n")).message.find("must be a string, found integer") !=
              std::string::npos);
        CHECK(Rejected(WithPackage("Authors = \"solo\"\n")).message.find("must be an array") != std::string::npos);
    }
    SUBCASE("missing required field") {
        CHECK(Rejected("[Manifest]\nVersion = 1\n\n[Package]\nName = \"App\"\nVersion = \"0.1.0\"\n")
                  .message.find("must declare 'Type'") != std::string::npos);
    }
    SUBCASE("unknown package type") {
        CHECK(Rejected("[Manifest]\nVersion = 1\n\n[Package]\nName = \"App\"\nVersion = \"0.1.0\"\n"
                       "Type = \"bin\"\n")
                  .message.find("'Executable', 'SharedLibrary', 'StaticLibrary' or 'SourceLibrary'") !=
              std::string::npos);
    }
    SUBCASE("the four package types are exact and the retired spellings are rejected") {
        for (const std::string_view type : {"Executable", "SharedLibrary", "StaticLibrary", "SourceLibrary"}) {
            const auto parsedType = ParseManifestPackageType(type);
            REQUIRE(parsedType.has_value());
            CHECK(Accepted(WithPackage("", type)).package.type == *parsedType);
        }
        for (const std::string_view type :
             {"Program", "Library", "Source", "executable", "sharedlibrary", "staticlibrary", "sourcelibrary"}) {
            CHECK(Rejected(WithPackage("", type)).message.find("must be 'Executable'") != std::string::npos);
        }
    }
    SUBCASE("invalid identity") {
        CHECK(Rejected("[Manifest]\nVersion = 1\n\n[Package]\nName = \"My__Pkg\"\nVersion = \"0.1.0\"\n"
                       "Type = \"Executable\"\n")
                  .message.find("not a valid identity") != std::string::npos);
    }
    SUBCASE("the retired license URL field") {
        CHECK(Rejected(WithPackage("LicenseUrl = \"https://example.com/LICENSE.md\"\n"))
                  .message.find("unknown field 'LicenseUrl'") != std::string::npos);
    }
    SUBCASE("the retired unsuffixed readme field") {
        CHECK(Rejected(WithPackage("Readme = \"README.md\"\n")).message.find("unknown field 'Readme'") !=
              std::string::npos);
    }
    SUBCASE("colliding keywords") {
        CHECK(Rejected(WithPackage("Keywords = [\"My_Word\", \"my-word\"]\n")).message.find("collides") !=
              std::string::npos);
    }
}

TEST_CASE("The license expression and the license file are independent") {
    SUBCASE("both together") {
        const Manifest manifest = Accepted(WithPackage("License = \"MIT\"\nLicenseFile = \"LICENSE.md\"\n"));
        CHECK(manifest.package.license == "MIT");
        CHECK(manifest.package.licenseFile == "LICENSE.md");
    }
    SUBCASE("expression alone") {
        const Manifest manifest = Accepted(WithPackage("License = \"MIT\"\n"));
        CHECK(manifest.package.license == "MIT");
        CHECK(manifest.package.licenseFile.empty());
    }
    SUBCASE("file alone") {
        const Manifest manifest = Accepted(WithPackage("LicenseFile = \"LICENSE.md\"\n"));
        CHECK(manifest.package.license.empty());
        CHECK(manifest.package.licenseFile == "LICENSE.md");
    }
    SUBCASE("a license file is a path, not a URL") {
        CHECK(Rejected(WithPackage("LicenseFile = \"https://example.com/LICENSE.md\"\n")).message.find("path") !=
              std::string::npos);
    }
}

TEST_CASE("Metadata URLs must be absolute, hosted and free of credentials") {
    SUBCASE("relative") {
        CHECK(Rejected(WithPackage("Repository = \"github.com/rux-lang/Rux\"\n")).message.find("absolute") !=
              std::string::npos);
    }
    SUBCASE("unsupported scheme") {
        CHECK(Rejected(WithPackage("Repository = \"ftp://example.com/repo\"\n")).message.find("'http' or 'https'") !=
              std::string::npos);
    }
    SUBCASE("missing host") {
        CHECK(Rejected(WithPackage("Repository = \"https:///repo\"\n")).message.find("must include a host") !=
              std::string::npos);
    }
    SUBCASE("credentials") {
        CHECK(Rejected(WithPackage("Repository = \"https://user:secret@example.com/r\"\n"))
                  .message.find("cannot contain credentials") != std::string::npos);
    }
    SUBCASE("embedded space") {
        CHECK(Rejected(WithPackage("Repository = \"https://example.com/a b\"\n")).message.find("invalid character") !=
              std::string::npos);
    }
    SUBCASE("over the length limit") {
        const std::string url = "https://example.com/" + std::string(manifestMaxUrlBytes, 'a');
        CHECK(Rejected(WithPackage("Repository = \"" + url + "\"\n")).message.find("byte URL limit") !=
              std::string::npos);
    }
    SUBCASE("repository and homepage are named in their own diagnostics") {
        CHECK(
            Rejected(WithPackage("Repository = \"github.com/rux-lang/Rux\"\n")).message.find("[Package].Repository") !=
            std::string::npos);
        CHECK(Rejected(WithPackage("Homepage = \"rux-lang.dev\"\n")).message.find("[Package].Homepage") !=
              std::string::npos);
    }
}

TEST_CASE("Malformed TOML is rejected with a location") {
    SUBCASE("unterminated string") {
        CHECK(Rejected(WithPackage("Description = \"open\n")).message.find("unterminated string") != std::string::npos);
    }
    SUBCASE("unknown escape") {
        CHECK(Rejected(WithPackage("Description = \"bad \\q\"\n")).message.find("unknown string escape") !=
              std::string::npos);
    }
    SUBCASE("missing equals") {
        CHECK(Rejected(WithPackage("Description\n")).message.find("expected '='") != std::string::npos);
    }
    SUBCASE("trailing text") {
        CHECK(Rejected(WithPackage("Description = \"a\" extra\n")).message.find("unexpected text") !=
              std::string::npos);
    }
    SUBCASE("keys before any table") {
        CHECK(Rejected("Version = 1\n\n[Manifest]\nVersion = 1\n").message.find("expected a table header") !=
              std::string::npos);
    }
    SUBCASE("unsupported value syntax") {
        CHECK(Rejected(WithPackage("\n[Build.Defines]\nRatio = 1.5\n")).message.find("expected an integer") !=
              std::string::npos);
    }
}

TEST_CASE("Dependency entries follow the documented rules") {
    SUBCASE("registry entries need a namespace and a version") {
        CHECK(Rejected(WithPackage("\n[Dependencies]\nIo = { Version = \"*\" }\n"))
                  .message.find("must declare 'Namespace'") != std::string::npos);
        CHECK(Rejected(WithPackage("\n[Dependencies]\nIo = { Namespace = \"Rux\" }\n"))
                  .message.find("must declare 'Version'") != std::string::npos);
    }
    SUBCASE("path entries cannot carry registry fields") {
        CHECK(Rejected(WithPackage("\n[Dependencies]\nIo = { Namespace = \"Rux\", Path = \"../Io\" }\n"))
                  .message.find("cannot also declare") != std::string::npos);
    }
    SUBCASE("a bare string is not a dependency") {
        CHECK(Rejected(WithPackage("\n[Dependencies]\nIo = \"*\"\n")).message.find("must be an inline table") !=
              std::string::npos);
    }
    SUBCASE("unknown dependency field") {
        CHECK(Rejected(WithPackage("\n[Dependencies]\nIo = { Namespace = \"Rux\", Version = \"*\", Git = \"x\" }\n"))
                  .message.find("unknown dependency field 'Git'") != std::string::npos);
    }
    SUBCASE("invalid version requirement") {
        CHECK(Rejected(WithPackage("\n[Dependencies]\nIo = { Namespace = \"Rux\", Version = \"1 || 2\" }\n"))
                  .message.find("not a valid requirement") != std::string::npos);
    }
    SUBCASE("import names collide after normalization") {
        const auto diagnostic =
            Rejected(WithPackage("\n[Dependencies]\nMy_Pkg = { Path = \"../A\" }\n\"my-pkg\" = { Path = \"../B\" }\n"));
        CHECK(diagnostic.message.find("collides with 'My_Pkg' after normalization") != std::string::npos);
    }
    SUBCASE("an alias keeps both names") {
        const Manifest manifest =
            Accepted(WithPackage("\n[Dependencies]\nJson = { Namespace = \"Acme\", Package = \"FastJson\", "
                                 "Version = \"^2.0.0\" }\n"));
        CHECK(manifest.dependencies.front().importName.Text() == "Json");
        CHECK(manifest.dependencies.front().package.Text() == "FastJson");
    }
    SUBCASE("target operating systems are exact non-empty allow-lists") {
        const Manifest manifest = Accepted(
            WithPackage("\n[Dependencies]\nPlatform = { Path = \"../Platform\", TargetOS = [\"Windows\", \"Linux\", "
                        "\"macOS\"] }\n"));
        const auto &dependency = manifest.dependencies.front();
        CHECK(dependency.targetOS.size() == 3);
        CHECK(dependency.MatchesTarget(Target::OS::Windows));
        CHECK(dependency.MatchesTarget(Target::OS::MacOS));
        CHECK_FALSE(dependency.MatchesTarget(Target::OS::FreeBSD));

        CHECK(Rejected(WithPackage("\n[Dependencies]\nIo = { Path = \"../Io\", TargetOS = [] }\n"))
                  .message.find("cannot be empty") != std::string::npos);
        CHECK(Rejected(
                  WithPackage("\n[Dependencies]\nIo = { Path = \"../Io\", TargetOS = [\"Windows\", \"Windows\"] }\n"))
                  .message.find("duplicate TargetOS") != std::string::npos);
        const auto targetCase =
            Rejected(WithPackage("\n[Dependencies]\nIo = { Path = \"../Io\", TargetOS = [\"MacOS\"] }\n"));
        CHECK(targetCase.message.find("not a supported TargetOS") != std::string::npos);
        CHECK(targetCase.help == "enum values are case-sensitive; use 'macOS'");
        CHECK(Rejected(WithPackage("\n[Dependencies]\nIo = { Path = \"../Io\", TargetOS = \"Windows\" }\n"))
                  .message.find("must be an array") != std::string::npos);
        CHECK(Rejected(WithPackage("\n[Dependencies]\nIo = { Path = \"../Io\", TargetOS = [1] }\n"))
                  .message.find("must be a string") != std::string::npos);
    }
}

TEST_CASE("TargetOS round trips on registry and path dependencies") {
    constexpr std::string_view source = R"([Manifest]
Version = 1

[Package]
Name = "App"
Version = "0.1.0"
Type = "SourceLibrary"

[Dependencies]
Local = { Path = "../Local", TargetOS = ["Linux"] }
macOS = { Namespace = "Rux", Version = "0.1.0", TargetOS = ["macOS"] }
Windows = { Namespace = "Rux", Version = "0.1.0", TargetOS = ["Windows"] }
)";
    const Manifest manifest = Accepted(source);
    CHECK(manifest.Serialize() == source);
}

TEST_CASE("Workspace tables follow the documented rules") {
    SUBCASE("packages cannot be empty") {
        CHECK(Rejected("[Manifest]\nVersion = 1\n\n[Workspace]\nPackages = []\n").message.find("cannot be empty") !=
              std::string::npos);
    }
    SUBCASE("duplicate members are rejected") {
        CHECK(Rejected("[Manifest]\nVersion = 1\n\n[Workspace]\nPackages = [\"A\", \"A\"]\n")
                  .message.find("duplicate workspace package") != std::string::npos);
    }
    SUBCASE("a workspace cannot declare dependencies or build settings") {
        CHECK(Rejected("[Manifest]\nVersion = 1\n\n[Workspace]\nPackages = [\"A\"]\n\n[Dependencies]\n"
                       "Io = { Namespace = \"Rux\", Version = \"*\" }\n")
                  .message.find("cannot declare dependencies") != std::string::npos);
        CHECK(Rejected("[Manifest]\nVersion = 1\n\n[Workspace]\nPackages = [\"A\"]\n\n[Build]\nOutput = \"Out\"\n")
                  .message.find("cannot declare build settings") != std::string::npos);
    }
}

TEST_CASE("Manifest paths must be relative and portable") {
    CHECK(Rejected(WithPackage("\n[Build]\nOutput = \"C:/Out\"\n")).message.find("must be a relative path") !=
          std::string::npos);
    CHECK(Rejected(WithPackage("\n[Build]\nOutput = \"Out\\\\Sub\"\n")).message.find("'/' separators") !=
          std::string::npos);
    CHECK(Rejected(WithPackage("\n[Build]\nOutput = \"./Out\"\n")).message.find("'.' component") != std::string::npos);
    CHECK(Rejected(WithPackage("ReadmeFile = \"../README.md\"\n")).message.find("'..' component") != std::string::npos);
    CHECK(Rejected(WithPackage("\n[Dependencies]\nIo = { Path = \"A/../B\" }\n"))
              .message.find("'..' after a normal component") != std::string::npos);

    // A dependency may still climb out of its own directory first.
    CHECK(Accepted(WithPackage("\n[Dependencies]\nIo = { Path = \"../../Packages/Io\" }\n")).dependencies.size() == 1);
}

TEST_CASE("Oversized manifests are rejected before parsing") {
    std::string huge = std::string(WithPackage("")) + "\n";
    huge.append(manifestMaxBytes, '#');
    auto result = Manifest::Parse(huge, "Rux.toml");
    REQUIRE_FALSE(result.Ok());
    CHECK(result.diagnostics.front().message.find("larger than") != std::string::npos);
}

TEST_CASE("Dependency editing keeps the manifest canonical") {
    Manifest manifest = Accepted(
        WithPackage("\n[Dependencies]\nIo = { Namespace = \"Rux\", Version = \"*\", TargetOS = [\"Windows\"] }\n"));
    const auto io = *IdentitySegment::Parse("Io");
    const auto rux = *IdentitySegment::Parse("Rux");

    CHECK(manifest.AddRegistryDependency(io, rux, *VersionRange::Parse("^1.0.0")));
    REQUIRE(manifest.dependencies.front().targetOS.size() == 1);
    CHECK(manifest.dependencies.front().targetOS.front() == Target::OS::Windows);
    CHECK_FALSE(manifest.AddRegistryDependency(io, rux, *VersionRange::Parse("^1.0.0")));
    CHECK(manifest.AddPathDependency(io, "../Io"));
    REQUIRE(manifest.dependencies.size() == 1);
    CHECK(manifest.dependencies.front().IsPath());
    CHECK(manifest.dependencies.front().targetOS.front() == Target::OS::Windows);

    CHECK(manifest.RemoveDependency(*IdentitySegment::Parse("io")));
    CHECK(manifest.dependencies.empty());
    CHECK_FALSE(manifest.RemoveDependency(io));
}

TEST_CASE("Loading a missing manifest reports a diagnostic instead of throwing") {
    const auto result = Manifest::Load(TempDirectory() / "rux-does-not-exist" / "Rux.toml");
    REQUIRE_FALSE(result.Ok());
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics.front().message.find("could not open") != std::string::npos);
}

TEST_CASE("Manifest-less workspace discovery finds members and test packages") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path root = TempDirectory() / ("rux-workspace-discovery-test-" + std::to_string(nonce));

    const std::vector<std::filesystem::path> expected = {
        root / "Member" / "Rux.toml",
        root / "Member" / "Tests" / "MemberTest" / "Rux.toml",
        root / "Tests" / "Direct" / "Rux.toml",
        root / "Tests" / "Integration" / "Nested" / "Rux.toml",
    };
    for (const auto &manifestPath : expected) {
        std::filesystem::create_directories(manifestPath.parent_path());
        std::ofstream manifest(manifestPath);
        manifest << WithPackage("");
        REQUIRE(manifest.good());
    }

    // Package roots are terminal: manifests nested inside a discovered package
    // do not become additional workspace members.
    const auto hiddenNestedManifest = root / "Tests" / "Direct" / "Nested" / "Rux.toml";
    std::filesystem::create_directories(hiddenNestedManifest.parent_path());
    {
        std::ofstream manifest(hiddenNestedManifest);
        manifest << WithPackage("");
        REQUIRE(manifest.good());
    }

    auto actual = DiscoverManifestlessWorkspaceManifests(root);
    auto sortedExpected = expected;
    std::ranges::sort(sortedExpected);
    CHECK(actual == sortedExpected);

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

TEST_CASE("Package specifications parse into validated identities") {
    const auto qualified = ParsePackageSpec("Rux/Io@^1.2.0");
    REQUIRE(qualified.has_value());
    REQUIRE(qualified->ns.has_value());
    CHECK(qualified->ns->Text() == "Rux");
    CHECK(qualified->name.Text() == "Io");
    REQUIRE(qualified->version.has_value());
    CHECK(qualified->version->Text() == "^1.2.0");

    const auto bare = ParsePackageSpec("Io");
    REQUIRE(bare.has_value());
    CHECK_FALSE(bare->ns.has_value());
    CHECK_FALSE(bare->version.has_value());

    CHECK_FALSE(ParsePackageSpec("Rux/Io@").has_value());
    CHECK_FALSE(ParsePackageSpec("Rux/Io@1 || 2").has_value());
    CHECK_FALSE(ParsePackageSpec("My__Pkg").has_value());
    CHECK_FALSE(ParsePackageSpec("-bad/Io").has_value());
}

// --- Publication validation profile ------------------------------------------

TEST_CASE("the publication profile accepts a complete package manifest") {
    const Manifest manifest = Accepted(R"([Manifest]
Version = 1
MinRux = "0.4.0"

[Package]
Namespace = "Rux"
Name = "App"
Version = "1.2.3"
Type = "SourceLibrary"

[Dependencies]
Io = { Namespace = "Rux", Version = "^1.0.0" }
)");

    CHECK(ValidateForPublication(manifest).empty());
}

TEST_CASE("the publication profile rejects what local validation allows") {
    SUBCASE("a workspace is not a package") {
        const auto rejections = ValidateForPublication(Accepted(canonicalWorkspace));
        REQUIRE(rejections.size() == 1);
        CHECK(rejections.front().contains("workspace"));
    }

    SUBCASE("a namespace-free package is local-only") {
        const auto rejections = ValidateForPublication(Accepted(WithPackage("", "SourceLibrary")));
        // The same manifest also omits MinRux, so both rules report.
        REQUIRE(rejections.size() == 2);
        CHECK(rejections[0].contains("Namespace"));
        CHECK(rejections[1].contains("MinRux"));
    }

    SUBCASE("MinRux is required") {
        const auto rejections = ValidateForPublication(Accepted(R"([Manifest]
Version = 1

[Package]
Namespace = "Rux"
Name = "App"
Version = "0.1.0"
Type = "SourceLibrary"
)"));
        REQUIRE(rejections.size() == 1);
        CHECK(rejections.front().contains("MinRux"));
    }

    SUBCASE("MinRux below the floor is rejected") {
        const auto rejections = ValidateForPublication(Accepted(R"([Manifest]
Version = 1
MinRux = "0.3.9"

[Package]
Namespace = "Rux"
Name = "App"
Version = "0.1.0"
Type = "SourceLibrary"
)"));
        REQUIRE(rejections.size() == 1);
        CHECK(rejections.front().contains("0.3.9"));
    }

    SUBCASE("a path dependency cannot be resolved from the registry") {
        const auto rejections = ValidateForPublication(Accepted(R"([Manifest]
Version = 1
MinRux = "0.4.0"

[Package]
Namespace = "Rux"
Name = "App"
Version = "0.1.0"
Type = "SourceLibrary"

[Dependencies]
Util = { Path = "../Util" }
)"));
        REQUIRE(rejections.size() == 1);
        CHECK(rejections.front().contains("Util"));
    }

    SUBCASE("only SourceLibrary is publishable in Rux 0.4.0") {
        for (const std::string_view type : {"Executable", "SharedLibrary", "StaticLibrary"}) {
            const auto rejections = ValidateForPublication(Accepted(WithPackage("Namespace = \"Rux\"\n", type)));
            REQUIRE(rejections.size() == 2);
            CHECK(rejections[0] == "[Package].Type = \"" + std::string(type) +
                                       "\" cannot be published by Rux 0.4.0; this release publishes only Type = "
                                       "\"SourceLibrary\"");
            CHECK(rejections[1].contains("MinRux"));
        }
    }
}

// --- Repository manifest policy ----------------------------------------------
//
// The repository is the first consumer of Manifest Version 1, so these cases
// hold every checked-in Rux.toml to the contract the registry will apply.

namespace {

// True when `path` is `root` or lies below it.
bool IsBelow(const std::filesystem::path &path, const std::filesystem::path &root) {
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && *relative.begin() != "..";
}

// Every Rux.toml tracked in the repository, in a stable order.
std::vector<std::filesystem::path> RepositoryManifests() {
    std::vector<std::filesystem::path> manifests;
    const auto root = std::filesystem::weakly_canonical(std::filesystem::path(RUX_ROOT_DIR));
    manifests.push_back(root / "Rux.toml");
    for (const auto *subtree : {RUX_PACKAGES_DIR, RUX_TESTS_DIR}) {
        for (const auto &entry :
             std::filesystem::recursive_directory_iterator(std::filesystem::weakly_canonical(subtree))) {
            if (entry.is_regular_file() && entry.path().filename() == "Rux.toml") {
                manifests.push_back(entry.path());
            }
        }
    }
    std::ranges::sort(manifests);
    return manifests;
}

std::string ReadFileText(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("every repository manifest declares the Version 1 schema and is canonical") {
    const auto manifests = RepositoryManifests();
    REQUIRE_MESSAGE(manifests.size() > 1, "no repository manifests found below ", RUX_ROOT_DIR);

    for (const auto &path : manifests) {
        const auto result = Manifest::Load(path);
        REQUIRE_MESSAGE(result.Ok(), "invalid manifest: ",
                        result.diagnostics.empty() ? path.string() : result.diagnostics.front().Format());

        CHECK_MESSAGE(result.manifest->header.schemaVersion == manifestSchemaVersion,
                      "manifest must declare [Manifest] Version = 1: ", path.string());
        // Formatting is byte-exact, so `rux fmt --manifest-only` is a no-op on a
        // checked-in manifest and never shows up as an unrelated diff.
        CHECK_MESSAGE(result.manifest->Serialize() == ReadFileText(path),
                      "manifest is not in canonical form; run 'rux fmt --manifest-only': ", path.string());
    }
}

TEST_CASE("the workspace root lists every first-party package") {
    const auto root = std::filesystem::weakly_canonical(std::filesystem::path(RUX_ROOT_DIR));
    const auto packagesRoot = std::filesystem::weakly_canonical(std::filesystem::path(RUX_PACKAGES_DIR));

    const auto result = Manifest::Load(root / "Rux.toml");
    REQUIRE(result.Ok());
    REQUIRE_MESSAGE(result.manifest->IsWorkspace(), "the repository root must be a workspace manifest");

    for (const auto &member : result.manifest->workspace.packages) {
        CHECK_MESSAGE(std::filesystem::is_regular_file((root / member / "Rux.toml").lexically_normal()),
                      "workspace member has no Rux.toml: ", member);
    }
    for (const auto &entry : std::filesystem::directory_iterator(packagesRoot)) {
        if (!entry.is_directory() || !std::filesystem::is_regular_file(entry.path() / "Rux.toml")) {
            continue;
        }
        const auto member = ("Packages/" + entry.path().filename().string());
        CHECK_MESSAGE(std::ranges::contains(result.manifest->workspace.packages, member),
                      "package is missing from the root workspace: ", member);
    }
}

TEST_CASE("first-party packages are publishable under the Rux namespace") {
    const auto packagesRoot = std::filesystem::weakly_canonical(std::filesystem::path(RUX_PACKAGES_DIR));
    std::size_t packageCount = 0;

    for (const auto &entry : std::filesystem::directory_iterator(packagesRoot)) {
        const auto path = entry.path() / "Rux.toml";
        if (!entry.is_directory() || !std::filesystem::is_regular_file(path)) {
            continue;
        }
        ++packageCount;
        const auto result = Manifest::Load(path);
        REQUIRE_MESSAGE(result.Ok(), "invalid package manifest: ", path.string());
        const Manifest &manifest = *result.manifest;

        // Publication needs a qualified identity, so every shipped package
        // declares one even though the field is optional locally.
        REQUIRE_MESSAGE(manifest.package.ns.has_value(), "first-party package must declare Namespace: ", path.string());
        CHECK_MESSAGE(manifest.package.ns->Text() == "Rux",
                      "first-party package must publish under 'Rux': ", path.string(), ", found '",
                      manifest.package.ns->Text(), "'");
        CHECK_MESSAGE(manifest.package.name.Text() == entry.path().filename().string(),
                      "package name must match its directory: ", path.string());
        CHECK_MESSAGE(!manifest.package.description.empty(), "package needs a description: ", path.string());
        CHECK_MESSAGE(!manifest.package.authors.empty(), "package needs authors: ", path.string());

        // The expression is what a dependency-tree license audit reads and the
        // file is what a human reads, so every shipped package declares both,
        // and the declared file has to exist to survive packing.
        CHECK_MESSAGE(!manifest.package.license.empty(), "package needs a license: ", path.string());
        CHECK_MESSAGE(!manifest.package.licenseFile.empty(), "package needs a license file: ", path.string());
        CHECK_MESSAGE(std::filesystem::is_regular_file(entry.path() / manifest.package.licenseFile),
                      "declared license file is missing: ", path.string());
        CHECK_MESSAGE(std::filesystem::is_regular_file(entry.path() / manifest.package.readmeFile),
                      "declared readme file is missing: ", path.string());

        // MinRux is the other field publication requires and local builds do not.
        CHECK_MESSAGE(ValidateForPublication(manifest).empty(),
                      "first-party package must satisfy the publication profile: ", path.string());

        for (const auto &dependency : manifest.dependencies) {
            // A published package cannot carry a path dependency, so first-party
            // packages depend on each other through the registry form only.
            REQUIRE_MESSAGE(!dependency.IsPath(), "first-party dependency must be a registry entry: ", path.string(),
                            " -> ", dependency.importName.Text());
            const auto *registry = dependency.Registry();
            CHECK_MESSAGE(registry->ns.Text() == "Rux",
                          "first-party dependency must resolve under 'Rux': ", path.string(), " -> ",
                          dependency.importName.Text());
            CHECK_MESSAGE(std::filesystem::is_regular_file(packagesRoot / dependency.package.Text() / "Rux.toml"),
                          "dependency names no first-party package: ", path.string(), " -> ",
                          dependency.package.Text());
        }
    }

    CHECK_MESSAGE(packageCount > 0, "no first-party packages found below ", packagesRoot.string());
}

TEST_CASE("repository Rux tests use canonical local manifests") {
    const auto testsRoot = std::filesystem::weakly_canonical(std::filesystem::path(RUX_TESTS_DIR));
    const auto packagesRoot = std::filesystem::weakly_canonical(std::filesystem::path(RUX_PACKAGES_DIR));
    const auto binariesRoot = std::filesystem::weakly_canonical(std::filesystem::path(RUX_TEST_BIN_DIR));
    std::size_t manifestCount = 0;

    for (const auto &entry : std::filesystem::recursive_directory_iterator(testsRoot)) {
        if (!entry.is_regular_file() || entry.path().filename() != "Rux.toml") {
            continue;
        }
        ++manifestCount;
        const auto result = Manifest::Load(entry.path());
        REQUIRE_MESSAGE(result.Ok(), "invalid test manifest: ",
                        result.diagnostics.empty() ? entry.path().string() : result.diagnostics.front().Format());
        const Manifest &manifest = *result.manifest;

        CHECK_MESSAGE(manifest.package.type == ManifestPackageType::Executable,
                      "test package must use Type = \"Executable\": ", entry.path().string());
        // A test package is built in place and never published, so it stays
        // namespace-free on purpose.
        CHECK_MESSAGE(!manifest.package.ns.has_value(),
                      "test package must stay namespace-free: ", entry.path().string());
        CHECK_MESSAGE(!manifest.package.description.empty(),
                      "test package needs a description: ", entry.path().string());
        CHECK_MESSAGE(std::filesystem::is_regular_file(entry.path().parent_path() / "Src" / "Main.rux"),
                      "test package needs Src/Main.rux: ", entry.path().string());

        const auto output = std::filesystem::weakly_canonical(entry.path().parent_path() / manifest.build.output);
        CHECK_MESSAGE(IsBelow(output, binariesRoot), "test output must stay below Bin/Tests: ", entry.path().string());

        for (const auto &dependency : manifest.dependencies) {
            REQUIRE_MESSAGE(dependency.IsPath(),
                            "test dependencies must use local Path entries: ", entry.path().string(), " -> ",
                            dependency.importName.Text());
            const auto dependencyRoot =
                std::filesystem::weakly_canonical(entry.path().parent_path() / dependency.Path());
            CHECK_MESSAGE(IsBelow(dependencyRoot, packagesRoot),
                          "test dependency must resolve below Packages: ", entry.path().string(), " -> ",
                          dependency.importName.Text());
            const auto dependencyResult = Manifest::Load(dependencyRoot / "Rux.toml");
            REQUIRE_MESSAGE(dependencyResult.Ok(), "local dependency has no valid manifest: ", dependencyRoot.string());
            CHECK_MESSAGE(dependencyResult.manifest->package.name == dependency.package,
                          "dependency name/path mismatch in ", entry.path().string(), ": expected ",
                          dependency.package.Text(), ", found ", dependencyResult.manifest->package.name.Text());
        }
    }

    CHECK_MESSAGE(manifestCount > 0, "no Rux test manifests found below ", testsRoot.string());
}
